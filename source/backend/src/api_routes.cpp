#include "job_queue.hpp"
#include "api_routes.hpp"
#include "disc_range.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <thread>

#include "accounts.hpp"
#include "assets.hpp"
#include "avatar.hpp"
#include "elfldr_client.hpp"
#include "file_jobs.hpp"
#include "file_manager.hpp"
#include "jailbreak.hpp"
#include "json_writer.hpp"
#include "klog_client.hpp"
#include "log.hpp"
#include "payload_catalog.hpp"
#include "pkg_installer.hpp"
#include "pkg_ws_upload.hpp"
#include "process_manager.hpp"
#include "repository.hpp"
#include "savedata.hpp"
#include "shadowmount.hpp"
#include "system_info.hpp"
#include "thermal.hpp"
#include "titles.hpp"

namespace fs = std::filesystem;

namespace prospero {

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGiB = 1024ull * kMiB;

// PS5's FreeBSD kernel returns EAGAIN with zero bytes under sendfile() on PFS/exFAT, and
// MHD's fd-backed response path has no retry for that. Reading via pread() chunks avoids it.
constexpr size_t kDownloadReadChunk = 2 * 1024 * 1024;

class FileReadStream final : public HttpBodyStream {
  public:
    FileReadStream(int fd, std::shared_ptr<void> lease, uint64_t base, uint64_t count)
        : fd_(fd), lease_(std::move(lease)), base_(base), count_(count) {}
    ~FileReadStream() override {
        if (fd_ >= 0) close(fd_);
#ifdef PROSPEROMGR_DEBUG_NET
        double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        double mb_s = elapsed_s > 0 ? (static_cast<double>(bytes_read_) / (1024.0 * 1024.0)) / elapsed_s : 0;
        LogEvent("[net] download stream: " + std::to_string(bytes_read_) + " bytes in " +
                 std::to_string(elapsed_s) + "s (" + std::to_string(mb_s) + " MB/s)");
#endif
    }

    ssize_t Read(uint64_t position, char *buffer, size_t max_bytes) override {
        if (position >= count_) return MHD_CONTENT_READER_END_OF_STREAM;
        size_t wanted = static_cast<size_t>(std::min<uint64_t>(
            std::min(max_bytes, kDownloadReadChunk), count_ - position));
        ssize_t got;
        do {
            got = pread(fd_, buffer, wanted, static_cast<off_t>(base_ + position));
        } while (got < 0 && errno == EINTR);

        if (got > 0) {
#ifdef PROSPEROMGR_DEBUG_NET
            bytes_read_ += static_cast<uint64_t>(got);
#endif
            return got;
        }
        if (got == 0) return MHD_CONTENT_READER_END_OF_STREAM;
        return MHD_CONTENT_READER_END_WITH_ERROR;
    }

  private:
    int fd_ = -1;
    std::shared_ptr<void> lease_;
    uint64_t base_ = 0;
    uint64_t count_ = 0;
#ifdef PROSPEROMGR_DEBUG_NET
    uint64_t bytes_read_ = 0;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
#endif
};

class FileReadLease {
  public:
    explicit FileReadLease(std::string path) : path_(std::move(path)), active_(BeginFileRead(path_, &error_)) {}
    ~FileReadLease() { if (active_) EndFileRead(path_); }

    explicit operator bool() const { return active_; }
    const std::string &Error() const { return error_; }

  private:
    std::string path_;
    std::string error_;
    bool active_ = false;
};

std::string FileVersion(const struct stat &st) {
    return std::to_string(static_cast<unsigned long long>(st.st_dev)) + "-" +
           std::to_string(static_cast<unsigned long long>(st.st_ino)) + "-" +
           std::to_string(static_cast<unsigned long long>(st.st_size)) + "-" +
           std::to_string(static_cast<long long>(st.st_mtim.tv_sec)) + "-" +
           std::to_string(st.st_mtim.tv_nsec);
}

bool ValidUtf8(const std::string &text) {
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0) return false;
        if (c < 0x80) { i++; continue; }
        size_t count = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
        if (count == 0 || i + count > text.size()) return false;
        uint32_t value = c & ((1u << (7 - count)) - 1u);
        for (size_t j = 1; j < count; j++) {
            unsigned char next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xC0) != 0x80) return false;
            value = (value << 6) | (next & 0x3F);
        }
        if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
            (count == 4 && value < 0x10000) || value > 0x10FFFF ||
            (value >= 0xD800 && value <= 0xDFFF)) return false;
        i += count;
    }
    return true;
}

std::string ContentDisposition(const std::string &filename) {
    std::string fallback;
    std::string encoded;
    const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : filename) {
        fallback += c >= 0x20 && c < 0x7F && c != '"' && c != '\\' ? static_cast<char>(c) : '_';
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.') encoded += static_cast<char>(c);
        else {
            encoded.push_back('%');
            encoded.push_back(hex[c >> 4]);
            encoded.push_back(hex[c & 15]);
        }
    }
    if (fallback.empty()) fallback = "download";
    return "attachment; filename=\"" + fallback + "\"; filename*=UTF-8''" + encoded;
}

#ifdef PROSPEROMGR_DEBUG_NET
// Streams a fixed, reused buffer up to `total_` bytes — constant-memory even for a multi-GiB
// request, isolating MHD/TCP throughput from filesystem cost. Mirror of net-sink on upload.
class GeneratedStream final : public HttpBodyStream {
  public:
    explicit GeneratedStream(uint64_t total) : total_(total) {
        pattern_.resize(4 * 1024 * 1024);
        for (size_t i = 0; i < pattern_.size(); i++) pattern_[i] = static_cast<char>(i);
    }
    ~GeneratedStream() override {
        double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        double mb_s = elapsed_s > 0 ? (static_cast<double>(sent_) / (1024.0 * 1024.0)) / elapsed_s : 0;
        LogEvent("[net] net-source: " + std::to_string(sent_) + " bytes in " + std::to_string(elapsed_s) +
                 "s (" + std::to_string(mb_s) + " MB/s)");
    }

    ssize_t Read(uint64_t position, char *buffer, size_t max_bytes) override {
        if (position >= total_) return MHD_CONTENT_READER_END_OF_STREAM;
        size_t wanted = std::min<uint64_t>(max_bytes, total_ - position);
        wanted = std::min(wanted, pattern_.size());
        std::memcpy(buffer, pattern_.data(), wanted);
        sent_ += wanted;
        return static_cast<ssize_t>(wanted);
    }

  private:
    uint64_t total_;
    uint64_t sent_ = 0;
    std::vector<char> pattern_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};
#endif

bool ParseUint64(const std::string &text, uint64_t *value) {
    if (!value || text.empty()) return false;
    char *end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno || !end || *end != '\0') return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
}

bool SafeUploadId(const std::string &id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id) {
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}

class ImmediateFileMutationGuard {
public:
    explicit ImmediateFileMutationGuard(std::string *err) : active_(BeginImmediateFileMutation(err)) {}
    ~ImmediateFileMutationGuard() { if (active_) EndImmediateFileMutation(); }

    explicit operator bool() const { return active_; }

private:
    bool active_ = false;
};

std::string PayloadToJson(const PayloadInfo &p) {
    return JsonWriter()
        .Str("name", p.name)
        .Str("path", p.path)
        .Int("size_bytes", static_cast<long long>(p.size_bytes))
        .Int("mtime_unix", static_cast<long long>(p.mtime_unix))
        .Bool("bundled", p.bundled)
        .Bool("enabled", p.enabled)
        .Bool("favorite", p.favorite)
        .Int("launch_count", static_cast<long long>(p.launch_count))
        .Int("last_launched_unix", static_cast<long long>(p.last_launched_unix))
        .Str("installed_version", p.installed_version)
        .Str("source", p.source)
        .Str("source_url", p.source_url)
        .Done();
}

std::string FileEntryToJson(const FileManagerEntry &e) {
    return JsonWriter()
        .Str("name", e.name)
        .Bool("is_dir", e.is_dir)
        .Int("size", static_cast<long long>(e.size))
        .Int("mode", e.mode)
        .Str("modified", e.modified)
        .Done();
}

std::string FileJobToJson(const FileJobInfo &job) {
    return JsonWriter()
        .Int("id", static_cast<long long>(job.id))
        .Str("type", job.type)
        .Str("state", job.state)
        .Str("source", job.source)
        .Str("destination", job.destination)
        .Str("current", job.current)
        .Int("current_index", static_cast<long long>(job.current_index))
        .Int("total_items", static_cast<long long>(job.total_items))
        .Int("completed", static_cast<long long>(job.completed_bytes))
        .Int("total", static_cast<long long>(job.total_bytes))
        .Str("error", job.error)
        .Str("error_code", job.error_code)
        .Str("compression", job.compression)
        .Str("conflict_policy", job.conflict_policy)
        .Bool("delete_source", job.delete_source_on_success)
        .Bool("ephemeral", job.ephemeral)
        .Done();
}

std::string ArchiveEntryToJson(const ArchiveEntryInfo &e) {
    return JsonWriter()
        .Str("name", e.name)
        .Bool("is_dir", e.is_dir)
        .Int("compressed_size", static_cast<long long>(e.compressed_size))
        .Int("uncompressed_size", static_cast<long long>(e.uncompressed_size))
        .Done();
}

// Stable, lowercase-alnum id derived from the entry name — lets the frontend look up a
// translated description without a second hardcoded id per entry in payload_catalog.cpp.
std::string CatalogEntrySlug(const std::string &name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string PayloadCatalogEntryToJson(const PayloadCatalogEntry &entry, const CatalogVersionStatus &status) {
    return JsonWriter()
        .Str("id", CatalogEntrySlug(entry.name))
        .Str("developer", entry.developer)
        .Str("name", entry.name)
        .Str("description", entry.description)
        .Str("url", entry.url)
        .Bool("custom", entry.custom)
        .Str("version", status.version)
        .Str("version_state", status.state)
        .Str("version_error", status.error)
        .Int("checked_at", status.checked_at_unix)
        .Done();
}

std::string ProcessToJson(const ProcessInfo &p) {
    return JsonWriter().Int("pid", p.pid).Int("ppid", p.ppid).Str("name", p.name).Done();
}

std::string AccountSlotToJson(const AccountSlot &a) {
    return JsonWriter()
        .Int("slot", a.slot)
        .Str("name", a.name)
        .Str("id_hex", a.id_hex)
        .Str("type", a.type)
        .Int("flags", a.flags)
        .Bool("activated", a.flags != 0 && a.id_hex != "0x0")
        .Bool("is_foreground", a.is_foreground)
        .Bool("has_backup", a.has_backup)
        .Done();
}

std::string SaveEntryToJson(const SaveEntry &s, const std::string &title_name, const std::string &platform,
                             const std::string &user_name) {
    return JsonWriter()
        .Str("user_id_hex", s.user_id_hex)
        .Str("user_name", user_name)
        .Str("title_id", s.title_id)
        .Str("title_name", title_name)
        .Str("platform", platform)
        .Bool("has_save_icon", !s.icon_path.empty())
        .Str("filename", s.filename)
        .Int("size_bytes", s.size_bytes)
        .Bool("is_ps4", s.is_ps4)
        .Done();
}

std::string TitleToJson(const TitleInfo &t) {
    return JsonWriter()
        .Str("title_id", t.title_id)
        .Str("name", t.name)
        .Str("platform", t.platform)
        .Bool("has_icon", !t.icon_path.empty())
        .Int("played_seconds", t.played_seconds)
        .Str("last_played", t.last_played)
        .Str("version", t.version)
        .Done();
}

std::string GuessContentType(const std::string &filename) {
    auto ends_with = [&](const char *suffix) {
        size_t n = std::strlen(suffix);
        return filename.size() >= n && filename.compare(filename.size() - n, n, suffix) == 0;
    };
    if (ends_with(".png")) return "image/png";
    if (ends_with(".jpg") || ends_with(".jpeg")) return "image/jpeg";
    if (ends_with(".json")) return "application/json";
    if (ends_with(".txt") || ends_with(".ini") || ends_with(".log")) return "text/plain; charset=utf-8";
    if (IsZipFilename(filename)) return "application/zip";
    if (IsRarFilename(filename)) return "application/vnd.rar";
    return "application/octet-stream";
}

} // namespace

void RegisterApiRoutes(HttpServer *server, const Config &cfg, PayloadRegistry *registry,
                        Autoloader *autoloader) {
    // ---- General status --------------------------------------------------

    server->Route("GET", "/api/status", [](const HttpRequest &) {
        return HttpResponse::Json(JsonWriter()
                                       .Str("name", "Prospero Manager")
                                       .Str("version", kVersion)
                                       .Str("author", kAuthor)
                                       .Int("http_port", Config::kHttpPort)
                                       .Int("ftp_port", Config::kFtpsrvPort)
                                       .Int("elfldr_port", Config::kElfldrPort)
                                       .Int("uptime_seconds", UptimeSeconds())
                                       .Str("instance_id", ProcessInstanceId())
                                       .Bool("privileged", IsPrivileged())
                                       .Done());
    });

    // One snapshot for the shell's activity dock, so pages don't poll two job
    // endpoints just to learn whether work is still running.
    server->Route("GET", "/api/tasks", [autoloader](const HttpRequest &) {
        DownloadJobStatus download = GetDownloadJobStatus();
        PkgJobStatus pkg = GetPkgJobStatus();
        AutoloaderStatus automatic = autoloader->GetStatus();
        std::vector<std::string> file_jobs;
        for (const auto &job : GetFileJobs()) {
            if (job.state == "queued" || job.state == "scanning" || job.state == "running") {
                file_jobs.push_back(FileJobToJson(job));
            }
        }

        const char *download_phase = download.phase == DownloadJobPhase::kIdle         ? "idle"
                                     : download.phase == DownloadJobPhase::kDownloading ? "downloading"
                                     : download.phase == DownloadJobPhase::kInstalling  ? "launching"
                                     : download.phase == DownloadJobPhase::kDone        ? "done"
                                                                                         : "error";
        const char *pkg_phase = pkg.phase == PkgJobPhase::kIdle         ? "idle"
                                : pkg.phase == PkgJobPhase::kDownloading ? "downloading"
                                : pkg.phase == PkgJobPhase::kInstalling  ? "installing"
                                : pkg.phase == PkgJobPhase::kQueued      ? "queued"
                                                                          : "error";

        std::string download_json = JsonWriter()
                                        .Str("phase", download_phase)
                                        .Int("loaded", download.loaded)
                                        .Int("total", download.total)
                                        .Int("speed_bps", static_cast<long long>(download.speed_bps))
                                        .Str("filename", download.filename)
                                        .Str("error", download.error)
                                        .Done();
        std::string pkg_json = JsonWriter()
                                   .Str("phase", pkg_phase)
                                   .Int("loaded", pkg.loaded)
                                   .Int("total", pkg.total)
                                   .Int("speed_bps", static_cast<long long>(pkg.speed_bps))
                                   .Str("content_id", pkg.content_id)
                                   .Str("title", pkg.title)
                                   .Str("error", pkg.error)
                                   .Done();
        SaveJobStatus save_job = GetSaveJobStatus();
        const char *save_phase = save_job.phase == SaveJobPhase::kRunning ? "running"
                                 : save_job.phase == SaveJobPhase::kDone  ? "done"
                                 : save_job.phase == SaveJobPhase::kError ? "error"
                                                                          : "idle";
        const char *save_kind = save_job.kind == SaveJobKind::kImport      ? "import"
                                : save_job.kind == SaveJobKind::kRestore   ? "restore"
                                : save_job.kind == SaveJobKind::kBackupAll ? "backup_all"
                                                                           : "none";
        std::string save_json = JsonWriter()
                                    .Str("kind", save_kind)
                                    .Str("phase", save_phase)
                                    .Str("label", save_job.label)
                                    .Int("done", save_job.done)
                                    .Int("total", save_job.total)
                                    .Int("failed", save_job.failed)
                                    .Str("error", save_job.error)
                                    .Str("error_code", save_job.error_code)
                                    .Done();

        std::string autoloader_json = JsonWriter()
                                          .Bool("running", automatic.running)
                                          .Int("remaining_seconds", automatic.remaining_seconds)
                                          .Int("total", automatic.total)
                                          .Int("done", automatic.done)
                                          .Int("failed", automatic.failed)
                                          .Str("current", automatic.current)
                                          .Done();
        return HttpResponse::Json(JsonWriter()
                                      .Raw("payload", download_json)
                                      .Raw("pkg", pkg_json)
                                      .Raw("autoloader", autoloader_json)
                                      .Raw("save_job", save_json)
                                      .Raw("files", JsonWriter::Array(file_jobs))
                                      .Done());
    });

    server->Route("GET", "/api/paths", [cfg](const HttpRequest &) {
        return HttpResponse::Json(JsonWriter()
                                       .Str("root_dir", cfg.root_dir)
                                       .Str("payloads_dir", cfg.payloads_dir)
                                       .Str("bundled_dir", cfg.bundled_dir)
                                       .Str("tmp_dir", cfg.tmp_dir)
                                       .Str("autoloader_ini", cfg.autoloader_ini)
                                       .Done());
    });

    // Repoints payloads_dir at an existing folder (e.g. shared with another homebrew
    // manager) instead of Prospero's own default. Only takes effect after a restart.
    server->Route("POST", "/api/paths/payloads-dir", [cfg](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        std::string err;
        if (!SetPayloadsDirOverride(cfg, path, &err)) return HttpResponse::Error(err);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("GET", "/api/system/info", [](const HttpRequest &) {
        SystemInfo info = ReadSystemInfo();
        std::vector<std::string> storage;
        for (const auto &volume : info.storage) {
            storage.push_back(JsonWriter()
                                  .Str("label", volume.label)
                                  .Str("kind", volume.kind)
                                  .Int("index", volume.index)
                                  .Str("path", volume.path)
                                  .Int("total", static_cast<long long>(volume.total_bytes))
                                  .Int("free", static_cast<long long>(volume.free_bytes))
                                  .Done());
        }
        return HttpResponse::Json(JsonWriter()
                                      .Str("platform", info.platform)
                                      .Str("firmware", info.firmware)
                                      .Str("sdk_version", info.sdk_version)
                                      .Str("model", info.model)
                                      .Str("hostname", info.hostname)
                                      .Str("ip_address", info.ip_address)
                                      .Str("user_name", info.user_name)
                                      .Str("user_id", info.user_id)
                                      .Raw("storage", JsonWriter::Array(storage))
                                      .Done());
    });

    server->Route("GET", "/api/logs", [](const HttpRequest &) {
        std::vector<std::string> items;
        for (const auto &line : GetLogs()) items.push_back(JsonWriter::Quote(line));
        return HttpResponse::Json(JsonWriter().Raw("lines", JsonWriter::Array(items)).Done());
    });

    server->Route("POST", "/api/logs/clear", [](const HttpRequest &) {
        ClearLogs();
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("GET", "/api/logs/archive", [](const HttpRequest &) {
        std::string path = PersistentLogPath();
        struct stat st {};
        if (path.empty() || stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            return HttpResponse::Error("persistent log is not available", 404);
        }
        HttpResponse resp;
        resp.content_type = "text/plain; charset=utf-8";
        resp.body_file = path;
        resp.extra_headers["Content-Disposition"] = "attachment; filename=prosperomgr-diagnostics.log";
        return resp;
    });

    // Bundled klogsrv's /dev/klog tail — separate from ProsperoMgr's own
    // event log above, see klog_client.hpp.
    server->Route("GET", "/api/klogs", [](const HttpRequest &) {
        std::vector<std::string> items;
        for (const auto &line : GetKlogs()) items.push_back(JsonWriter::Quote(line));
        return HttpResponse::Json(
            JsonWriter().Bool("connected", IsKlogsrvConnected()).Raw("lines", JsonWriter::Array(items)).Done());
    });

    server->Route("POST", "/api/klogs/clear", [](const HttpRequest &) {
        ClearKlogs();
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // Kills this process from the outside in — the same self-replace path a
    // fresh deploy uses, just triggered by the UI instead of a new ELF landing.
    server->Route("POST", "/api/shutdown", [](const HttpRequest &) {
        StartBackgroundThread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let the response flush first
            std::string err;
            KillProcess(getpid(), &err);
        });
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- Games library (installed PS5 titles) -------------------------------

    server->Route("GET", "/api/titles", [cfg](const HttpRequest &) {
        std::vector<std::string> items;
        for (const auto &t : ListTitles(cfg)) items.push_back(TitleToJson(t));
        return HttpResponse::Json(JsonWriter().Raw("titles", JsonWriter::Array(items)).Done());
    });

    // Separate from /api/titles so the Home page can poll "what's running"
    // often without re-running the app.db query every time.
    server->Route("GET", "/api/titles/running", [](const HttpRequest &) {
        return HttpResponse::Json(JsonWriter().Str("title_id", GetRunningTitleId()).Done());
    });

    server->Route("GET", "/api/titles/icon", [](const HttpRequest &req) {
        std::string title_id = req.Query("id");
        if (title_id.empty()) return HttpResponse::Error("missing 'id'");

        std::string path = TitleIconPath(title_id);
        if (path.empty()) return HttpResponse::Error("no icon for that title", 404);

        std::ifstream in(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.empty()) return HttpResponse::Error("could not read icon", 500);

        HttpResponse resp;
        resp.status = 200;
        resp.content_type = "image/png";
        resp.body = std::move(data);
        return resp;
    });

    server->Route("POST", "/api/titles/launch", [](const HttpRequest &req) {
        std::string title_id = JsonExtractString(req.body, "title_id");
        if (title_id.empty()) return HttpResponse::Error("missing 'title_id'");

        std::string err;
        if (!LaunchTitle(title_id, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- Avatar changer (upload -> DDS build -> apply to profile cache) ------

    server->Route("GET", "/api/avatar/whoami", [](const HttpRequest &) {
        AvatarUser u = GetAvatarUser();
        return HttpResponse::Json(JsonWriter()
                                       .Bool("ok", u.ok)
                                       .Str("user_id_hex", u.user_id_hex)
                                       .Str("user_name", u.user_name)
                                       .Str("error", u.error)
                                       .Done());
    });

    server->UploadRoute("POST", "/api/avatar/upload", cfg.tmp_dir, 24 * kMiB, [cfg](const HttpRequest &req) {
        if (req.body_file.empty() || req.body_size == 0) return HttpResponse::Error("empty upload");
        std::ifstream in(req.body_file, std::ios::binary);
        std::string image_bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (image_bytes.empty()) return HttpResponse::Error("could not read uploaded image", 500);
        std::string err;
        if (!BuildAvatarWork(cfg, image_bytes, &err)) return HttpResponse::Error(err, 400);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("GET", "/api/avatar/preview", [cfg](const HttpRequest &req) {
        int size = std::atoi(req.Query("size", "440").c_str());
        std::string path = AvatarPreviewPath(cfg, size);
        if (path.empty()) return HttpResponse::Error("size must be 64, 128, 260 or 440");

        std::ifstream in(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.empty()) return HttpResponse::Error("no preview — upload an image first", 404);

        HttpResponse resp;
        resp.status = 200;
        resp.content_type = "image/png";
        resp.body = std::move(data);
        return resp;
    });

    server->Route("POST", "/api/avatar/apply", [cfg](const HttpRequest &) {
        AvatarUser u = GetAvatarUser();
        std::string err;
        if (!ApplyAvatarToUser(cfg, u, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Str("user_name", u.user_name).Done());
    });

    server->Route("POST", "/api/avatar/rename", [](const HttpRequest &req) {
        std::string name = JsonExtractString(req.body, "name");
        if (name.empty()) return HttpResponse::Error("missing 'name'");
        if (name.size() > 16) return HttpResponse::Error("PS5 online IDs cap at 16 characters");

        std::string err;
        if (!SetForegroundUserName(name, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Str("user_name", name).Done());
    });

    // Cached PSN profile picture for an arbitrary user_id_hex — used to show
    // whose save data is whose on the Saves tab, not just the foreground user.
    server->Route("GET", "/api/avatar/user-icon", [](const HttpRequest &req) {
        std::string hex = req.Query("hex");
        if (hex.empty()) return HttpResponse::Error("missing 'hex'");

        std::string path = ProfileIconPath(hex);
        if (path.empty()) return HttpResponse::Error("no cached picture for that user", 404);

        std::ifstream in(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.empty()) return HttpResponse::Error("could not read picture", 500);

        HttpResponse resp;
        resp.status = 200;
        resp.content_type = "image/png";
        resp.body = std::move(data);
        return resp;
    });

    // ---- Thermal (temps + fan threshold, see thermal.hpp) -------------------

    server->Route("GET", "/api/thermal", [](const HttpRequest &) {
        ThermalInfo t = GetThermalInfo();
        return HttpResponse::Json(JsonWriter()
                                       .Bool("ok", t.ok)
                                       .Int("cpu_c", t.cpu_c)
                                       .Int("soc_c", t.soc_c)
                                       .Int("m2_c", t.m2_c)
                                       .Int("hottest_c", t.hottest_c)
                                       .Int("fan_duty_raw", t.fan_duty_raw)
                                       .Int("fan_duty_percent", t.fan_duty_percent)
                                       .Int("threshold_c", t.threshold_c)
                                       .Int("min_c", t.min_c)
                                       .Int("max_c", t.max_c)
                                       .Int("warm_c", t.warm_c)
                                       .Int("hot_c", t.hot_c)
                                       .Done());
    });

    server->Route("POST", "/api/thermal/fan", [cfg](const HttpRequest &req) {
        int temp = JsonExtractInt(req.body, "temp_c", -1);
        if (temp < 0) return HttpResponse::Error("missing 'temp_c'");
        std::string err;
        if (!SetFanThreshold(cfg, temp, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Int("threshold_c", temp).Done());
    });

    server->Route("POST", "/api/thermal/fan/reset", [cfg](const HttpRequest &) {
        std::string err;
        if (!ResetFanToAutomatic(cfg, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- Save-data backup (raw encrypted image files, see savedata.hpp) -----

    server->Route("GET", "/api/saves", [cfg](const HttpRequest &) {
        // Cross-reference titles and the user service so the UI never shows raw
        // title/user IDs. Both lookups are cheap at this scale.
        std::map<std::string, TitleInfo> titles_by_id;
        for (auto &t : ListTitles(cfg)) titles_by_id[t.title_id] = t;

        std::map<std::string, std::string> user_names; // hex -> name, avoid re-querying per file
        std::vector<std::string> items;
        for (const auto &s : ListSaves()) {
            if (s.is_sealed_key) continue; // travels with its image, never picked on its own
            std::string title_name = s.title_name.empty() ? s.title_id : s.title_name;
            std::string platform = s.is_ps4 ? "PS4" : "PS5";
            auto tit = titles_by_id.find(s.title_id);
            if (tit != titles_by_id.end()) {
                if (!tit->second.name.empty() && tit->second.name != s.title_id) {
                    title_name = tit->second.name;
                }
                if (!tit->second.platform.empty()) platform = tit->second.platform;
            }

            auto uit = user_names.find(s.user_id_hex);
            if (uit == user_names.end()) {
                uint32_t uid = static_cast<uint32_t>(std::strtoul(s.user_id_hex.c_str(), nullptr, 16));
                std::string name = GetUserDisplayName(uid);
                uit = user_names.emplace(s.user_id_hex, name.empty() ? s.user_id_hex : name).first;
            }

            items.push_back(SaveEntryToJson(s, title_name, platform, uit->second));
        }
        return HttpResponse::Json(JsonWriter().Raw("saves", JsonWriter::Array(items)).Done());
    });

    server->Route("GET", "/api/saves/icon", [](const HttpRequest &req) {
        const std::string user_hex = req.Query("user_id_hex");
        const std::string title_id = req.Query("title_id");
        const std::string filename = req.Query("filename");
        if (user_hex.empty() || title_id.empty() || filename.empty()) {
            return HttpResponse::Error("missing save icon identity");
        }

        // SaveIconPath validates each segment against the two fixed metadata roots,
        // which avoids a full save rescan per image.
        std::string path = SaveIconPath(user_hex, title_id, filename);
        if (path.empty()) path = TitleIconPath(title_id);
        if (path.empty()) return HttpResponse::Error("no icon for that save", 404);

        std::ifstream in(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.empty()) return HttpResponse::Error("could not read save icon", 500);

        HttpResponse resp;
        resp.status = 200;
        resp.content_type = "image/png";
        resp.body = std::move(data);
        return resp;
    });

    // Backs up every save file for one user+title as a single zip — never
    // touches a path the client sent, only ones ListSaves() itself found.
    server->Route("POST", "/api/saves/download-zip", [cfg](const HttpRequest &req) {
        std::string user_hex = JsonExtractString(req.body, "user_id_hex");
        std::string title_id = JsonExtractString(req.body, "title_id");
        if (user_hex.empty() || title_id.empty()) return HttpResponse::Error("missing 'user_id_hex'/'title_id'");

        std::vector<std::string> paths;
        for (const auto &s : ListSaves()) {
            if (s.user_id_hex == user_hex && s.title_id == title_id) paths.push_back(s.path);
        }
        if (paths.empty()) return HttpResponse::Error("no save files for that user/title", 404);

        // Streamed off disk rather than held in memory — a save image is big enough
        // that buffering the whole archive can take the payload down.
        std::string staged = cfg.tmp_dir + "/save_export.zip";
        std::string err;
        if (!ZipSavesToFile(paths, staged, &err)) return HttpResponse::Error(err, 500);

        HttpResponse resp;
        resp.status = 200;
        resp.content_type = "application/zip";
        resp.body_file = staged;
        resp.remove_body_file = true;
        resp.extra_headers["Content-Disposition"] = "attachment; filename=\"" + title_id + "-save.zip\"";
        return resp;
    });

    // Takes a zip produced by download-zip or by a backup. The same guarded path a
    // restore uses, so it snapshots the current save and applies all files or none.
    server->UploadRoute("POST", "/api/saves/import", cfg.tmp_dir, 4 * kGiB, [cfg](const HttpRequest &req) {
        if (req.body_file.empty() || req.body_size == 0) return HttpResponse::Error("empty upload");

        // The connection deletes its own temp file when this returns, so move the
        // upload somewhere the background job owns before handing it over.
        std::string staged = cfg.tmp_dir + "/save_import.zip";
        std::error_code ec;
        fs::rename(req.body_file, staged, ec);
        if (ec) return HttpResponse::Error("could not stage the uploaded save", 500);

        std::string label = req.Query("name");
        if (label.empty()) label = "save.zip";
        std::string err;
        if (!StartSaveImportJob(cfg, staged, fs::path(label).filename().string(), &err)) {
            fs::remove(staged, ec);
            return HttpResponse::Error(err, 409);
        }
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/saves/delete", [cfg](const HttpRequest &req) {
        std::string user_hex = JsonExtractString(req.body, "user_id_hex");
        std::string title_id = JsonExtractString(req.body, "title_id");
        if (user_hex.empty() || title_id.empty()) return HttpResponse::Error("missing 'user_id_hex'/'title_id'");

        std::string err, code;
        if (!DeleteTitleSaves(cfg, user_hex, title_id, &err, &code)) {
            return HttpResponse::CodedError(code, err, 409);
        }
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- Native save auto-backup (see savedata.hpp) --------------------------

    server->Route("GET", "/api/saves/backup-config", [cfg](const HttpRequest &) {
        SaveBackupConfig c = GetSaveBackupConfig(cfg);
        return HttpResponse::Json(JsonWriter()
                                       .Bool("enabled", c.enabled)
                                       .Int("settle_seconds", c.settle_seconds)
                                       .Str("backup_root", c.backup_root)
                                       .Done());
    });

    server->Route("POST", "/api/saves/backup-config", [cfg](const HttpRequest &req) {
        SaveBackupConfig c;
        c.enabled = JsonExtractBool(req.body, "enabled", false);
        c.settle_seconds = JsonExtractInt(req.body, "settle_seconds", 15);
        c.backup_root = JsonExtractString(req.body, "backup_root");
        std::string err;
        if (!SetSaveBackupConfig(cfg, c, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("GET", "/api/saves/backups", [cfg](const HttpRequest &) {
        std::vector<std::string> items;
        for (const auto &b : ListSaveBackups(cfg)) {
            items.push_back(JsonWriter()
                                .Str("title_id", b.title_id)
                                .Str("title_name", b.title_name)
                                .Str("platform", b.platform)
                                .Str("filename", b.filename)
                                .Str("path", b.path)
                                .Int("size_bytes", static_cast<long long>(b.size_bytes))
                                .Int("created_unix", static_cast<long long>(b.created_unix))
                                .Done());
        }
        return HttpResponse::Json(JsonWriter().Raw("backups", JsonWriter::Array(items)).Done());
    });

    server->Route("POST", "/api/saves/backup-now", [cfg](const HttpRequest &req) {
        std::string title_id = JsonExtractString(req.body, "title_id");
        if (title_id.empty()) return HttpResponse::Error("missing 'title_id'");
        std::string err;
        if (!BackupTitleSaves(cfg, title_id, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/saves/backup-all", [cfg](const HttpRequest &) {
        std::string err;
        if (!StartBackupAllJob(cfg, &err)) return HttpResponse::Error(err, 409);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("GET", "/api/saves/backup-all/status", [](const HttpRequest &) {
        SaveBackupAllStatus s = GetSaveBackupAllStatus();
        return HttpResponse::Json(JsonWriter()
                                       .Bool("running", s.running)
                                       .Int("done", s.done)
                                       .Int("total", s.total)
                                       .Int("failed", s.failed)
                                       .Str("current_title_id", s.current_title_id)
                                       .Done());
    });

    server->Route("GET", "/api/saves/backups/download", [cfg](const HttpRequest &req) {
        std::string path = req.Query("path");
        if (!IsSaveBackupPath(cfg, path)) return HttpResponse::Error("backup not found", 404);
        HttpResponse resp;
        resp.status = 200;
        resp.content_type = "application/zip";
        resp.body_file = path;
        resp.extra_headers["Content-Disposition"] = "attachment; filename=\"" + fs::path(path).filename().string() + "\"";
        return resp;
    });

    server->Route("POST", "/api/saves/backups/restore", [cfg](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        if (!IsSaveBackupPath(cfg, path)) return HttpResponse::Error("backup not found", 404);
        // Backgrounded like import: a restore copies the save twice and would
        // otherwise hold the request open for the whole thing.
        bool snapshot_first = JsonExtractString(req.body, "snapshot_first") != "false";
        std::string label = fs::path(fs::path(path).parent_path()).filename().string();
        std::string err;
        if (!StartSaveRestoreJob(cfg, path, label, snapshot_first, &err)) {
            return HttpResponse::Error(err, 409);
        }
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/saves/backups/delete", [cfg](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        if (!IsSaveBackupPath(cfg, path)) return HttpResponse::Error("backup not found", 404);
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) return HttpResponse::Error("could not delete backup: " + ec.message(), 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- PKG installer (see pkg_installer.hpp for what this can and can't do) --

    // Both routes just start a background job; progress comes from GET /api/pkg/status.
    // The install phase is indeterminate — AppInstUtil has no progress callback.

    // A PC->PS5 install streams over its own WebSocket port (pkg_ws_upload.hpp), not
    // through this server — started here since cfg is on hand for the job it kicks off.
    StartPkgWsUpload(cfg);

    server->Route("POST", "/api/pkg/install", [cfg](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        std::string url = JsonExtractString(req.body, "url");
        if (path.empty() && url.empty()) return HttpResponse::Error("missing 'path' or 'url'");

        std::string err;
        if (!StartPkgInstallJob(cfg, url, path, &err)) return HttpResponse::Error(err, 409);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Bool("started", true).Done());
    });

    // Best-effort teardown for a live (PC->PS5) upload — used by the page unload beacon
    // when the browser tab closes mid-transfer, and by an in-app Cancel action.
    server->Route("POST", "/api/pkg/direct/cancel", [](const HttpRequest &) {
        CancelLivePkgInstall();
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("GET", "/api/pkg/status", [](const HttpRequest &) {
        PkgJobStatus s = GetPkgJobStatus();
        const char *phase = s.phase == PkgJobPhase::kIdle         ? "idle"
                             : s.phase == PkgJobPhase::kDownloading ? "downloading"
                             : s.phase == PkgJobPhase::kInstalling  ? "installing"
                             : s.phase == PkgJobPhase::kQueued      ? "queued"
                                                                     : "error";
        return HttpResponse::Json(JsonWriter()
                                       .Str("phase", phase)
                                       .Int("loaded", s.loaded)
                                       .Int("total", s.total)
                                       .Int("speed_bps", static_cast<long long>(s.speed_bps))
                                       .Str("content_id", s.content_id)
                                       .Str("title", s.title)
                                       .Str("error", s.error)
                                       .Bool("has_staged_file", !s.staged_path.empty())
                                       .Done());
    });

    // Deletes the downloaded/uploaded .pkg for the most recent job — user-triggered only,
    // since AppInstUtil may still be reading it in the background right after "queued".
    server->Route("POST", "/api/pkg/delete-download", [cfg](const HttpRequest &) {
        std::string err;
        if (!DeleteStagedPkgFile(cfg, &err)) return HttpResponse::Error(err, 400);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- Payload registry / manual launch --------------------------------

    server->Route("GET", "/api/payloads", [registry](const HttpRequest &) {
        std::vector<std::string> items;
        for (const auto &p : registry->List()) items.push_back(PayloadToJson(p));
        return HttpResponse::Json(JsonWriter().Raw("payloads", JsonWriter::Array(items)).Done());
    });

    server->Route("POST", "/api/payloads/launch", [registry](const HttpRequest &req) {
        std::string name = JsonExtractString(req.body, "name");
        if (name.empty()) return HttpResponse::Error("missing 'name'");

        std::string err;
        if (!registry->Launch(name, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Str("launched", name).Done());
    });

    server->Route("POST", "/api/payloads/delete", [registry, cfg](const HttpRequest &req) {
        std::string name = JsonExtractString(req.body, "name");
        if (name.empty()) return HttpResponse::Error("missing 'name'");

        std::string err;
        if (!registry->Delete(name, &err)) return HttpResponse::Error(err, 500);
        std::string autoloader_err;
        bool cleaned = RemovePayloadFromAutoloaderConfig(cfg, name, &autoloader_err);
        if (!cleaned) LogEvent("Payload deleted, but its autoloader entry could not be removed: " + autoloader_err);
        return HttpResponse::Json(JsonWriter()
                                      .Bool("ok", true)
                                      .Bool("autoloader_updated", cleaned)
                                      .Str("warning", cleaned ? "" : autoloader_err)
                                      .Done());
    });

    server->Route("POST", "/api/payloads/enable", [registry](const HttpRequest &req) {
        std::string name = JsonExtractString(req.body, "name");
        bool enabled = JsonExtractBool(req.body, "enabled", true);
        if (name.empty()) return HttpResponse::Error("missing 'name'");

        std::string err;
        if (!registry->SetEnabled(name, enabled, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Bool("enabled", enabled).Done());
    });

    server->Route("POST", "/api/payloads/favorite", [registry](const HttpRequest &req) {
        std::string name = JsonExtractString(req.body, "name");
        bool favorite = JsonExtractBool(req.body, "favorite", true);
        if (name.empty()) return HttpResponse::Error("missing 'name'");
        std::string err;
        if (!registry->SetFavorite(name, favorite, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Bool("favorite", favorite).Done());
    });

    server->Route("POST", "/api/payloads/rename", [registry, cfg](const HttpRequest &req) {
        std::string name = JsonExtractString(req.body, "name");
        std::string new_name = JsonExtractString(req.body, "new_name");
        if (name.empty() || new_name.empty()) return HttpResponse::Error("missing 'name'/'new_name'");
        std::string err;
        if (!registry->Rename(name, new_name, &err)) return HttpResponse::Error(err, 409);
        std::string autoloader_error;
        bool updated = RenamePayloadInAutoloaderConfig(cfg, name, new_name, &autoloader_error);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Bool("autoloader_updated", updated)
                                      .Str("warning", updated ? "" : autoloader_error).Done());
    });

    // Drag-and-drop import — writes straight into payloads_dir, no FTP hop needed.
    server->UploadRoute("POST", "/api/payloads/upload", cfg.tmp_dir, 512 * kMiB,
                        [registry](const HttpRequest &req) {
        std::string name = req.Query("name");
        if (name.empty()) return HttpResponse::Error("missing 'name' query parameter");
        if (req.body_file.empty() || req.body_size == 0) return HttpResponse::Error("empty upload");

        std::string err;
        if (!registry->ImportFile(name, req.body_file, "uploaded", &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Str("name", name).Done());
    });

    // Import an ELF already present anywhere on the console (USB included)
    // into the managed payload library without routing it through a browser.
    server->Route("POST", "/api/payloads/import-path", [registry](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        if (path.empty() || path.front() != '/') return HttpResponse::Error("missing or invalid 'path'");
        std::string name = fs::path(path).filename().string();
        std::string err;
        if (!registry->ImportFile(name, path, "usb", &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Str("name", name).Done());
    });

    // Direct-URL import, e.g. a GitHub release asset — no repo index needed.
    // Bodyless query route starts a background download and returns immediately.
    server->Route("GET", "/api/payloads/download", [cfg, registry](const HttpRequest &req) {
        std::string url = req.Query("url");
        std::string version = req.Query("version"); // e.g. a picked release tag — empty is fine
        std::string repo_url = req.Query("repo_url"); // set when downloaded from a catalog entry
        bool launch = req.Query("launch") == "1";
        if (url.empty()) return HttpResponse::Error("missing 'url'");

        std::string err;
        if (!StartPayloadDownloadJob(cfg, url, "", "", version, launch, registry, &err, repo_url)) {
            return HttpResponse::Error(err, 409);
        }
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Bool("started", true).Done());
    });

    server->Route("GET", "/api/payloads/download/status", [](const HttpRequest &) {
        DownloadJobStatus s = GetDownloadJobStatus();
        const char *phase = s.phase == DownloadJobPhase::kIdle         ? "idle"
                            : s.phase == DownloadJobPhase::kDownloading ? "downloading"
                            : s.phase == DownloadJobPhase::kInstalling  ? "installing"
                            : s.phase == DownloadJobPhase::kDone        ? "done"
                                                                         : "error";
        return HttpResponse::Json(JsonWriter()
                                       .Str("phase", phase)
                                       .Int("loaded", s.loaded)
                                       .Int("total", s.total)
                                       .Int("speed_bps", static_cast<long long>(s.speed_bps))
                                       .Str("filename", s.filename)
                                       .Bool("launched", s.launched)
                                       .Str("error", s.error)
                                       .Done());
    });

    // ---- Autoloader --------------------------------------------------------

    server->Route("GET", "/api/autoloader/status", [autoloader](const HttpRequest &) {
        AutoloaderStatus s = autoloader->GetStatus();
        return HttpResponse::Json(JsonWriter()
                                       .Bool("enabled", s.enabled)
                                       .Bool("running", s.running)
                                       .Int("remaining_seconds", s.remaining_seconds)
                                       .Int("total", s.total)
                                       .Int("done", s.done)
                                       .Int("failed", s.failed)
                                       .Str("current", s.current)
                                       .Str("last_error", s.last_error)
                                       .Bool("already_loaded", s.already_loaded)
                                       .Int("skipped_count", s.skipped_count)
                                       .Done());
    });

    server->Route("POST", "/api/autoloader/abort", [autoloader](const HttpRequest &) {
        autoloader->Abort();
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/autoloader/run", [autoloader](const HttpRequest &) {
        std::string err;
        if (!autoloader->RunNow(&err)) return HttpResponse::Error(err, 409);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // Separate from the autoloader's own enabled flag: this only skips bundled
    // payloads, leaving the user's own steps untouched.
    server->Route("GET", "/api/autoloader/bundled-autostart", [cfg](const HttpRequest &) {
        return HttpResponse::Json(JsonWriter().Bool("enabled", GetBundledAutostart(cfg)).Done());
    });

    server->Route("POST", "/api/autoloader/bundled-autostart", [cfg](const HttpRequest &req) {
        bool enabled = JsonExtractBool(req.body, "enabled", true);
        std::string err;
        if (!SetBundledAutostart(cfg, enabled, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Bool("enabled", enabled).Done());
    });

    // ---- Native file manager ------------------------------------------------

    server->Route("GET", "/api/ftp/status", [](const HttpRequest &) {
        std::vector<FileManagerEntry> probe;
        std::string err;
        bool ready = ListNativeDirectory("/", &probe, &err);
        return HttpResponse::Json(JsonWriter().Bool("reachable", ready).Str("mode", "native").Str("error", err).Done());
    });

    server->Route("GET", "/api/ftp/list", [](const HttpRequest &req) {
        std::string path = req.Query("path", "/");

        auto lease = std::make_shared<FileReadLease>(path);
        if (!*lease) return HttpResponse::Error(lease->Error(), 409);
        std::string err;
        std::vector<FileManagerEntry> entries;
        if (!ListNativeDirectory(path, &entries, &err)) return HttpResponse::Error(err, 500);

        std::vector<std::string> items;
        for (const auto &e : entries) items.push_back(FileEntryToJson(e));
        return HttpResponse::Json(
            JsonWriter().Str("path", path).Raw("entries", JsonWriter::Array(items)).Done());
    });

    server->Route("POST", "/api/ftp/conflicts", [](const HttpRequest &req) {
        std::vector<std::string> paths = JsonExtractStringArray(req.body, "paths");
        if (paths.empty()) return HttpResponse::Json(JsonWriter().Raw("paths", "[]").Done());
        auto external = std::find_if(paths.begin(), paths.end(), IsExternalStoragePath);
        auto lease = std::make_shared<FileReadLease>(external == paths.end() ? paths.front() : *external);
        if (!*lease) return HttpResponse::Error(lease->Error(), 409);

        std::vector<std::string> existing;
        for (const auto &path : paths) {
            bool parent_traversal = false;
            for (const auto &part : fs::path(path)) if (part == "..") parent_traversal = true;
            if (path.empty() || path.front() != '/' || parent_traversal || IsKernelUnsafePath(path)) {
                return HttpResponse::Error("invalid conflict-check path");
            }
            struct stat st {};
            if (lstat(path.c_str(), &st) == 0) existing.push_back(JsonWriter::Quote(path));
            else if (errno != ENOENT) return HttpResponse::Error("could not inspect upload destination", 500);
        }
        return HttpResponse::Json(JsonWriter().Raw("paths", JsonWriter::Array(existing)).Done());
    });

    // Backs the upload conflict dialog's "Keep both" option — same renaming
    // UniqueSiblingName() already does for copy/move/extract collisions.
    server->Route("POST", "/api/ftp/unique-names", [](const HttpRequest &req) {
        std::vector<std::string> paths = JsonExtractStringArray(req.body, "paths");
        if (paths.empty()) return HttpResponse::Json(JsonWriter().Raw("paths", "[]").Done());

        std::vector<std::string> renamed;
        for (const auto &path : paths) {
            bool parent_traversal = false;
            for (const auto &part : fs::path(path)) if (part == "..") parent_traversal = true;
            if (path.empty() || path.front() != '/' || parent_traversal || IsKernelUnsafePath(path)) {
                return HttpResponse::Error("invalid path");
            }
            std::string unique = UniqueSiblingPath(path);
            if (unique.empty()) return HttpResponse::Error("too many name collisions: " + path, 409);
            renamed.push_back(JsonWriter::Quote(unique));
        }
        return HttpResponse::Json(JsonWriter().Raw("paths", JsonWriter::Array(renamed)).Done());
    });

    server->Route("GET", "/api/ftp/dirsize", [](const HttpRequest &req) {
        std::string path = req.Query("path");
        if (path.empty()) return HttpResponse::Error("missing 'path'");
        auto lease = std::make_shared<FileReadLease>(path);
        if (!*lease) return HttpResponse::Error(lease->Error(), 409);

        uint64_t size = 0;
        std::string err;
        errno = 0;
        if (!NativeDirectorySize(path, &size, &err)) return HttpResponse::Error(err, errno == EBUSY ? 409 : 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Int("size", static_cast<long long>(size)).Done());
    });

    server->Route("POST", "/api/ftp/mkdir", [](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        if (path.empty()) return HttpResponse::Error("missing 'path'");

        std::string err;
        ImmediateFileMutationGuard mutation(&err);
        if (!mutation) return HttpResponse::Error(err, 409);
        bool ok = CreateNativeDirectory(path, &err);
        if (!ok) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/ftp/delete", [](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        if (path.empty()) return HttpResponse::Error("missing 'path'");

        uint64_t id = 0;
        std::string err;
        if (!QueueDeleteJob(path, &id, &err)) return HttpResponse::Error(err, 409);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Int("job_id", static_cast<long long>(id)).Done());
    });

    server->Route("POST", "/api/ftp/chmod", [](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        std::string mode_text = JsonExtractString(req.body, "mode");
        if (path.empty() || mode_text.empty()) return HttpResponse::Error("missing 'path' or 'mode'");
        char *end = nullptr;
        errno = 0;
        unsigned long mode = std::strtoul(mode_text.c_str(), &end, 8);
        if (errno || !end || *end != '\0' || mode > 07777) return HttpResponse::Error("invalid octal permissions");

        std::string err;
        ImmediateFileMutationGuard mutation(&err);
        if (!mutation) return HttpResponse::Error(err, 409);
        if (!SetNativePermissions(path, static_cast<uint32_t>(mode), &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/ftp/rename", [](const HttpRequest &req) {
        std::string from = JsonExtractString(req.body, "from");
        std::string to = JsonExtractString(req.body, "to");
        if (from.empty() || to.empty()) return HttpResponse::Error("missing 'from'/'to'");

        std::string err;
        ImmediateFileMutationGuard mutation(&err);
        if (!mutation) return HttpResponse::Error(err, 409);
        bool ok = RenameNativePath(from, to, &err);
        if (!ok) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("GET", "/api/ftp/storage", [](const HttpRequest &) {
        std::vector<std::string> items;
        for (const auto &volume : ListStorageVolumes()) {
            items.push_back(JsonWriter()
                                .Str("label", volume.label)
                                .Str("kind", volume.kind)
                                .Int("index", volume.index)
                                .Str("path", volume.path)
                                .Int("total", static_cast<long long>(volume.total_bytes))
                                .Int("free", static_cast<long long>(volume.free_bytes))
                                .Done());
        }
        return HttpResponse::Json(JsonWriter().Raw("volumes", JsonWriter::Array(items)).Done());
    });

    // `path` is normally the file to be written, so its parent is checked. Pass
    // is_dir=1 when path is the destination itself, which matters at a mount boundary.
    server->Route("GET", "/api/ftp/preflight", [](const HttpRequest &req) {
        std::string path = req.Query("path");
        std::string raw_size = req.Query("size");
        bool is_dir = req.Query("is_dir") == "1";
        if (path.empty() || raw_size.empty()) return HttpResponse::Error("missing path/size");
        auto lease = std::make_shared<FileReadLease>(path);
        if (!*lease) return HttpResponse::Error(lease->Error(), 409);
        char *end = nullptr;
        errno = 0;
        unsigned long long size = std::strtoull(raw_size.c_str(), &end, 10);
        if (errno || !end || *end != '\0') return HttpResponse::Error("invalid upload size");
        uint64_t available = 0;
        std::string err;
        bool ok = is_dir ? CheckFreeSpaceAtDirectory(path, size, &available, &err)
                          : CheckUploadSpace(path, size, &available, &err);
        if (!ok) {
            return HttpResponse::Json(JsonWriter().Bool("ok", false).Int("available", available).Str("error", err).Done(), 507);
        }
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Int("available", available).Done());
    });

    server->Route("POST", "/api/ftp/zip", [](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        std::string compression = JsonExtractString(req.body, "compression");
        if (path.empty()) return HttpResponse::Error("missing 'path'");
        if (compression.empty()) compression = "smart";

        fs::path source(path);
        std::string destination = (source.parent_path() / (source.filename().string() + ".zip")).string();
        uint64_t id = 0;
        std::string err;
        if (!QueueZipJob({path}, destination, compression, &id, &err)) return HttpResponse::Error(err, 409);
        return HttpResponse::Json(JsonWriter()
                                      .Bool("ok", true)
                                      .Int("job_id", static_cast<long long>(id))
                                      .Str("zip_name", fs::path(destination).filename().string())
                                      .Done());
    });

    // One archive from several items, each keeping its top-level name so siblings
    // can't collide. `dir` is where it lands, `name` is the archive's filename.
    server->Route("POST", "/api/ftp/zip-selection", [](const HttpRequest &req) {
        std::vector<std::string> paths = JsonExtractStringArray(req.body, "paths");
        std::string dir = JsonExtractString(req.body, "dir");
        std::string name = JsonExtractString(req.body, "name");
        std::string compression = JsonExtractString(req.body, "compression");
        if (paths.empty() || dir.empty() || name.empty()) return HttpResponse::Error("missing 'paths', 'dir', or 'name'");
        if (compression.empty()) compression = "smart";
        if (!IsZipFilename(name)) name += ".zip";

        std::string destination = (fs::path(dir) / name).string();
        uint64_t id = 0;
        std::string err;
        if (!QueueZipJob(paths, destination, compression, &id, &err)) return HttpResponse::Error(err, 409);
        return HttpResponse::Json(JsonWriter()
                                      .Bool("ok", true)
                                      .Int("job_id", static_cast<long long>(id))
                                      .Str("zip_name", name)
                                      .Done());
    });

    // Temporary "Download as ZIP", written under Prospero's temp directory.
    // /api/ftp/download unlinks it as soon as it opens it for streaming.
    server->Route("POST", "/api/ftp/download-zip", [](const HttpRequest &req) {
        std::vector<std::string> paths = JsonExtractStringArray(req.body, "paths");
        std::string name = JsonExtractString(req.body, "name");
        if (paths.empty() || name.empty()) return HttpResponse::Error("missing 'paths' or 'name'");
        if (!IsZipFilename(name)) name += ".zip";

        uint64_t id = 0;
        std::string download_path, err;
        if (!QueueDownloadZipJob(paths, name, &id, &download_path, &err)) return HttpResponse::Error(err, 409);
        return HttpResponse::Json(JsonWriter()
                                      .Bool("ok", true)
                                      .Int("job_id", static_cast<long long>(id))
                                      .Str("download_path", download_path)
                                      .Done());
    });

    // Reads only the ZIP central directory (never decompresses) — backs both
    // the Extract Archive dialog's stats and the Open Archive inspector.
    server->Route("GET", "/api/ftp/archive-info", [](const HttpRequest &req) {
        std::string path = req.Query("path");
        if (path.empty() || !(IsZipFilename(path) || IsRarFilename(path))) {
            return HttpResponse::Error("missing 'path' or not a .zip/.rar file");
        }
        auto lease = std::make_shared<FileReadLease>(path);
        if (!*lease) return HttpResponse::Error(lease->Error(), 409);

        ArchiveInfo info;
        std::string err, code;
        if (!ReadArchiveInfo(path, req.Query("password"), &info, &err, &code)) {
            return code.empty() ? HttpResponse::Error(err, 500) : HttpResponse::CodedError(code, err, 409);
        }

        std::vector<std::string> entries;
        for (const auto &e : info.entries) entries.push_back(ArchiveEntryToJson(e));
        return HttpResponse::Json(JsonWriter()
                                      .Bool("ok", true)
                                      .Int("items", static_cast<long long>(info.items))
                                      .Int("compressed_size", static_cast<long long>(info.compressed_size))
                                      .Int("uncompressed_size", static_cast<long long>(info.uncompressed_size))
                                      .Bool("entries_truncated", info.entries_truncated)
                                      .Raw("entries", JsonWriter::Array(entries))
                                      .Done());
    });

    server->Route("POST", "/api/ftp/copy", [](const HttpRequest &req) {
        std::string source = JsonExtractString(req.body, "source");
        std::string destination = JsonExtractString(req.body, "destination");
        std::string conflict_policy = JsonExtractString(req.body, "conflict_policy");
        if (conflict_policy.empty()) conflict_policy = "cancel";
        uint64_t id = 0;
        std::string err;
        if (!QueueFileJob("copy", source, destination, conflict_policy, &id, &err)) return HttpResponse::Error(err, 409);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Int("job_id", static_cast<long long>(id)).Done());
    });

    server->Route("POST", "/api/ftp/move", [](const HttpRequest &req) {
        std::string source = JsonExtractString(req.body, "source");
        std::string destination = JsonExtractString(req.body, "destination");
        std::string conflict_policy = JsonExtractString(req.body, "conflict_policy");
        if (conflict_policy.empty()) conflict_policy = "cancel";
        uint64_t id = 0;
        std::string err;
        if (!QueueFileJob("move", source, destination, conflict_policy, &id, &err)) return HttpResponse::Error(err, 409);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Int("job_id", static_cast<long long>(id)).Done());
    });

    server->Route("GET", "/api/files/jobs", [](const HttpRequest &) {
        std::vector<std::string> items;
        for (const auto &job : GetFileJobs()) items.push_back(FileJobToJson(job));
        return HttpResponse::Json(JsonWriter().Raw("jobs", JsonWriter::Array(items)).Done());
    });

    server->Route("POST", "/api/files/jobs/cancel", [](const HttpRequest &req) {
        int id = JsonExtractInt(req.body, "id", 0);
        std::string err;
        if (id <= 0 || !CancelFileJob(static_cast<uint64_t>(id), &err)) {
            return HttpResponse::Error(err.empty() ? "invalid job id" : err, 409);
        }
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/files/jobs/clear", [](const HttpRequest &) {
        ClearFinishedFileJobs();
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/ftp/unzip", [](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        std::string destination = JsonExtractString(req.body, "destination");
        std::string conflict_policy = JsonExtractString(req.body, "conflict_policy");
        std::string password = JsonExtractString(req.body, "password");
        bool delete_source = JsonExtractBool(req.body, "delete_source", false);
        if (path.empty() || !(IsZipFilename(path) || IsRarFilename(path))) {
            return HttpResponse::Error("missing 'path' or not a .zip/.rar file");
        }
        if (conflict_policy.empty()) conflict_policy = "skip";

        fs::path source(path);
        // Default target keeps the friendly "archive.zip" -> "archive/" behaviour
        // unless the dialog picked a destination.
        std::string folder = source.filename().string();
        folder = folder.substr(0, folder.size() - 4);
        if (destination.empty()) destination = (source.parent_path() / folder).string();
        uint64_t id = 0;
        std::string err;
        if (!QueueUnzipJob(path, destination, conflict_policy, delete_source, password, &id, &err)) {
            return HttpResponse::Error(err, 409);
        }
        return HttpResponse::Json(JsonWriter()
                                      .Bool("ok", true)
                                      .Int("job_id", static_cast<long long>(id))
                                      .Str("folder", fs::path(destination).filename().string())
                                      .Done());
    });

    // Returns the text with the metadata needed for a conflict-safe save.
    server->Route("GET", "/api/ftp/text", [](const HttpRequest &req) {
        std::string path = req.Query("path");
        if (path.empty()) return HttpResponse::Error("missing 'path'");

        auto lease = std::make_shared<FileReadLease>(path);
        if (!*lease) return HttpResponse::Error(lease->Error(), 409);
        struct stat st {};
        if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            return HttpResponse::Error("file does not exist or is not a regular file", 404);
        }
        constexpr uint64_t kMaxEditableBytes = 4 * kMiB;
        if (static_cast<uint64_t>(st.st_size) > kMaxEditableBytes) {
            return HttpResponse::Error("file is too large to edit here (over 4 MiB) — download it instead", 413);
        }

        std::ifstream in(path, std::ios::binary);
        if (!in) return HttpResponse::Error("could not open file", 500);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        struct stat after_read {};
        if (lstat(path.c_str(), &after_read) != 0 || FileVersion(after_read) != FileVersion(st)) {
            return HttpResponse::Error("file changed while it was being read; reload it", 409);
        }
        bool bom = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                   static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF;
        if (bom) text.erase(0, 3);
        if (!ValidUtf8(text)) return HttpResponse::Error("file is not valid UTF-8 text", 415);
        std::string newline = text.find("\r\n") != std::string::npos ? "crlf" :
                              text.find('\r') != std::string::npos ? "cr" : "lf";
        return HttpResponse::Json(JsonWriter()
                                      .Str("text", text)
                                      .Str("version", FileVersion(st))
                                      .Str("newline", newline)
                                      .Bool("bom", bom)
                                      .Done());
    });

    server->DirectUploadRoute("POST", "/api/ftp/text/write", "path", 5 * kMiB, [](const HttpRequest &req) {
        std::string path = req.Query("path");
        std::string version = req.Query("version");
        if (path.empty() || version.empty() || req.body_file.empty()) {
            return HttpResponse::Error("missing text save parameters");
        }

        struct stat current {};
        if (lstat(path.c_str(), &current) != 0 || !S_ISREG(current.st_mode)) {
            return HttpResponse::Error("file no longer exists or is not a regular file", 409);
        }
        if (FileVersion(current) != version) {
            return HttpResponse::Error("file changed on disk; reload it before saving", 409);
        }

        struct stat staged {};
        if (lstat(req.body_file.c_str(), &staged) != 0 || !S_ISREG(staged.st_mode) || staged.st_size < 0 ||
            static_cast<uint64_t>(staged.st_size) > 4 * kMiB + 3) {
            return HttpResponse::Error("invalid text upload", 413);
        }
        std::ifstream in(req.body_file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bool bom = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                   static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF;
        if (!ValidUtf8(bom ? text.substr(3) : text)) return HttpResponse::Error("saved text is not valid UTF-8", 415);
        if (chmod(req.body_file.c_str(), current.st_mode & 07777) != 0) {
            return HttpResponse::Error(std::string("could not preserve file permissions: ") + std::strerror(errno), 500);
        }
        if (rename(req.body_file.c_str(), path.c_str()) != 0) {
            return HttpResponse::Error(std::string("could not commit text file: ") + std::strerror(errno), 500);
        }
        struct stat saved {};
        if (stat(path.c_str(), &saved) != 0) return HttpResponse::Error("could not verify saved file", 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Str("version", FileVersion(saved)).Done());
    });

    server->Route("GET", "/api/ftp/download", [cfg](const HttpRequest &req) {
        std::string path = req.Query("path");
        if (path.empty()) return HttpResponse::Error("missing 'path'");

        auto lease = std::make_shared<FileReadLease>(path);
        if (!*lease) return HttpResponse::Error(lease->Error(), 409);
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            return HttpResponse::Error("file does not exist or is not a regular file", 404);
        }
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return HttpResponse::Error("could not open file", 500);
        // VLC may seek to moov at the END of an MP4. Preserve the HTTP Range.
        const uint64_t total = static_cast<uint64_t>(st.st_size);
        uint64_t start = 0, count = total;
        const bool partial = !req.range.empty();
        disc_range::Result range;
        if (partial) {
            range = disc_range::parse(req.range, total);
            if (!range.valid) {
                close(fd);
                HttpResponse invalid = HttpResponse::Error("unsatisfiable byte range", 416);
                invalid.extra_headers["Content-Range"] = "bytes */" + std::to_string(total);
                invalid.extra_headers["Accept-Ranges"] = "bytes";
                return invalid;
            }
            start = range.first;
            count = range.last - range.first + 1;
        }
        // A range request is random access, not sequential read-ahead.
        (void)posix_fadvise(fd, static_cast<off_t>(start), static_cast<off_t>(count),
                            partial ? POSIX_FADV_RANDOM : POSIX_FADV_SEQUENTIAL);

        // Unlink now, with the fd already open: the stream keeps working and the
        // temp archive can't linger however the transfer ends.
        std::string ephemeral_root = cfg.tmp_dir + "/archives/";
        if (path.compare(0, ephemeral_root.size(), ephemeral_root) == 0) std::remove(path.c_str());

        std::string filename = fs::path(path).filename().string();
        HttpResponse resp;
        resp.status = partial ? 206 : 200;
        resp.content_type = GuessContentType(filename);
        // Stream only the requested extent; libmicrohttpd supplies a relative
        // offset, FileReadStream adds the file base to the pread() call.
        resp.body_stream_size = count;
        resp.body_stream = std::make_shared<FileReadStream>(fd, lease, start, count);
        resp.extra_headers["Content-Disposition"] = ContentDisposition(filename);
        resp.extra_headers["Accept-Ranges"] = "bytes";
        if (partial)
            resp.extra_headers["Content-Range"] = "bytes " + std::to_string(start) + "-" +
                                                  std::to_string(range.last) + "/" +
                                                  std::to_string(total);
        return resp;
    });

    server->DirectUploadRoute("POST", "/api/ftp/upload", "path", 256 * kGiB, [](const HttpRequest &req) {
        std::string path = req.Query("path");
        bool overwrite = req.Query("overwrite") == "1";
        if (path.empty()) return HttpResponse::Error("missing 'path' query parameter");
        if (req.body_file.empty()) return HttpResponse::Error("empty upload");

        struct stat existing {};
        if (req.body_file != path && lstat(path.c_str(), &existing) == 0) {
            if (!overwrite) return HttpResponse::Error("destination already exists", 409);
            if (!S_ISREG(existing.st_mode)) return HttpResponse::Error("destination is not a regular file", 409);
        } else if (req.body_file != path && errno != 0 && errno != ENOENT) {
            return HttpResponse::Error(std::string("could not inspect upload destination: ") + std::strerror(errno), 500);
        }

        // The body was written to a synced sibling of `path` and dispatched only once
        // complete, so this rename can never expose a partial upload.
        if (rename(req.body_file.c_str(), path.c_str()) != 0) {
            return HttpResponse::Error(std::string("could not commit upload: ") + std::strerror(errno), 500);
        }
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Str("path", path).Done());
    });

    // Large files append straight into their checkpoint — no staging copy, so each
    // byte hits PFS once and a dropped connection only retries the current chunk.
    server->AppendUploadRoute("POST", "/api/ftp/upload/chunk", "path", "upload_id", "offset", "total",
                              264 * kMiB, [](const HttpRequest &req) {
        std::string path = req.Query("path");
        std::string upload_id = req.Query("upload_id");
        bool overwrite = req.Query("overwrite") == "1";
        uint64_t offset = 0, total = 0;
        if (path.empty() || !SafeUploadId(upload_id) ||
            !ParseUint64(req.Query("offset"), &offset) || !ParseUint64(req.Query("total"), &total)) {
            return HttpResponse::Error("invalid resumable-upload parameters");
        }
        if (total == 0 || total > 256 * kGiB || req.body_file.empty()) {
            return HttpResponse::Error("invalid resumable-upload size");
        }
        uint64_t next = offset + req.body_size;
        if (req.body_size == 0 || next > total) return HttpResponse::Error("invalid upload chunk size");
        struct stat checkpoint {};
        if (stat(req.body_file.c_str(), &checkpoint) != 0 || checkpoint.st_size < 0 ||
            static_cast<uint64_t>(checkpoint.st_size) != next) {
            return HttpResponse::Error("saved upload checkpoint has an unexpected size", 409);
        }
        bool complete = next == total;
        if (complete && req.body_file != path) {
            struct stat existing {};
            errno = 0;
            if (lstat(path.c_str(), &existing) == 0) {
                if (!overwrite || !S_ISREG(existing.st_mode)) {
                    std::remove(req.body_file.c_str());
                    ReleaseFileUpload(path, upload_id);
                    return HttpResponse::Error(!overwrite ? "destination already exists" :
                                               "destination is not a regular file", 409);
                }
            } else if (errno != ENOENT) {
                int inspect_error = errno;
                ReleaseFileUpload(path, upload_id);
                return HttpResponse::Error(std::string("could not inspect upload destination: ") +
                                               std::strerror(inspect_error), 500);
            }
            chmod(req.body_file.c_str(), 0777);
            if (rename(req.body_file.c_str(), path.c_str()) != 0) {
                int publish_error = errno;
                ReleaseFileUpload(path, upload_id);
                return HttpResponse::Error(std::string("could not publish completed upload: ") +
                                               std::strerror(publish_error), 500);
            }
        }
        if (complete) {
            ReleaseFileUpload(path, upload_id);
            LogEvent("Completed resumable upload: " + path + " (" + std::to_string(total) + " bytes)");
        }
        return HttpResponse::Json(JsonWriter()
                                      .Bool("ok", true)
                                      .Bool("complete", complete)
                                      .Int("next_offset", static_cast<long long>(next))
                                      .Done());
    });

    server->Route("GET", "/api/elfldr/status", [](const HttpRequest &) {
        ElfldrClient client(Config::kElfldrHost, Config::kElfldrPort);
        std::string err;
        bool reachable = client.Reachable(&err);
        return HttpResponse::Json(JsonWriter().Bool("reachable", reachable).Str("error", err).Done());
    });

    // ---- Payload Repositories (curated directory, version check only) -------
    // Browse-only: entries are compiled in and link out, never installed from here.

    server->Route("GET", "/api/catalog", [](const HttpRequest &req) {
        // Only ever refreshes when asked: a sweep is one upstream request per
        // entry, which alone exhausts GitHub's unauthenticated hourly budget.
        bool refresh = req.Query("refresh") == "1";
        std::string err;
        if (refresh && !StartCatalogRefreshJob(true, &err)) return HttpResponse::Error(err, 500);
        auto versions = GetCachedCatalogVersions();
        CatalogRefreshStatus status = GetCatalogRefreshStatus();

        std::vector<std::string> items;
        for (const auto &entry : GetPayloadCatalog()) {
            auto it = versions.find(entry.url);
            items.push_back(PayloadCatalogEntryToJson(entry, it != versions.end() ? it->second : CatalogVersionStatus{}));
        }
        std::string body = JsonWriter()
                               .Raw("entries", JsonWriter::Array(items))
                               .Bool("refreshing", status.running)
                               .Int("completed", status.completed)
                               .Int("total", status.total)
                               .Str("error", err)
                               .Done();
        return HttpResponse::Json(std::move(body));
    });

    // Track a repo for its latest release; still browse-only. The URL goes through
    // the same ParseRepositoryUrl the direct-URL install job uses.
    server->Route("POST", "/api/catalog/sources/add", [](const HttpRequest &req) {
        std::string url = JsonExtractString(req.body, "url");
        if (url.empty()) return HttpResponse::Error("missing 'url'");

        PayloadCatalogEntry entry;
        std::string err;
        if (!AddCustomRepository(url, &entry, &err)) return HttpResponse::Error(err, 409);

        // Just this one entry — adding a repository shouldn't re-check every
        // other one the user already has.
        CatalogVersionStatus status = RefreshCatalogEntry(entry.url);

        return HttpResponse::Json(JsonWriter()
                                       .Bool("ok", true)
                                       .Raw("entry", PayloadCatalogEntryToJson(entry, status))
                                       .Done());
    });

    server->Route("POST", "/api/catalog/sources/remove", [](const HttpRequest &req) {
        std::string url = JsonExtractString(req.body, "url");
        if (url.empty()) return HttpResponse::Error("missing 'url'");

        std::string err;
        if (!RemoveCustomRepository(url, &err)) return HttpResponse::Error(err, 404);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- Processes (sysctl KERN_PROC listing, SIGKILL) -----------------------

    server->Route("GET", "/api/processes", [](const HttpRequest &) {
        std::vector<std::string> items;
        for (const auto &p : ListProcesses()) items.push_back(ProcessToJson(p));
        return HttpResponse::Json(JsonWriter().Raw("processes", JsonWriter::Array(items)).Done());
    });

    server->Route("POST", "/api/processes/kill", [](const HttpRequest &req) {
        int pid = JsonExtractInt(req.body, "pid", 0);
        if (pid <= 0) return HttpResponse::Error("missing/invalid 'pid'");

        std::string err;
        if (!KillProcess(pid, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- Accounts / offline activation (see accounts.hpp) -------------------
    // High risk: each write backs up the slot first and needs a typed confirmation.

    server->Route("GET", "/api/accounts/offline-activation", [cfg](const HttpRequest &) {
        std::vector<std::string> items;
        for (const auto &a : ListAccountSlots(cfg)) items.push_back(AccountSlotToJson(a));
        return HttpResponse::Json(JsonWriter().Raw("slots", JsonWriter::Array(items)).Done());
    });

    server->Route("POST", "/api/accounts/offline-activation/apply", [cfg](const HttpRequest &req) {
        int slot = JsonExtractInt(req.body, "slot", 0);
        std::string account_id = JsonExtractString(req.body, "account_id_hex");
        std::string err;
        if (!ApplyOfflineActivation(cfg, slot, account_id, &err)) return HttpResponse::Error(err, 400);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/accounts/offline-activation/restore", [cfg](const HttpRequest &req) {
        int slot = JsonExtractInt(req.body, "slot", 0);
        std::string err;
        if (!RestoreAccountSlot(cfg, slot, &err)) return HttpResponse::Error(err, 400);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // The fuller sign-in flow — only works on the current foreground
    // user (see accounts.hpp). Sends a native toast asking for a reboot.
    server->Route("POST", "/api/accounts/offline-activation/sign-in", [cfg](const HttpRequest &req) {
        std::string account_id = JsonExtractString(req.body, "account_id_hex");
        std::string err;
        if (!ApplySignIn(cfg, account_id, &err)) return HttpResponse::Error(err, 400);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- Autoloader config editor --------------------------------------------

    server->Route("GET", "/api/autoloader/config", [cfg](const HttpRequest &) {
        AutoloaderConfig config = ReadAutoloaderConfig(cfg);

        std::vector<std::string> steps;
        for (const auto &s : config.sequence) {
            steps.push_back(JsonWriter().Str("type", s.type).Str("value", s.value).Done());
        }
        return HttpResponse::Json(JsonWriter()
                                       .Bool("enabled", config.enabled)
                                       .Int("delay", config.delay)
                                       .Int("settle_ms", config.settle_ms)
                                       .Int("retries", config.retries)
                                       .Bool("continue_on_error", config.continue_on_error)
                                       .Raw("sequence", JsonWriter::Array(steps))
                                       .Done());
    });

    server->Route("POST", "/api/autoloader/config", [cfg](const HttpRequest &req) {
        AutoloaderConfig config;
        config.enabled = JsonExtractBool(req.body, "enabled", false);
        config.delay = JsonExtractInt(req.body, "delay", 5);
        config.settle_ms = JsonExtractInt(req.body, "settle_ms", 500);
        config.retries = JsonExtractInt(req.body, "retries", 0);
        config.continue_on_error = JsonExtractBool(req.body, "continue_on_error", true);

        for (const auto &step_json : JsonExtractArrayItems(req.body, "sequence")) {
            if (config.sequence.size() >= 128) return HttpResponse::Error("autoloader sequence is too long");
            AutoloaderSequenceStep step;
            step.type = JsonExtractString(step_json, "type");
            step.value = JsonExtractString(step_json, "value");
            if (step.type != "load" && step.type != "delay") {
                return HttpResponse::Error("invalid autoloader step type");
            }
            if (step.value.empty()) return HttpResponse::Error("empty autoloader step");
            config.sequence.push_back(step);
        }

        std::string err;
        if (!WriteAutoloaderConfig(cfg, config, &err)) return HttpResponse::Error(err, 500);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    // ---- Experimental: ShadowMountPlus scan paths ---------------------------

    server->Route("GET", "/api/experimental/shadowmount/scan-paths", [](const HttpRequest &) {
        std::vector<std::string> paths = ReadShadowMountScanPaths();
        std::vector<std::string> items;
        for (const auto &p : paths) items.push_back(JsonWriter::Quote(p));
        return HttpResponse::Json(JsonWriter()
                                      .Bool("ok", true)
                                      .Bool("usingDefaults", paths.empty())
                                      .Raw("scanPaths", JsonWriter::Array(items))
                                      .Done());
    });

    server->Route("POST", "/api/experimental/shadowmount/scan-paths", [](const HttpRequest &req) {
        std::vector<std::string> paths = JsonExtractStringArray(req.body, "scan_paths");
        std::string err;
        if (!WriteShadowMountScanPaths(paths, &err)) return HttpResponse::Error(err, 400);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Bool("usingDefaults", paths.empty()).Done());
    });

    // ---- Payload Repositories: release list + download ----------------------
    // Opt-in per entry; reuses the Payloads tab's direct-URL install job.

    server->Route("GET", "/api/catalog/releases", [](const HttpRequest &req) {
        std::string url = req.Query("url");
        if (url.empty()) return HttpResponse::Error("missing 'url'");
        ReleaseListResult result = FetchReleases(url);
        if (result.state != "ok") return HttpResponse::Error(result.error, 502);

        std::vector<std::string> items;
        for (const auto &r : result.releases) {
            items.push_back(JsonWriter()
                                 .Str("tag", r.tag)
                                 .Str("date", r.published_at)
                                 .Str("asset", r.asset_name)
                                 .Str("assetUrl", r.asset_url)
                                 .Int("assetSize", static_cast<long long>(r.asset_size))
                                 .Bool("zipOnly", r.zip_only)
                                 .Bool("prerelease", r.prerelease)
                                 .Done());
        }
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Raw("releases", JsonWriter::Array(items)).Done());
    });

    server->Route("GET", "/api/catalog/refresh", [](const HttpRequest &req) {
        std::string url = req.Query("url");
        if (url.empty()) return HttpResponse::Error("missing 'url'");
        CatalogVersionStatus status = RefreshCatalogEntry(url);
        return HttpResponse::Json(JsonWriter()
                                       .Bool("ok", true)
                                       .Str("version", status.version)
                                       .Str("state", status.state)
                                       .Str("error", status.error)
                                       .Int("checked_at", status.checked_at_unix)
                                       .Done());
    });

    server->Route("GET", "/api/catalog/github-token", [](const HttpRequest &) {
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Bool("hasToken", HasGithubToken()).Done());
    });

    server->Route("POST", "/api/catalog/github-token", [](const HttpRequest &req) {
        std::string token = JsonExtractString(req.body, "token");
        std::string err;
        if (!SetGithubToken(token, &err)) return HttpResponse::Error(err, 400);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

    server->Route("POST", "/api/catalog/github-token/clear", [](const HttpRequest &) {
        ClearGithubToken();
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });

#ifdef PROSPEROMGR_DEBUG_NET
    // ---- Transfer-pipeline instrumentation (debug builds only) --------------
    // Built only with `make DEBUG_NET=1`, never in a normal build.
    LogEvent("[net] debug net-sink/net-source endpoints are ENABLED (DEBUG_NET build) — not for production use");

    server->CountingUploadRoute("POST", "/api/debug/net-sink", 64ull * kGiB, [](const HttpRequest &req) {
        double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - req.received_at).count();
        double mb_s = elapsed_s > 0 ? (static_cast<double>(req.body_size) / (1024.0 * 1024.0)) / elapsed_s : 0;
        LogEvent("[net] net-sink: " + std::to_string(req.body_size) + " bytes in " + std::to_string(elapsed_s) +
                 "s (" + std::to_string(mb_s) + " MB/s)");
        return HttpResponse::Json(JsonWriter()
                                      .Bool("ok", true)
                                      .Int("bytes", static_cast<long long>(req.body_size))
                                      .Done());
    });

    server->Route("GET", "/api/debug/net-source", [](const HttpRequest &req) {
        uint64_t size = 0;
        if (!ParseUint64(req.Query("size", "134217728"), &size) || size == 0) {
            return HttpResponse::Error("invalid 'size'");
        }
        size = std::min<uint64_t>(size, 64ull * kGiB);
        HttpResponse resp;
        resp.content_type = "application/octet-stream";
        resp.body_stream_size = size;
        resp.body_stream = std::make_shared<GeneratedStream>(size);
        return resp;
    });
#endif

    // ---- Embedded frontend --------------------------------------------------

    server->SetStaticFallback([](const std::string &url) {
        HttpResponse response = ServeEmbeddedAsset(url);
        // A redeployed ELF must never keep the PS5 browser on an older JS
        // bundle whose API method/body contract belongs to the previous build.
        response.extra_headers["Cache-Control"] = "no-store, no-cache, must-revalidate";
        response.extra_headers["Pragma"] = "no-cache";
        return response;
    });
}

} // namespace prospero
