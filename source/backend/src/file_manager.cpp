#include "file_manager.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <dirent.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <set>

#include "file_jobs.hpp"

namespace fs = std::filesystem;

namespace prospero {
namespace {

constexpr uint64_t kUploadReserve = 32ull * 1024ull * 1024ull;

// Walking these panics the console, so they are refused everywhere rather
// than merely hidden from the listing.
bool KernelUnsafe(const fs::path &path) {
    static const char *const roots[] = {"/dev", "/proc", "/sys", "/kern"};
    std::string text = path.string();
    for (const char *root : roots) {
        size_t len = std::strlen(root);
        if (text.compare(0, len, root) == 0 && (text.size() == len || text[len] == '/')) return true;
    }
    return false;
}

bool NormalizeAbsolute(const std::string &raw, fs::path *out, std::string *err) {
    if (raw.empty() || raw.front() != '/') {
        if (err) *err = "path must be absolute";
        return false;
    }
    fs::path input(raw);
    for (const auto &part : input) {
        if (part == "..") {
            if (err) *err = "parent traversal is not allowed";
            return false;
        }
    }
    fs::path normalized = input.lexically_normal();
    if (!normalized.is_absolute()) {
        if (err) *err = "path must be absolute";
        return false;
    }
    if (KernelUnsafe(normalized)) {
        if (err) *err = "this path is a kernel device tree and cannot be browsed safely";
        return false;
    }
    *out = normalized;
    return true;
}

bool ProtectedRoot(const fs::path &path) {
    static const std::set<std::string> protected_paths = {
        "/", "/data", "/user", "/mnt", "/system", "/system_data", "/dev", "/proc", "/sys", "/kern"};
    if (protected_paths.count(path.string()) != 0) return true;
    // Mount points are protected, but their contents remain writable.
    return path.parent_path() == "/mnt";
}

std::string ErrorText(const std::string &action, const std::error_code &ec) {
    return action + ": " + (ec ? ec.message() : std::string("unknown error"));
}

std::string ModifiedUtc(std::time_t raw) {
    std::tm tm {};
    gmtime_r(&raw, &tm);
    char text[20] {};
    std::strftime(text, sizeof(text), "%Y%m%d%H%M%S", &tm);
    return text;
}

bool VolumeAt(const char *label, const char *kind, int index, const char *path, bool removable,
              StorageVolume *out) {
    struct stat st {};
    struct statvfs usage {};
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode) || statvfs(path, &usage) != 0) return false;
    if (removable) {
        // Empty placeholders under /mnt report the tiny mount-root filesystem and look
        // like a drive with ~2 MB free. A real mount has its own device id.
        struct stat mount_root {};
        if (stat("/mnt", &mount_root) != 0 || st.st_dev == mount_root.st_dev) return false;
    }
    out->label = label;
    out->kind = kind;
    out->index = index;
    out->path = path;
    out->total_bytes = static_cast<uint64_t>(usage.f_blocks) * usage.f_frsize;
    out->free_bytes = static_cast<uint64_t>(usage.f_bavail) * usage.f_frsize;
    return true;
}

bool RemoveTreePosix(const std::string &path, dev_t root_device, std::string *err) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (err) *err = errno == ENOENT ? "path does not exist" : std::string("could not inspect path: ") + std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        if (unlink(path.c_str()) == 0) return true;
        if (err) *err = std::string("could not delete file: ") + std::strerror(errno);
        return false;
    }
    if (st.st_dev != root_device) {
        if (err) *err = "refusing to cross into another mounted filesystem";
        return false;
    }

    DIR *directory = opendir(path.c_str());
    if (!directory) {
        if (err) *err = std::string("could not open directory for deletion: ") + std::strerror(errno);
        return false;
    }
    bool ok = true;
    errno = 0;
    while (dirent *entry = readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        std::string child = path;
        if (child.back() != '/') child.push_back('/');
        child += entry->d_name;
        if (!RemoveTreePosix(child, root_device, err)) { ok = false; break; }
        errno = 0;
    }
    if (ok && errno != 0) {
        if (err) *err = std::string("directory traversal failed: ") + std::strerror(errno);
        ok = false;
    }
    int close_error = closedir(directory);
    if (ok && close_error != 0) {
        if (err) *err = std::string("could not close directory: ") + std::strerror(errno);
        ok = false;
    }
    if (!ok) return false;
    if (rmdir(path.c_str()) != 0) {
        if (err) *err = std::string("could not delete directory: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool DirectorySizePosix(const std::string &path, dev_t root_device, uint64_t *total,
                        size_t *scanned, std::string *err) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (err) *err = std::string("could not inspect directory entry: ") + std::strerror(errno);
        return false;
    }
    if (((*scanned)++ & 255u) == 0 && FileMutationInProgress()) {
        if (err) *err = "directory size is paused during a file operation";
        errno = EBUSY;
        return false;
    }
    if (S_ISREG(st.st_mode)) {
        if (st.st_size > 0 && UINT64_MAX - *total >= static_cast<uint64_t>(st.st_size)) {
            *total += static_cast<uint64_t>(st.st_size);
        }
        return true;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return true;
    if (st.st_dev != root_device) return true;

    DIR *directory = opendir(path.c_str());
    if (!directory) {
        if (err) *err = std::string("could not open directory: ") + std::strerror(errno);
        return false;
    }
    bool ok = true;
    errno = 0;
    while (dirent *entry = readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        std::string child = path;
        if (child.back() != '/') child.push_back('/');
        child += entry->d_name;
        if (!DirectorySizePosix(child, root_device, total, scanned, err)) { ok = false; break; }
        errno = 0;
    }
    if (ok && errno != 0) {
        if (err) *err = std::string("directory traversal failed: ") + std::strerror(errno);
        ok = false;
    }
    if (closedir(directory) != 0 && ok) {
        if (err) *err = std::string("could not close directory: ") + std::strerror(errno);
        ok = false;
    }
    return ok;
}

} // namespace

bool ListNativeDirectory(const std::string &raw_path, std::vector<FileManagerEntry> *entries, std::string *err) {
    if (!entries) return false;
    entries->clear();
    fs::path path;
    if (!NormalizeAbsolute(raw_path, &path, err)) return false;

    DIR *directory = opendir(path.c_str());
    if (!directory) {
        if (err) *err = std::string("could not open directory: ") + std::strerror(errno);
        return false;
    }

    bool ok = true;
    errno = 0;
    while (dirent *item = readdir(directory)) {
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0) continue;
        std::string child = path.string();
        if (child.back() != '/') child.push_back('/');
        child += item->d_name;

        struct stat st {};
        if (lstat(child.c_str(), &st) != 0) { errno = 0; continue; }
        FileManagerEntry entry;
        entry.name = item->d_name;
        entry.is_dir = S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
        if (S_ISREG(st.st_mode) && st.st_size >= 0) entry.size = static_cast<uint64_t>(st.st_size);
        entry.mode = static_cast<uint32_t>(st.st_mode & 07777);
        entry.modified = ModifiedUtc(st.st_mtime);
        entries->push_back(std::move(entry));
        errno = 0;
    }
    if (errno != 0) {
        if (err) *err = std::string("directory listing interrupted: ") + std::strerror(errno);
        ok = false;
    }
    if (closedir(directory) != 0 && ok) {
        if (err) *err = std::string("could not close directory: ") + std::strerror(errno);
        ok = false;
    }
    return ok;
}

bool NativeDirectorySize(const std::string &raw_path, uint64_t *out_size, std::string *err) {
    if (!out_size) return false;
    fs::path path;
    if (!NormalizeAbsolute(raw_path, &path, err)) return false;

    struct stat root {};
    if (lstat(path.c_str(), &root) != 0 || !S_ISDIR(root.st_mode) || S_ISLNK(root.st_mode)) {
        if (err) *err = "path is not a directory";
        return false;
    }
    size_t scanned = 0;
    uint64_t total = 0;
    if (!DirectorySizePosix(path.string(), root.st_dev, &total, &scanned, err)) return false;
    *out_size = total;
    return true;
}

bool CreateNativeDirectory(const std::string &raw_path, std::string *err) {
    fs::path path;
    if (!NormalizeAbsolute(raw_path, &path, err)) return false;
    std::error_code ec;
    if (!fs::create_directory(path, ec)) {
        if (!ec && fs::is_directory(path)) return true;
        if (err) *err = ErrorText("could not create directory", ec);
        return false;
    }
    chmod(path.c_str(), 0777); // FAT/exFAT may ignore it; creation still succeeded.
    return true;
}

bool DeleteNativePath(const std::string &raw_path, std::string *err) {
    fs::path path;
    if (!NormalizeAbsolute(raw_path, &path, err)) return false;
    if (ProtectedRoot(path)) {
        if (err) *err = "refusing to delete a protected filesystem root";
        return false;
    }
    return RemoveNativeTree(path.string(), err);
}

bool SetNativePermissions(const std::string &raw_path, uint32_t mode, std::string *err) {
    fs::path path;
    if (!NormalizeAbsolute(raw_path, &path, err)) return false;
    if (ProtectedRoot(path)) {
        if (err) *err = "refusing to change permissions on a protected filesystem root";
        return false;
    }
    if (mode > 07777) {
        if (err) *err = "permissions must be an octal mode between 0000 and 7777";
        return false;
    }
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (err) *err = std::string("could not inspect path: ") + std::strerror(errno);
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        if (err) *err = "refusing to change permissions through a symbolic link";
        return false;
    }
    if (chmod(path.c_str(), static_cast<mode_t>(mode)) != 0) {
        if (err) *err = std::string("could not change permissions: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool SetNativePermissionsTree(const std::string &raw_path, uint32_t mode, std::string *err) {
    fs::path path;
    if (!NormalizeAbsolute(raw_path, &path, err)) return false;
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (err) *err = std::string("could not inspect path: ") + std::strerror(errno);
        return false;
    }
    // A folder without the execute bit can't be entered, so it keeps it wherever it can be read.
    bool is_dir = S_ISDIR(st.st_mode);
    uint32_t own = is_dir ? (mode | ((mode & 0444) >> 2)) : mode;
    if (!SetNativePermissions(raw_path, own, err)) return false;
    if (!is_dir) return true;
    std::error_code ec;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        struct stat child {};
        if (lstat(it->path().c_str(), &child) != 0 || S_ISLNK(child.st_mode)) continue;
        if (!SetNativePermissionsTree(it->path().string(), mode, err)) return false;
    }
    return true;
}

bool RemoveNativeTree(const std::string &raw_path, std::string *err) {
    fs::path path;
    if (!NormalizeAbsolute(raw_path, &path, err)) return false;
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (err) *err = errno == ENOENT ? "path does not exist" : std::string("could not inspect path: ") + std::strerror(errno);
        return false;
    }
    return RemoveTreePosix(path.string(), st.st_dev, err);
}

bool RenameNativePath(const std::string &raw_from, const std::string &raw_to, std::string *err) {
    fs::path from, to;
    if (!NormalizeAbsolute(raw_from, &from, err) || !NormalizeAbsolute(raw_to, &to, err)) return false;
    if (ProtectedRoot(from) || ProtectedRoot(to)) {
        if (err) *err = "refusing to rename a protected filesystem root";
        return false;
    }
    RenameDeviceRelation relation = CheckRenameDeviceRelation(from.string(), to.string());
    if (relation != RenameDeviceRelation::same) {
        if (err) {
            *err = relation == RenameDeviceRelation::crosses
                       ? "cannot rename across drives — copy or move it instead"
                       : "could not verify that rename stays on one drive";
        }
        return false;
    }
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) {
        if (err) *err = ErrorText("could not rename path", ec);
        return false;
    }
    return true;
}

bool IsKernelUnsafePath(const std::string &path) { return KernelUnsafe(fs::path(path)); }

bool IsProtectedNativeRoot(const std::string &path) {
    return ProtectedRoot(fs::path(path).lexically_normal());
}

bool IsExternalStoragePath(const std::string &raw_path) {
    std::string path = fs::path(raw_path).lexically_normal().string();
    for (const char *root : {"/mnt/ext0", "/mnt/ext1"}) {
        size_t len = std::strlen(root);
        if (path.compare(0, len, root) == 0 && (path.size() == len || path[len] == '/')) return true;
    }
    return false;
}

RenameDeviceRelation CheckRenameDeviceRelation(const std::string &from, const std::string &to) {
    struct stat source {};
    if (lstat(from.c_str(), &source) != 0) return RenameDeviceRelation::unknown;

    fs::path parent = fs::path(to).parent_path();
    struct stat target {};
    if (stat(parent.empty() ? "." : parent.c_str(), &target) != 0) return RenameDeviceRelation::unknown;
    return source.st_dev == target.st_dev ? RenameDeviceRelation::same : RenameDeviceRelation::crosses;
}

std::vector<StorageVolume> ListStorageVolumes() {
    std::vector<StorageVolume> volumes;
    static std::mutex cache_mu;
    static std::vector<StorageVolume> removable_cache;
    auto probe = [&volumes](const std::string &label, const char *kind, int index, const std::string &path,
                            bool removable) {
        StorageVolume volume;
        if (VolumeAt(label.c_str(), kind, index, path.c_str(), removable, &volume)) {
            volumes.push_back(std::move(volume));
        }
    };

    probe("Internal data", "internal", -1, "/data", false);
    probe("User storage", "user", -1, "/user", false);
    std::string lease_error;
    bool removable_lease = BeginFileRead("/mnt/ext0", &lease_error);
    if (!removable_lease) {
        std::lock_guard<std::mutex> lock(cache_mu);
        volumes.insert(volumes.end(), removable_cache.begin(), removable_cache.end());
        return volumes;
    }

    size_t removable_start = volumes.size();
    for (int i = 0; i < 8; i++) probe("USB " + std::to_string(i), "usb", i, "/mnt/usb" + std::to_string(i), true);
    probe("USB extended storage", "ext_usb", -1, "/mnt/ext0", true);
    probe("M.2 extended storage", "ext_m2", -1, "/mnt/ext1", true);
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        removable_cache.assign(volumes.begin() + removable_start, volumes.end());
    }
    EndFileRead("/mnt/ext0");
    return volumes;
}

bool CheckUploadSpace(const std::string &raw_target, uint64_t bytes, uint64_t *available, std::string *err) {
    fs::path target;
    if (!NormalizeAbsolute(raw_target, &target, err)) return false;
    fs::path parent = target.parent_path();
    struct statvfs usage {};
    if (statvfs(parent.c_str(), &usage) != 0) {
        if (err) *err = std::string("could not inspect destination storage: ") + std::strerror(errno);
        return false;
    }
    uint64_t free_bytes = static_cast<uint64_t>(usage.f_bavail) * usage.f_frsize;
    if (available) *available = free_bytes;
    if (bytes > free_bytes || (free_bytes - bytes) < kUploadReserve) {
        if (err) *err = "not enough free space (32 MB safety reserve required)";
        return false;
    }
    return true;
}

bool CheckFreeSpaceAtDirectory(const std::string &raw_dir, uint64_t bytes, uint64_t *available, std::string *err) {
    fs::path dir;
    if (!NormalizeAbsolute(raw_dir, &dir, err)) return false;
    std::error_code ec;
    fs::path probe = dir;
    while (!fs::is_directory(probe, ec) && probe.has_parent_path() && probe != probe.parent_path()) {
        ec.clear();
        probe = probe.parent_path();
    }
    struct statvfs usage {};
    if (statvfs(probe.c_str(), &usage) != 0) {
        if (err) *err = std::string("could not inspect destination storage: ") + std::strerror(errno);
        return false;
    }
    uint64_t free_bytes = static_cast<uint64_t>(usage.f_bavail) * usage.f_frsize;
    if (available) *available = free_bytes;
    if (bytes > free_bytes || (free_bytes - bytes) < kUploadReserve) {
        if (err) *err = "not enough free space (32 MB safety reserve required)";
        return false;
    }
    return true;
}

} // namespace prospero
