#include "gdrive.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "https_client.hpp"
#include "job_queue.hpp"
#include "json_writer.hpp"
#include "log.hpp"

namespace fs = std::filesystem;

namespace prospero {

namespace {

constexpr const char *kDeviceCodeUrl = "https://oauth2.googleapis.com/device/code";
constexpr const char *kTokenUrl = "https://oauth2.googleapis.com/token";
constexpr const char *kRevokeUrl = "https://oauth2.googleapis.com/revoke";
// drive.file: only files this app created. It is also the broadest Drive scope Google allows in the
// device flow, so "see the user's whole Drive" is not an option here by design.
constexpr const char *kScope = "https://www.googleapis.com/auth/drive.file";
constexpr const char *kFilesUrl = "https://www.googleapis.com/drive/v3/files";
constexpr const char *kUploadUrl = "https://www.googleapis.com/upload/drive/v3/files";
constexpr const char *kRootFolderName = "ProsperoPS5";
constexpr const char *kFolderMime = "application/vnd.google-apps.folder";
constexpr size_t kBackupsKept = 5;
// Resumable-upload chunks must be a multiple of 256 KiB (except the last one).
constexpr size_t kUploadChunk = 8 * 256 * 1024;
constexpr int kChunkAttempts = 3;

// ---- stored settings ---------------------------------------------------------------------------

struct Stored {
    std::string client_id;
    std::string client_secret;
    std::string refresh_token;
    bool enabled = false;
    bool drive_only = false;
};

std::mutex g_mu; // settings file, access-token cache, folder-id cache

std::string ConfigPath(const Config &cfg) { return cfg.root_dir + "/gdrive.cfg"; }

Stored LoadStored(const Config &cfg) {
    Stored s;
    std::ifstream in(ConfigPath(cfg));
    for (std::string line; std::getline(in, line);) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (key == "client_id") s.client_id = value;
        else if (key == "client_secret") s.client_secret = value;
        else if (key == "refresh_token") s.refresh_token = value;
        else if (key == "enabled") s.enabled = value == "1";
        else if (key == "drive_only") s.drive_only = value == "1";
    }
    return s;
}

// The file holds a long-lived credential, so it is created owner-only and swapped in atomically.
bool SaveStored(const Config &cfg, const Stored &s, std::string *err) {
    std::string path = ConfigPath(cfg);
    std::string staged = path + ".tmp";
    std::string text = "client_id=" + s.client_id + "\nclient_secret=" + s.client_secret +
                       "\nrefresh_token=" + s.refresh_token + "\nenabled=" + (s.enabled ? "1" : "0") +
                       "\ndrive_only=" + (s.drive_only ? "1" : "0") + "\n";
    int fd = open(staged.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = fd >= 0;
    if (ok) {
        size_t written = 0;
        while (written < text.size()) {
            ssize_t w = write(fd, text.data() + written, text.size() - written);
            if (w <= 0) { ok = false; break; }
            written += static_cast<size_t>(w);
        }
        fsync(fd);
        close(fd);
    }
    std::error_code ec;
    if (ok) fs::rename(staged, path, ec);
    if (!ok || ec) {
        fs::remove(staged, ec);
        if (err) *err = "could not save the Google Drive settings";
        return false;
    }
    return true;
}

// Credentials end up in a form body and a file, so only the characters Google's own ids use are allowed.
bool ValidCredential(const std::string &value) {
    if (value.empty() || value.size() > 256) return false;
    for (unsigned char c : value) {
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-' && c != '~') return false;
    }
    return true;
}

// Title ids and folder names are interpolated into Drive search queries.
bool ValidName(const std::string &value) {
    if (value.empty() || value.size() > 64) return false;
    for (unsigned char c : value) {
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

// ---- small helpers -----------------------------------------------------------------------------

std::string UrlEncode(const std::string &raw) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() * 3);
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string FormBody(const std::vector<std::pair<std::string, std::string>> &fields) {
    std::string body;
    for (const auto &f : fields) {
        if (!body.empty()) body += '&';
        body += UrlEncode(f.first) + "=" + UrlEncode(f.second);
    }
    return body;
}

const HttpHeaders kFormHeaders = {{"Content-Type", "application/x-www-form-urlencoded"}};

// Google's own words for what went wrong, falling back to the bare status.
std::string ApiMessage(const HttpsReply &r) {
    std::string m = JsonExtractString(r.body, "error_description");
    if (m.empty()) m = JsonExtractString(r.body, "message");
    if (m.empty()) m = JsonExtractString(r.body, "error");
    if (m.empty()) m = "Google returned HTTP " + std::to_string(r.status);
    return m;
}

bool IsOk(const HttpsReply &r) { return r.status >= 200 && r.status < 300; }

int64_t ParseRfc3339(const std::string &text) {
    int y, mo, d, h, mi, s;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return static_cast<int64_t>(timegm(&tm));
}

void SetFail(std::string *err, std::string *code, const std::string &message, const char *reason) {
    if (err) *err = message;
    if (code) *code = reason;
}

// ---- access token ------------------------------------------------------------------------------

std::string g_access_token;
std::chrono::steady_clock::time_point g_access_expiry;
std::map<std::string, std::string> g_folder_ids; // "<parent>/<name>" -> folder id

void InvalidateAccessToken() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_access_token.clear();
}

bool AccessToken(const Config &cfg, std::string *token, std::string *err, std::string *code) {
    Stored s;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_access_token.empty() && std::chrono::steady_clock::now() < g_access_expiry) {
            *token = g_access_token;
            return true;
        }
        s = LoadStored(cfg);
    }
    if (s.refresh_token.empty() || s.client_id.empty() || s.client_secret.empty()) {
        SetFail(err, code, "Google Drive is not linked", "gdrive_not_linked");
        return false;
    }

    std::string body = FormBody({{"client_id", s.client_id},
                                 {"client_secret", s.client_secret},
                                 {"refresh_token", s.refresh_token},
                                 {"grant_type", "refresh_token"}});
    HttpsReply r;
    std::string net_err;
    if (!HttpsClient::Request("POST", kTokenUrl, kFormHeaders, body.data(), body.size(), &r, &net_err)) {
        SetFail(err, code, net_err, "gdrive_network");
        return false;
    }
    if (r.status != 200) {
        // invalid_grant is Google saying the sign-in is gone (revoked, expired, password change);
        // retrying can't fix it, so drop the dead token and let the UI ask for a new link.
        if (JsonExtractString(r.body, "error") == "invalid_grant") {
            std::lock_guard<std::mutex> lock(g_mu);
            Stored now = LoadStored(cfg);
            now.refresh_token.clear();
            now.enabled = false;
            SaveStored(cfg, now, nullptr);
            g_access_token.clear();
            SetFail(err, code, "Google rejected the saved sign-in. Link Google Drive again.", "gdrive_auth");
            return false;
        }
        SetFail(err, code, ApiMessage(r), "gdrive_api");
        return false;
    }
    std::string access = JsonExtractString(r.body, "access_token");
    if (access.empty()) {
        SetFail(err, code, "Google sent no access token", "gdrive_api");
        return false;
    }
    int lifetime = JsonExtractInt(r.body, "expires_in", 3600);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_access_token = access;
        g_access_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(60, lifetime - 120));
    }
    *token = access;
    return true;
}

// One authorized call. Returns true when Google answered at all; the caller reads r->status.
bool CallApi(const Config &cfg, const std::string &method, const std::string &url, const HttpHeaders &extra,
             const void *body, size_t body_len, HttpsReply *r, std::string *err, std::string *code) {
    for (int attempt = 0; attempt < 2; attempt++) {
        std::string token;
        if (!AccessToken(cfg, &token, err, code)) return false;
        HttpsReply local;
        HttpHeaders headers = extra;
        headers.emplace_back("Authorization", "Bearer " + token);
        std::string net_err;
        if (!HttpsClient::Request(method, url, headers, body, body_len, &local, &net_err)) {
            SetFail(err, code, net_err, "gdrive_network");
            return false;
        }
        if (local.status == 401 && attempt == 0) { // token went stale early; fetch a fresh one once
            InvalidateAccessToken();
            continue;
        }
        *r = std::move(local);
        return true;
    }
    return false;
}

// ---- folders -----------------------------------------------------------------------------------

// Finds (or creates) a folder this app owns. drive.file hides everything else, so a same-named
// folder the user made by hand is invisible here and a fresh one is created beside it.
bool EnsureFolder(const Config &cfg, const std::string &name, const std::string &parent_id, std::string *id,
                  std::string *err, std::string *code) {
    if (!ValidName(name) || (!parent_id.empty() && !IsValidGdriveId(parent_id))) {
        SetFail(err, code, "invalid folder name", "gdrive_api");
        return false;
    }
    const std::string cache_key = parent_id + "/" + name;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_folder_ids.find(cache_key);
        if (it != g_folder_ids.end()) {
            *id = it->second;
            return true;
        }
    }

    std::string query = std::string("mimeType='") + kFolderMime + "' and name='" + name + "' and trashed=false";
    if (!parent_id.empty()) query += " and '" + parent_id + "' in parents";
    std::string url = std::string(kFilesUrl) + "?q=" + UrlEncode(query) +
                      "&fields=" + UrlEncode("files(id)") + "&orderBy=createdTime&pageSize=1&spaces=drive";
    HttpsReply r;
    if (!CallApi(cfg, "GET", url, {}, nullptr, 0, &r, err, code)) return false;
    if (!IsOk(r)) {
        SetFail(err, code, ApiMessage(r), "gdrive_api");
        return false;
    }
    std::vector<std::string> found = JsonExtractArrayItems(r.body, "files");
    std::string folder_id = found.empty() ? "" : JsonExtractString(found.front(), "id");

    if (folder_id.empty()) {
        std::string meta = JsonWriter().Str("name", name).Str("mimeType", kFolderMime).Done();
        if (!parent_id.empty()) {
            meta = JsonWriter()
                       .Str("name", name)
                       .Str("mimeType", kFolderMime)
                       .Raw("parents", "[" + JsonWriter::Quote(parent_id) + "]")
                       .Done();
        }
        HttpsReply c;
        if (!CallApi(cfg, "POST", std::string(kFilesUrl) + "?fields=id",
                     {{"Content-Type", "application/json; charset=UTF-8"}}, meta.data(), meta.size(), &c, err, code)) {
            return false;
        }
        if (!IsOk(c)) {
            SetFail(err, code, ApiMessage(c), "gdrive_api");
            return false;
        }
        folder_id = JsonExtractString(c.body, "id");
    }
    if (!IsValidGdriveId(folder_id)) {
        SetFail(err, code, "Google returned no folder id", "gdrive_api");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_folder_ids[cache_key] = folder_id;
    }
    *id = folder_id;
    return true;
}

// A folder the user deleted in Drive leaves a stale cached id; forget it so the next try recreates it.
void ForgetFolders() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_folder_ids.clear();
}

// ---- resumable upload --------------------------------------------------------------------------

// "bytes=0-2097151" -> 2097152, the next byte Google wants. No header means Google holds nothing yet.
uint64_t NextOffset(const std::string &range) {
    size_t dash = range.rfind('-');
    if (dash == std::string::npos) return 0;
    return std::strtoull(range.c_str() + dash + 1, nullptr, 10) + 1;
}

bool ReadExactly(int fd, char *buf, size_t n, uint64_t offset) {
    size_t done = 0;
    while (done < n) {
        ssize_t got = pread(fd, buf + done, n - done, static_cast<off_t>(offset + done));
        if (got <= 0) return false;
        done += static_cast<size_t>(got);
    }
    return true;
}

bool UploadResumable(const Config &cfg, const std::string &folder_id, const std::string &title_id,
                     const std::string &file_name, const std::string &local_zip, std::string *err,
                     std::string *code) {
    struct stat st {};
    if (stat(local_zip.c_str(), &st) != 0 || st.st_size <= 0) {
        SetFail(err, code, "the backup file to upload is missing", "gdrive_upload");
        return false;
    }
    const uint64_t total = static_cast<uint64_t>(st.st_size);
    int fd = open(local_zip.c_str(), O_RDONLY);
    if (fd < 0) {
        SetFail(err, code, "could not read the backup file", "gdrive_upload");
        return false;
    }
    struct FdGuard {
        int fd;
        ~FdGuard() { close(fd); }
    } guard{fd};

    std::string meta = JsonWriter()
                           .Str("name", file_name)
                           .Raw("parents", "[" + JsonWriter::Quote(folder_id) + "]")
                           .Raw("appProperties", JsonWriter().Str("pmgr_title", title_id).Done())
                           .Done();
    HttpsReply init;
    if (!CallApi(cfg, "POST", std::string(kUploadUrl) + "?uploadType=resumable&fields=id",
                 {{"Content-Type", "application/json; charset=UTF-8"},
                  {"X-Upload-Content-Type", "application/zip"},
                  {"X-Upload-Content-Length", std::to_string(total)}},
                 meta.data(), meta.size(), &init, err, code)) {
        return false;
    }
    if (!IsOk(init) || init.location.rfind("https://", 0) != 0) {
        if (init.status == 404) ForgetFolders(); // the folder is gone; recreate it next time
        SetFail(err, code, IsOk(init) ? "Google gave no upload address" : ApiMessage(init), "gdrive_api");
        return false;
    }
    const std::string session = init.location;

    std::vector<char> chunk(kUploadChunk);
    uint64_t offset = 0;
    int failures = 0;
    while (offset < total) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(kUploadChunk, total - offset));
        if (!ReadExactly(fd, chunk.data(), n, offset)) {
            SetFail(err, code, "could not read the backup file", "gdrive_upload");
            return false;
        }
        std::string range = "bytes " + std::to_string(offset) + "-" + std::to_string(offset + n - 1) + "/" +
                            std::to_string(total);
        HttpsReply put;
        std::string put_err, put_code;
        bool answered = CallApi(cfg, "PUT", session, {{"Content-Range", range}, {"Content-Type", "application/zip"}},
                                chunk.data(), n, &put, &put_err, &put_code);
        if (answered && (put.status == 200 || put.status == 201)) return true;
        if (answered && put.status == 308) {
            offset = NextOffset(put.range);
            failures = 0;
            continue;
        }

        // A rejected request (not a hiccup) will not get better by repeating it.
        bool permanent = answered && put.status >= 400 && put.status < 500 && put.status != 408 && put.status != 429;
        if (permanent || ++failures >= kChunkAttempts) {
            SetFail(err, code, answered ? ApiMessage(put) : put_err, answered ? "gdrive_api" : "gdrive_network");
            return false;
        }

        // Ask Google how far it really got, then carry on from there.
        std::this_thread::sleep_for(std::chrono::seconds(2));
        HttpsReply probe;
        std::string probe_err, probe_code;
        if (CallApi(cfg, "PUT", session, {{"Content-Range", "bytes */" + std::to_string(total)}}, nullptr, 0, &probe,
                    &probe_err, &probe_code)) {
            if (probe.status == 200 || probe.status == 201) return true;
            if (probe.status == 308) offset = NextOffset(probe.range);
        }
    }
    SetFail(err, code, "the upload did not complete", "gdrive_upload");
    return false;
}

// Deletes everything past the newest few in one title's folder, so an unattended watcher can't
// fill the user's Drive.
void PruneRemote(const Config &cfg, const std::string &folder_id) {
    std::string url = std::string(kFilesUrl) + "?q=" + UrlEncode("'" + folder_id + "' in parents and trashed=false") +
                      "&fields=" + UrlEncode("files(id)") + "&orderBy=" + UrlEncode("createdTime desc") +
                      "&pageSize=100";
    HttpsReply r;
    std::string err, code;
    if (!CallApi(cfg, "GET", url, {}, nullptr, 0, &r, &err, &code) || !IsOk(r)) return;
    std::vector<std::string> files = JsonExtractArrayItems(r.body, "files");
    for (size_t i = kBackupsKept; i < files.size(); i++) {
        std::string id = JsonExtractString(files[i], "id");
        if (!IsValidGdriveId(id)) continue;
        HttpsReply d;
        CallApi(cfg, "DELETE", std::string(kFilesUrl) + "/" + id, {}, nullptr, 0, &d, &err, &code);
    }
}

// ---- device-code sign-in -----------------------------------------------------------------------

std::mutex g_link_mu;
std::string g_link_phase = "idle";
std::string g_link_user_code;
std::string g_link_url;
std::string g_link_error;
uint64_t g_link_generation = 0; // bumped on every start/cancel so an old polling thread stops itself

void SetLinkState(uint64_t generation, const char *phase, const std::string &error) {
    std::lock_guard<std::mutex> lock(g_link_mu);
    if (generation != g_link_generation) return;
    g_link_phase = phase;
    g_link_error = error;
    if (std::string(phase) != "waiting") {
        g_link_user_code.clear();
        g_link_url.clear();
    }
}

bool LinkStillCurrent(uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_link_mu);
    return generation == g_link_generation;
}

void PollForToken(const Config &cfg, uint64_t generation, std::string device_code, int interval, int lifetime) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(lifetime);
    interval = std::max(interval, 5);

    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < interval; i++) {
            if (!LinkStillCurrent(generation)) return;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!LinkStillCurrent(generation)) return;

        Stored s;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            s = LoadStored(cfg);
        }
        std::string body = FormBody({{"client_id", s.client_id},
                                     {"client_secret", s.client_secret},
                                     {"device_code", device_code},
                                     {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}});
        HttpsReply r;
        std::string net_err;
        if (!HttpsClient::Request("POST", kTokenUrl, kFormHeaders, body.data(), body.size(), &r, &net_err)) {
            continue; // a dropped connection mid-wait is not the end of the sign-in
        }
        if (r.status == 200) {
            std::string refresh = JsonExtractString(r.body, "refresh_token");
            if (refresh.empty()) {
                SetLinkState(generation, "error", "Google sent no refresh token");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(g_mu);
                Stored now = LoadStored(cfg);
                now.refresh_token = refresh;
                now.enabled = true; // linking is an explicit "yes, back up to Drive"
                if (!SaveStored(cfg, now, nullptr)) {
                    SetLinkState(generation, "error", "could not save the Google Drive sign-in");
                    return;
                }
                g_access_token = JsonExtractString(r.body, "access_token");
                g_access_expiry = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(std::max(60, JsonExtractInt(r.body, "expires_in", 3600) - 120));
            }
            LogEvent("Google Drive linked");
            SetLinkState(generation, "linked", "");
            return;
        }
        std::string error = JsonExtractString(r.body, "error");
        if (error == "authorization_pending") continue;
        if (error == "slow_down") {
            interval += 5;
            continue;
        }
        if (error == "access_denied") {
            SetLinkState(generation, "error", "Access was denied on the Google page.");
            return;
        }
        if (error == "expired_token") break;
        SetLinkState(generation, "error", ApiMessage(r));
        return;
    }
    SetLinkState(generation, "error", "The code expired before it was entered. Start again.");
}

} // namespace

// ---- public API --------------------------------------------------------------------------------

bool IsValidGdriveId(const std::string &id) {
    if (id.empty() || id.size() > 128) return false;
    for (unsigned char c : id) {
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

GdriveStatus GetGdriveStatus(const Config &cfg) {
    GdriveStatus out;
    Stored s;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = LoadStored(cfg);
    }
    out.configured = !s.client_id.empty() && !s.client_secret.empty();
    out.linked = !s.refresh_token.empty();
    out.enabled = s.enabled && out.linked;
    out.drive_only = s.drive_only;
    out.client_id = s.client_id;
    out.folder_name = kRootFolderName;
    {
        std::lock_guard<std::mutex> lock(g_link_mu);
        out.link_phase = g_link_phase;
        out.user_code = g_link_user_code;
        out.verification_url = g_link_url;
        out.link_error = g_link_error;
    }
    return out;
}

bool SetGdriveConfig(const Config &cfg, const std::string &client_id, const std::string &client_secret, bool enabled,
                     bool drive_only, std::string *err) {
    std::lock_guard<std::mutex> lock(g_mu);
    Stored s = LoadStored(cfg);

    if (!client_id.empty() && client_id != s.client_id) {
        if (!ValidCredential(client_id)) {
            if (err) *err = "that client ID has characters Google does not use";
            return false;
        }
        s.client_id = client_id;
        s.refresh_token.clear(); // a sign-in belongs to the client that obtained it
        g_access_token.clear();
        g_folder_ids.clear();
    }
    if (!client_secret.empty()) {
        if (!ValidCredential(client_secret)) {
            if (err) *err = "that client secret has characters Google does not use";
            return false;
        }
        if (client_secret != s.client_secret) {
            s.client_secret = client_secret;
            s.refresh_token.clear();
            g_access_token.clear();
        }
    }
    if (enabled && s.refresh_token.empty()) {
        if (err) *err = "link Google Drive before turning it on";
        return false;
    }
    s.enabled = enabled;
    s.drive_only = drive_only;
    return SaveStored(cfg, s, err);
}

bool StartGdriveLink(const Config &cfg, std::string *err) {
    Stored s;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = LoadStored(cfg);
    }
    if (s.client_id.empty() || s.client_secret.empty()) {
        if (err) *err = "enter the Google client ID and secret first";
        return false;
    }

    std::string body = FormBody({{"client_id", s.client_id}, {"scope", kScope}});
    HttpsReply r;
    std::string net_err;
    if (!HttpsClient::Request("POST", kDeviceCodeUrl, kFormHeaders, body.data(), body.size(), &r, &net_err)) {
        if (err) *err = net_err;
        return false;
    }
    if (r.status != 200) {
        if (err) *err = ApiMessage(r);
        return false;
    }
    std::string device_code = JsonExtractString(r.body, "device_code");
    std::string user_code = JsonExtractString(r.body, "user_code");
    std::string url = JsonExtractString(r.body, "verification_url");
    if (url.empty()) url = JsonExtractString(r.body, "verification_uri");
    if (device_code.empty() || user_code.empty() || url.empty()) {
        if (err) *err = "Google sent an incomplete sign-in code";
        return false;
    }
    int interval = JsonExtractInt(r.body, "interval", 5);
    int lifetime = JsonExtractInt(r.body, "expires_in", 1800);

    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(g_link_mu);
        generation = ++g_link_generation; // retires any earlier polling thread
        g_link_phase = "waiting";
        g_link_user_code = user_code;
        g_link_url = url;
        g_link_error.clear();
    }
    StartBackgroundThread([cfg, generation, device_code, interval, lifetime] {
        try {
            PollForToken(cfg, generation, device_code, interval, lifetime);
        } catch (...) {
            SetLinkState(generation, "error", "Google Drive sign-in failed unexpectedly");
        }
    });
    return true;
}

void CancelGdriveLink() {
    std::lock_guard<std::mutex> lock(g_link_mu);
    g_link_generation++;
    g_link_phase = "idle";
    g_link_user_code.clear();
    g_link_url.clear();
    g_link_error.clear();
}

void UnlinkGdrive(const Config &cfg) {
    CancelGdriveLink();
    std::string token_to_revoke;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        Stored s = LoadStored(cfg);
        token_to_revoke = s.refresh_token;
        s.refresh_token.clear();
        s.enabled = false;
        SaveStored(cfg, s, nullptr);
        g_access_token.clear();
        g_folder_ids.clear();
    }
    if (token_to_revoke.empty()) return;
    // Best effort: the local sign-in is already gone, so a network failure here changes nothing.
    std::string body = FormBody({{"token", token_to_revoke}});
    HttpsReply r;
    std::string ignored;
    HttpsClient::Request("POST", kRevokeUrl, kFormHeaders, body.data(), body.size(), &r, &ignored);
}

bool GdriveBackupActive(const Config &cfg) {
    std::lock_guard<std::mutex> lock(g_mu);
    Stored s = LoadStored(cfg);
    return s.enabled && !s.refresh_token.empty();
}

bool GdriveDriveOnly(const Config &cfg) {
    std::lock_guard<std::mutex> lock(g_mu);
    Stored s = LoadStored(cfg);
    return s.enabled && !s.refresh_token.empty() && s.drive_only;
}

bool GdriveUploadBackup(const Config &cfg, const std::string &title_id, const std::string &file_name,
                        const std::string &local_zip, std::string *err, std::string *code) {
    if (!ValidName(title_id) || !ValidName(file_name)) {
        SetFail(err, code, "invalid backup name", "gdrive_api");
        return false;
    }
    // Retried once after forgetting cached folder ids: the user may have deleted the folder in Drive.
    for (int attempt = 0; attempt < 2; attempt++) {
        std::string root_id, title_folder_id, e, c;
        if (!EnsureFolder(cfg, kRootFolderName, "", &root_id, &e, &c) ||
            !EnsureFolder(cfg, title_id, root_id, &title_folder_id, &e, &c)) {
            SetFail(err, code, e, c.c_str());
            return false;
        }
        if (UploadResumable(cfg, title_folder_id, title_id, file_name, local_zip, &e, &c)) {
            PruneRemote(cfg, title_folder_id);
            return true;
        }
        if (attempt == 0 && c == "gdrive_api") {
            ForgetFolders();
            continue;
        }
        SetFail(err, code, e, c.c_str());
        return false;
    }
    return false;
}

bool GdriveListBackups(const Config &cfg, std::vector<GdriveBackup> *out, std::string *err, std::string *code) {
    out->clear();
    std::string page_token;
    for (int page = 0; page < 5; page++) { // 5000 backups is far past the 5-per-title retention
        std::string url = std::string(kFilesUrl) + "?q=" + UrlEncode("mimeType='application/zip' and trashed=false") +
                          "&fields=" + UrlEncode("nextPageToken,files(id,name,size,createdTime,appProperties)") +
                          "&orderBy=" + UrlEncode("createdTime desc") + "&pageSize=1000&spaces=drive";
        if (!page_token.empty()) url += "&pageToken=" + UrlEncode(page_token);
        HttpsReply r;
        if (!CallApi(cfg, "GET", url, {}, nullptr, 0, &r, err, code)) return false;
        if (!IsOk(r)) {
            SetFail(err, code, ApiMessage(r), "gdrive_api");
            return false;
        }
        for (const auto &item : JsonExtractArrayItems(r.body, "files")) {
            GdriveBackup b;
            b.id = JsonExtractString(item, "id");
            b.title_id = JsonExtractString(item, "pmgr_title"); // only zips ProsperoMgr itself uploaded
            b.name = JsonExtractString(item, "name");
            if (!IsValidGdriveId(b.id) || !ValidName(b.title_id)) continue;
            b.size_bytes = std::strtoull(JsonExtractString(item, "size").c_str(), nullptr, 10);
            b.created_unix = ParseRfc3339(JsonExtractString(item, "createdTime"));
            out->push_back(std::move(b));
        }
        page_token = JsonExtractString(r.body, "nextPageToken");
        if (page_token.empty()) break;
    }
    return true;
}

bool GdriveDownload(const Config &cfg, const std::string &file_id, const std::string &local_path, std::string *err,
                    std::string *code) {
    if (!IsValidGdriveId(file_id)) {
        SetFail(err, code, "invalid Google Drive file id", "gdrive_api");
        return false;
    }
    std::string token;
    if (!AccessToken(cfg, &token, err, code)) return false;
    std::string net_err;
    if (!HttpsClient::DownloadToFile(std::string(kFilesUrl) + "/" + file_id + "?alt=media", local_path, &net_err,
                                     nullptr, nullptr, nullptr, token)) {
        SetFail(err, code, net_err, "gdrive_network");
        return false;
    }
    return true;
}

bool GdriveDelete(const Config &cfg, const std::string &file_id, std::string *err, std::string *code) {
    if (!IsValidGdriveId(file_id)) {
        SetFail(err, code, "invalid Google Drive file id", "gdrive_api");
        return false;
    }
    HttpsReply r;
    if (!CallApi(cfg, "DELETE", std::string(kFilesUrl) + "/" + file_id, {}, nullptr, 0, &r, err, code)) return false;
    if (!IsOk(r) && r.status != 404) { // already gone counts as deleted
        SetFail(err, code, ApiMessage(r), "gdrive_api");
        return false;
    }
    return true;
}

} // namespace prospero
