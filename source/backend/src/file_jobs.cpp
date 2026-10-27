#include "job_queue.hpp"
#include "file_jobs.hpp"

#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "archive_paths.hpp"
#include "log.hpp"
#include "file_manager.hpp"
#include "miniz.h"
#include "unrar/dll.hpp"
#include "zip_crypto.hpp"

namespace fs = std::filesystem;

namespace prospero {
namespace {

constexpr size_t kCopyBuffer = 1024 * 1024;
constexpr uint64_t kCopySyncCheckpoint = 256ull * 1024ull * 1024ull;
constexpr uint64_t kCopyPreallocateThreshold = 256ull * 1024ull * 1024ull;
constexpr size_t kMaxJobs = 32;
// Bounds extraction memory and write() granularity. Archive creation reads
// through miniz's own fixed 64 KiB buffer instead — not our call to make.
constexpr size_t kExtractBuffer = 1024 * 1024;
// How often per-file progress is pushed into the job's shared state,
// instead of locking the job mutex on every single read.
constexpr auto kProgressThrottle = std::chrono::milliseconds(100);
// Backstop for a "Download as ZIP" temp file that never got downloaded —
// the normal path unlinks it the instant the download route opens it.
constexpr auto kEphemeralArchiveTtl = std::chrono::minutes(30);
constexpr auto kUploadReservationTtl = std::chrono::minutes(30);

Config g_cfg;
std::mutex g_mu;
std::mutex g_mutation_mu;
std::deque<FileJobInfo> g_jobs;
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_started{false};
bool g_immediate_mutation = false;
size_t g_external_readers = 0;
uint64_t g_next_id = 1;

struct UploadReservation {
    std::string token;
    std::chrono::steady_clock::time_point last_seen;
    bool expires_when_idle = true;
    bool in_use = true;
};

std::unordered_map<std::string, UploadReservation> g_uploads;

bool SyncFile(int fd);

std::string QueuePath() { return g_cfg.root_dir + "/file_jobs.queue"; }
std::string EphemeralArchiveDir() { return g_cfg.tmp_dir + "/archives"; }

bool ValidAbsolute(const std::string &raw) {
    if (raw.empty() || raw.front() != '/') return false;
    fs::path p(raw);
    for (const auto &part : p) if (part == "..") return false;
    fs::path normalized = p.lexically_normal();
    return normalized.is_absolute() && !IsKernelUnsafePath(normalized.string());
}

bool PathContains(const fs::path &parent, const fs::path &child) {
    fs::path parent_normal = parent.lexically_normal();
    fs::path child_normal = child.lexically_normal();
    if (parent_normal == child_normal) return true;
    std::string prefix = parent_normal.string();
    if (prefix.empty() || prefix.back() != '/') prefix.push_back('/');
    return child_normal.string().rfind(prefix, 0) == 0;
}

bool PathsOverlap(const fs::path &first, const fs::path &second) {
    return PathContains(first, second) || PathContains(second, first);
}

bool PathEntryExists(const fs::path &path, std::error_code *out_ec = nullptr) {
    std::error_code ec;
    fs::file_status status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        ec.clear();
        status = fs::file_status(fs::file_type::not_found);
    }
    if (out_ec) *out_ec = ec;
    return !ec && status.type() != fs::file_type::not_found;
}

uint64_t PathHash(const std::string &text) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

fs::path StagingRoot(const fs::path &destination, uint64_t job_id) {
    char name[64];
    std::snprintf(name, sizeof(name), ".pmgr-%llu-%016llx.tmp",
                  static_cast<unsigned long long>(job_id),
                  static_cast<unsigned long long>(PathHash(destination.lexically_normal().string())));
    return destination.parent_path() / name;
}

std::string HexEncode(const std::string &input) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char c : input) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
    return out;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string HexDecode(const std::string &input) {
    if (input.size() % 2) return {};
    std::string out;
    out.reserve(input.size() / 2);
    for (size_t i = 0; i < input.size(); i += 2) {
        int hi = HexValue(input[i]), lo = HexValue(input[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

// A multi-source zip job packs its source paths into one newline-joined string — safe since
// PersistLocked always hex-encodes this field, and a PS5 path never legitimately has a newline.
std::vector<std::string> SplitLines(const std::string &joined) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= joined.size()) {
        size_t nl = joined.find('\n', start);
        out.push_back(joined.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

bool ActiveState(const FileJobInfo &job) {
    return job.state == "queued" || job.state == "scanning" || job.state == "running";
}

bool AnyExternalPath(const std::vector<std::string> &sources, const std::string &destination) {
    if (!destination.empty() && IsExternalStoragePath(destination)) return true;
    return std::any_of(sources.begin(), sources.end(), IsExternalStoragePath);
}

bool JobOverlaps(const FileJobInfo &job, const std::vector<std::string> &sources,
                 const std::string &destination) {
    std::vector<std::string> existing = SplitLines(job.source);
    for (const auto &candidate : sources) {
        if (PathsOverlap(candidate, job.destination)) return true;
        for (const auto &source : existing) {
            if (PathsOverlap(candidate, source)) return true;
        }
    }
    if (PathsOverlap(destination, job.destination)) return true;
    for (const auto &source : existing) {
        if (PathsOverlap(destination, source)) return true;
    }
    return false;
}

void PruneUploadReservationsLocked() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = g_uploads.begin(); it != g_uploads.end();) {
        if (!it->second.in_use && it->second.expires_when_idle &&
            now - it->second.last_seen > kUploadReservationTtl) {
            it = g_uploads.erase(it);
        } else ++it;
    }
}

bool UploadOverlapsLocked(const std::vector<std::string> &sources, const std::string &destination) {
    PruneUploadReservationsLocked();
    for (const auto &[path, reservation] : g_uploads) {
        (void)reservation;
        if (PathsOverlap(path, destination)) return true;
        for (const auto &source : sources) {
            if (PathsOverlap(path, source)) return true;
        }
    }
    return false;
}

void PersistLocked() {
    std::string path = QueuePath();
    std::string temp = path + ".tmp";
    std::string content;
    for (const auto &job : g_jobs) {
        if (job.state != "queued" && job.state != "scanning" && job.state != "running") continue;
        // One opaque hex blob so old 4-field lines still parse: compression mode for
        // zip jobs, "conflict_policy,delete_flag" for unzip.
        std::string extra;
        if (job.type == "zip") extra = job.compression;
        else if (job.type == "unzip") extra = job.conflict_policy + "," + (job.delete_source_on_success ? "1" : "0");
        else if (job.type == "copy" || job.type == "move") extra = job.conflict_policy;
        content += std::to_string(job.id) + '|' + job.type + '|' + HexEncode(job.source) + '|' +
                   HexEncode(job.destination) + '|' + HexEncode(extra) + '\n';
    }
    int fd = open(temp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0) return;
    size_t offset = 0;
    while (offset < content.size()) {
        ssize_t written = write(fd, content.data() + offset, content.size() - offset);
        if (written > 0) offset += static_cast<size_t>(written);
        else if (written < 0 && errno == EINTR) continue;
        else break;
    }
    bool complete = offset == content.size() && SyncFile(fd);
    if (close(fd) != 0) complete = false;
    if (!complete || std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        return;
    }
    int parent_fd = open(fs::path(path).parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (parent_fd >= 0) {
        (void)SyncFile(parent_fd);
        close(parent_fd);
    }
}

void UpdateJob(uint64_t id, const std::function<void(FileJobInfo &)> &update, bool persist = false) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto &job : g_jobs) {
        if (job.id == id) {
            update(job);
            if (persist) PersistLocked();
            return;
        }
    }
}

bool IsCanceled() { return g_cancel.load(); }

bool WriteAll(int fd, const char *data, size_t size) {
    size_t written = 0;
    while (written < size) {
        ssize_t rc = write(fd, data + written, size - written);
        if (rc > 0) { written += static_cast<size_t>(rc); continue; }
        if (rc < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void SetErrorCode(uint64_t job_id, const char *code) {
    UpdateJob(job_id, [&](FileJobInfo &j) { j.error_code = code; });
}

std::string GetErrorCode(uint64_t job_id) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const auto &job : g_jobs) if (job.id == job_id) return job.error_code;
    return {};
}

struct TreeContents {
    struct SnapshotEntry {
        std::string relative;
        uint64_t size = 0;
        uint64_t device = 0;
        uint64_t inode = 0;
        int64_t modified_sec = 0;
        long modified_nsec = 0;
        bool is_dir = false;

        bool operator==(const SnapshotEntry &other) const {
            return relative == other.relative && size == other.size && device == other.device &&
                   inode == other.inode && modified_sec == other.modified_sec &&
                   modified_nsec == other.modified_nsec && is_dir == other.is_dir;
        }
    };

    std::vector<fs::path> directories;
    std::vector<fs::path> files;
    std::vector<SnapshotEntry> snapshot;
    uint64_t total_bytes = 0;
    bool single_file = false;
};

bool UniqueSiblingName(const fs::path &target, fs::path *out);
bool MergeStagedTree(const fs::path &staged, const fs::path &destination, const std::string &policy,
                     std::string *err);

bool SyncFile(int fd) {
    int rc;
    do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
    return rc == 0;
}

bool CopyFileSafely(const fs::path &source, const fs::path &staged, uint64_t job_id,
                    uint64_t *completed, std::string *err) {
    std::error_code ec;
    fs::create_directories(staged.parent_path(), ec);
    if (ec) {
        if (err) *err = "could not create staging directory: " + ec.message();
        return false;
    }

    int in = open(source.c_str(), O_RDONLY);
    if (in < 0) {
        int open_error = errno;
        if (err) *err = std::string("could not open source file: ") + std::strerror(open_error);
        SetErrorCode(job_id, "read_failed");
        return false;
    }
    struct stat source_stat {};
    if (fstat(in, &source_stat) != 0 || !S_ISREG(source_stat.st_mode) || source_stat.st_size < 0) {
        if (err) *err = "source is no longer a regular file";
        SetErrorCode(job_id, "read_failed");
        close(in);
        return false;
    }
    int out = open(staged.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (out < 0) {
        int open_error = errno;
        if (err) *err = std::string("could not create destination file: ") + std::strerror(open_error);
        SetErrorCode(job_id, open_error == ENOSPC ? "no_space" : "write_failed");
        close(in);
        return false;
    }
    (void)fchmod(out, 0777);

    uint64_t source_size = static_cast<uint64_t>(source_stat.st_size);
    (void)posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (source_size >= kCopyPreallocateThreshold && !IsExternalStoragePath(staged.string())) {
        int allocation_error = posix_fallocate(out, 0, source_stat.st_size);
        if (allocation_error == 0) {
            if (!SyncFile(out)) {
                int sync_error = errno;
                if (err) *err = std::string("destination allocation sync failed: ") + std::strerror(sync_error);
                SetErrorCode(job_id, sync_error == ENOSPC ? "no_space" : "write_failed");
                close(in); close(out);
                return false;
            }
        } else if (allocation_error == ENOSPC) {
            if (err) *err = "not enough space to allocate the destination file";
            SetErrorCode(job_id, "no_space");
            close(in); close(out);
            return false;
        } else if (allocation_error != EINVAL && allocation_error != ENOSYS &&
                   allocation_error != ENOTSUP && allocation_error != EOPNOTSUPP) {
            if (err) *err = std::string("could not allocate destination file: ") + std::strerror(allocation_error);
            SetErrorCode(job_id, "write_failed");
            close(in); close(out);
            return false;
        }
    }
    uint64_t copied = 0;
    uint64_t unsynced = 0;
    std::vector<char> buffer(kCopyBuffer);
    auto last_update = std::chrono::steady_clock::now();
    while (copied < source_size && !IsCanceled()) {
        size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), source_size - copied));
        ssize_t got;
        do { got = read(in, buffer.data(), wanted); } while (got < 0 && errno == EINTR);
        if (got <= 0) {
            if (err) {
                *err = got < 0 ? std::string("source read failed: ") + std::strerror(errno)
                               : "source changed while it was being copied";
            }
            SetErrorCode(job_id, "read_failed");
            close(in); close(out);
            return false;
        }
        size_t sent = 0;
        while (sent < static_cast<size_t>(got)) {
            ssize_t n;
            do { n = write(out, buffer.data() + sent, static_cast<size_t>(got) - sent); }
            while (n < 0 && errno == EINTR);
            if (n <= 0) {
                if (n == 0) errno = ENOSPC;
                int write_error = errno;
                if (err) *err = std::string("destination write failed: ") + std::strerror(write_error);
                SetErrorCode(job_id, write_error == ENOSPC ? "no_space" : "write_failed");
                close(in); close(out);
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        copied += static_cast<uint64_t>(got);
        unsynced += static_cast<uint64_t>(got);
        *completed += static_cast<uint64_t>(got);
        if (unsynced >= kCopySyncCheckpoint) {
            if (!SyncFile(out)) {
                int sync_error = errno;
                if (err) *err = std::string("destination sync failed: ") + std::strerror(sync_error);
                SetErrorCode(job_id, "write_failed");
                close(in); close(out);
                return false;
            }
            unsynced = 0;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_update >= kProgressThrottle) {
            last_update = now;
            uint64_t snapshot = *completed;
            UpdateJob(job_id, [&](FileJobInfo &job) {
                job.completed_bytes = snapshot;
                job.current = source.string();
            });
        }
    }
    bool ok = !IsCanceled();
    if (ok && unsynced > 0 && !SyncFile(out)) {
        int sync_error = errno;
        if (err) *err = std::string("destination sync failed: ") + std::strerror(sync_error);
        SetErrorCode(job_id, "write_failed");
        ok = false;
    }
    struct stat final_source_stat {};
    if (ok && (fstat(in, &final_source_stat) != 0 || final_source_stat.st_dev != source_stat.st_dev ||
               final_source_stat.st_ino != source_stat.st_ino || final_source_stat.st_size != source_stat.st_size ||
               final_source_stat.st_mtim.tv_sec != source_stat.st_mtim.tv_sec ||
               final_source_stat.st_mtim.tv_nsec != source_stat.st_mtim.tv_nsec)) {
        if (err) *err = "source changed while it was being copied";
        SetErrorCode(job_id, "source_changed");
        ok = false;
    }
    close(in);
    if (close(out) != 0 && ok) {
        if (err) *err = std::string("could not finish destination file: ") + std::strerror(errno);
        SetErrorCode(job_id, "write_failed");
        ok = false;
    }
    if (ok) {
        uint64_t snapshot = *completed;
        UpdateJob(job_id, [&](FileJobInfo &job) {
            job.completed_bytes = snapshot;
            job.current = source.string();
        });
    }
    return ok;
}

bool CollectTreeEntry(const fs::path &root, const fs::path &path, dev_t root_device,
                      TreeContents *out, std::string *err) {
    if (IsCanceled()) return false;
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (err) *err = std::string("could not inspect source: ") + std::strerror(errno);
        return false;
    }
    if (st.st_dev != root_device) {
        if (err) *err = "source crosses into another mounted filesystem";
        return false;
    }
    if (S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
        if (err) *err = "source contains an unsupported link or special file: " + path.string();
        return false;
    }

    TreeContents::SnapshotEntry snapshot;
    fs::path relative = path == root ? fs::path(".") : path.lexically_relative(root);
    snapshot.relative = relative.string();
    snapshot.device = static_cast<uint64_t>(st.st_dev);
    snapshot.inode = static_cast<uint64_t>(st.st_ino);
    snapshot.modified_sec = st.st_mtim.tv_sec;
    snapshot.modified_nsec = st.st_mtim.tv_nsec;
    snapshot.is_dir = S_ISDIR(st.st_mode);
    if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0 || UINT64_MAX - out->total_bytes < static_cast<uint64_t>(st.st_size)) {
            if (err) *err = "source tree is too large";
            return false;
        }
        snapshot.size = static_cast<uint64_t>(st.st_size);
        out->total_bytes += snapshot.size;
        out->files.push_back(path);
    } else if (path != root) {
        out->directories.push_back(path);
    }
    out->snapshot.push_back(std::move(snapshot));
    if (!S_ISDIR(st.st_mode)) return true;

    DIR *directory = opendir(path.c_str());
    if (!directory) {
        if (err) *err = std::string("could not open source directory: ") + std::strerror(errno);
        return false;
    }
    bool ok = true;
    errno = 0;
    while (dirent *entry = readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        if (!CollectTreeEntry(root, path / entry->d_name, root_device, out, err)) { ok = false; break; }
        errno = 0;
    }
    if (ok && errno != 0) {
        if (err) *err = std::string("source traversal failed: ") + std::strerror(errno);
        ok = false;
    }
    if (closedir(directory) != 0 && ok) {
        if (err) *err = std::string("could not close source directory: ") + std::strerror(errno);
        ok = false;
    }
    return ok;
}

bool CollectTree(const fs::path &source, TreeContents *out, std::string *err) {
    if (!out) return false;
    *out = {};
    struct stat st {};
    if (lstat(source.c_str(), &st) != 0) {
        if (err) *err = std::string("could not inspect source: ") + std::strerror(errno);
        return false;
    }
    out->single_file = S_ISREG(st.st_mode);
    if (!CollectTreeEntry(source, source, st.st_dev, out, err)) return false;
    auto by_path = [](const auto &first, const auto &second) { return first.string() < second.string(); };
    std::sort(out->directories.begin(), out->directories.end(), by_path);
    std::sort(out->files.begin(), out->files.end(), by_path);
    std::sort(out->snapshot.begin(), out->snapshot.end(), [](const auto &first, const auto &second) {
        return first.relative < second.relative;
    });
    return true;
}

bool CopyCollected(const fs::path &source, const fs::path &staged, const TreeContents &tree, uint64_t job_id,
                   uint64_t *completed, std::string *err) {
    if (tree.single_file) return CopyFileSafely(source, staged, job_id, completed, err);

    std::error_code ec;
    fs::create_directories(staged, ec);
    if (ec) {
        if (err) *err = "could not create staging directory: " + ec.message();
        return false;
    }
    (void)chmod(staged.c_str(), 0777);
    for (const auto &directory : tree.directories) {
        fs::path created = staged / directory.lexically_relative(source);
        fs::create_directories(created, ec);
        if (ec) {
            if (err) *err = "could not create staging directory: " + ec.message();
            return false;
        }
        (void)chmod(created.c_str(), 0777);
    }
    for (const auto &file : tree.files) {
        if (IsCanceled()) return false;
        fs::path relative = file.lexically_relative(source);
        if (!CopyFileSafely(file, staged / relative, job_id, completed, err)) return false;
    }
    return !IsCanceled();
}

bool PublishStaging(const fs::path &staged, const fs::path &destination, std::string *err) {
    std::error_code ec;
    if (PathEntryExists(destination, &ec)) {
        if (err) *err = "destination already exists";
        return false;
    }
    fs::rename(staged, destination, ec);
    if (ec) {
        if (err) *err = "could not publish completed job: " + ec.message();
        return false;
    }
    return true;
}

bool RunCopyOrMove(FileJobInfo &job, bool move, std::string *err) {
    fs::path source(job.source), destination(job.destination);
    std::error_code ec;
    fs::file_status source_status = fs::symlink_status(source, ec);
    if (ec || (source_status.type() != fs::file_type::regular && source_status.type() != fs::file_type::directory)) {
        if (err) *err = "source does not exist or is not a regular file/directory";
        return false;
    }
    bool destination_exists = PathEntryExists(destination, &ec);
    if (ec) {
        if (err) *err = "could not inspect destination: " + ec.message();
        return false;
    }
    if (destination_exists) {
        if (job.conflict_policy == "cancel") {
            if (err) *err = "destination already exists";
            SetErrorCode(job.id, "destination_exists");
            return false;
        }
        if (job.conflict_policy == "skip") return true;
        if (job.conflict_policy == "keep_both") {
            if (!UniqueSiblingName(destination, &destination)) {
                if (err) *err = "could not find an available destination name";
                return false;
            }
            job.destination = destination.string();
            UpdateJob(job.id, [&](FileJobInfo &current) { current.destination = job.destination; }, true);
            destination_exists = false;
        } else {
            fs::file_status destination_status = fs::symlink_status(destination, ec);
            bool source_is_dir = fs::is_directory(source_status);
            if (ec || fs::is_symlink(destination_status) ||
                source_is_dir != fs::is_directory(destination_status)) {
                if (err) *err = "source and destination types do not match";
                SetErrorCode(job.id, "type_conflict");
                return false;
            }
        }
    }
    fs::path stage_root = StagingRoot(destination, job.id);
    fs::path legacy_stage = destination.string() + ".pmgr-job-" + std::to_string(job.id);
    for (const auto &stale : {stage_root, legacy_stage}) {
        ec.clear();
        bool exists = PathEntryExists(stale, &ec);
        if (ec) {
            if (err) *err = "could not inspect stale staging data: " + ec.message();
            return false;
        }
        if (!exists) continue;
        std::string cleanup_error;
        if (!RemoveNativeTree(stale.string(), &cleanup_error)) {
            if (err) *err = "could not clear stale staging data: " + cleanup_error;
            return false;
        }
    }
    bool merging_directories = destination_exists && fs::is_directory(source_status);
    if (move && !merging_directories &&
        CheckRenameDeviceRelation(source.string(), destination.string()) == RenameDeviceRelation::same) {
        fs::rename(source, destination, ec);
        if (!ec) {
            uint64_t total = fs::is_regular_file(destination, ec) ? fs::file_size(destination, ec) : 0;
            UpdateJob(job.id, [&](FileJobInfo &j) { j.total_bytes = total; j.completed_bytes = total; });
            return true;
        }
        ec.clear();
    }

    UpdateJob(job.id, [](FileJobInfo &j) { j.state = "scanning"; });
    TreeContents tree;
    if (!CollectTree(source, &tree, err)) return false;

    // Checked before writing anything: a game-sized copy can otherwise fill
    // the system drive and only fail tens of GB in.
    uint64_t available = 0;
    std::string space_err;
    if (!CheckUploadSpace(destination.string(), tree.total_bytes, &available, &space_err)) {
        if (err) *err = "not enough free space at the destination";
        SetErrorCode(job.id, "no_space");
        return false;
    }
    UpdateJob(job.id, [&](FileJobInfo &j) { j.total_bytes = tree.total_bytes; j.state = "running"; });

    ec.clear();
    fs::create_directory(stage_root, ec);
    if (ec) {
        if (err) *err = "could not create staging area: " + ec.message();
        return false;
    }
    fs::path staged = stage_root / "payload";
    uint64_t completed = 0;
    bool copied = CopyCollected(source, staged, tree, job.id, &completed, err);
    bool published = false;
    if (copied) {
        if (!destination_exists) published = PublishStaging(staged, destination, err);
        else if (tree.single_file) {
            fs::rename(staged, destination, ec);
            published = !ec;
            if (!published && err) *err = "could not replace destination: " + ec.message();
        } else {
            published = MergeStagedTree(staged, destination, "replace", err);
        }
    }
    if (!copied || !published) {
        std::string ignored;
        (void)RemoveNativeTree(stage_root.string(), &ignored);
        return false;
    }
    fs::remove(stage_root, ec);
    if (move) {
        TreeContents current;
        if (!CollectTree(source, &current, err) || current.snapshot != tree.snapshot) {
            if (err && err->empty()) *err = "source tree changed after it was copied; both copies were kept";
            SetErrorCode(job.id, "source_tree_changed");
            return false;
        }
        std::string cleanup_error;
        if (!RemoveNativeTree(source.string(), &cleanup_error)) {
            if (err) *err = "copy completed but source cleanup failed: " + cleanup_error;
            SetErrorCode(job.id, "cleanup_failed");
            return false;
        }
    }
    return true;
}

// ---- Archive support: compression modes, safe paths, streamed zip/unzip ---

// Formats not worth spending CPU deflating under Smart mode.
const std::unordered_set<std::string> &IncompressibleExtensions() {
    static const std::unordered_set<std::string> exts = {
        "pkg", "zip", "7z", "rar", "mp4", "mkv", "jpg", "jpeg", "png", "webp", "mp3", "flac", "gz", "xz",
    };
    return exts;
}

std::string LowerExtension(const std::string &filename) {
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= filename.size()) return "";
    std::string ext = filename.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool IsIncompressible(const std::string &filename) {
    return IncompressibleExtensions().count(LowerExtension(filename)) != 0;
}

// Unrecognized modes fall through to Smart.
mz_uint CompressionLevelFor(const std::string &mode, const std::string &filename) {
    if (mode == "store") return MZ_NO_COMPRESSION;
    if (mode == "fast") return MZ_BEST_SPEED;
    if (mode == "balanced") return MZ_DEFAULT_LEVEL;
    return IsIncompressible(filename) ? static_cast<mz_uint>(MZ_NO_COMPRESSION) : static_cast<mz_uint>(MZ_BEST_SPEED);
}

bool IsValidCompressionMode(const std::string &mode) {
    return mode == "smart" || mode == "store" || mode == "fast" || mode == "balanced";
}

bool IsValidConflictPolicy(const std::string &policy) {
    return policy == "cancel" || policy == "skip" || policy == "replace" || policy == "keep_both";
}

struct ZipEntry {
    fs::path path;
    std::string archive_name;
    uint64_t size = 0;
};

// Reads archive inputs directly from disk.
bool ScanZipTree(const fs::path &source, const fs::path &base, std::vector<std::string> *directories,
                 std::vector<ZipEntry> *files, uint64_t *total, std::string *err) {
    TreeContents tree;
    if (!CollectTree(source, &tree, err)) return false;
    for (const auto &directory : tree.directories) {
        directories->push_back(directory.lexically_relative(base).string() + "/");
    }
    for (const auto &file : tree.files) {
        struct stat st {};
        if (lstat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
            if (err) *err = "source changed while preparing the archive";
            return false;
        }
        uint64_t size = static_cast<uint64_t>(st.st_size);
        if (UINT64_MAX - *total < size) {
            if (err) *err = "archive input is too large";
            return false;
        }
        *total += size;
        files->push_back({file, file.lexically_relative(base).string(), size});
    }
    return true;
}

// Handed to miniz's read callback for a streamed zip-add (RunZip).
// `completed_total` only runs on the worker thread, so no locking needed.
struct ZipReadCtx {
    int fd = -1;
    uint64_t bytes_read = 0;
    bool read_error = false;
    uint64_t job_id = 0;
    uint64_t *completed_total = nullptr;
    std::chrono::steady_clock::time_point last_update;
};

size_t ZipReadCallback(void *opaque, mz_uint64 file_ofs, void *buf, size_t n) {
    auto *ctx = static_cast<ZipReadCtx *>(opaque);
    if (IsCanceled() || n == 0) return 0;
    ssize_t got;
    do { got = pread(ctx->fd, buf, n, static_cast<off_t>(file_ofs)); } while (got < 0 && errno == EINTR);
    if (got < 0) { ctx->read_error = true; return 0; }
    if (got == 0) return 0; // legitimate end of this file
    ctx->bytes_read += static_cast<uint64_t>(got);
    *ctx->completed_total += static_cast<uint64_t>(got);
    auto now = std::chrono::steady_clock::now();
    if (now - ctx->last_update >= kProgressThrottle) {
        ctx->last_update = now;
        uint64_t snapshot = *ctx->completed_total;
        UpdateJob(ctx->job_id, [&](FileJobInfo &j) { j.completed_bytes = snapshot; });
    }
    return static_cast<size_t>(got);
}

bool RunZip(FileJobInfo job, std::string *err) {
    fs::path destination(job.destination);
    std::error_code ec;
    if (fs::exists(destination, ec)) {
        if (err) *err = "destination already exists";
        return false;
    }

    // One path, or several joined by newlines for a multi-selection. Each keeps its
    // own name as a prefix so siblings can't collide.
    std::vector<std::string> sources = SplitLines(job.source);
    bool multi = sources.size() > 1;

    UpdateJob(job.id, [](FileJobInfo &j) { j.state = "scanning"; });
    std::vector<std::string> directories;
    std::vector<ZipEntry> files;
    uint64_t total = 0;
    for (const auto &source_str : sources) {
        fs::path source(source_str);
        bool source_is_file = fs::is_regular_file(source, ec);
        bool source_is_dir = !source_is_file && fs::is_directory(source, ec);
        if (!source_is_file && !source_is_dir) {
            if (err) *err = "source does not exist: " + source_str;
            return false;
        }
        if (source_is_file) {
            uint64_t size = fs::file_size(source, ec);
            if (ec) { size = 0; ec.clear(); }
            total += size;
            files.push_back({source, source.filename().string(), size});
            continue;
        }
        fs::path base = multi ? source.parent_path() : source;
        if (multi) directories.push_back(source.filename().string() + "/");
        if (!ScanZipTree(source, base, &directories, &files, &total, err)) return false;
    }
    UpdateJob(job.id, [&](FileJobInfo &j) {
        j.total_bytes = total;
        j.total_items = files.size();
        j.state = "running";
    });

    // Store mode needs roughly the input size and Deflate always less, so one
    // conservative bound covers every mode without guessing a ratio.
    uint64_t available = 0;
    std::string space_err;
    if (!CheckUploadSpace(destination.string(), total, &available, &space_err)) {
        if (err) *err = "not enough free space to create this archive";
        SetErrorCode(job.id, "no_space");
        return false;
    }

    fs::path staged = destination.string() + ".pmgr-job-" + std::to_string(job.id);
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_file(&zip, staged.c_str(), 0)) {
        if (err) *err = "could not initialize zip output";
        SetErrorCode(job.id, "create_failed");
        return false;
    }
    bool ok = true;
    for (const auto &archive_dir : directories) {
        if (!mz_zip_writer_add_mem(&zip, archive_dir.c_str(), nullptr, 0, MZ_NO_COMPRESSION)) {
            if (err) *err = "failed to add folder " + archive_dir + " to archive";
            SetErrorCode(job.id, "write_failed");
            ok = false;
            break;
        }
    }

    uint64_t completed = 0;
    uint64_t item_index = 0;
    for (const auto &file : files) {
        if (!ok) break;
        if (IsCanceled()) { ok = false; break; }
        item_index++;
        UpdateJob(job.id, [&](FileJobInfo &j) { j.current = file.path.string(); j.current_index = item_index; });

        int fd = open(file.path.c_str(), O_RDONLY);
        if (fd < 0) {
            if (err) *err = "could not open " + file.archive_name + " for reading";
            SetErrorCode(job.id, "read_failed");
            ok = false;
            break;
        }
        struct stat st {};
        MZ_TIME_T mtime {};
        bool has_mtime = fstat(fd, &st) == 0;
        if (has_mtime) mtime = st.st_mtime;

        ZipReadCtx ctx;
        ctx.fd = fd;
        ctx.job_id = job.id;
        ctx.completed_total = &completed;
        ctx.last_update = std::chrono::steady_clock::now();
        mz_uint level = CompressionLevelFor(job.compression, file.archive_name);
        mz_bool added = mz_zip_writer_add_read_buf_callback(
            &zip, file.archive_name.c_str(), ZipReadCallback, &ctx, file.size,
            has_mtime ? &mtime : nullptr, nullptr, 0, level, nullptr, 0, nullptr, 0);
        close(fd);

        if (IsCanceled()) { ok = false; break; }
        if (ctx.read_error) {
            if (err) *err = "read failure while adding " + file.archive_name + " to archive";
            SetErrorCode(job.id, "read_failed");
            ok = false;
            break;
        }
        if (!added || ctx.bytes_read != file.size) {
            if (err) *err = "failed to add " + file.archive_name + " to archive";
            SetErrorCode(job.id, "write_failed");
            ok = false;
            break;
        }
        UpdateJob(job.id, [&](FileJobInfo &j) { j.completed_bytes = completed; });
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&zip) != 0;
    mz_zip_writer_end(&zip);
    if (!ok) {
        fs::remove(staged, ec);
        if (!IsCanceled() && err->empty()) *err = "could not finalize zip archive";
        return false;
    }
    return PublishStaging(staged, destination, err);
}

// Deterministic collision-safe rename, "name.ext" -> "name (1).ext",
// "name (2).ext", ... — used by the "keep both" extraction conflict policy.
bool UniqueSiblingName(const fs::path &target, fs::path *out) {
    std::error_code ec;
    if (!PathEntryExists(target, &ec) && !ec) { *out = target; return true; }
    std::string stem = target.stem().string();
    std::string ext = target.extension().string();
    for (int i = 1; i < 10000; i++) {
        fs::path candidate = target.parent_path() / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!PathEntryExists(candidate, &ec) && !ec) { *out = candidate; return true; }
    }
    return false;
}

// Only reached when the destination already exists. Directories always
// merge; each individual file collision follows `policy`.
bool MergeStagedTree(const fs::path &staged, const fs::path &destination, const std::string &policy,
                     std::string *err) {
    std::error_code ec;
    fs::create_directories(destination, ec);
    TreeContents tree;
    if (!CollectTree(staged, &tree, err)) return false;
    for (const auto &directory : tree.directories) {
        fs::path relative = directory.lexically_relative(staged);
        fs::path target = destination / relative;
        fs::create_directories(target, ec);
        if (ec) {
            if (err) *err = "could not publish staged directory: " + relative.string();
            return false;
        }
        (void)chmod(target.c_str(), 0777);
    }
    for (const auto &file : tree.files) {
        fs::path relative = file.lexically_relative(staged);
        fs::path target = destination / relative;
        fs::create_directories(target.parent_path(), ec);
        if (PathEntryExists(target, &ec)) {
            fs::file_status target_status = fs::symlink_status(target, ec);
            if (ec || fs::is_symlink(target_status) || !fs::is_regular_file(target_status)) {
                if (err) *err = "an extracted file conflicts with a non-regular destination: " + relative.string();
                return false;
            }
            if (policy == "cancel") {
                if (err) *err = "destination already has conflicting files: " + relative.string();
                return false;
            }
            if (policy == "skip") {
                fs::remove(file, ec);
                ec.clear();
                continue;
            }
            if (policy == "keep_both") {
                fs::path unique;
                if (!UniqueSiblingName(target, &unique)) {
                    if (err) *err = "too many name collisions while merging extracted files";
                    return false;
                }
                target = unique;
            }
        }
        fs::rename(file, target, ec);
        if (ec) {
            if (err) *err = "could not publish staged file: " + relative.string() + ": " + ec.message();
            return false;
        }
    }
    std::string ignored;
    (void)RemoveNativeTree(staged.string(), &ignored); // best-effort — only empty dir shells remain
    return true;
}

// RAR is sequential-only, so listing and extracting each need their own handle.
// password_buf must outlive the handle.
HANDLE OpenRarForRead(const std::string &path, const std::string &password,
                      std::vector<char> *arcname_buf, std::vector<char> *password_buf) {
    arcname_buf->assign(path.begin(), path.end());
    arcname_buf->push_back('\0');
    RAROpenArchiveDataEx data{};
    data.ArcName = arcname_buf->data();
    data.OpenMode = RAR_OM_EXTRACT;
    HANDLE h = RAROpenArchiveEx(&data);
    if (!h || data.OpenResult != 0) return nullptr;
    if (!password.empty()) {
        password_buf->assign(password.begin(), password.end());
        password_buf->push_back('\0');
        RARSetPassword(h, password_buf->data());
    }
    return h;
}

// ERAR_MISSING_PASSWORD (22) / ERAR_BAD_PASSWORD (24) — distinct from a genuinely
// corrupt archive, so the UI can prompt for a password instead of just failing.
bool IsRarPasswordError(int code) { return code == 22 || code == 24; }

// UCM_PROCESSDATA fires synchronously inside RARProcessFile — this carries the
// open fd and job state so the callback can write, throttle, sync, and cancel.
struct UnrarWriteCtx {
    int fd = -1;
    uint64_t job_id = 0;
    uint64_t completed = 0;
    uint64_t unsynced = 0;
    std::chrono::steady_clock::time_point last_update;
    bool failed = false;
};

int UnrarWriteCallback(UINT msg, LPARAM UserData, LPARAM P1, LPARAM P2) {
    if (msg != UCM_PROCESSDATA) return 0;
    auto *ctx = reinterpret_cast<UnrarWriteCtx *>(UserData);
    if (IsCanceled()) return -1;
    auto *data = reinterpret_cast<const char *>(P1);
    size_t size = static_cast<size_t>(P2);
    if (!WriteAll(ctx->fd, data, size)) { ctx->failed = true; return -1; }
    ctx->completed += size;
    ctx->unsynced += size;
    if (ctx->unsynced >= kCopySyncCheckpoint) {
        if (!SyncFile(ctx->fd)) { ctx->failed = true; return -1; }
        ctx->unsynced = 0;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - ctx->last_update >= kProgressThrottle) {
        ctx->last_update = now;
        uint64_t snapshot = ctx->completed;
        UpdateJob(ctx->job_id, [&](FileJobInfo &j) { j.completed_bytes = snapshot; });
    }
    return 0;
}

// Same staged-extraction shape as RunUnzip, via UnRAR's DLL API (vendored under
// third_party/unrar/) instead of miniz — see that license for terms.
bool RunUnrarExtraction(FileJobInfo job, std::string *err) {
    fs::path source(job.source), destination(job.destination);

    struct Entry { fs::path relative; bool is_dir; uint64_t size; };
    std::vector<Entry> entries;
    uint64_t total = 0, item_count = 0;
    {
        std::vector<char> arcname_buf, password_buf;
        HANDLE h = OpenRarForRead(source.string(), job.archive_password, &arcname_buf, &password_buf);
        if (!h) {
            if (err) *err = "could not open the rar — is it valid?";
            SetErrorCode(job.id, "invalid_zip");
            return false;
        }
        RARHeaderDataEx hdr{};
        int rc;
        while ((rc = RARReadHeaderEx(h, &hdr)) == 0) {
            fs::path relative;
            std::string path_err;
            if (!SafeArchiveRelativePath(hdr.FileName, &relative, &path_err)) {
                RARCloseArchive(h);
                if (err) *err = "archive contains an unsafe path: " + std::string(hdr.FileName);
                SetErrorCode(job.id, "unsafe_path");
                return false;
            }
            bool is_dir = (hdr.Flags & RHDF_DIRECTORY) != 0;
            uint64_t size = (static_cast<uint64_t>(hdr.UnpSizeHigh) << 32) | hdr.UnpSize;
            if (!is_dir) {
                if (UINT64_MAX - total < size) {
                    RARCloseArchive(h);
                    if (err) *err = "archive expands beyond the supported size";
                    SetErrorCode(job.id, "invalid_zip");
                    return false;
                }
                total += size;
                item_count++;
            }
            entries.push_back({relative, is_dir, size});
            // RAR_SKIP never touches the encrypted payload — RAR_TEST actually
            // decrypts, so a missing/wrong password fails here, not mid-extraction.
            bool encrypted = (hdr.Flags & RHDF_ENCRYPTED) != 0;
            rc = RARProcessFile(h, encrypted ? RAR_TEST : RAR_SKIP, nullptr, nullptr);
            if (rc != 0) break;
        }
        RARCloseArchive(h);
        if (IsRarPasswordError(rc)) {
            if (err) *err = "this archive needs a password";
            SetErrorCode(job.id, "password_required");
            return false;
        }
    }
    UpdateJob(job.id, [&](FileJobInfo &j) { j.total_bytes = total; j.total_items = item_count; j.state = "running"; });

    uint64_t available = 0;
    std::string space_err;
    if (!CheckFreeSpaceAtDirectory(destination.string(), total, &available, &space_err)) {
        if (err) *err = "not enough free space to extract this archive";
        SetErrorCode(job.id, "no_space");
        return false;
    }

    fs::path staged = destination.string() + ".pmgr-job-" + std::to_string(job.id);
    std::error_code ec;
    fs::create_directories(staged, ec);
    if (ec) {
        if (err) *err = "could not create extraction staging: " + ec.message();
        SetErrorCode(job.id, "create_failed");
        return false;
    }
    (void)chmod(staged.c_str(), 0777);

    for (const auto &entry : entries) {
        if (!entry.is_dir) continue;
        fs::path target = (staged / entry.relative).lexically_normal();
        if (!IsWithinRoot(staged, target)) continue;
        fs::create_directories(target, ec);
        if (ec) {
            std::string ignored;
            (void)RemoveNativeTree(staged.string(), &ignored);
            if (err) *err = "could not create extracted directory: " + entry.relative.string();
            SetErrorCode(job.id, "create_failed");
            return false;
        }
        (void)chmod(target.c_str(), 0777);
    }

    std::vector<char> arcname_buf, password_buf;
    HANDLE h = OpenRarForRead(source.string(), job.archive_password, &arcname_buf, &password_buf);
    if (!h) {
        std::string ignored;
        (void)RemoveNativeTree(staged.string(), &ignored);
        if (err) *err = "could not reopen the rar for extraction";
        SetErrorCode(job.id, "invalid_zip");
        return false;
    }

    uint64_t completed = 0, item_index = 0;
    bool ok = true;
    RARHeaderDataEx hdr{};
    while (ok && RARReadHeaderEx(h, &hdr) == 0) {
        if (IsCanceled()) { ok = false; break; }
        bool is_dir = (hdr.Flags & RHDF_DIRECTORY) != 0;
        if (is_dir) {
            RARProcessFile(h, RAR_SKIP, nullptr, nullptr);
            continue;
        }
        fs::path relative;
        SafeArchiveRelativePath(hdr.FileName, &relative, nullptr); // re-validated; already passed above
        fs::path target = (staged / relative).lexically_normal();
        if (!IsWithinRoot(staged, target)) {
            if (err) *err = "archive contains an unsafe path: " + std::string(hdr.FileName);
            SetErrorCode(job.id, "unsafe_path");
            ok = false;
            break;
        }

        item_index++;
        UpdateJob(job.id, [&](FileJobInfo &j) { j.current = hdr.FileName; j.current_index = item_index; });
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            if (err) *err = "could not create directory for " + relative.string();
            SetErrorCode(job.id, "create_failed");
            ok = false;
            break;
        }
        (void)chmod(target.parent_path().c_str(), 0777);

        int out_fd = open(target.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (out_fd < 0) {
            if (err) *err = "could not create " + relative.string();
            SetErrorCode(job.id, "write_failed");
            ok = false;
            break;
        }
        (void)fchmod(out_fd, 0777);

        UnrarWriteCtx ctx;
        ctx.fd = out_fd;
        ctx.job_id = job.id;
        ctx.completed = completed;
        ctx.last_update = std::chrono::steady_clock::now();
        RARSetCallback(h, UnrarWriteCallback, reinterpret_cast<LPARAM>(&ctx));
        int res = RARProcessFile(h, RAR_TEST, nullptr, nullptr);
        completed = ctx.completed;

        bool entry_ok = !ctx.failed && res == 0 && !IsCanceled();
        if (entry_ok && ctx.unsynced > 0 && !SyncFile(out_fd)) {
            if (err) *err = "failed syncing " + relative.string();
            SetErrorCode(job.id, "write_failed");
            entry_ok = false;
        }
        if (close(out_fd) != 0 && entry_ok) {
            if (err) *err = "failed closing " + relative.string();
            SetErrorCode(job.id, "write_failed");
            entry_ok = false;
        }
        if (!entry_ok) {
            if (!ctx.failed && res != 0 && !IsCanceled()) {
                if (IsRarPasswordError(res)) {
                    if (err) *err = "this archive needs a password";
                    SetErrorCode(job.id, "password_required");
                } else {
                    if (err) *err = "archive data is corrupt: " + std::string(hdr.FileName);
                    SetErrorCode(job.id, "crc_failed");
                }
            }
            ok = false;
            break;
        }
        UpdateJob(job.id, [&](FileJobInfo &j) { j.completed_bytes = completed; });
    }
    RARCloseArchive(h);

    if (!ok) {
        std::string ignored;
        (void)RemoveNativeTree(staged.string(), &ignored);
        if (!IsCanceled() && err->empty()) *err = "could not extract archive";
        return false;
    }

    std::error_code exists_ec;
    if (!fs::exists(destination, exists_ec)) {
        if (!PublishStaging(staged, destination, err)) return false;
    } else {
        if (!MergeStagedTree(staged, destination, job.conflict_policy, err)) return false;
    }
    if (job.delete_source_on_success) {
        std::error_code rm_ec;
        fs::remove(source, rm_ec);
    }
    return true;
}

struct FdGuard {
    int fd = -1;
    ~FdGuard() { if (fd >= 0) close(fd); }
};

zipcrypto::EntryInfo EncryptedEntryInfo(const mz_zip_archive_file_stat &stat) {
    zipcrypto::EntryInfo info;
    info.local_header_ofs = stat.m_local_header_ofs;
    info.comp_size = stat.m_comp_size;
    info.crc = stat.m_crc32;
    info.bit_flag = stat.m_bit_flag;
    info.method = stat.m_method;
    return info;
}

// Maps a failed zipcrypto open onto the job error code and message the UI keys off.
void DescribeZipOpenFailure(zipcrypto::OpenResult result, const char *entry, std::string *err, std::string *code) {
    const char *c = "invalid_zip";
    std::string msg = "could not read " + std::string(entry) + " from the archive";
    switch (result) {
    case zipcrypto::OpenResult::kBadPassword:
        c = "password_required";
        msg = "this archive needs a password";
        break;
    case zipcrypto::OpenResult::kUnsupported:
        c = "unsupported_encryption";
        msg = "this archive uses an encryption or compression method that isn't supported";
        break;
    default:
        break;
    }
    if (err) *err = msg;
    if (code) *code = c;
}

// A ZIP's central directory is never encrypted, so an encrypted archive opens and
// lists fine — the password only matters once an entry is read. Trial-opens the
// first encrypted entry so a missing/wrong password surfaces up front, the same
// way a .rar does. True when the archive isn't encrypted or the password fits.
bool VerifyZipPassword(mz_zip_archive *zip, const std::string &path, const std::string &password,
                       std::string *err, std::string *code) {
    mz_uint count = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(zip, i, &stat)) continue;
        if (mz_zip_reader_is_file_a_directory(zip, i) || !zipcrypto::IsEncrypted(stat.m_bit_flag)) continue;

        FdGuard guard;
        guard.fd = open(path.c_str(), O_RDONLY);
        if (guard.fd < 0) {
            if (err) *err = "could not open the zip — is it valid?";
            if (code) *code = "invalid_zip";
            return false;
        }
        zipcrypto::OpenResult result;
        auto reader = zipcrypto::EntryReader::Open(guard.fd, EncryptedEntryInfo(stat), password, &result);
        if (reader) return true;
        DescribeZipOpenFailure(result, stat.m_filename, err, code);
        return false;
    }
    return true;
}

// Streamed via mz_zip_reader_extract_iter_* instead of extract_to_file —
// bounded memory, and cancellation/progress checked every buffer, not per file.
// Encrypted entries can't go through miniz, so they're read raw, decrypted and
// inflated by zipcrypto::EntryReader, which hands back plaintext in the same
// buffer-at-a-time shape.
bool RunUnzip(FileJobInfo job, std::string *err) {
    if (IsRarFilename(job.source)) return RunUnrarExtraction(job, err);
    fs::path source(job.source), destination(job.destination);

    mz_zip_archive zip {};
    if (!mz_zip_reader_init_file(&zip, source.c_str(), 0)) {
        if (err) *err = "could not open the zip — is it valid?";
        SetErrorCode(job.id, "invalid_zip");
        return false;
    }

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    uint64_t total = 0, item_count = 0;
    std::vector<mz_uint> file_indices;
    std::vector<fs::path> directory_entries;
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        fs::path relative;
        std::string path_err;
        if (!SafeArchiveRelativePath(stat.m_filename, &relative, &path_err)) {
            mz_zip_reader_end(&zip);
            if (err) *err = "archive contains an unsafe path: " + std::string(stat.m_filename);
            SetErrorCode(job.id, "unsafe_path");
            return false;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            directory_entries.push_back(relative);
            continue;
        }
        if (UINT64_MAX - total < stat.m_uncomp_size) {
            mz_zip_reader_end(&zip);
            if (err) *err = "archive expands beyond the supported size";
            SetErrorCode(job.id, "invalid_zip");
            return false;
        }
        total += stat.m_uncomp_size;
        item_count++;
        file_indices.push_back(i);
    }
    {
        std::string password_err, password_code;
        if (!VerifyZipPassword(&zip, job.source, job.archive_password, &password_err, &password_code)) {
            mz_zip_reader_end(&zip);
            if (err) *err = password_err;
            SetErrorCode(job.id, password_code.c_str());
            return false;
        }
    }
    FdGuard raw_fd; // raw reads of encrypted entries; opened on first use
    UpdateJob(job.id, [&](FileJobInfo &j) { j.total_bytes = total; j.total_items = item_count; j.state = "running"; });

    // Exact, not an estimate: `total` is the archive's declared uncompressed size.
    // Checks the destination itself, since its parent can be a different filesystem.
    uint64_t available = 0;
    std::string space_err;
    if (!CheckFreeSpaceAtDirectory(destination.string(), total, &available, &space_err)) {
        mz_zip_reader_end(&zip);
        if (err) *err = "not enough free space to extract this archive";
        SetErrorCode(job.id, "no_space");
        return false;
    }

    fs::path staged = destination.string() + ".pmgr-job-" + std::to_string(job.id);
    std::error_code ec;
    fs::create_directories(staged, ec);
    if (ec) {
        mz_zip_reader_end(&zip);
        if (err) *err = "could not create extraction staging: " + ec.message();
        SetErrorCode(job.id, "create_failed");
        return false;
    }
    (void)chmod(staged.c_str(), 0777);

    // Empty directories don't survive a file-by-file walk — create every
    // directory entry the archive declares, even empty ones.
    for (const auto &dir : directory_entries) {
        fs::path target = (staged / dir).lexically_normal();
        if (!IsWithinRoot(staged, target)) continue; // already validated above; defense in depth
        fs::create_directories(target, ec);
        if (ec) {
            mz_zip_reader_end(&zip);
            std::string ignored;
            (void)RemoveNativeTree(staged.string(), &ignored);
            if (err) *err = "could not create extracted directory: " + dir.string();
            SetErrorCode(job.id, "create_failed");
            return false;
        }
        (void)chmod(target.c_str(), 0777);
    }

    std::vector<char> buffer(kExtractBuffer);
    uint64_t completed = 0, item_index = 0;
    bool ok = true;
    for (mz_uint i : file_indices) {
        if (IsCanceled()) { ok = false; break; }
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        fs::path relative;
        SafeArchiveRelativePath(stat.m_filename, &relative, nullptr); // re-validated below; already passed once above
        fs::path target = (staged / relative).lexically_normal();
        if (!IsWithinRoot(staged, target)) {
            if (err) *err = "archive contains an unsafe path: " + std::string(stat.m_filename);
            SetErrorCode(job.id, "unsafe_path");
            ok = false;
            break;
        }

        item_index++;
        UpdateJob(job.id, [&](FileJobInfo &j) { j.current = stat.m_filename; j.current_index = item_index; });
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            if (err) *err = "could not create directory for " + relative.string();
            SetErrorCode(job.id, "create_failed");
            ok = false;
            break;
        }
        (void)chmod(target.parent_path().c_str(), 0777);

        // Exactly one of these is set: miniz handles plain entries, zipcrypto the encrypted ones.
        mz_zip_reader_extract_iter_state *iter = nullptr;
        std::unique_ptr<zipcrypto::EntryReader> encrypted;
        if (zipcrypto::IsEncrypted(stat.m_bit_flag)) {
            if (raw_fd.fd < 0) raw_fd.fd = open(job.source.c_str(), O_RDONLY);
            zipcrypto::OpenResult open_result = zipcrypto::OpenResult::kCorrupt;
            if (raw_fd.fd >= 0) encrypted = zipcrypto::EntryReader::Open(raw_fd.fd, EncryptedEntryInfo(stat), job.archive_password, &open_result);
            if (!encrypted) {
                std::string open_err, open_code;
                DescribeZipOpenFailure(open_result, stat.m_filename, &open_err, &open_code);
                if (err) *err = open_err;
                SetErrorCode(job.id, open_code.c_str());
                ok = false;
                break;
            }
        } else {
            iter = mz_zip_reader_extract_iter_new(&zip, i, 0);
            if (!iter) {
                if (err) *err = std::string("could not read ") + stat.m_filename + " from the archive";
                SetErrorCode(job.id, "crc_failed");
                ok = false;
                break;
            }
        }
        int out_fd = open(target.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (out_fd < 0) {
            if (iter) mz_zip_reader_extract_iter_free(iter);
            if (err) *err = "could not create " + relative.string();
            SetErrorCode(job.id, "write_failed");
            ok = false;
            break;
        }
        (void)fchmod(out_fd, 0777);

        bool entry_ok = true;
        uint64_t unsynced = 0;
        auto last_update = std::chrono::steady_clock::now();
        bool encrypted_reached_end = false;
        for (;;) {
            if (IsCanceled()) { entry_ok = false; break; }
            const char *chunk = buffer.data();
            size_t got = 0;
            if (encrypted) {
                const uint8_t *plain = nullptr;
                zipcrypto::Chunk next = encrypted->Next(&plain, &got);
                if (next == zipcrypto::Chunk::kEnd) { encrypted_reached_end = true; break; }
                if (next == zipcrypto::Chunk::kError) break; // reported below as corrupt
                chunk = reinterpret_cast<const char *>(plain);
            } else {
                got = mz_zip_reader_extract_iter_read(iter, buffer.data(), buffer.size());
                if (got == 0) break; // end of this entry, or a failure — checked via iter_free() below
            }
            if (!WriteAll(out_fd, chunk, got)) {
                if (err) *err = "failed writing " + relative.string();
                SetErrorCode(job.id, "write_failed");
                entry_ok = false;
                break;
            }
            completed += got;
            unsynced += got;
            if (unsynced >= kCopySyncCheckpoint) {
                if (!SyncFile(out_fd)) {
                    if (err) *err = "failed syncing " + relative.string();
                    SetErrorCode(job.id, "write_failed");
                    entry_ok = false;
                    break;
                }
                unsynced = 0;
            }
            auto now = std::chrono::steady_clock::now();
            if (now - last_update >= kProgressThrottle) {
                last_update = now;
                uint64_t snapshot = completed;
                UpdateJob(job.id, [&](FileJobInfo &j) { j.completed_bytes = snapshot; });
            }
        }
        bool decompress_ok;
        bool wrong_password = false;
        if (encrypted) {
            zipcrypto::VerifyResult verified = zipcrypto::VerifyResult::kIoError;
            if (encrypted_reached_end) verified = encrypted->Verify();
            decompress_ok = verified == zipcrypto::VerifyResult::kOk;
            // ZipCrypto's one-byte password check lets ~1 in 128 wrong passwords through.
            wrong_password = verified == zipcrypto::VerifyResult::kBadCrc && encrypted->WeakPasswordCheck();
        } else {
            decompress_ok = mz_zip_reader_extract_iter_free(iter) != 0;
        }
        if (entry_ok && unsynced > 0 && !SyncFile(out_fd)) {
            if (err) *err = "failed syncing " + relative.string();
            SetErrorCode(job.id, "write_failed");
            entry_ok = false;
        }
        if (close(out_fd) != 0 && entry_ok) {
            if (err) *err = "failed closing " + relative.string();
            SetErrorCode(job.id, "write_failed");
            entry_ok = false;
        }

        if (!entry_ok || IsCanceled()) { ok = false; break; }
        if (!decompress_ok) {
            if (wrong_password) {
                if (err) *err = "this archive needs a password";
                SetErrorCode(job.id, "password_required");
            } else {
                if (err) *err = "archive data is corrupt: " + std::string(stat.m_filename);
                SetErrorCode(job.id, "crc_failed");
            }
            ok = false;
            break;
        }
        UpdateJob(job.id, [&](FileJobInfo &j) { j.completed_bytes = completed; });
    }
    mz_zip_reader_end(&zip);

    if (!ok) {
        std::string ignored;
        (void)RemoveNativeTree(staged.string(), &ignored);
        if (!IsCanceled() && err->empty()) *err = "could not extract archive";
        return false;
    }

    std::error_code exists_ec;
    if (!fs::exists(destination, exists_ec)) {
        if (!PublishStaging(staged, destination, err)) return false;
    } else {
        if (!MergeStagedTree(staged, destination, job.conflict_policy, err)) return false;
    }
    if (job.delete_source_on_success) {
        std::error_code rm_ec;
        fs::remove(source, rm_ec); // extraction already fully succeeded — a failed delete shouldn't fail the job
    }
    return true;
}

struct DeleteEntry {
    std::string path;
    uint64_t size = 0;
    bool is_dir = false;
};

bool CollectDeleteEntries(const std::string &path, dev_t root_device, std::vector<DeleteEntry> *entries,
                          uint64_t *total, std::string *err) {
    if (IsCanceled()) return false;
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (err) *err = std::string("could not inspect path for deletion: ") + std::strerror(errno);
        return false;
    }
    bool directory_entry = S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
    if (!directory_entry) {
        uint64_t size = S_ISREG(st.st_mode) && st.st_size > 0 ? static_cast<uint64_t>(st.st_size) : 0;
        if (UINT64_MAX - *total < size) {
            if (err) *err = "delete tree is too large";
            return false;
        }
        *total += size;
        entries->push_back({path, size, false});
        return true;
    }
    if (st.st_dev != root_device) {
        if (err) *err = "refusing to delete across another mounted filesystem";
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
        if (!CollectDeleteEntries(child, root_device, entries, total, err)) { ok = false; break; }
        errno = 0;
    }
    if (ok && errno != 0) {
        if (err) *err = std::string("delete traversal failed: ") + std::strerror(errno);
        ok = false;
    }
    if (closedir(directory) != 0 && ok) {
        if (err) *err = std::string("could not close directory for deletion: ") + std::strerror(errno);
        ok = false;
    }
    if (ok) entries->push_back({path, 0, true});
    return ok;
}

bool RunDelete(FileJobInfo job, std::string *err) {
    struct stat root {};
    if (lstat(job.source.c_str(), &root) != 0) {
        if (err) *err = "path no longer exists";
        return false;
    }
    UpdateJob(job.id, [](FileJobInfo &current) { current.state = "scanning"; });
    std::vector<DeleteEntry> entries;
    uint64_t total = 0;
    if (!CollectDeleteEntries(job.source, root.st_dev, &entries, &total, err)) return false;
    UpdateJob(job.id, [&](FileJobInfo &current) {
        current.state = "running";
        current.total_bytes = total;
        current.total_items = entries.size();
    });

    uint64_t completed = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        if (IsCanceled()) return false;
        const DeleteEntry &entry = entries[i];
        int rc = entry.is_dir ? rmdir(entry.path.c_str()) : unlink(entry.path.c_str());
        if (rc != 0) {
            if (err) *err = std::string("could not delete ") + entry.path + ": " + std::strerror(errno);
            return false;
        }
        completed += entry.size;
        UpdateJob(job.id, [&](FileJobInfo &current) {
            current.completed_bytes = completed;
            current.current = entry.path;
            current.current_index = i + 1;
        });
    }
    return true;
}

void SweepStaleEphemeralArchives() {
    std::error_code ec;
    std::string dir = EphemeralArchiveDir();
    if (!fs::is_directory(dir, ec)) return;
    auto now = fs::file_time_type::clock::now();
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto age = now - entry.last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        if (age > kEphemeralArchiveTtl) fs::remove(entry.path(), ec);
    }
}

void Worker() {
    int idle_ticks = 0;
    for (;;) {
        FileJobInfo job;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (auto &candidate : g_jobs) {
                if (candidate.state == "queued") {
                    candidate.state = "running";
                    job = candidate;
                    g_cancel = false;
                    PersistLocked();
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            // ~1200 ticks * 250ms ≈ 5 minutes between sweeps — cheap enough
            // to just piggyback on the idle wait instead of a second thread.
            if (++idle_ticks >= 1200) {
                idle_ticks = 0;
                SweepStaleEphemeralArchives();
            }
            continue;
        }

        std::string err;
        bool ok = false;
        {
            std::unique_lock<std::mutex> mutation_lock(g_mutation_mu);
            try {
                ok = job.type == "zip" ? RunZip(job, &err)
                   : job.type == "unzip" ? RunUnzip(job, &err)
                   : job.type == "delete" ? RunDelete(job, &err)
                                           : RunCopyOrMove(job, job.type == "move", &err);
            } catch (const std::exception &e) {
                err = std::string("file worker failed safely: ") + e.what();
            } catch (...) {
                err = "file worker failed safely";
            }
        }
        bool canceled = IsCanceled();
        if (!ok || canceled) {
            std::string ignored;
            if (job.type != "delete") {
                fs::path staged = (job.type == "copy" || job.type == "move")
                                      ? StagingRoot(job.destination, job.id)
                                      : fs::path(job.destination + ".pmgr-job-" + std::to_string(job.id));
                (void)RemoveNativeTree(staged.string(), &ignored);
            }
        }
        std::string error_code = GetErrorCode(job.id);
        bool needs_attention = error_code == "cleanup_failed" || error_code == "source_tree_changed";
        UpdateJob(job.id, [&](FileJobInfo &current) {
            current.state = canceled ? "canceled" : ok ? "done" :
                            (current.error_code == "cleanup_failed" || current.error_code == "source_tree_changed")
                                ? "attention" : "error";
            current.error = canceled ? "Canceled by user" : err;
            if (canceled) current.error_code = "canceled";
            current.current.clear();
            if (ok) current.completed_bytes = current.total_bytes;
        }, true);
        std::string result = ok ? "completed" : canceled ? "canceled" :
                             needs_attention ? "needs attention" : "failed";
        LogEvent("File job #" + std::to_string(job.id) + " " + result +
                 ": " + job.type + " " + job.source + (err.empty() ? "" : " (" + err + ")"));
    }
}

void LoadQueue() {
    std::ifstream in(QueuePath());
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> parts;
        size_t start = 0;
        for (;;) {
            size_t split = line.find('|', start);
            parts.push_back(line.substr(start, split == std::string::npos ? split : split - start));
            if (split == std::string::npos) break;
            start = split + 1;
        }
        if (parts.size() != 4 && parts.size() != 5) continue;
        char *end = nullptr;
        uint64_t id = std::strtoull(parts[0].c_str(), &end, 10);
        std::string source = HexDecode(parts[2]), destination = HexDecode(parts[3]);
        if (!end || *end || id == 0 ||
            (parts[1] != "copy" && parts[1] != "move" && parts[1] != "delete" &&
             parts[1] != "zip" && parts[1] != "unzip") ||
            !ValidAbsolute(source) || !ValidAbsolute(destination)) continue;

        FileJobInfo job;
        job.id = id;
        job.type = parts[1];
        job.state = "queued";
        job.source = source;
        job.destination = destination;
        std::string extra = parts.size() == 5 ? HexDecode(parts[4]) : "";
        if (job.type == "zip" && IsValidCompressionMode(extra)) job.compression = extra;
        if ((job.type == "copy" || job.type == "move") && IsValidConflictPolicy(extra)) {
            job.conflict_policy = extra;
        } else if (job.type == "copy" || job.type == "move") {
            job.conflict_policy = "cancel";
        }
        if (job.type == "unzip") {
            size_t comma = extra.find(',');
            std::string policy = comma == std::string::npos ? extra : extra.substr(0, comma);
            if (IsValidConflictPolicy(policy)) job.conflict_policy = policy;
            job.delete_source_on_success = comma != std::string::npos && extra.substr(comma + 1) == "1";
        }
        g_jobs.push_back(std::move(job));
        g_next_id = std::max(g_next_id, id + 1);
    }
}

// Shared by QueueZipJob and QueueDownloadZipJob — only the destination and
// `ephemeral` flag differ.
bool EnqueueZip(const std::vector<std::string> &sources, const std::string &destination,
                const std::string &compression, bool ephemeral, uint64_t *job_id, std::string *err) {
    if (sources.empty()) {
        if (err) *err = "no sources selected";
        return false;
    }
    if (!IsValidCompressionMode(compression)) {
        if (err) *err = "invalid compression mode";
        return false;
    }
    for (const auto &s : sources) {
        if (!ValidAbsolute(s)) {
            if (err) *err = "source and destination must be absolute paths without '..'";
            return false;
        }
    }
    if (!ValidAbsolute(destination)) {
        if (err) *err = "source and destination must be absolute paths without '..'";
        return false;
    }
    if (IsProtectedNativeRoot(destination)) {
        if (err) *err = "protected filesystem roots cannot be replaced";
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(fs::path(destination).parent_path(), ec)) {
        fs::create_directories(fs::path(destination).parent_path(), ec); // e.g. tmp/archives/ on first use
        if (!fs::is_directory(fs::path(destination).parent_path(), ec)) {
            if (err) *err = "destination parent directory does not exist";
            return false;
        }
    }
    for (const auto &s : sources) {
        if (!fs::exists(s, ec)) {
            if (err) *err = "source does not exist: " + s;
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_immediate_mutation) {
        if (err) *err = "another file operation is in progress";
        return false;
    }
    if (AnyExternalPath(sources, destination) && g_external_readers != 0) {
        if (err) *err = "external storage is busy with another file operation";
        return false;
    }
    if (UploadOverlapsLocked(sources, destination)) {
        if (err) *err = "that path overlaps an active upload";
        return false;
    }
    size_t active = 0;
    for (const auto &job : g_jobs) {
        if (!ActiveState(job)) continue;
        active++;
        if (JobOverlaps(job, sources, destination)) {
            if (err) *err = "that path overlaps a queued file operation";
            return false;
        }
    }
    if (active >= kMaxJobs) {
        if (err) *err = "file job queue is full";
        return false;
    }

    std::string joined;
    for (size_t i = 0; i < sources.size(); i++) {
        if (i) joined += '\n';
        joined += sources[i];
    }
    uint64_t id = g_next_id++;
    FileJobInfo job;
    job.id = id;
    job.type = "zip";
    job.state = "queued";
    job.source = joined;
    job.destination = destination;
    job.compression = compression;
    job.ephemeral = ephemeral;
    g_jobs.push_back(std::move(job));
    while (g_jobs.size() > kMaxJobs * 2) g_jobs.pop_front();
    PersistLocked();
    if (job_id) *job_id = id;
    return true;
}

} // namespace

std::string UniqueSiblingPath(const std::string &path) {
    fs::path out;
    return UniqueSiblingName(fs::path(path), &out) ? out.string() : std::string();
}

void InitializeFileJobs(const Config &cfg) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    g_cfg = cfg;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        LoadQueue();
    }
    StartBackgroundThread(Worker);
}

bool BeginImmediateFileMutation(std::string *err) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        PruneUploadReservationsLocked();
        if (g_immediate_mutation || g_external_readers != 0 || !g_uploads.empty() ||
            std::any_of(g_jobs.begin(), g_jobs.end(), ActiveState)) {
            if (err) *err = "another file operation is in progress";
            return false;
        }
        g_immediate_mutation = true;
    }
    try {
        g_mutation_mu.lock();
    } catch (...) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_immediate_mutation = false;
        throw;
    }
    return true;
}

void EndImmediateFileMutation() {
    g_mutation_mu.unlock();
    std::lock_guard<std::mutex> lock(g_mu);
    g_immediate_mutation = false;
}

bool FileMutationInProgress() {
    std::lock_guard<std::mutex> lock(g_mu);
    PruneUploadReservationsLocked();
    return g_immediate_mutation || !g_uploads.empty() || std::any_of(g_jobs.begin(), g_jobs.end(), ActiveState);
}

bool BeginFileRead(const std::string &path, std::string *err) {
    if (!ValidAbsolute(path)) {
        if (err) *err = "invalid file read path";
        return false;
    }
    if (!IsExternalStoragePath(path)) return true;

    std::lock_guard<std::mutex> lock(g_mu);
    PruneUploadReservationsLocked();
    if (g_external_readers != 0 || g_immediate_mutation || !g_uploads.empty() ||
        std::any_of(g_jobs.begin(), g_jobs.end(), ActiveState)) {
        if (err) *err = "external storage is busy with another file operation";
        return false;
    }
    g_external_readers++;
    return true;
}

void EndFileRead(const std::string &path) {
    if (!IsExternalStoragePath(path)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_external_readers != 0) g_external_readers--;
}

bool ReserveFileUpload(const std::string &path, const std::string &token, bool expires_when_idle,
                       std::string *err) {
    if (!ValidAbsolute(path) || IsProtectedNativeRoot(path) || token.empty()) {
        if (err) *err = "invalid upload reservation";
        return false;
    }
    std::string normalized = fs::path(path).lexically_normal().string();
    std::lock_guard<std::mutex> lock(g_mu);
    PruneUploadReservationsLocked();
    if (g_immediate_mutation || (IsExternalStoragePath(normalized) && g_external_readers != 0) ||
        std::any_of(g_jobs.begin(), g_jobs.end(), ActiveState)) {
        if (err) *err = "another file operation is in progress";
        return false;
    }
    auto existing = g_uploads.find(normalized);
    if (existing != g_uploads.end() && existing->second.token == token) {
        if (existing->second.in_use) {
            if (err) *err = "this upload checkpoint is already in use";
            return false;
        }
        existing->second.last_seen = std::chrono::steady_clock::now();
        existing->second.expires_when_idle = expires_when_idle;
        existing->second.in_use = true;
        return true;
    }
    for (const auto &[reserved_path, reservation] : g_uploads) {
        if (PathsOverlap(reserved_path, normalized) &&
            (reserved_path != normalized || reservation.token != token)) {
            if (err) *err = "another upload is writing this path";
            return false;
        }
    }
    g_uploads[normalized] = {token, std::chrono::steady_clock::now(), expires_when_idle, true};
    return true;
}

void IdleFileUpload(const std::string &path, const std::string &token) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto found = g_uploads.find(fs::path(path).lexically_normal().string());
    if (found == g_uploads.end() || found->second.token != token) return;
    found->second.in_use = false;
    found->second.last_seen = std::chrono::steady_clock::now();
}

void ReleaseFileUpload(const std::string &path, const std::string &token) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto found = g_uploads.find(fs::path(path).lexically_normal().string());
    if (found != g_uploads.end() && found->second.token == token) g_uploads.erase(found);
}

bool QueueFileJob(const std::string &type, const std::string &source, const std::string &destination,
                  const std::string &conflict_policy, uint64_t *job_id, std::string *err) {
    if (type != "copy" && type != "move") {
        if (err) *err = "unsupported file job type";
        return false;
    }
    if (!IsValidConflictPolicy(conflict_policy)) {
        if (err) *err = "invalid conflict policy";
        return false;
    }
    if (!ValidAbsolute(source) || !ValidAbsolute(destination) || source == destination) {
        if (err) *err = "source and destination must be different absolute paths without '..'";
        return false;
    }
    fs::path normalized_source = fs::path(source).lexically_normal();
    fs::path normalized_destination = fs::path(destination).lexically_normal();
    std::string source_text = normalized_source.string();
    std::string destination_text = normalized_destination.string();
    if (destination_text.size() > source_text.size() &&
        destination_text.compare(0, source_text.size(), source_text) == 0 &&
        destination_text[source_text.size()] == '/') {
        if (err) *err = "destination cannot be inside the source";
        return false;
    }
    if ((type == "move" && IsProtectedNativeRoot(source)) || IsProtectedNativeRoot(destination)) {
        if (err) *err = "protected filesystem roots cannot be moved or replaced";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        if (err) *err = "source does not exist";
        return false;
    }
    if (!fs::is_directory(fs::path(destination).parent_path(), ec)) {
        if (err) *err = "destination parent directory does not exist";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_immediate_mutation) {
        if (err) *err = "another file operation is in progress";
        return false;
    }
    if (AnyExternalPath({source}, destination) && g_external_readers != 0) {
        if (err) *err = "external storage is busy with another file operation";
        return false;
    }
    if (UploadOverlapsLocked({source}, destination)) {
        if (err) *err = "that path overlaps an active upload";
        return false;
    }
    size_t active = 0;
    for (const auto &job : g_jobs) {
        if (ActiveState(job)) {
            active++;
            if (JobOverlaps(job, {source}, destination)) {
                if (err) *err = "that path overlaps a queued file operation";
                return false;
            }
        }
    }
    if (active >= kMaxJobs) {
        if (err) *err = "file job queue is full";
        return false;
    }
    uint64_t id = g_next_id++;
    FileJobInfo job;
    job.id = id;
    job.type = type;
    job.state = "queued";
    job.source = source;
    job.destination = destination;
    job.conflict_policy = conflict_policy;
    g_jobs.push_back(std::move(job));
    while (g_jobs.size() > kMaxJobs * 2) g_jobs.pop_front();
    PersistLocked();
    if (job_id) *job_id = id;
    return true;
}

bool QueueDeleteJob(const std::string &path, uint64_t *job_id, std::string *err) {
    if (!ValidAbsolute(path) || IsProtectedNativeRoot(path)) {
        if (err) *err = "protected filesystem roots cannot be deleted";
        return false;
    }
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (err) *err = "path does not exist";
        return false;
    }
    std::string destination = path;

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_immediate_mutation) {
        if (err) *err = "another file operation is in progress";
        return false;
    }
    if (IsExternalStoragePath(path) && g_external_readers != 0) {
        if (err) *err = "external storage is busy with another file operation";
        return false;
    }
    if (UploadOverlapsLocked({path}, destination)) {
        if (err) *err = "that path overlaps an active upload";
        return false;
    }
    size_t active = 0;
    for (const auto &job : g_jobs) {
        if (!ActiveState(job)) continue;
        active++;
        if (JobOverlaps(job, {path}, destination)) {
            if (err) *err = "that path overlaps a queued file operation";
            return false;
        }
    }
    if (active >= kMaxJobs) {
        if (err) *err = "file job queue is full";
        return false;
    }

    uint64_t id = g_next_id++;
    FileJobInfo job;
    job.id = id;
    job.type = "delete";
    job.state = "queued";
    job.source = path;
    job.destination = destination;
    g_jobs.push_back(std::move(job));
    while (g_jobs.size() > kMaxJobs * 2) g_jobs.pop_front();
    PersistLocked();
    if (job_id) *job_id = id;
    return true;
}

bool QueueZipJob(const std::vector<std::string> &sources, const std::string &destination,
                 const std::string &compression, uint64_t *job_id, std::string *err) {
    return EnqueueZip(sources, destination, compression, false, job_id, err);
}

bool QueueDownloadZipJob(const std::vector<std::string> &sources, const std::string &name,
                         uint64_t *job_id, std::string *out_path, std::string *err) {
    if (name.empty() || name.find('/') != std::string::npos) {
        if (err) *err = "invalid archive name";
        return false;
    }
    std::string destination = EphemeralArchiveDir() + "/" +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) + "-" + name;
    if (!EnqueueZip(sources, destination, "smart", true, job_id, err)) return false;
    if (out_path) *out_path = destination;
    return true;
}

bool QueueUnzipJob(const std::string &source, const std::string &destination, const std::string &conflict_policy,
                   bool delete_source_on_success, const std::string &password, uint64_t *job_id, std::string *err) {
    if (!IsValidConflictPolicy(conflict_policy)) {
        if (err) *err = "invalid conflict policy";
        return false;
    }
    if (!ValidAbsolute(source) || !ValidAbsolute(destination) || source == destination) {
        if (err) *err = "source and destination must be different absolute paths without '..'";
        return false;
    }
    if (IsProtectedNativeRoot(destination)) {
        if (err) *err = "protected filesystem roots cannot be replaced";
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        if (err) *err = "source does not exist";
        return false;
    }
    // Extracting into a fresh sibling folder means only the parent has to exist,
    // so create the destination when it's missing.
    fs::path destination_parent = fs::exists(destination, ec) ? fs::path(destination) : fs::path(destination).parent_path();
    if (!fs::is_directory(destination_parent, ec)) {
        fs::create_directories(destination_parent, ec);
        if (!fs::is_directory(destination_parent, ec)) {
            if (err) *err = "destination parent directory does not exist";
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_immediate_mutation) {
        if (err) *err = "another file operation is in progress";
        return false;
    }
    if (AnyExternalPath({source}, destination) && g_external_readers != 0) {
        if (err) *err = "external storage is busy with another file operation";
        return false;
    }
    if (UploadOverlapsLocked({source}, destination)) {
        if (err) *err = "that path overlaps an active upload";
        return false;
    }
    size_t active = 0;
    for (const auto &job : g_jobs) {
        if (ActiveState(job)) {
            active++;
            if (JobOverlaps(job, {source}, destination)) {
                if (err) *err = "that path overlaps a queued file operation";
                return false;
            }
        }
    }
    if (active >= kMaxJobs) {
        if (err) *err = "file job queue is full";
        return false;
    }
    uint64_t id = g_next_id++;
    FileJobInfo job;
    job.id = id;
    job.type = "unzip";
    job.state = "queued";
    job.source = source;
    job.destination = destination;
    job.conflict_policy = conflict_policy;
    job.delete_source_on_success = delete_source_on_success;
    job.archive_password = password;
    g_jobs.push_back(std::move(job));
    while (g_jobs.size() > kMaxJobs * 2) g_jobs.pop_front();
    PersistLocked();
    if (job_id) *job_id = id;
    return true;
}

std::vector<FileJobInfo> GetFileJobs() {
    std::lock_guard<std::mutex> lock(g_mu);
    return {g_jobs.begin(), g_jobs.end()};
}

bool CancelFileJob(uint64_t id, std::string *err) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto &job : g_jobs) {
        if (job.id != id) continue;
        if (job.state == "queued") {
            job.state = "canceled";
            job.error = "Canceled by user";
            job.error_code = "canceled";
            PersistLocked();
            return true;
        }
        if (job.state == "running" || job.state == "scanning") {
            g_cancel = true; // exactly one worker can be active
            return true;
        }
        if (err) *err = "job is already finished";
        return false;
    }
    if (err) *err = "file job not found";
    return false;
}

void ClearFinishedFileJobs() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [](const FileJobInfo &job) {
        return job.state == "done" || job.state == "attention" || job.state == "canceled" || job.state == "error";
    }), g_jobs.end());
    PersistLocked();
}

// Caps how many entries the inspector response carries, so a huge archive
// can't blow up one JSON response.
constexpr size_t kArchiveInfoEntryCap = 20000;

bool ReadRarArchiveInfoImpl(const std::string &path, const std::string &password, ArchiveInfo *out,
                            std::string *err, std::string *code) {
    std::vector<char> arcname_buf, password_buf;
    HANDLE h = OpenRarForRead(path, password, &arcname_buf, &password_buf);
    if (!h) {
        if (err) *err = "could not open the rar — is it valid?";
        return false;
    }
    RARHeaderDataEx hdr{};
    int rc;
    while ((rc = RARReadHeaderEx(h, &hdr)) == 0) {
        bool is_dir = (hdr.Flags & RHDF_DIRECTORY) != 0;
        uint64_t unpsize = (static_cast<uint64_t>(hdr.UnpSizeHigh) << 32) | hdr.UnpSize;
        uint64_t packsize = (static_cast<uint64_t>(hdr.PackSizeHigh) << 32) | hdr.PackSize;
        if (!is_dir) {
            out->items++;
            out->compressed_size += packsize;
            out->uncompressed_size += unpsize;
        }
        if (out->entries.size() < kArchiveInfoEntryCap) {
            fs::path relative;
            std::string display = SafeArchiveRelativePath(hdr.FileName, &relative, nullptr)
                                       ? relative.generic_string()
                                       : std::string(hdr.FileName);
            out->entries.push_back({display, is_dir, packsize, unpsize});
        } else {
            out->entries_truncated = true;
        }
        // RAR_SKIP never touches the encrypted payload, so a missing/wrong password
        // silently succeeds — RAR_TEST actually decrypts, still without writing anything.
        bool encrypted = (hdr.Flags & RHDF_ENCRYPTED) != 0;
        rc = RARProcessFile(h, encrypted ? RAR_TEST : RAR_SKIP, nullptr, nullptr);
        if (rc != 0) break;
    }
    RARCloseArchive(h);
    if (IsRarPasswordError(rc)) {
        if (err) *err = "this archive needs a password";
        if (code) *code = "password_required";
        return false;
    }
    return true;
}

bool ReadArchiveInfo(const std::string &path, const std::string &password, ArchiveInfo *out,
                     std::string *err, std::string *code) {
    if (!out) return false;
    if (IsRarFilename(path)) return ReadRarArchiveInfoImpl(path, password, out, err, code);
    mz_zip_archive zip {};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
        if (err) *err = "could not open the zip — is it valid?";
        return false;
    }
    // The listing itself needs no password, but gating it on one is what makes the
    // UI ask for it before the user starts an extraction that would just fail.
    if (!VerifyZipPassword(&zip, path, password, err, code)) {
        mz_zip_reader_end(&zip);
        return false;
    }
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        bool is_dir = mz_zip_reader_is_file_a_directory(&zip, i);
        if (!is_dir) {
            out->items++;
            out->compressed_size += stat.m_comp_size;
            out->uncompressed_size += stat.m_uncomp_size;
        }
        if (out->entries.size() < kArchiveInfoEntryCap) {
            fs::path relative;
            // Listing only: an unsafe name still shows so the archive looks suspect,
            // but it never resolves to a real path.
            std::string display = SafeArchiveRelativePath(stat.m_filename, &relative, nullptr)
                                       ? relative.generic_string()
                                       : std::string(stat.m_filename);
            out->entries.push_back({display, static_cast<bool>(is_dir), stat.m_comp_size, stat.m_uncomp_size});
        } else {
            out->entries_truncated = true;
        }
    }
    mz_zip_reader_end(&zip);
    return true;
}

bool IsZipFilename(const std::string &name) {
    if (name.size() < 4) return false;
    std::string suffix = name.substr(name.size() - 4);
    for (char &c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return suffix == ".zip";
}

bool IsRarFilename(const std::string &name) {
    if (name.size() < 4) return false;
    std::string suffix = name.substr(name.size() - 4);
    for (char &c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return suffix == ".rar";
}

} // namespace prospero
