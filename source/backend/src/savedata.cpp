#include "job_queue.hpp"
#include "savedata.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <climits>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "archive_paths.hpp"
#include "file_jobs.hpp"
#include "file_manager.hpp"
#include "log.hpp"
#include "miniz.h"
#include "notify.hpp"
#include "notify_text.hpp"
#include "sqlite3.h"
#include "titles.hpp"

namespace fs = std::filesystem;

namespace prospero {

namespace {

constexpr const char *kHomeDir = "/user/home";

long FileSize(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : 0;
}

bool IsRegularNonEmptyFile(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool IsSafePathSegment(const std::string &value) {
    if (value.empty() || value.size() > 255) return false;
    for (unsigned char c : value) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return value != "." && value != "..";
}

bool IsSafeFilename(const std::string &value) {
    return !value.empty() && value.size() <= 255 && value != "." && value != ".." &&
           value.find('/') == std::string::npos && value.find('\\') == std::string::npos;
}

std::string SaveDirectoryName(std::string filename) {
    if (filename.ends_with(".bin")) filename.resize(filename.size() - 4);
    if (filename.rfind("sdimg_", 0) == 0) filename.erase(0, 6);
    return filename;
}

std::string ReadSaveDatabaseTitle(const SaveEntry &entry) {
    const char *kind = entry.is_ps4 ? "savedata" : "savedata_prospero";
    std::string db_path = std::string("/system_data/") + kind + "/" + entry.user_id_hex +
                          "/db/user/savedata.db";
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return "";
    }
    sqlite3_busy_timeout(db, 1000);

    const char *sql =
        "SELECT main_title FROM savedata WHERE title_id=?1 AND main_title IS NOT NULL AND main_title!='' "
        "ORDER BY CASE WHEN dir_name=?2 THEN 0 ELSE 1 END LIMIT 1";
    sqlite3_stmt *stmt = nullptr;
    std::string title;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        const std::string dir_name = SaveDirectoryName(entry.filename);
        sqlite3_bind_text(stmt, 1, entry.title_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, dir_name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(stmt, 0);
            if (value) title = reinterpret_cast<const char *>(value);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return title;
}

// Every entry directly under <user>/savedata_prospero/<title_id>/ is a PS5
// save image — no fixed naming convention to filter on, unlike PS4's sdimg_*.
void ScanPs5Saves(const std::string &user_hex, const std::string &title_dir, const std::string &title_id,
                   std::vector<SaveEntry> *out) {
    DIR *d = opendir(title_dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        std::string path = title_dir + "/" + ent->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        SaveEntry e;
        e.user_id_hex = user_hex;
        e.title_id = title_id;
        e.filename = ent->d_name;
        e.path = path;
        e.size_bytes = static_cast<long>(st.st_size);
        e.is_ps4 = false;
        out->push_back(std::move(e));
    }
    closedir(d);
}

// PS4/BC saves are the image (sdimg_<name>) plus its sealed key (<name>.bin).
// Both are listed, so a backup carries the pair.
void ScanPs4Saves(const std::string &user_hex, const std::string &title_dir, const std::string &title_id,
                   std::vector<SaveEntry> *out) {
    DIR *d = opendir(title_dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name.rfind("sdimg_", 0) != 0) continue;
        if (name.ends_with(".bin")) continue; // never treat a key as an image

        std::string path = title_dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        SaveEntry e;
        e.user_id_hex = user_hex;
        e.title_id = title_id;
        e.filename = name;
        e.path = path;
        e.size_bytes = static_cast<long>(st.st_size);
        e.is_ps4 = true;
        out->push_back(std::move(e));

        // The sealed key travels with the image — without it the image is unreadable.
        // Console saves drop the sdimg_ prefix.
        for (const std::string &key_name : {name.substr(6) + ".bin", name + ".bin"}) {
            std::string key_path = title_dir + "/" + key_name;
            long key_size = FileSize(key_path);
            if (key_size <= 0) continue;
            SaveEntry key;
            key.user_id_hex = user_hex;
            key.title_id = title_id;
            key.filename = key_name;
            key.path = key_path;
            key.size_bytes = key_size;
            key.is_ps4 = true;
            key.is_sealed_key = true;
            out->push_back(std::move(key));
        }
    }
    closedir(d);
}

void ScanTitles(const std::string &user_hex, const std::string &container_dir, bool is_ps4,
                 std::vector<SaveEntry> *out) {
    DIR *d = opendir(container_dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        std::string title_dir = container_dir + "/" + ent->d_name;
        struct stat st;
        if (stat(title_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (is_ps4) {
            ScanPs4Saves(user_hex, title_dir, ent->d_name, out);
        } else {
            ScanPs5Saves(user_hex, title_dir, ent->d_name, out);
        }
    }
    closedir(d);
}

} // namespace

namespace {

struct SaveSfo {
    std::string title, subtitle, detail, account_id;
};

uint32_t ReadLe32(const std::vector<uint8_t> &b, size_t at) {
    return static_cast<uint32_t>(b[at]) | static_cast<uint32_t>(b[at + 1]) << 8 | static_cast<uint32_t>(b[at + 2]) << 16 |
           static_cast<uint32_t>(b[at + 3]) << 24;
}

// Text as the console wrote it, cut at the first NUL and dropped if it isn't valid UTF-8.
std::string CleanSfoText(const uint8_t *data, size_t len) {
    std::string out(reinterpret_cast<const char *>(data), strnlen(reinterpret_cast<const char *>(data), len));
    for (size_t i = 0; i < out.size();) {
        unsigned char c = static_cast<unsigned char>(out[i]);
        size_t extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : 4;
        if (c < 0x20 && c != '\n') out[i] = ' ';
        if (extra == 4 || i + extra >= out.size()) return "";
        for (size_t k = 1; k <= extra; k++) {
            if ((static_cast<unsigned char>(out[i + k]) & 0xC0) != 0x80) return "";
        }
        i += extra + 1;
    }
    return out;
}

// Reads TITLE/SUBTITLE/DETAIL and the 8-byte ACCOUNT_ID out of a param.sfo; false if it isn't one.
bool ReadSaveSfo(const std::string &path, SaveSfo *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::vector<uint8_t> data(64 * 1024);
    in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(in.gcount()));
    if (data.size() < 20 || data[0] != 0 || data[1] != 'P' || data[2] != 'S' || data[3] != 'F') return false;

    uint32_t key_table = ReadLe32(data, 8), data_table = ReadLe32(data, 12), count = ReadLe32(data, 16);
    if (count > 256) count = 256;
    for (uint32_t i = 0; i < count; i++) {
        size_t entry = 20 + static_cast<size_t>(i) * 16;
        if (entry + 16 > data.size()) break;
        size_t key_at = static_cast<size_t>(key_table) + (data[entry] | data[entry + 1] << 8);
        uint32_t len = ReadLe32(data, entry + 4);
        size_t value_at = static_cast<size_t>(data_table) + ReadLe32(data, entry + 12);
        if (key_at >= data.size() || value_at > data.size() || len > data.size() - value_at) continue;
        std::string key(reinterpret_cast<const char *>(&data[key_at]),
                        strnlen(reinterpret_cast<const char *>(&data[key_at]), std::min<size_t>(32, data.size() - key_at)));
        const uint8_t *value = &data[value_at];
        if (key == "TITLE" || key == "MAINTITLE") out->title = CleanSfoText(value, len);
        else if (key == "SUBTITLE") out->subtitle = CleanSfoText(value, len);
        else if (key == "DETAIL") out->detail = CleanSfoText(value, len);
        else if (key == "ACCOUNT_ID" && len == 8) {
            bool any = false;
            char hex[24] = "0x";
            for (int k = 0; k < 8; k++) {
                any = any || value[k] != 0;
                std::snprintf(hex + 2 + k * 2, 3, "%02x", value[k]);
            }
            if (any) out->account_id = hex;
        }
    }
    return true;
}

// The metadata sfo sits in the save's meta folder under the same name as its icon.
void AttachSaveMetadata(SaveEntry *entry) {
    if (!IsSafePathSegment(entry->user_id_hex) || !IsSafePathSegment(entry->title_id) || !IsSafeFilename(entry->filename)) return;
    const std::string dir_name = SaveDirectoryName(entry->filename);
    const char *roots[] = {"savedata_prospero_meta", "savedata_meta"};
    if (entry->is_ps4) std::swap(roots[0], roots[1]);
    for (const char *root : roots) {
        for (const std::string &name : {dir_name, "sce_bu_" + dir_name}) {
            SaveSfo sfo;
            std::string path = "/user/home/" + entry->user_id_hex + "/" + root + "/user/" + entry->title_id + "/" + name + ".sfo";
            if (!ReadSaveSfo(path, &sfo)) continue;
            entry->save_title = sfo.title;
            entry->save_subtitle = sfo.subtitle;
            entry->save_detail = sfo.detail;
            entry->account_id = sfo.account_id;
            return;
        }
    }
}

} // namespace

std::vector<SaveEntry> ListSaves() {
    std::vector<SaveEntry> out;

    DIR *home = opendir(kHomeDir);
    if (!home) return out;

    struct dirent *ent;
    while ((ent = readdir(home))) {
        if (ent->d_name[0] == '.') continue;
        std::string user_dir = std::string(kHomeDir) + "/" + ent->d_name;
        struct stat st;
        if (stat(user_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        ScanTitles(ent->d_name, user_dir + "/savedata_prospero", false, &out);
        ScanTitles(ent->d_name, user_dir + "/savedata", true, &out);
    }
    closedir(home);

    // main_title lives in the per-user save database, not app.db. One lookup per
    // user/title/platform, attached to every container file.
    std::map<std::string, std::string> title_cache;
    for (auto &entry : out) {
        const std::string key = entry.user_id_hex + (entry.is_ps4 ? ":ps4:" : ":ps5:") + entry.title_id;
        auto it = title_cache.find(key);
        if (it == title_cache.end()) {
            it = title_cache.emplace(key, ReadSaveDatabaseTitle(entry)).first;
        }
        entry.title_name = it->second;
        entry.icon_path = SaveIconPath(entry.user_id_hex, entry.title_id, entry.filename);
        if (!entry.is_sealed_key) AttachSaveMetadata(&entry);
    }
    return out;
}

std::string SaveIconPath(const std::string &user_id_hex, const std::string &title_id,
                         const std::string &filename) {
    if (!IsSafePathSegment(user_id_hex) || !IsSafePathSegment(title_id) || !IsSafeFilename(filename)) {
        return "";
    }

    const std::string dir_name = SaveDirectoryName(filename);
    static const char *const kMetaRoots[] = {
        "savedata_prospero_meta", // PS5 native saves and imported ones alike
        "savedata_meta",          // older/PS4 metadata layouts
        nullptr,
    };
    for (int i = 0; kMetaRoots[i]; i++) {
        const std::string dir = "/user/home/" + user_id_hex + "/" + kMetaRoots[i] +
                                "/user/" + title_id;
        const std::string exact = dir + "/" + dir_name + "_icon0.png";
        if (IsRegularNonEmptyFile(exact)) return exact;

        // Some firmware/import combinations rename the save directory but keep the
        // title-level icon, which still beats an empty square.
        DIR *meta = opendir(dir.c_str());
        if (!meta) continue;
        std::string fallback;
        while (dirent *item = readdir(meta)) {
            std::string name = item->d_name;
            if (name.size() < 10 || !name.ends_with("_icon0.png")) continue;
            std::string candidate = dir + "/" + name;
            if (IsRegularNonEmptyFile(candidate)) {
                fallback = std::move(candidate);
                break;
            }
        }
        closedir(meta);
        if (!fallback.empty()) return fallback;
    }
    return "";
}

namespace {

// Save images are already encrypted, so deflate burns CPU for no gain. Storing them
// turns a backup into a plain copy.
constexpr mz_uint kSaveZipLevel = MZ_NO_COMPRESSION;

// One layout for exports and backups: the path under /user/home, which is what a
// restore prepends back. Anything else imports to the wrong place.
std::string SaveArchiveName(const std::string &path) {
    const std::string prefix = std::string(kHomeDir) + "/";
    return path.rfind(prefix, 0) == 0 ? path.substr(prefix.size()) : path;
}

} // namespace

bool ZipSavesToFile(const std::vector<std::string> &paths, const std::string &out_path, std::string *err) {
    if (paths.empty()) {
        if (err) *err = "no files selected";
        return false;
    }

    // Straight to disk, never a heap archive: a save image is far too big to hold
    // a second copy of in this payload's address space.
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, out_path.c_str(), 0)) {
        if (err) *err = "zip init failed";
        return false;
    }

    bool ok = true;
    for (const auto &path : paths) {
        if (!mz_zip_writer_add_file(&zip, SaveArchiveName(path).c_str(), path.c_str(), nullptr, 0,
                                    kSaveZipLevel)) {
            ok = false;
            break;
        }
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);

    if (!ok) {
        std::error_code ec;
        fs::remove(out_path, ec);
        if (err) *err = "failed to add a save file to the zip";
        return false;
    }
    return true;
}

// ---- Native auto-backup -------------------------------------------------

namespace {

constexpr size_t kBackupsKept = 5;
constexpr size_t kMaxDestinations = 8;
// Room for zip headers and whatever else is writing to the drive meanwhile.
constexpr uint64_t kBackupSpaceMargin = 16ull * 1024 * 1024;

// A destination has to live under one of the console's USB mount points.
const char *const kUsbMounts[] = {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4",
                                  "/mnt/usb5", "/mnt/usb6", "/mnt/usb7", "/mnt/ext0"};

std::string DefaultBackupRoot(const Config &cfg) { return cfg.root_dir + "/save_backups"; }
std::string BackupConfigPath(const Config &cfg) { return cfg.root_dir + "/save_backup_config.txt"; }

// Holds the file manager's external-storage lease, so a backup never overlaps a copy on the same
// drive. Nested use on one thread (restore taking a snapshot) shares the outer lease.
class ExternalLease {
  public:
    explicit ExternalLease(std::string path) : path_(std::move(path)) {
        if (!IsExternalStoragePath(path_)) {
            ok_ = true;
            return;
        }
        if (depth_ > 0) {
            ok_ = true;
        } else {
            std::string error;
            ok_ = BeginFileRead(path_, &error);
        }
        if (ok_) {
            depth_++;
            counted_ = true;
        }
    }
    ~ExternalLease() {
        if (!counted_) return;
        if (--depth_ == 0) EndFileRead(path_);
    }
    ExternalLease(const ExternalLease &) = delete;
    ExternalLease &operator=(const ExternalLease &) = delete;
    bool ok() const { return ok_; }

  private:
    static thread_local int depth_;
    std::string path_;
    bool ok_ = false;
    bool counted_ = false;
};
thread_local int ExternalLease::depth_ = 0;

struct UsbCheck {
    std::string path;    // normalized destination
    std::string mount;   // the USB mount point it lives under
    std::string state;   // ok | not_usb | unsafe | busy | disconnected | read_only
    std::string message;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
};

std::string UsbMountOf(const std::string &path) {
    for (const char *mount : kUsbMounts) {
        size_t len = std::strlen(mount);
        if (path.compare(0, len, mount) == 0 && (path.size() == len || path[len] == '/')) return mount;
    }
    return "";
}

std::string NormalizeDestination(const std::string &raw) {
    std::string path = fs::path(raw).lexically_normal().string();
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

// Never trusts the client: the path has to be a real, writable USB mount right now.
// `leased` is true when the caller already holds the external-storage lease.
UsbCheck CheckUsbDestination(const std::string &raw, bool leased) {
    UsbCheck r;
    r.path = raw;
    auto fail = [&r](const char *state, const char *message) {
        r.state = state;
        r.message = message;
        return r;
    };
    if (raw.empty() || raw[0] != '/') return fail("not_usb", "not a USB drive path");
    for (const auto &part : fs::path(raw)) {
        if (part == "..") return fail("unsafe", "path may not contain ..");
    }
    r.path = NormalizeDestination(raw);
    if (IsKernelUnsafePath(r.path)) return fail("unsafe", "path is not safe to access");
    r.mount = UsbMountOf(r.path);
    if (r.mount.empty()) return fail("not_usb", "not a USB drive path");

    std::unique_ptr<ExternalLease> lease;
    if (!leased) {
        lease = std::make_unique<ExternalLease>(r.mount);
        if (!lease->ok()) return fail("busy", "the drive is busy with another file operation");
    }

    // An empty placeholder under /mnt shares the mount root's device id; a real drive has its own.
    struct stat mount_st {}, root_st {};
    if (stat(r.mount.c_str(), &mount_st) != 0 || !S_ISDIR(mount_st.st_mode) || stat("/mnt", &root_st) != 0 ||
        mount_st.st_dev == root_st.st_dev) {
        return fail("disconnected", "the USB drive is not connected");
    }

    // The deepest folder that exists decides writability and where symlinks may lead.
    fs::path anchor(r.path);
    std::error_code ec;
    while (!fs::exists(anchor, ec) && anchor.has_parent_path() && anchor != anchor.root_path()) {
        anchor = anchor.parent_path();
    }
    char real_mount[PATH_MAX], real_anchor[PATH_MAX];
    if (!realpath(r.mount.c_str(), real_mount) || !realpath(anchor.c_str(), real_anchor)) {
        return fail("disconnected", "the USB drive is not readable");
    }
    size_t mount_len = std::strlen(real_mount);
    if (std::strncmp(real_anchor, real_mount, mount_len) != 0 ||
        (real_anchor[mount_len] != '\0' && real_anchor[mount_len] != '/')) {
        return fail("unsafe", "path leads outside the USB drive");
    }

    struct statvfs usage {};
    if (statvfs(anchor.c_str(), &usage) != 0) return fail("disconnected", "the USB drive is not readable");
    r.total_bytes = static_cast<uint64_t>(usage.f_blocks) * usage.f_frsize;
    r.free_bytes = static_cast<uint64_t>(usage.f_bavail) * usage.f_frsize;
    if ((usage.f_flag & ST_RDONLY) || access(anchor.c_str(), W_OK) != 0) {
        return fail("read_only", "the USB drive is read-only");
    }
    r.state = "ok";
    return r;
}

// Same PPSA/CUSA prefix convention titles.cpp uses — duplicated here rather than exported,
// since it's one line and the only other place that needs it.
std::string PlatformFromTitleId(const std::string &title_id) {
    if (title_id.rfind("PPSA", 0) == 0) return "PS5";
    if (title_id.rfind("CUSA", 0) == 0) return "PS4";
    return "APP";
}

std::string Timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// Keeps only the newest backups for a title, so an unattended watcher can't fill a drive.
// Ordered by modification time: names carry local time, which isn't comparable across zone changes.
void PruneOldBackups(const std::string &title_dir, size_t keep) {
    std::error_code ec;
    std::vector<std::pair<time_t, fs::path>> zips;
    for (const auto &entry : fs::directory_iterator(title_dir, ec)) {
        if (entry.path().extension() != ".zip") continue;
        struct stat st {};
        if (stat(entry.path().c_str(), &st) == 0) zips.emplace_back(st.st_mtime, entry.path());
    }
    if (zips.size() <= keep) return;
    std::sort(zips.begin(), zips.end());
    for (size_t i = 0; i + keep < zips.size(); i++) fs::remove(zips[i].second, ec);
}

// Roots whose backups may be listed, downloaded, restored or deleted: the local default
// plus every configured USB destination that is connected right now.
std::vector<std::string> ReadableBackupRoots(const Config &cfg) {
    std::vector<std::string> roots{DefaultBackupRoot(cfg)};
    for (const auto &destination : GetSaveBackupConfig(cfg).destinations) {
        UsbCheck check = CheckUsbDestination(destination, false);
        if (check.state == "ok" || check.state == "read_only") roots.push_back(check.path);
    }
    return roots;
}

// Writes one backup zip under `root`/<title_id>/. The zip is built and verified beside its final
// name, so a failure at any point leaves earlier backups untouched.
SaveBackupResult WriteBackup(const std::string &root, bool is_usb, const std::string &title_id,
                             const std::vector<std::string> &paths, uint64_t source_bytes) {
    SaveBackupResult result;
    result.destination = root;
    auto fail = [&result](const char *code, const std::string &message) {
        result.code = code;
        result.error = message;
        return result;
    };

    std::unique_ptr<ExternalLease> lease;
    std::string verified_root = root;
    if (is_usb) {
        std::string mount = UsbMountOf(NormalizeDestination(root));
        lease = std::make_unique<ExternalLease>(mount.empty() ? root : mount);
        if (!lease->ok()) return fail("busy", "the drive is busy with another file operation");
        UsbCheck check = CheckUsbDestination(root, true);
        if (check.state != "ok") return fail(check.state.c_str(), check.message);
        verified_root = check.path;
        if (check.free_bytes < source_bytes + kBackupSpaceMargin) {
            return fail("no_space", "not enough free space on the USB drive");
        }
    }

    std::error_code ec;
    std::string title_dir = verified_root + "/" + title_id;
    fs::create_directories(title_dir, ec);
    if (ec) return fail("write_failed", "could not create the backup folder: " + ec.message());

    std::string stamp = Timestamp();
    std::string final_path = title_dir + "/" + stamp + ".zip";
    for (int n = 2; fs::exists(final_path, ec) && n < 50; n++) {
        final_path = title_dir + "/" + stamp + "-" + std::to_string(n) + ".zip";
    }
    std::string staged_path = final_path + ".tmp";

    std::string zip_err;
    if (!ZipSavesToFile(paths, staged_path, &zip_err)) {
        fs::remove(staged_path, ec);
        return fail("write_failed", zip_err);
    }

    // A truncated archive (drive pulled, disk full) fails to open or comes up short.
    mz_zip_archive check_zip{};
    bool intact = mz_zip_reader_init_file(&check_zip, staged_path.c_str(), 0) &&
                  mz_zip_reader_get_num_files(&check_zip) == paths.size();
    mz_zip_reader_end(&check_zip);
    if (!intact) {
        fs::remove(staged_path, ec);
        return fail("verify_failed", "the written backup did not verify");
    }

    int fd = open(staged_path.c_str(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    fs::rename(staged_path, final_path, ec);
    if (ec) {
        fs::remove(staged_path, ec);
        return fail("write_failed", "could not publish the backup file: " + ec.message());
    }

    PruneOldBackups(title_dir, kBackupsKept);
    result.ok = true;
    result.code = "ok";
    return result;
}

} // namespace

SaveBackupConfig GetSaveBackupConfig(const Config &cfg) {
    SaveBackupConfig out;
    std::ifstream in(BackupConfigPath(cfg));
    if (!in.is_open()) return out;
    std::string enabled_line;
    std::getline(in, enabled_line);
    in >> out.settle_seconds;
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    // The third line was the single destination before multi-drive support; more follow it.
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] != '/') continue;
        std::string path = NormalizeDestination(line);
        if (std::find(out.destinations.begin(), out.destinations.end(), path) == out.destinations.end()) {
            out.destinations.push_back(std::move(path));
        }
    }
    out.enabled = enabled_line == "1";
    if (out.settle_seconds < 3) out.settle_seconds = 3;
    return out;
}

bool SetSaveBackupConfig(const Config &cfg, const SaveBackupConfig &config, std::string *err) {
    if (config.destinations.size() > kMaxDestinations) {
        if (err) *err = "too many backup destinations";
        return false;
    }
    std::vector<std::string> known = GetSaveBackupConfig(cfg).destinations;
    std::vector<std::string> destinations;
    for (const auto &raw : config.destinations) {
        UsbCheck check = CheckUsbDestination(raw, false);
        if (check.state == "not_usb" || check.state == "unsafe") {
            if (err) *err = "backup destinations must be folders on a USB drive";
            return false;
        }
        // A drive that is unplugged may stay in the list; a new one has to be usable now.
        bool already_saved = std::find(known.begin(), known.end(), check.path) != known.end();
        if (check.state != "ok" && !already_saved) {
            if (err) *err = check.message;
            return false;
        }
        if (std::find(destinations.begin(), destinations.end(), check.path) == destinations.end()) {
            destinations.push_back(check.path);
        }
    }

    std::string path = BackupConfigPath(cfg);
    std::string staged = path + ".tmp";
    {
        std::ofstream out(staged, std::ios::trunc);
        if (!out.is_open()) {
            if (err) *err = "could not write save backup config";
            return false;
        }
        out << (config.enabled ? "1" : "0") << '\n' << std::max(3, config.settle_seconds) << '\n';
        // Kept as the third line so a build from before multi-drive support still reads it.
        if (destinations.empty()) out << '\n';
        for (const auto &destination : destinations) out << destination << '\n';
        out.flush();
        if (!out) {
            std::error_code ignored;
            fs::remove(staged, ignored);
            if (err) *err = "could not write save backup config";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(staged, path, ec);
    if (ec) {
        fs::remove(staged, ec);
        if (err) *err = "could not write save backup config";
        return false;
    }
    return true;
}

std::vector<SaveBackupDestination> GetSaveBackupDestinations(const Config &cfg) {
    std::vector<SaveBackupDestination> out;
    for (const auto &destination : GetSaveBackupConfig(cfg).destinations) {
        UsbCheck check = CheckUsbDestination(destination, false);
        SaveBackupDestination item;
        item.path = check.path;
        item.state = check.state;
        item.message = check.message;
        item.total_bytes = check.total_bytes;
        item.free_bytes = check.free_bytes;
        out.push_back(std::move(item));
    }
    return out;
}

bool IsSaveBackupPath(const Config &cfg, const std::string &path) {
    if (path.empty() || path[0] != '/') return false;
    for (const auto &part : fs::path(path)) {
        if (part == "..") return false;
    }
    std::string normalized = fs::path(path).lexically_normal().string();
    for (const auto &root : ReadableBackupRoots(cfg)) {
        std::string prefix = fs::path(root).lexically_normal().string() + "/";
        if (normalized.rfind(prefix, 0) != 0) continue;

        // Exactly <root>/<title_id>/<file>.zip, a real file and not a link out of the root.
        fs::path relative = fs::path(normalized.substr(prefix.size()));
        if (std::distance(relative.begin(), relative.end()) != 2) return false;
        if (!IsSafePathSegment(relative.begin()->string()) || relative.extension() != ".zip") return false;
        struct stat st {};
        if (lstat(normalized.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
        char real_file[PATH_MAX], real_root[PATH_MAX];
        if (!realpath(normalized.c_str(), real_file) || !realpath(root.c_str(), real_root)) return false;
        size_t root_len = std::strlen(real_root);
        return std::strncmp(real_file, real_root, root_len) == 0 && real_file[root_len] == '/';
    }
    return false;
}

std::vector<SaveBackupEntry> ListSaveBackups(const Config &cfg) {
    std::vector<SaveBackupEntry> out;
    std::error_code ec;

    auto titles = ListTitles(cfg);
    std::map<std::string, std::string> title_names;
    for (const auto &t : titles) title_names[t.title_id] = t.name;

    for (const auto &root : ReadableBackupRoots(cfg)) {
        ExternalLease lease(root);
        if (!lease.ok() || !fs::is_directory(root, ec)) continue;
        for (const auto &title_dir : fs::directory_iterator(root, ec)) {
            if (!title_dir.is_directory()) continue;
            std::string title_id = title_dir.path().filename().string();
            for (const auto &zip_entry : fs::directory_iterator(title_dir.path(), ec)) {
                if (zip_entry.path().extension() != ".zip") continue;
                struct stat st {};
                if (stat(zip_entry.path().c_str(), &st) != 0) continue;

                SaveBackupEntry b;
                b.title_id = title_id;
                auto it = title_names.find(title_id);
                b.title_name = it != title_names.end() ? it->second : title_id;
                b.platform = PlatformFromTitleId(title_id);
                b.filename = zip_entry.path().filename().string();
                b.path = zip_entry.path().string();
                b.destination = root;
                b.size_bytes = static_cast<uint64_t>(st.st_size);
                b.created_unix = static_cast<int64_t>(st.st_mtime);
                out.push_back(std::move(b));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.created_unix > b.created_unix; });
    return out;
}

std::vector<SaveBackupResult> BackupTitleSavesEach(const Config &cfg, const std::string &title_id) {
    std::vector<SaveEntry> matching;
    for (auto &entry : ListSaves()) {
        if (entry.title_id == title_id) matching.push_back(std::move(entry));
    }
    if (matching.empty()) {
        SaveBackupResult none;
        none.code = "no_saves";
        none.error = "no save data found for that title";
        return {none};
    }

    std::vector<std::string> paths;
    uint64_t source_bytes = 0;
    for (const auto &entry : matching) {
        paths.push_back(entry.path);
        source_bytes += static_cast<uint64_t>(entry.size_bytes);
    }

    // Every configured drive gets its own copy; the internal default is only used when no
    // drive was chosen, never as a stand-in for one that is missing.
    std::vector<SaveBackupResult> results;
    std::vector<std::string> destinations = GetSaveBackupConfig(cfg).destinations;
    if (destinations.empty()) {
        results.push_back(WriteBackup(DefaultBackupRoot(cfg), false, title_id, paths, source_bytes));
    }
    for (const auto &destination : destinations) {
        results.push_back(WriteBackup(destination, true, title_id, paths, source_bytes));
    }
    return results;
}

namespace {

std::string DescribeFailures(const std::vector<SaveBackupResult> &results) {
    std::string text;
    for (const auto &r : results) {
        if (r.ok) continue;
        if (!text.empty()) text += "; ";
        text += (r.destination.empty() ? "" : r.destination + ": ") + r.error;
    }
    return text;
}

} // namespace

namespace {

// "USB 0" for /mnt/usb0, so a notification names the drive instead of a path.
std::string DriveLabel(const std::string &path) {
    if (path.rfind("/mnt/usb", 0) == 0 && path.size() > 8) return std::string("USB ") + path[8];
    if (path.rfind("/mnt/ext0", 0) == 0) return NotifyText(Msg::kDriveExtUsb);
    return NotifyText(Msg::kDriveInternal);
}

std::string ShortReason(const SaveBackupResult &r) {
    if (r.code == "disconnected") return NotifyText(Msg::kReasonDisconnected);
    if (r.code == "read_only") return NotifyText(Msg::kReasonReadOnly);
    if (r.code == "busy") return NotifyText(Msg::kReasonBusy);
    if (r.code == "no_space") return NotifyText(Msg::kReasonNoSpace);
    if (r.code == "write_failed") return NotifyText(Msg::kReasonWriteFailed);
    if (r.code == "verify_failed") return NotifyText(Msg::kReasonVerifyFailed);
    if (r.code == "no_saves") return NotifyText(Msg::kReasonNoSaves);
    if (r.code == "not_usb" || r.code == "unsafe") return NotifyText(Msg::kReasonNotUsb);
    return r.error;
}

// The text of the on-screen notice for one finished backup: where it went and what did not work.
std::string BackupNotice(const std::string &name, const std::vector<SaveBackupResult> &results) {
    std::string saved, missed;
    for (const auto &r : results) {
        std::string label = DriveLabel(r.destination);
        if (r.ok) saved += (saved.empty() ? "" : ", ") + label;
        else missed += (missed.empty() ? "" : "; ") + label + ": " + ShortReason(r);
    }
    if (missed.empty()) return NotifyText(Msg::kSavesBackedUp, {{"name", name}, {"drives", saved}});
    if (saved.empty()) return NotifyText(Msg::kSaveBackupFailed, {{"name", name}, {"problems", missed}});
    return NotifyText(Msg::kSavesBackedUpTo, {{"drives", saved}, {"name", name}, {"problems", missed}});
}

} // namespace

bool BackupTitleSaves(const Config &cfg, const std::string &title_id, std::string *err) {
    std::vector<SaveBackupResult> results = BackupTitleSavesEach(cfg, title_id);
    std::string failures = DescribeFailures(results);
    if (failures.empty()) return true;
    if (err) *err = failures;
    return false;
}

bool SnapshotTitleSaves(const Config &cfg, const std::string &title_id, std::string *err) {
    std::vector<SaveBackupResult> results = BackupTitleSavesEach(cfg, title_id);
    for (const auto &r : results) {
        if (r.ok) return true;
    }
    if (err) *err = DescribeFailures(results);
    return false;
}

namespace {

constexpr const char *kRestoreTempSuffix = ".pmgr-restore";

// A PS4 image is unreadable without its sealed key, so a backup that lost one
// would restore a save the console rejects as corrupt. PS5 saves have no key.
bool PlanHasEveryKey(const std::vector<std::pair<mz_uint, fs::path>> &plan, std::string *err) {
    std::set<std::string> names;
    for (const auto &item : plan) names.insert(item.second.string());
    for (const auto &item : plan) {
        std::string file = item.second.filename().string();
        if (file.rfind("sdimg_", 0) != 0 || file.ends_with(".bin")) continue;
        if (item.second.parent_path().parent_path().filename() != "savedata") continue;
        std::string dir = item.second.parent_path().string() + "/";
        if (names.count(dir + file.substr(6) + ".bin") || names.count(dir + file + ".bin")) continue;
        if (err) *err = "backup is incomplete: " + file + " has no sealed key";
        return false;
    }
    return true;
}

} // namespace

bool RestoreSaveBackup(const Config &cfg, const std::string &backup_path, std::string *err,
                       std::string *code, bool snapshot_first) {
    auto fail = [&](const char *reason, const std::string &message) {
        if (code) *code = reason;
        if (err) *err = message;
        return false;
    };

    ExternalLease lease(backup_path);
    if (!lease.ok()) return fail("busy", "the drive is busy with another file operation");

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, backup_path.c_str(), 0)) {
        return fail("invalid_zip", "could not open backup — is it valid?");
    }

    // Validate every entry before writing anything, so a backup we would reject
    // never gets part-way applied.
    const fs::path home_root(kHomeDir);
    std::vector<std::pair<mz_uint, fs::path>> plan;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat_info;
        if (!mz_zip_reader_file_stat(&zip, i, &stat_info)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        // Every entry name is "<hex>/savedata.../<title>/<file>" — same
        // ZIP Slip guard the general file-manager unzip uses (archive_paths.hpp).
        fs::path relative;
        std::string path_err;
        if (!SafeArchiveRelativePath(stat_info.m_filename, &relative, &path_err)) {
            mz_zip_reader_end(&zip);
            return fail("unsafe_path", "backup contains an unsafe path");
        }
        fs::path normalized = (home_root / relative).lexically_normal();
        if (!IsWithinRoot(home_root, normalized)) {
            mz_zip_reader_end(&zip);
            return fail("unsafe_path", "backup contains an unsafe path");
        }
        plan.emplace_back(i, normalized);
    }
    if (plan.empty()) {
        mz_zip_reader_end(&zip);
        return fail("no_files", "backup contains no save files");
    }
    // Saves belong to a user id. One that isn't on this console (after a factory reset, say)
    // would land in a folder no game ever reads, and still report success.
    for (const auto &item : plan) {
        std::error_code user_ec;
        fs::path relative = item.second.lexically_relative(home_root);
        if (relative.empty() || !fs::is_directory(home_root / *relative.begin(), user_ec)) {
            mz_zip_reader_end(&zip);
            return fail("unknown_user", "this backup belongs to a user that is not on this console");
        }
    }
    if (!PlanHasEveryKey(plan, err)) {
        mz_zip_reader_end(&zip);
        if (code) *code = "missing_key";
        return false;
    }

    // Taken from the entries, not the file's own location, so an uploaded save
    // is identified the same way a stored backup is.
    std::string title_id = plan.front().second.parent_path().filename().string();

    // The console holds a running title's save open; replacing it underneath is
    // how a save gets corrupted.
    if (!title_id.empty() && GetRunningTitleId() == title_id) {
        mz_zip_reader_end(&zip);
        return fail("title_running", "close the game before restoring its save");
    }

    // Snapshot what is on the console now, so a bad restore can be undone.
    bool has_existing = false;
    if (snapshot_first) {
        for (const auto &entry : ListSaves()) {
            if (entry.title_id == title_id) { has_existing = true; break; }
        }
    }
    if (has_existing) {
        std::string snapshot_err;
        if (!SnapshotTitleSaves(cfg, title_id, &snapshot_err)) {
            mz_zip_reader_end(&zip);
            return fail("snapshot_failed", "could not back up the current save first: " + snapshot_err);
        }
    }

    // Unpack beside each target first. A failure here leaves the existing save
    // untouched rather than half-replaced.
    std::error_code ec;
    bool ok = true;
    for (const auto &item : plan) {
        fs::create_directories(item.second.parent_path(), ec);
        std::string temp = item.second.string() + kRestoreTempSuffix;
        if (!mz_zip_reader_extract_to_file(&zip, item.first, temp.c_str(), 0)) {
            ok = false;
            if (code) *code = "unpack_failed";
            if (err) *err = "failed to unpack " + item.second.filename().string();
            break;
        }
        chmod(temp.c_str(), 0666);
    }
    mz_zip_reader_end(&zip);

    if (ok) {
        for (const auto &item : plan) {
            std::string temp = item.second.string() + kRestoreTempSuffix;
            if (std::rename(temp.c_str(), item.second.c_str()) != 0) {
                ok = false;
                if (code) *code = "replace_failed";
                if (err) *err = "failed to replace " + item.second.filename().string();
                break;
            }
        }
    }
    for (const auto &item : plan) std::remove((item.second.string() + kRestoreTempSuffix).c_str());
    return ok;
}

bool DeleteTitleSaves(const Config &cfg, const std::string &user_id_hex, const std::string &title_id, bool backup_first,
                      std::string *err, std::string *code) {
    auto fail = [&](const char *reason, const std::string &message) {
        if (code) *code = reason;
        if (err) *err = message;
        return false;
    };

    // Deleting a save the console still has open corrupts what's left of it.
    if (GetRunningTitleId() == title_id) {
        return fail("title_running", "close the game before deleting its save");
    }

    // Only paths ListSaves() found itself, never anything the client sent.
    std::vector<std::string> paths;
    for (const auto &entry : ListSaves()) {
        if (entry.user_id_hex == user_id_hex && entry.title_id == title_id) paths.push_back(entry.path);
    }
    if (paths.empty()) {
        return fail("no_saves", "no save data found for that user and title");
    }

    // Without the backup this cannot be undone; the caller chooses.
    if (backup_first) {
        std::string backup_err;
        if (!SnapshotTitleSaves(cfg, title_id, &backup_err)) {
            return fail("snapshot_failed", "could not back up the save before deleting it: " + backup_err);
        }
    }

    for (const auto &path : paths) {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            return fail("delete_failed", "could not delete " + fs::path(path).filename().string());
        }
    }
    LogEvent("Deleted save data for " + title_id + (backup_first ? " (backed up first)" : " (no backup)"));
    return true;
}

namespace {

std::mutex g_save_job_mu;
SaveJobStatus g_save_job;

// One slot for import, restore and back-up-all alike: they all rewrite the same
// files, so a second one starting mid-flight is never right.
bool BeginSaveJob(SaveJobKind kind, const std::string &label, std::string *err) {
    std::lock_guard<std::mutex> lock(g_save_job_mu);
    if (g_save_job.phase == SaveJobPhase::kRunning) {
        if (err) *err = "another save operation is already in progress";
        return false;
    }
    g_save_job = SaveJobStatus{};
    g_save_job.kind = kind;
    g_save_job.phase = SaveJobPhase::kRunning;
    g_save_job.label = label;
    return true;
}

void FinishSaveJob(bool ok, const std::string &error, const std::string &code) {
    std::lock_guard<std::mutex> lock(g_save_job_mu);
    g_save_job.phase = ok ? SaveJobPhase::kDone : SaveJobPhase::kError;
    g_save_job.error = ok ? "" : error;
    g_save_job.error_code = ok ? "" : code;
}

} // namespace

SaveJobStatus GetSaveJobStatus() {
    std::lock_guard<std::mutex> lock(g_save_job_mu);
    return g_save_job;
}

bool StartSaveImportJob(const Config &cfg, const std::string &zip_path, const std::string &label,
                        std::string *err) {
    if (!BeginSaveJob(SaveJobKind::kImport, label, err)) return false;

    StartBackgroundThread([cfg, zip_path] {
        std::string import_err, import_code;
        bool ok = RestoreSaveBackup(cfg, zip_path, &import_err, &import_code);
        std::remove(zip_path.c_str());
        FinishSaveJob(ok, import_err, import_code);
        // Sony's notification service must not hold the status lock, or polling
        // stalls behind the toast.
        if (ok) Notify("Prospero Manager\n" + NotifyText(Msg::kSaveImported));
    });
    return true;
}

bool StartSaveRestoreJob(const Config &cfg, const std::string &backup_path, const std::string &label,
                         bool snapshot_first, std::string *err) {
    if (!BeginSaveJob(SaveJobKind::kRestore, label, err)) return false;

    StartBackgroundThread([cfg, backup_path, snapshot_first] {
        std::string restore_err, restore_code;
        bool ok = RestoreSaveBackup(cfg, backup_path, &restore_err, &restore_code, snapshot_first);
        FinishSaveJob(ok, restore_err, restore_code);
        if (ok) Notify("Prospero Manager\n" + NotifyText(Msg::kSaveRestored));
    });
    return true;
}

void StartSaveAutoBackupWatcher(const Config &cfg) {
    StartBackgroundThread([cfg] {
        std::string previous_title;
        std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> pending;

        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            try {
                std::string current = GetRunningTitleId();
                if (current != previous_title) {
                    if (!previous_title.empty()) pending.emplace_back(previous_title, std::chrono::steady_clock::now());
                    previous_title = current;
                }
                if (pending.empty()) continue;

                SaveBackupConfig conf = GetSaveBackupConfig(cfg);
                auto now = std::chrono::steady_clock::now();
                for (auto it = pending.begin(); it != pending.end();) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
                    if (elapsed < conf.settle_seconds) {
                        ++it;
                        continue;
                    }
                    if (conf.enabled) {
                        std::string name = it->first;
                        for (const auto &title : ListTitles(cfg)) {
                            if (title.title_id == it->first && !title.name.empty()) name = title.name;
                        }
                        Notify("Prospero Manager\n" + NotifyText(Msg::kSavesBackingUp, {{"name", name}}));
                        std::vector<SaveBackupResult> results = BackupTitleSavesEach(cfg, it->first);
                        std::string failures = DescribeFailures(results);
                        if (failures.empty()) LogEvent("Auto-backed up saves for " + it->first);
                        else LogEvent("Auto-backup incomplete for " + it->first + ": " + failures);
                        // Always say how it went: a silent failure would leave the saves unprotected.
                        Notify("Prospero Manager\n" + BackupNotice(name, results));
                    }
                    it = pending.erase(it);
                }
            } catch (const std::exception &e) {
                LogEvent(std::string("Save auto-backup watcher failed safely: ") + e.what());
            } catch (...) {
                LogEvent("Save auto-backup watcher failed safely");
            }
        }
    });
}

// Kept so the Saves panel's own progress row keeps working — it reads the same
// slot the activity dock does.
SaveBackupAllStatus GetSaveBackupAllStatus() {
    SaveJobStatus job = GetSaveJobStatus();
    SaveBackupAllStatus out;
    if (job.kind != SaveJobKind::kBackupAll) return out;
    out.running = job.phase == SaveJobPhase::kRunning;
    out.done = job.done;
    out.total = job.total;
    out.failed = job.failed;
    out.current_title_id = out.running ? job.label : "";
    return out;
}

bool StartBackupAllJob(const Config &cfg, std::string *err) {
    if (!BeginSaveJob(SaveJobKind::kBackupAll, "", err)) return false;

    StartBackgroundThread([cfg] {
        std::set<std::string> title_ids;
        for (const auto &entry : ListSaves()) title_ids.insert(entry.title_id);
        {
            std::lock_guard<std::mutex> lock(g_save_job_mu);
            g_save_job.total = static_cast<int>(title_ids.size());
        }

        std::string last_error;
        for (const auto &title_id : title_ids) {
            {
                std::lock_guard<std::mutex> lock(g_save_job_mu);
                g_save_job.label = title_id;
            }
            std::string title_err;
            bool ok = BackupTitleSaves(cfg, title_id, &title_err);
            std::lock_guard<std::mutex> lock(g_save_job_mu);
            g_save_job.done++;
            if (!ok) {
                g_save_job.failed++;
                LogEvent("Back up all: " + title_id + " incomplete: " + title_err);
                if (last_error.empty()) last_error = title_err;
            }
        }

        int done = 0, total = 0, failed = 0;
        {
            std::lock_guard<std::mutex> lock(g_save_job_mu);
            g_save_job.label.clear();
            done = g_save_job.done;
            total = g_save_job.total;
            failed = g_save_job.failed;
        }
        FinishSaveJob(failed == 0, last_error, "backup_incomplete");
        LogEvent("Back up all: finished (" + std::to_string(done - failed) + "/" + std::to_string(total) +
                 " succeeded)");
    });
    return true;
}

} // namespace prospero
