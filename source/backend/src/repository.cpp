#include "repository.hpp"

#include <mbedtls/sha256.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "file_jobs.hpp"
#include "https_client.hpp"
#include "job_queue.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "payload_registry.hpp"

namespace fs = std::filesystem;

namespace prospero {

namespace {

std::string UniqueSuffix() {
    static std::atomic<uint64_t> counter{0};
    return std::to_string(static_cast<long long>(getpid())) + "_" + std::to_string(counter.fetch_add(1));
}

std::string Sha256HexFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        return "";
    }

    // Heap, not stack: this inlines all the way up into the download job's
    // thread entry frame, and 64K there blows a worker thread's stack.
    std::vector<char> buf(65536);
    while (f.read(buf.data(), static_cast<std::streamsize>(buf.size())) || f.gcount() > 0) {
        if (mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char *>(buf.data()),
                                  static_cast<size_t>(f.gcount())) != 0) {
            mbedtls_sha256_free(&ctx);
            return "";
        }
    }

    unsigned char digest[32];
    if (mbedtls_sha256_finish(&ctx, digest) != 0) {
        mbedtls_sha256_free(&ctx);
        return "";
    }
    mbedtls_sha256_free(&ctx);

    char hex[65];
    for (size_t i = 0; i < sizeof(digest); i++) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, 64);
}

// ---- Installed-version tracking (payloads_dir/.versions.txt): plain "filename=version"
// lines, read back by payload_registry.cpp for the Payloads tab's installed-version badge.

std::string VersionsPath(const Config &cfg) { return cfg.payloads_dir + "/.versions.txt"; }

std::unordered_map<std::string, std::string> LoadVersions(const Config &cfg) {
    std::unordered_map<std::string, std::string> out;
    std::ifstream f(VersionsPath(cfg));
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

void RecordVersion(const Config &cfg, const std::string &filename, const std::string &version) {
    auto versions = LoadVersions(cfg);
    versions[filename] = version;

    std::ofstream f(VersionsPath(cfg), std::ios::trunc);
    for (const auto &[name, ver] : versions) f << name << '=' << ver << '\n';
}

// ---- Repository URL parsing: only ever inspects the URL text itself — never touches the
// network, so a malformed URL and an unreachable repository stay distinguishable.

bool ParseRepositoryUrlImpl(const std::string &raw, RepoRef *out, std::string *err) {
    if (raw.compare(0, 8, "https://") != 0) {
        if (err) *err = "Repository URL must start with https://";
        return false;
    }

    size_t host_start = 8;
    size_t path_start = raw.find('/', host_start);
    std::string host = path_start == std::string::npos ? raw.substr(host_start)
                                                         : raw.substr(host_start, path_start - host_start);
    if (host.empty()) {
        if (err) *err = "Invalid repository URL: missing host";
        return false;
    }
    if (path_start == std::string::npos) {
        if (err) *err = "Invalid repository URL: expected https://host/owner/repo";
        return false;
    }

    std::string path = raw.substr(path_start + 1);
    size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path = path.substr(0, cut);

    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        std::string part = slash == std::string::npos ? path.substr(pos) : path.substr(pos, slash - pos);
        if (!part.empty()) parts.push_back(part);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    if (parts.size() < 2) {
        if (err) *err = "Invalid repository URL: expected https://host/owner/repo";
        return false;
    }

    std::string owner = parts[0];
    std::string repo = parts[1];
    if (repo.size() > 4 && repo.compare(repo.size() - 4, 4, ".git") == 0) repo = repo.substr(0, repo.size() - 4);
    if (owner.empty() || repo.empty()) {
        if (err) *err = "Invalid repository URL: empty owner or repository name";
        return false;
    }

    out->host = host;
    out->owner = owner;
    out->repo = repo;
    out->provider = (host == "github.com") ? RepositoryProvider::kGitHub : RepositoryProvider::kGitea;
    return true;
}

// GitHub's releases-list endpoint takes "per_page"; Gitea/Forgejo's takes "limit" — same
// response shape, different query parameter name.
std::string ReleasesListApiUrl(const RepoRef &ref) {
    if (ref.provider == RepositoryProvider::kGitHub) {
        return "https://api.github.com/repos/" + ref.owner + "/" + ref.repo + "/releases?per_page=20";
    }
    return "https://" + ref.host + "/api/v1/repos/" + ref.owner + "/" + ref.repo + "/releases?limit=20";
}

struct ReleaseInfo {
    std::string tag;
    bool draft = false;
    bool prerelease = false;
};

// True only when `body` (whitespace aside) opens with '[' — lets a malformed response (HTML
// error page, an object instead of an array) be reported distinctly from a genuine empty list.
bool LooksLikeJsonArray(const std::string &body) {
    for (unsigned char c : body) {
        if (std::isspace(c)) continue;
        return c == '[';
    }
    return false;
}

std::vector<ReleaseInfo> ParseReleaseList(const std::string &json) {
    std::vector<ReleaseInfo> out;
    for (const auto &item : JsonExtractArrayItems(json, "")) {
        ReleaseInfo r;
        r.tag = JsonExtractString(item, "tag_name");
        r.draft = JsonExtractBool(item, "draft", false);
        r.prerelease = JsonExtractBool(item, "prerelease", false);
        if (r.tag.empty()) continue;
        out.push_back(std::move(r));
    }
    return out;
}

// True if `name` contains "ps4" (case-insensitive) — repos with separate PS4/PS5 builds
// (nanoDNS: nanodns.elf vs nanodns-ps4.elf) tag it this way; PS5-only ProsperoMgr should skip it.
bool LooksPs4Specific(const std::string &name) {
    std::string lower = name;
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("ps4") != std::string::npos;
}

// The first .elf/.bin/.self, preferring one that isn't PS4-specific over one
// that is — or a .zip (zip_only set) if that's all there is.
bool PickReleaseAsset(const std::string &release_json, ReleaseEntry *out) {
    std::string best_name, best_url, zip_name, zip_url;
    int best_size = 0, zip_size = 0;
    bool have_best = false, best_is_ps4 = false;

    for (const auto &asset : JsonExtractArrayItems(release_json, "assets")) {
        std::string name = JsonExtractString(asset, "name");
        std::string url = JsonExtractString(asset, "browser_download_url");
        if (name.empty() || url.empty()) continue;
        int size = JsonExtractInt(asset, "size", 0);

        if (IsSafePayloadFilename(name)) {
            bool is_ps4 = LooksPs4Specific(name);
            // Take the first candidate outright; after that, only replace it
            // by trading a PS4 build for a non-PS4 one.
            if (!have_best || (best_is_ps4 && !is_ps4)) {
                best_name = name;
                best_url = url;
                best_size = size;
                best_is_ps4 = is_ps4;
                have_best = true;
            }
            continue;
        }
        if (zip_name.empty() && name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0) {
            zip_name = name;
            zip_url = url;
            zip_size = size;
        }
    }

    if (have_best) {
        out->asset_name = best_name;
        out->asset_url = best_url;
        out->asset_size = static_cast<uint64_t>(std::max(0, best_size));
        out->zip_only = false;
        return true;
    }
    if (zip_name.empty()) return false;
    out->asset_name = zip_name;
    out->asset_url = zip_url;
    out->asset_size = static_cast<uint64_t>(std::max(0, zip_size));
    out->zip_only = true;
    return true;
}

// Moves a finished download into payloads_dir, tolerating a cross-filesystem
// rename() failure by falling back to copy+remove.
bool FinishDownload(const std::string &tmp_path, const fs::path &dest, std::string *err) {
    std::error_code ec;
    fs::rename(tmp_path, dest, ec);
    if (ec) {
        fs::copy_file(tmp_path, dest, fs::copy_options::overwrite_existing, ec);
        std::remove(tmp_path.c_str());
        if (ec) {
            if (err) *err = "failed to move downloaded file into place: " + ec.message();
            return false;
        }
    }
    return true;
}

// Last path segment of `url`, query string stripped — "https://.../v0.17/payload.elf" -> "payload.elf".
std::string FilenameFromUrl(const std::string &url) {
    std::string path = url;
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

bool InstallPayloadAsset(const Config &cfg, const std::string &url, std::string filename,
                         const std::string &checksum, const std::string &version, std::string *out_filename,
                         std::string *err, const DownloadProgressFn &progress, const std::string &repo_url = "") {
    if (filename.empty()) filename = FilenameFromUrl(url);
    if (!IsSafePayloadFilename(filename)) {
        if (err) *err = "URL doesn't end in a safe .elf/.bin/.self filename: " + filename;
        return false;
    }

    std::string tmp_path = cfg.tmp_dir + "/dl_" + UniqueSuffix() + "_" + filename;
    // tmp_dir is a plain folder under the Files browser — the user can delete it.
    std::error_code mkdir_ec;
    fs::create_directories(cfg.tmp_dir, mkdir_ec);
    // Streams to disk with a stall timeout — buffering in memory had none,
    // so a stalled read could hang the process indefinitely.
    if (!HttpsClient::DownloadToFile(url, tmp_path, err, progress)) {
        std::remove(tmp_path.c_str());
        return false;
    }

    if (!checksum.empty()) {
        std::string actual = Sha256HexFile(tmp_path);
        if (actual != checksum) {
            std::remove(tmp_path.c_str());
            if (err) *err = "checksum mismatch (expected " + checksum + ", got " + actual + ")";
            return false;
        }
    }

    fs::path dest = fs::path(cfg.payloads_dir) / filename;
    if (!FinishDownload(tmp_path, dest, err)) return false;
    if (!version.empty()) RecordVersion(cfg, filename, version);
    RecordPayloadSource(cfg, filename, "repository");
    if (!repo_url.empty()) RecordPayloadSourceUrl(cfg, filename, repo_url);
    if (out_filename) *out_filename = filename;
    return true;
}

// ---- Optional GitHub token (root_dir/github_token.txt) --------------------
// Raises api.github.com's unauthenticated 60/hour rate limit to 5000/hour.

std::string g_token_path;
std::mutex g_token_mu;

std::string TrimToken(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Caller holds g_token_mu.
std::string ReadTokenLocked() {
    if (g_token_path.empty()) return "";
    std::ifstream f(g_token_path);
    if (!f.is_open()) return "";
    std::string token;
    std::getline(f, token);
    return TrimToken(token);
}

// Only api.github.com calls carry it — Gitea instances use their own auth.
std::string GithubTokenFor(const RepoRef &ref) {
    if (ref.provider != RepositoryProvider::kGitHub) return "";
    std::lock_guard<std::mutex> lock(g_token_mu);
    return ReadTokenLocked();
}

// A stale or revoked token must not break repos that would work fine anonymously —
// retry once without it on a 401 specifically, not on rate limiting or other errors.
bool GetGithubApi(const std::string &url, const RepoRef &ref, HttpsResponse *resp, std::string *err,
                  NetErrorKind *kind) {
    std::string token = GithubTokenFor(ref);
    if (HttpsClient::Get(url, resp, err, kind, token)) return true;
    if (token.empty() || resp->status != 401) return false;
    LogEvent("GitHub token rejected (401); retrying " + url + " anonymously");
    return HttpsClient::Get(url, resp, err, kind, "");
}

} // namespace

bool ParseRepositoryUrl(const std::string &url, RepoRef *out, std::string *err) {
    return ParseRepositoryUrlImpl(url, out, err);
}

void InitializeGithubToken(const Config &cfg) {
    std::lock_guard<std::mutex> lock(g_token_mu);
    if (!g_token_path.empty()) return; // already initialized
    g_token_path = cfg.root_dir + "/github_token.txt";
}

bool HasGithubToken() {
    std::lock_guard<std::mutex> lock(g_token_mu);
    return !ReadTokenLocked().empty();
}

bool SetGithubToken(const std::string &token, std::string *err) {
    std::string trimmed = TrimToken(token);
    if (trimmed.empty()) {
        if (err) *err = "token is empty";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_token_mu);
    if (g_token_path.empty()) {
        if (err) *err = "not initialized";
        return false;
    }
    std::string tmp_path = g_token_path + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::trunc);
        if (!f.is_open()) {
            if (err) *err = "could not write token file";
            return false;
        }
        f << trimmed;
    }
    std::error_code ec;
    fs::rename(tmp_path, g_token_path, ec);
    if (ec) {
        if (err) *err = "could not save token: " + ec.message();
        return false;
    }
    return true;
}

void ClearGithubToken() {
    std::lock_guard<std::mutex> lock(g_token_mu);
    if (!g_token_path.empty()) std::remove(g_token_path.c_str());
}

LatestReleaseResult FetchLatestReleaseTag(const std::string &repo_url) {
    LatestReleaseResult result;

    RepoRef ref;
    std::string parse_err;
    if (!ParseRepositoryUrlImpl(repo_url, &ref, &parse_err)) {
        result.state = "invalid_url";
        result.error = parse_err;
        return result;
    }

    HttpsResponse resp;
    std::string fetch_err;
    NetErrorKind kind = NetErrorKind::kNone;
    if (!GetGithubApi(ReleasesListApiUrl(ref), ref, &resp, &fetch_err, &kind)) {
        result.state = "unreachable";
        result.error = fetch_err;
        return result;
    }

    if (!LooksLikeJsonArray(resp.body)) {
        result.state = "unreachable";
        result.error = "The server's response could not be understood as a release list.";
        return result;
    }

    auto releases = ParseReleaseList(resp.body);
    for (const auto &release : releases) {
        if (release.draft || release.prerelease) continue;
        result.tag = release.tag;
        result.state = "ok";
        return result;
    }

    result.state = "no_releases";
    result.error = "The repository was reached successfully, but it has no stable releases.";
    return result;
}

ReleaseListResult FetchReleases(const std::string &repo_url) {
    ReleaseListResult result;

    RepoRef ref;
    std::string parse_err;
    if (!ParseRepositoryUrlImpl(repo_url, &ref, &parse_err)) {
        result.state = "invalid_url";
        result.error = parse_err;
        return result;
    }

    HttpsResponse resp;
    std::string fetch_err;
    if (!GetGithubApi(ReleasesListApiUrl(ref), ref, &resp, &fetch_err, nullptr)) {
        result.state = "unreachable";
        result.error = fetch_err;
        return result;
    }
    if (!LooksLikeJsonArray(resp.body)) {
        result.state = "unreachable";
        result.error = "The server's response could not be understood as a release list.";
        return result;
    }

    for (const auto &item : JsonExtractArrayItems(resp.body, "")) {
        if (JsonExtractBool(item, "draft", false)) continue;
        std::string tag = JsonExtractString(item, "tag_name");
        if (tag.empty()) continue;

        ReleaseEntry entry;
        if (!PickReleaseAsset(item, &entry)) continue; // no installable asset in this release
        entry.tag = tag;
        entry.prerelease = JsonExtractBool(item, "prerelease", false);
        std::string published = JsonExtractString(item, "published_at");
        entry.published_at = published.size() >= 10 ? published.substr(0, 10) : "";
        result.releases.push_back(std::move(entry));
    }

    if (result.releases.empty()) {
        result.state = "no_releases";
        result.error = "The repository was reached successfully, but it has no installable releases or pre-releases.";
        return result;
    }
    result.state = "ok";
    return result;
}

// ---- Payload download job --------------------------------------------------

namespace {
std::mutex g_job_mu;
DownloadJobStatus g_job;
std::chrono::steady_clock::time_point g_window_at;
long long g_window_loaded = 0;
bool g_job_running = false;

// Speed over a moving window rather than since the job started — DNS, TLS and
// redirect hops move no bytes and would drag the average well below the truth.
void UpdateDownloadSpeedLocked(long long loaded) {
    auto now = std::chrono::steady_clock::now();
    double span = std::chrono::duration<double>(now - g_window_at).count();
    if (span < 0.5) return;
    double sample = static_cast<double>(loaded - g_window_loaded) / span;
    g_job.speed_bps = g_job.speed_bps > 0 ? g_job.speed_bps * 0.6 + sample * 0.4 : sample;
    g_window_at = now;
    g_window_loaded = loaded;
}
} // namespace

DownloadJobStatus GetDownloadJobStatus() {
    std::lock_guard<std::mutex> lock(g_job_mu);
    return g_job;
}

namespace {

struct DownloadJobArgs {
    Config cfg;
    std::string url;
    std::string filename;
    std::string checksum;
    std::string version;
    bool launch_after;
    PayloadRegistry *registry;
    std::string repo_url;
};

} // namespace

bool StartPayloadDownloadJob(const Config &cfg, const std::string &url, const std::string &filename,
                              const std::string &checksum, const std::string &version, bool launch_after,
                              PayloadRegistry *registry, std::string *err, const std::string &repo_url) {
    {
        std::lock_guard<std::mutex> lock(g_job_mu);
        if (g_job_running) {
            if (err) *err = "a download is already in progress";
            return false;
        }
        g_job_running = true;
        g_job = DownloadJobStatus{};
        g_job.phase = DownloadJobPhase::kDownloading;
        g_window_at = std::chrono::steady_clock::now();
        g_window_loaded = 0;
    }

    LogEvent("Payload download started: " + (filename.empty() ? FilenameFromUrl(url) : filename));

    RunOnWorker([args = DownloadJobArgs{cfg, url, filename, checksum, version, launch_after, registry, repo_url}]() {
        try {
            std::string out_filename, install_err;
            bool ok = InstallPayloadAsset(args.cfg, args.url, args.filename, args.checksum, args.version,
                                          &out_filename, &install_err, [](long long loaded, long long total) {
                                              std::lock_guard<std::mutex> lock(g_job_mu);
                                              UpdateDownloadSpeedLocked(loaded);
                                              g_job.loaded = loaded;
                                              g_job.total = total;
                                          }, args.repo_url);

            if (!ok) {
                LogEvent("Payload download failed: " + install_err);
                std::lock_guard<std::mutex> lock(g_job_mu);
                g_job.phase = DownloadJobPhase::kError;
                g_job.error = install_err;
                g_job_running = false;
                return;
            }

            bool launched = false;
            if (args.launch_after && args.registry) {
                {
                    std::lock_guard<std::mutex> lock(g_job_mu);
                    g_job.phase = DownloadJobPhase::kInstalling;
                }
                std::string launch_err;
                launched = args.registry->Launch(out_filename, &launch_err);
            }

            std::lock_guard<std::mutex> lock(g_job_mu);
            g_job.phase = DownloadJobPhase::kDone;
            g_job.filename = out_filename;
            g_job.launched = launched;
            g_job_running = false;
            LogEvent("Payload download job completed: " + out_filename);
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(g_job_mu);
            g_job.phase = DownloadJobPhase::kError;
            g_job.error = std::string("download worker failed: ") + e.what();
            g_job_running = false;
        } catch (...) {
            std::lock_guard<std::mutex> lock(g_job_mu);
            g_job.phase = DownloadJobPhase::kError;
            g_job.error = "download worker failed unexpectedly";
            g_job_running = false;
        }
    });

    return true;
}

} // namespace prospero
