#include "repo_icons.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "https_client.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "repository.hpp"

namespace prospero {
namespace {

constexpr const char *kMirrorIndexUrl = "https://itsplk.github.io/ps5-payloads-mirror/payloads_v2.json";
constexpr int64_t kRetrySeconds = 300;
constexpr size_t kMaxIconBytes = 1024 * 1024;
// A cached logo is looked at again after two weeks; "no logo here" after one.
constexpr int64_t kIconFreshSeconds = 14 * 24 * 3600;
constexpr int64_t kNoneFreshSeconds = 7 * 24 * 3600;

// Where PS5 projects usually keep their logo, most common first.
constexpr const char *kRepoIconPaths[] = {
    "sce_sys/icon0.png", "assets/icon0.png", "icon0.png", "assets/icon.png", "icon.png", "assets/logo.png", "logo.png",
};

struct IconData {
    std::string mime;
    std::string bytes;
};

std::mutex g_mu;
bool g_running = false;
bool g_mirror_loaded = false;
int64_t g_mirror_failed_at = 0;
std::unordered_map<std::string, IconData> g_own_icons;    // found in the repository itself
std::unordered_map<std::string, IconData> g_mirror_icons; // from the mirror's index
std::set<std::string> g_checked;                          // repositories already looked at
std::vector<std::string> g_queue;                         // repositories waiting to be looked at
std::string g_cache_dir;

std::string LowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool RepoKey(const std::string &url, std::string *key, RepoRef *ref_out = nullptr) {
    RepoRef ref;
    std::string ignored;
    if (!ParseRepositoryUrl(url, &ref, &ignored) || ref.provider != RepositoryProvider::kGitHub) return false;
    *key = LowerAscii(ref.owner + "/" + ref.repo);
    if (ref_out) *ref_out = ref;
    return true;
}

bool DecodeBase64(const std::string &text, std::string *out) {
    static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(text.size() / 4 * 3);
    unsigned buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=') break;
        size_t value = kAlphabet.find(c);
        if (value == std::string::npos) return false;
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    *out = std::move(result);
    return !out->empty();
}

// "data:image/png;base64,...." -> type and bytes. Only image types the browser draws safely in an <img>.
bool DecodeDataUri(const std::string &uri, IconData *out) {
    static const std::string kPrefix = "data:";
    static const std::string kBase64 = ";base64,";
    size_t marker = uri.find(kBase64);
    if (uri.compare(0, kPrefix.size(), kPrefix) != 0 || marker == std::string::npos) return false;
    out->mime = uri.substr(kPrefix.size(), marker - kPrefix.size());
    if (out->mime != "image/png" && out->mime != "image/jpeg" && out->mime != "image/svg+xml" &&
        out->mime != "image/webp" && out->mime != "image/gif") {
        return false;
    }
    return DecodeBase64(uri.substr(marker + kBase64.size()), &out->bytes);
}

// `status` is the HTTP status when the server answered at all, 0 when there was no answer.
bool Fetch(const std::string &url, std::string *body, bool quiet = false, int *status = nullptr) {
    HttpsResponse response;
    std::string err;
    bool got = HttpsClient::Get(url, &response, &err, nullptr, "", quiet);
    if (status) *status = response.status;
    if (!got || response.status != 200) return false;
    *body = std::move(response.body);
    return true;
}

// A file named like an image can be anything, so go by the first bytes.
bool SniffImage(const std::string &body, IconData *out) {
    if (body.size() < 4 || body.size() > kMaxIconBytes) return false;
    if (body.compare(0, 4, "\x89PNG") == 0) out->mime = "image/png";
    else if (body.compare(0, 3, "\xFF\xD8\xFF") == 0) out->mime = "image/jpeg";
    else return false;
    out->bytes = body;
    return true;
}

// `answered` is false when some request got no reply, so a miss then says nothing about the repository.
bool FindInRepository(const RepoRef &ref, IconData *out, bool *answered) {
    *answered = true;
    for (const char *path : kRepoIconPaths) {
        std::string body;
        int status = 0;
        std::string url = "https://raw.githubusercontent.com/" + ref.owner + "/" + ref.repo + "/HEAD/" + path;
        // Most of these paths do not exist, so a 404 is not worth a log line.
        if (Fetch(url, &body, /*quiet=*/true, &status) && SniffImage(body, out)) return true;
        if (status == 0) *answered = false;
    }
    return false;
}

// ---- Disk cache: one file per repository ("owner~repo.png"), or "owner~repo.none" for no logo ----

const char *ExtensionFor(const std::string &mime) {
    if (mime == "image/png") return "png";
    if (mime == "image/jpeg") return "jpg";
    if (mime == "image/svg+xml") return "svg";
    if (mime == "image/webp") return "webp";
    if (mime == "image/gif") return "gif";
    return "";
}

const char *MimeForExtension(const std::string &ext) {
    if (ext == "png") return "image/png";
    if (ext == "jpg") return "image/jpeg";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "webp") return "image/webp";
    if (ext == "gif") return "image/gif";
    return "";
}

// GitHub names never contain '~', so it can stand in for the '/'.
std::string CacheName(const std::string &key) {
    std::string name = key;
    std::replace(name.begin(), name.end(), '/', '~');
    return name;
}

std::string CachePath(const std::string &key, const std::string &ext) { return g_cache_dir + "/" + CacheName(key) + "." + ext; }

void RemoveCacheEntries(const std::string &key) {
    for (const char *ext : {"png", "jpg", "svg", "webp", "gif", "none"}) std::remove(CachePath(key, ext).c_str());
}

bool WriteFileAtomic(const std::string &path, const std::string &bytes) {
    std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            std::remove(temp.c_str());
            return false;
        }
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        return false;
    }
    return true;
}

void SaveIcon(const std::string &key, const IconData &data) {
    if (g_cache_dir.empty() || !*ExtensionFor(data.mime)) return;
    RemoveCacheEntries(key);
    WriteFileAtomic(CachePath(key, ExtensionFor(data.mime)), data.bytes);
}

void SaveNone(const std::string &key) {
    if (g_cache_dir.empty()) return;
    RemoveCacheEntries(key);
    WriteFileAtomic(CachePath(key, "none"), "");
}

// Reads the mirror's index into g_mirror_icons. Runs on the scan thread, so no lock is held while on the network.
void LoadMirror() {
    std::string index;
    if (!Fetch(kMirrorIndexUrl, &index)) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_mirror_failed_at = std::time(nullptr);
        return;
    }
    std::string icons_url = JsonExtractString(index, "icons_url");
    std::string icons_json;
    if (!icons_url.empty()) {
        if (icons_url.rfind("https://", 0) != 0) {
            std::string base = kMirrorIndexUrl;
            icons_url = base.substr(0, base.find_last_of('/') + 1) + icons_url;
        }
        if (!Fetch(icons_url, &icons_json)) {
            std::lock_guard<std::mutex> lock(g_mu);
            g_mirror_failed_at = std::time(nullptr);
            return;
        }
    }
    std::unordered_map<std::string, IconData> loaded;
    for (const auto &item : JsonExtractArrayItems(index, "payloads")) {
        std::string key;
        std::string icon_key = JsonExtractString(item, "icon_key");
        if (icon_key.empty() || !RepoKey(JsonExtractString(item, "source"), &key) || loaded.count(key)) continue;
        IconData data;
        if (DecodeDataUri(JsonExtractString(icons_json, icon_key), &data)) loaded[key] = std::move(data);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_mirror_icons = std::move(loaded);
    g_mirror_loaded = true;
}

// Works through the queue until it is empty, so repositories queued while a scan runs are not left out.
void ScanThread() {
    size_t found = 0;
    for (;;) {
        std::vector<std::string> batch;
        bool need_mirror;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            batch.swap(g_queue);
            need_mirror = !batch.empty() && !g_mirror_loaded && std::time(nullptr) - g_mirror_failed_at >= kRetrySeconds;
            if (batch.empty()) {
                g_running = false;
                LogEvent("Repository icons: " + std::to_string(g_mirror_icons.size()) + " in the mirror, " + std::to_string(found) +
                         " found in repositories");
                return;
            }
        }
        // The mirror's thumbnails are small, so it goes first and only the repositories it lacks are searched.
        if (need_mirror) LoadMirror();
        for (const auto &url : batch) {
            std::string key;
            RepoRef ref;
            if (!RepoKey(url, &key, &ref)) continue;
            IconData mirrored;
            bool listed = false;
            bool mirror_known;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                auto it = g_mirror_icons.find(key);
                if (it != g_mirror_icons.end()) {
                    mirrored = it->second;
                    listed = true;
                }
                mirror_known = g_mirror_loaded;
            }
            if (listed) {
                SaveIcon(key, mirrored);
                std::lock_guard<std::mutex> lock(g_mu);
                g_checked.insert(key);
                continue;
            }
            IconData data;
            bool answered = false;
            bool own = FindInRepository(ref, &data, &answered);
            if (own) SaveIcon(key, data);
            // "No logo" is only remembered when the mirror and every path really answered, not when the net was down.
            else if (answered && mirror_known) SaveNone(key);
            std::lock_guard<std::mutex> lock(g_mu);
            if (own) {
                g_own_icons[key] = std::move(data);
                found++;
            }
            if (own || (answered && mirror_known)) g_checked.insert(key);
        }
    }
}

// Reads what the cache holds. Entries that are still fresh need no network look-up.
void LoadCacheFromDisk() {
    DIR *dir = opendir(g_cache_dir.c_str());
    if (!dir) return;
    int64_t now = std::time(nullptr);
    while (dirent *entry = readdir(dir)) {
        std::string file = entry->d_name;
        size_t dot = file.find_last_of('.');
        if (dot == std::string::npos || dot == 0) continue;
        std::string name = file.substr(0, dot);
        std::string ext = file.substr(dot + 1);
        std::string key = name;
        std::replace(key.begin(), key.end(), '~', '/');
        struct stat st {};
        if (stat((g_cache_dir + "/" + file).c_str(), &st) != 0) continue;
        bool fresh = now - static_cast<int64_t>(st.st_mtime) < (ext == "none" ? kNoneFreshSeconds : kIconFreshSeconds);
        if (ext == "none") {
            if (fresh) g_checked.insert(key);
            continue;
        }
        const char *mime = MimeForExtension(ext);
        if (!*mime || st.st_size <= 0 || static_cast<size_t>(st.st_size) > kMaxIconBytes) continue;
        std::ifstream in(g_cache_dir + "/" + file, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty()) continue;
        g_own_icons[key] = {mime, std::move(bytes)};
        if (fresh) g_checked.insert(key);
    }
    closedir(dir);
}

} // namespace

void InitializeRepoIcons(const Config &cfg) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_cache_dir = cfg.root_dir + "/repo_icons";
    mkdir(g_cache_dir.c_str(), 0755);
    LoadCacheFromDisk();
    LogEvent("Repository icons cached: " + std::to_string(g_own_icons.size()));
}

void ScanRepoIcons(const std::vector<std::string> &repo_urls) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const auto &url : repo_urls) {
        std::string key;
        if (RepoKey(url, &key) && !g_checked.count(key) && std::find(g_queue.begin(), g_queue.end(), url) == g_queue.end()) {
            g_queue.push_back(url);
        }
    }
    if (g_running || g_queue.empty()) return;
    g_running = true;
    try {
        std::thread(ScanThread).detach();
    } catch (...) {
        g_running = false;
    }
}

bool RepoIconScanRunning() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_running;
}

bool FindRepoIcon(const std::string &repo_url, std::string *mime, std::string *bytes) {
    std::string key;
    if (!RepoKey(repo_url, &key)) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    auto mirrored = g_mirror_icons.find(key);
    const IconData *data = nullptr;
    if (mirrored != g_mirror_icons.end()) {
        data = &mirrored->second;
    } else {
        auto own = g_own_icons.find(key);
        if (own != g_own_icons.end()) data = &own->second;
    }
    if (!data) return false;
    *mime = data->mime;
    *bytes = data->bytes;
    return true;
}

} // namespace prospero
