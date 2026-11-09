#include "media.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "file_jobs.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "sqlite3.h"
#include "stb_image.h"
#include "stb_image_write.h"

namespace prospero {
namespace {

namespace fs = std::filesystem;

constexpr const char *kRoots[] = {"/user/av_contents", "/mnt/ext0/user/av_contents", "/mnt/ext1/user/av_contents"};
constexpr size_t kMaxItems = 5000;
constexpr int kMaxDepth = 6;
constexpr int kThumbSide = 560;
constexpr uint64_t kThumbMaxPixels = 40ull * 1000 * 1000;
constexpr size_t kStreamChunk = 1024 * 1024;

std::string Extension(const std::string &name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

const char *ContentTypeFor(const std::string &ext) {
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "webp") return "image/webp";
    if (ext == "webm") return "video/webm";
    if (ext == "mp4") return "video/mp4";
    return nullptr;
}

bool IsVideo(const std::string &ext) { return ext == "webm" || ext == "mp4"; }

// Only files under the console's capture folders may be served from these routes.
bool IsMediaPath(const std::string &path) {
    if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos) return false;
    for (const char *root : kRoots) {
        std::string prefix = std::string(root) + "/";
        if (path.compare(0, prefix.size(), prefix) == 0) return ContentTypeFor(Extension(path)) != nullptr;
    }
    return false;
}

// A game folder looks like PPSA01234 or CUSA01234.
bool LooksLikeTitleId(const std::string &text) {
    if (text.size() < 9) return false;
    for (int i = 0; i < 4; i++) if (!std::isalpha(static_cast<unsigned char>(text[i]))) return false;
    for (int i = 4; i < 9; i++) if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}

// Files are named YYYYMMDD_HHMMSS_xxxx; the modified time covers anything else.
std::string CapturedAt(const std::string &name, time_t modified) {
    bool stamped = name.size() >= 15 && name[8] == '_';
    for (int i = 0; stamped && i < 15; i++) if (i != 8 && !std::isdigit(static_cast<unsigned char>(name[i]))) stamped = false;
    char text[32];
    if (stamped) {
        std::snprintf(text, sizeof(text), "%.4s-%.2s-%.2sT%.2s:%.2s:%.2s", name.c_str(), name.c_str() + 4, name.c_str() + 6,
                      name.c_str() + 9, name.c_str() + 11, name.c_str() + 13);
        return text;
    }
    tm local {};
    localtime_r(&modified, &local);
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
    return text;
}

struct MediaItem {
    std::string path, game_id, game_name, name, type, captured;
    uint64_t size = 0;
};

void Walk(const std::string &dir, const std::string &relative, int depth, std::vector<MediaItem> *items, bool *truncated) {
    if (depth > kMaxDepth || *truncated) return;
    DIR *handle = opendir(dir.c_str());
    if (!handle) return;
    while (dirent *entry = readdir(handle)) {
        if (entry->d_name[0] == '.') continue;
        std::string child = dir + "/" + entry->d_name;
        struct stat st {};
        if (stat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            Walk(child, relative.empty() ? entry->d_name : relative + "/" + entry->d_name, depth + 1, items, truncated);
        } else if (S_ISREG(st.st_mode)) {
            std::string ext = Extension(entry->d_name);
            if (!ContentTypeFor(ext)) continue;
            if (items->size() >= kMaxItems) { *truncated = true; break; }
            MediaItem item;
            item.path = child;
            item.name = entry->d_name;
            item.type = IsVideo(ext) ? "video" : "photo";
            item.size = static_cast<uint64_t>(st.st_size);
            item.captured = CapturedAt(item.name, st.st_mtime);
            size_t start = 0;
            while (start <= relative.size()) {
                size_t end = relative.find('/', start);
                std::string segment = relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
                // NPXS ids belong to the system UI that saves the capture, not the game.
                if (LooksLikeTitleId(segment)) {
                    bool system = segment.compare(0, 4, "NPXS") == 0;
                    if (!system) { item.game_id = segment.substr(0, 9); break; }
                    if (item.game_id.empty()) item.game_id = segment.substr(0, 9);
                }
                if (end == std::string::npos) break;
                start = end + 1;
            }
            items->push_back(std::move(item));
        }
    }
    closedir(handle);
}

// Serves one byte range of an open file; the lease keeps file jobs from touching it meanwhile.
class RangeStream final : public HttpBodyStream {
  public:
    RangeStream(int fd, uint64_t start, uint64_t length, std::shared_ptr<void> lease)
        : fd_(fd), start_(start), length_(length), lease_(std::move(lease)) {}
    ~RangeStream() override { if (fd_ >= 0) close(fd_); }

    ssize_t Read(uint64_t position, char *buffer, size_t max_bytes) override {
        if (position >= length_) return MHD_CONTENT_READER_END_OF_STREAM;
        size_t want = static_cast<size_t>(std::min<uint64_t>({max_bytes, length_ - position, kStreamChunk}));
        ssize_t got;
        do {
            got = pread(fd_, buffer, want, static_cast<off_t>(start_ + position));
        } while (got < 0 && errno == EINTR);
        if (got > 0) return got;
        return got == 0 ? MHD_CONTENT_READER_END_OF_STREAM : MHD_CONTENT_READER_END_WITH_ERROR;
    }

  private:
    int fd_;
    uint64_t start_, length_;
    std::shared_ptr<void> lease_;
};

// Understands "bytes=a-b", "bytes=a-" and "bytes=-n"; anything else falls back to the whole file.
bool ParseRange(const std::string &header, uint64_t size, uint64_t *start, uint64_t *end) {
    if (header.compare(0, 6, "bytes=") != 0 || header.find(',') != std::string::npos) return false;
    size_t dash = header.find('-', 6);
    if (dash == std::string::npos) return false;
    std::string first = header.substr(6, dash - 6), last = header.substr(dash + 1);
    char *tail = nullptr;
    if (first.empty()) {
        unsigned long long count = std::strtoull(last.c_str(), &tail, 10);
        if (last.empty() || *tail || count == 0) return false;
        *start = count >= size ? 0 : size - count;
        *end = size - 1;
    } else {
        *start = std::strtoull(first.c_str(), &tail, 10);
        if (*tail) return false;
        *end = last.empty() ? size - 1 : std::strtoull(last.c_str(), &tail, 10);
        if (!last.empty() && *tail) return false;
    }
    if (*end >= size) *end = size - 1;
    return *start <= *end && *start < size;
}

uint64_t Fnv1a(const std::string &text) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
    return hash;
}

// Averages each block of source pixels, which is plenty for a grid thumbnail.
bool MakeThumbnail(const std::string &source, const std::string &target) {
    int width = 0, height = 0, channels = 0;
    if (!stbi_info(source.c_str(), &width, &height, &channels) || width <= 0 || height <= 0 ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > kThumbMaxPixels) return false;
    unsigned char *pixels = stbi_load(source.c_str(), &width, &height, &channels, 3);
    if (!pixels) return false;

    int out_w = std::min(width, kThumbSide);
    int out_h = std::max(1, static_cast<int>(static_cast<int64_t>(height) * out_w / width));
    std::vector<unsigned char> out(static_cast<size_t>(out_w) * out_h * 3);
    for (int y = 0; y < out_h; y++) {
        int y0 = static_cast<int>(static_cast<int64_t>(y) * height / out_h);
        int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * height / out_h));
        for (int x = 0; x < out_w; x++) {
            int x0 = static_cast<int>(static_cast<int64_t>(x) * width / out_w);
            int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * width / out_w));
            uint64_t sum[3] = {0, 0, 0};
            for (int sy = y0; sy < y1; sy++) {
                const unsigned char *row = pixels + (static_cast<size_t>(sy) * width + x0) * 3;
                for (int sx = 0; sx < x1 - x0; sx++) {
                    sum[0] += row[sx * 3];
                    sum[1] += row[sx * 3 + 1];
                    sum[2] += row[sx * 3 + 2];
                }
            }
            uint64_t count = static_cast<uint64_t>(y1 - y0) * (x1 - x0);
            unsigned char *dst = &out[(static_cast<size_t>(y) * out_w + x) * 3];
            for (int c = 0; c < 3; c++) dst[c] = static_cast<unsigned char>(sum[c] / count);
        }
    }
    stbi_image_free(pixels);
    return stbi_write_jpg(target.c_str(), out_w, out_h, 3, out.data(), 80) != 0;
}

std::string ThumbKey(const std::string &path, const struct stat &st) {
    char key[17];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(Fnv1a(path + std::to_string(st.st_size) + std::to_string(st.st_mtime) + std::to_string(kThumbSide))));
    return key;
}

// A capture is the media file plus a .meta and a .dat beside it and a thumbnail under thumbnails/.
// The console still lists a capture while any of them is left.
std::vector<std::string> CaptureCompanions(const std::string &path) {
    std::vector<std::string> files;
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return files;
    std::string stem = path.substr(0, dot);
    files.push_back(stem + ".meta");
    files.push_back(stem + ".dat");
    for (const char *root : kRoots) {
        for (const char *kind : {"photo", "video"}) {
            std::string prefix = std::string(root) + "/" + kind + "/";
            if (path.compare(0, prefix.size(), prefix) == 0) {
                files.push_back(std::string(root) + "/thumbnails/" + kind + "/" + path.substr(prefix.size()) + ".jpeg");
            }
        }
    }
    return files;
}

// The console's gallery lists captures from this database, not from the files. content_path is
// relative to /user.
constexpr const char *kGalleryDb = "/system_data/priv/mms/av_content.db";
constexpr size_t kKeptBackups = 5;
std::mutex g_db_mu;

struct CaptureRow {
    std::string content_path, title;
};

// "God of War_20261106172057" becomes "God of War"; a title without that suffix has no game name.
std::string GameNameFromTitle(const std::string &title) {
    size_t cut = title.rfind('_');
    if (cut == std::string::npos || title.size() - cut - 1 != 14) return "";
    for (size_t i = cut + 1; i < title.size(); i++) if (!std::isdigit(static_cast<unsigned char>(title[i]))) return "";
    return title.substr(0, cut);
}

// Only captures on the internal drive are mapped; other paths are not touched.
std::string InternalCapturePath(const std::string &content_path) {
    return content_path.rfind("/av_contents/", 0) == 0 ? "/user" + content_path : "";
}

std::vector<CaptureRow> ReadGalleryRows() {
    std::vector<CaptureRow> rows;
    std::lock_guard<std::mutex> lock(g_db_mu);
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(kGalleryDb, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return rows;
    }
    sqlite3_busy_timeout(db, 2000);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT content_path, title FROM tbl_PhotoVideo", -1, &stmt, nullptr) == SQLITE_OK) {
        auto text = [&](int column) {
            const unsigned char *value = sqlite3_column_text(stmt, column);
            return value ? std::string(reinterpret_cast<const char *>(value)) : std::string();
        };
        while (sqlite3_step(stmt) == SQLITE_ROW) rows.push_back({text(0), text(1)});
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}

// Copies the gallery database before it is changed, and keeps only the latest few copies.
bool BackupGalleryDb(sqlite3 *source, const std::string &dir, std::string *err) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string target = dir + "/av_content-" + std::to_string(static_cast<long long>(time(nullptr))) + ".db";
    sqlite3 *copy = nullptr;
    bool ok = sqlite3_open(target.c_str(), &copy) == SQLITE_OK;
    if (ok) {
        sqlite3_backup *job = sqlite3_backup_init(copy, "main", source, "main");
        ok = job != nullptr;
        if (job) {
            ok = sqlite3_backup_step(job, -1) == SQLITE_DONE;
            sqlite3_backup_finish(job);
        }
    }
    sqlite3_close(copy);
    if (!ok) {
        std::remove(target.c_str());
        if (err) *err = "could not back up the gallery database";
        return false;
    }
    std::vector<std::string> names;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("av_content-", 0) == 0) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    while (names.size() > kKeptBackups) {
        fs::remove(fs::path(dir) / names.front(), ec);
        names.erase(names.begin());
    }
    return true;
}

// Removes one capture. The gallery row goes first, so a database error leaves the files alone.
bool RemoveCapture(sqlite3 *db, const std::string &path, const std::string &thumb_dir, std::string *err) {
    if (path.rfind("/user/av_contents/", 0) == 0) {
        sqlite3_stmt *stmt = nullptr;
        std::string content_path = path.substr(5);
        bool ok = sqlite3_prepare_v2(db, "DELETE FROM tbl_PhotoVideo WHERE content_path = ?", -1, &stmt, nullptr) == SQLITE_OK &&
                  sqlite3_bind_text(stmt, 1, content_path.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                  sqlite3_step(stmt) == SQLITE_DONE;
        if (!ok && err) *err = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        if (!ok) return false;
    }
    struct stat st {};
    if (stat(path.c_str(), &st) == 0) std::remove((thumb_dir + "/" + ThumbKey(path, st) + ".jpg").c_str());
    bool removed = true;
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
        LogEvent("media: could not delete " + path + " (" + std::strerror(errno) + ")");
        removed = false;
    }
    for (const std::string &companion : CaptureCompanions(path)) {
        if (unlink(companion.c_str()) != 0 && errno != ENOENT) {
            LogEvent("media: could not delete " + companion + " (" + std::strerror(errno) + ")");
        }
    }
    return removed;
}

struct RemovalResult {
    int deleted = 0, failed = 0;
    std::string error;
};

RemovalResult RemoveCaptures(const std::vector<std::string> &paths, const std::string &thumb_dir, const std::string &backup_dir) {
    RemovalResult result;
    std::lock_guard<std::mutex> lock(g_db_mu);
    sqlite3 *db = nullptr;
    bool indexed = std::any_of(paths.begin(), paths.end(), [](const std::string &path) { return path.rfind("/user/av_contents/", 0) == 0; });
    if (indexed && sqlite3_open_v2(kGalleryDb, &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        result.error = "the gallery database is not available";
    } else if (indexed) {
        sqlite3_busy_timeout(db, 5000);
        BackupGalleryDb(db, backup_dir, &result.error);
    }
    // Without the database and its backup nothing is deleted, or the console would list ghosts.
    if (!result.error.empty()) {
        LogEvent("media: delete refused (" + result.error + ")");
        sqlite3_close(db);
        result.failed = static_cast<int>(paths.size());
        return result;
    }
    for (const std::string &path : paths) {
        std::string err;
        if (RemoveCapture(db, path, thumb_dir, &err)) {
            result.deleted++;
        } else {
            if (!err.empty()) LogEvent("media: database refused to delete " + path + " (" + err + ")");
            result.failed++;
        }
    }
    sqlite3_close(db);
    return result;
}

std::mutex g_thumb_mu; // One decode at a time keeps the memory use of a big screenshot bounded.

} // namespace

void RegisterMediaRoutes(HttpServer *server, const Config &cfg) {
    server->Route("GET", "/api/media/list", [](const HttpRequest &) {
        std::vector<MediaItem> items;
        bool truncated = false;
        for (const char *root : kRoots) {
            for (const char *kind : {"photo", "video"}) Walk(std::string(root) + "/" + kind, "", 0, &items, &truncated);
        }
        std::sort(items.begin(), items.end(), [](const MediaItem &a, const MediaItem &b) { return a.captured > b.captured; });

        // The gallery database knows each capture's game name, even for games that are no longer installed.
        // A row whose file is gone is a leftover the console still lists.
        std::unordered_map<std::string, std::string> names;
        int leftovers = 0;
        for (const CaptureRow &row : ReadGalleryRows()) {
            std::string path = InternalCapturePath(row.content_path);
            if (path.empty()) continue;
            struct stat st {};
            if (stat(path.c_str(), &st) != 0 && errno == ENOENT) leftovers++;
            else names[path] = GameNameFromTitle(row.title);
        }
        for (MediaItem &item : items) {
            auto found = names.find(item.path);
            if (found != names.end()) item.game_name = found->second;
        }
        std::vector<std::string> rows;
        rows.reserve(items.size());
        for (const MediaItem &item : items) {
            rows.push_back(JsonWriter()
                               .Str("path", item.path)
                               .Str("name", item.name)
                               .Str("game_id", item.game_id)
                               .Str("game_name", item.game_name)
                               .Str("type", item.type)
                               .Str("captured", item.captured)
                               .Int("size", static_cast<long long>(item.size))
                               .Done());
        }
        return HttpResponse::Json(JsonWriter().Raw("items", JsonWriter::Array(rows)).Bool("truncated", truncated).Int("leftovers", leftovers).Done());
    });

    server->Route("GET", "/api/media/file", [](const HttpRequest &req) {
        std::string path = req.Query("path");
        if (!IsMediaPath(path)) return HttpResponse::Error("not a capture file", 404);
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return HttpResponse::Error("file not found", 404);

        std::string err;
        if (!BeginFileRead(path, &err)) return HttpResponse::Error(err, 409);
        std::shared_ptr<void> lease(&st, [path](void *) { EndFileRead(path); });
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return HttpResponse::Error("could not open file", 500);

        uint64_t size = static_cast<uint64_t>(st.st_size), start = 0, end = size ? size - 1 : 0;
        bool partial = size > 0 && ParseRange(req.range, size, &start, &end);
        HttpResponse resp;
        resp.status = partial ? 206 : 200;
        resp.content_type = ContentTypeFor(Extension(path));
        resp.body_stream_size = size ? end - start + 1 : 0;
        resp.body_stream = std::make_shared<RangeStream>(fd, start, resp.body_stream_size, lease);
        resp.extra_headers["Accept-Ranges"] = "bytes";
        resp.extra_headers["Cache-Control"] = "private, max-age=3600";
        if (partial) {
            resp.extra_headers["Content-Range"] = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size);
        }
        std::string name = fs::path(path).filename().string();
        resp.extra_headers["Content-Disposition"] =
            std::string(req.Query("download") == "1" ? "attachment" : "inline") + "; filename=\"" + name + "\"";
        return resp;
    });

    std::string thumb_dir = cfg.root_dir + "/media_thumbs";
    std::string backup_dir = cfg.root_dir + "/media_db_backups";
    server->Route("POST", "/api/media/delete", [thumb_dir, backup_dir](const HttpRequest &req) {
        std::string err;
        if (!BeginImmediateFileMutation(&err)) return HttpResponse::Error(err, 409);
        std::vector<std::string> targets;
        int invalid = 0;
        for (const std::string &path : JsonExtractStringArray(req.body, "paths")) {
            struct stat st {};
            if (IsMediaPath(path) && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) targets.push_back(path);
            else invalid++;
        }
        RemovalResult result = RemoveCaptures(targets, thumb_dir, backup_dir);
        EndImmediateFileMutation();
        return HttpResponse::Json(JsonWriter().Int("deleted", result.deleted).Int("failed", result.failed + invalid).Done());
    });

    // Removes the gallery rows (and any files left) of captures whose image no longer exists.
    server->Route("POST", "/api/media/cleanup", [thumb_dir, backup_dir](const HttpRequest &) {
        std::string err;
        if (!BeginImmediateFileMutation(&err)) return HttpResponse::Error(err, 409);
        std::vector<std::string> targets;
        for (const CaptureRow &row : ReadGalleryRows()) {
            std::string path = InternalCapturePath(row.content_path);
            struct stat st {};
            if (!path.empty() && IsMediaPath(path) && stat(path.c_str(), &st) != 0 && errno == ENOENT) targets.push_back(path);
        }
        RemovalResult result = targets.empty() ? RemovalResult {} : RemoveCaptures(targets, thumb_dir, backup_dir);
        EndImmediateFileMutation();
        return HttpResponse::Json(JsonWriter().Int("deleted", result.deleted).Int("failed", result.failed).Done());
    });

    server->Route("GET", "/api/media/thumb", [thumb_dir](const HttpRequest &req) {
        std::string path = req.Query("path");
        std::string ext = Extension(path);
        if (!IsMediaPath(path) || IsVideo(ext)) return HttpResponse::Error("no thumbnail for that file", 404);
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return HttpResponse::Error("file not found", 404);

        std::string cached = thumb_dir + "/" + ThumbKey(path, st) + ".jpg";
        {
            std::lock_guard<std::mutex> lock(g_thumb_mu);
            struct stat have {};
            if (stat(cached.c_str(), &have) != 0) {
                std::error_code ec;
                fs::create_directories(thumb_dir, ec);
                std::string partial = cached + ".part";
                if (!MakeThumbnail(path, partial) || std::rename(partial.c_str(), cached.c_str()) != 0) {
                    std::remove(partial.c_str());
                    return HttpResponse::Error("could not make a thumbnail", 404);
                }
            }
        }
        HttpResponse resp;
        resp.content_type = "image/jpeg";
        resp.body_file = cached;
        resp.extra_headers["Cache-Control"] = "private, max-age=86400";
        return resp;
    });
}

} // namespace prospero
