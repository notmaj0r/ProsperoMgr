#include "payload_catalog.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>

#include "https_client.hpp"
#include "job_queue.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "repository.hpp"

namespace prospero {

namespace {

// Curated at build time from the actual project READMEs/GitHub metadata.
// A-Z by developer.
const std::vector<PayloadCatalogEntry> kCatalog = {
    {"aydencharles", "onionHEN", "All-in-one HEN and toolbox for the PS5.",
     "https://github.com/aydencharles/onionHEN"},

    {"BestPig", "BackPork", "Lets you sideload system libraries into PS5 games.",
     "https://github.com/BestPig/BackPork"},

    {"drakmor", "fan_target", "Keeps the fan at a chosen target temperature while automatic fan control stays on.",
     "https://github.com/drakmor/fan_target"},
    {"drakmor", "nanoDNS", "DNS proxy for PS4/PS5 with local domain overrides.",
     "https://github.com/drakmor/nanoDNS"},
    {"drakmor", "ps5-hwinfo", "Reports PS5 hardware telemetry and runs read-only disk benchmarks.",
     "https://github.com/drakmor/ps5-hwinfo"},
    {"drakmor", "ShadowMountPlus", "Automated background auto-mounter for game dumps on jailbroken PS5s.",
     "https://github.com/drakmor/ShadowMountPlus"},

    {"earthonion", "garlic-savemgr", "PS5 save decrypt/encrypt/browse with an embedded web UI.",
     "https://git.etawen.dev/earthonion/garlic-savemgr"},
    {"earthonion", "garlic-worker", "Save decrypt/encrypt/resign/re-region worker for garlicsaves.com.",
     "https://git.etawen.dev/earthonion/garlic-worker"},
    {"earthonion", "np-account-restore", "Restores a PSN account's config.dat/auth.dat onto a jailbroken console.",
     "https://git.etawen.dev/earthonion/np-account-restore"},
    {"earthonion", "np-fake-signin", "Fakes PSN sign-in for the foreground user via NP files and registry patches.",
     "https://git.etawen.dev/earthonion/np-fake-signin"},

    {"EchoStretch", "kstuff-lite", "Kernel-level patch payload for fake-signed package execution and automatic game mounting.",
     "https://github.com/EchoStretch/kstuff-lite"},
    {"EchoStretch", "ps5-app-dumper", "Dumps PS5 app/game files from pfsmnt to a USB drive.",
     "https://github.com/EchoStretch/ps5-app-dumper"},

    {"etaHEN", "etaHEN", "All-in-one homebrew enabler payload.", "https://github.com/etaHEN/etaHEN"},

    {"idlesauce", "PS5-SELF-Decrypter", "Decrypts PS5 SELF files from the filesystem to a USB drive.",
     "https://github.com/idlesauce/PS5-SELF-Decrypter"},

    {"maj0r", "CheatRunner", "A web-based Cheat Trainer.", "https://github.com/notmaj0r/CheatRunner"},
    {"maj0r", "ProsperoMgr", "A web-based all-in-one homebrew manager — this project.",
     "https://github.com/notmaj0r/ProsperoMgr"},

    {"OpenSourcereR-dev", "ps5debug-NG", "Userland TCP debugger server for PS5, wire-compatible with ps5debug.",
     "https://github.com/OpenSourcereR-dev/ps5debug-NG"},

    {"owendswang", "ps5-fan-control", "Daemon that sets the system fan temperature threshold.",
     "https://github.com/owendswang/ps5-fan-control"},
    {"owendswang", "ps5-web-file-manager", "File manager with a web UI, made for copying game dumps between storages.",
     "https://github.com/owendswang/ps5-web-file-manager"},

    {"phantomptr", "ps5upload", "Fast PS5 file transfers and package installs from a desktop app.",
     "https://github.com/phantomptr/ps5upload"},

    {"Phoenixx1202", "Spectrum Library",
     "Lightweight content manager for browsing, downloading, and tracking supported game files.",
     "https://github.com/Phoenixx1202/Spectrum-Library"},

    {"PLK", "PS5-Lapy-JB-Daemon", "Standalone jailbreak-on-demand daemon for PS5 apps without requiring etaHEN.",
     "https://github.com/itsPLK/PS5-Lapy-JB-Daemon"},
    {"PLK", "ps5-payload-manager", "Web-based dashboard to manage, import, and auto-load PS5 payloads.",
     "https://github.com/itsPLK/ps5-payload-manager"},
    {"PLK", "ps5-pkg-manager", "Browse and install packages from USB or a network share.",
     "https://github.com/itsPLK/ps5-pkg-manager"},
    {"PLK", "ps5-webkit-autoloader", "Autoloads a payload through the PS5 browser's WebKit exploit chain.",
     "https://github.com/itsPLK/ps5-webkit-autoloader"},

    {"PS5-Linux", "ps5-linux-loader", "Turns a jailbroken PS5 into a Linux PC via HV exploits.",
     "https://github.com/ps5-linux/ps5-linux-loader"},

    {"PS5-Payload-Dev", "ftpsrv", "FTP server for jailbroken PS4/PS5, port 2121.",
     "https://github.com/ps5-payload-dev/ftpsrv"},
    {"PS5-Payload-Dev", "elfldr", "ELF loader that accepts payloads on port 9021.",
     "https://github.com/ps5-payload-dev/elfldr"},
    {"PS5-Payload-Dev", "websrv", "Web server for launching homebrew from a browser, port 8080.",
     "https://github.com/ps5-payload-dev/websrv"},
    {"PS5-Payload-Dev", "shsrv", "Telnet-like shell server, port 2323.",
     "https://github.com/ps5-payload-dev/shsrv"},
    {"PS5-Payload-Dev", "offact", "Activates offline accounts on a jailbroken PS5.",
     "https://github.com/ps5-payload-dev/offact"},
    {"PS5-Payload-Dev", "gdbsrv", "GDB server for jailbroken PS4/PS5, port 2159.",
     "https://github.com/ps5-payload-dev/gdbsrv"},
    {"PS5-Payload-Dev", "klogsrv", "Streams /dev/klog to sockets on port 3232.",
     "https://github.com/ps5-payload-dev/klogsrv"},

    {"seregonwar", "zftpd", "High-performance FTP daemon for jailbroken consoles.",
     "https://github.com/seregonwar/zftpd"},

    {"Sonic-Iso", "elf-arsenal", "All-in-one PS5 utility.", "https://git.etawen.dev/soniciso/elf-arsenal"},
};

std::string ToLowerAscii(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool SameUrl(const std::string &a, const std::string &b) { return ToLowerAscii(a) == ToLowerAscii(b); }

// owner (lowercased) -> display developer name an existing built-in entry from that owner
// uses, e.g. "notmaj0r" -> "maj0r". Built once from kCatalog — no separate hardcoded list.
const std::unordered_map<std::string, std::string> &OwnerAliasMap() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> out;
        for (const auto &entry : kCatalog) {
            RepoRef ref;
            std::string ignored;
            if (ParseRepositoryUrl(entry.url, &ref, &ignored)) out[ToLowerAscii(ref.owner)] = entry.developer;
        }
        return out;
    }();
    return map;
}

std::string CanonicalDeveloper(const std::string &owner) {
    const auto &alias = OwnerAliasMap();
    auto it = alias.find(ToLowerAscii(owner));
    return it != alias.end() ? it->second : owner;
}

// Best-effort — a repository is still worth tracking even if the host is briefly unreachable
// when added; the URL is the permanent identity, not this metadata.
void FetchRepoMetadata(const RepoRef &ref, std::string *name, std::string *description) {
    std::string api_url = ref.provider == RepositoryProvider::kGitHub
                               ? "https://api.github.com/repos/" + ref.owner + "/" + ref.repo
                               : "https://" + ref.host + "/api/v1/repos/" + ref.owner + "/" + ref.repo;
    HttpsResponse resp;
    std::string err;
    if (!HttpsClient::Get(api_url, &resp, &err, nullptr)) return;
    std::string fetched_name = JsonExtractString(resp.body, "name");
    std::string fetched_desc = JsonExtractString(resp.body, "description");
    if (!fetched_name.empty() && name) *name = fetched_name;
    if (!fetched_desc.empty() && description) *description = fetched_desc;
}

// ---- Custom entries: persisted under <root>/custom_repositories.json -----

std::mutex g_custom_mu;
std::vector<PayloadCatalogEntry> g_custom;
std::string g_custom_path;

void LoadCustomLocked() {
    std::ifstream in(g_custom_path, std::ios::binary);
    if (!in) return;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (const auto &item : JsonExtractArrayItems(json, "")) {
        PayloadCatalogEntry e;
        e.url = JsonExtractString(item, "url");
        e.developer = JsonExtractString(item, "developer");
        e.name = JsonExtractString(item, "name");
        e.description = JsonExtractString(item, "description");
        e.custom = true;
        if (!e.url.empty() && !e.developer.empty() && !e.name.empty()) g_custom.push_back(std::move(e));
    }
}

bool PersistCustomLocked(std::string *err) {
    std::vector<std::string> items;
    for (const auto &e : g_custom) {
        items.push_back(JsonWriter()
                             .Str("url", e.url)
                             .Str("developer", e.developer)
                             .Str("name", e.name)
                             .Str("description", e.description)
                             .Done());
    }
    std::string temp = g_custom_path + ".tmp";
    std::ofstream out(temp, std::ios::trunc | std::ios::binary);
    if (!out) {
        if (err) *err = "could not write custom repository file";
        return false;
    }
    out << JsonWriter::Array(items);
    out.flush();
    out.close();
    if (!out) {
        if (err) *err = "could not flush custom repository file";
        return false;
    }
    if (std::rename(temp.c_str(), g_custom_path.c_str()) != 0) {
        if (err) *err = "could not commit custom repository file";
        return false;
    }
    return true;
}

// Per-URL cache: GitHub's unauthenticated API allows 60 requests/hour per IP.
struct CacheEntry {
    CatalogVersionStatus status;
    std::chrono::steady_clock::time_point fetched_at;
};

constexpr auto kCacheTtl = std::chrono::minutes(45);
constexpr auto kMinRefreshInterval = std::chrono::seconds(60);

std::mutex g_cache_mu;
std::unordered_map<std::string, CacheEntry> g_cache;
std::string g_cache_path;

// Caller holds g_cache_mu. Kept on disk so a reboot doesn't force a fresh
// sweep of every entry just to show the version badges again.
void PersistCacheLocked() {
    if (g_cache_path.empty()) return;
    std::vector<std::string> items;
    for (const auto &[url, entry] : g_cache) {
        items.push_back(JsonWriter()
                             .Str("url", url)
                             .Str("version", entry.status.version)
                             .Str("state", entry.status.state)
                             .Str("checked_at", std::to_string(entry.status.checked_at_unix))
                             .Done());
    }
    std::ofstream out(g_cache_path, std::ios::trunc);
    if (out) out << JsonWriter::Array(items);
}

void LoadCacheLocked() {
    std::ifstream in(g_cache_path, std::ios::binary);
    if (!in) return;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (const auto &item : JsonExtractArrayItems(json, "")) {
        std::string url = JsonExtractString(item, "url");
        if (url.empty()) continue;
        CacheEntry entry;
        entry.status.version = JsonExtractString(item, "version");
        entry.status.state = JsonExtractString(item, "state");
        entry.status.checked_at_unix = strtoll(JsonExtractString(item, "checked_at").c_str(), nullptr, 10);
        // No steady_clock origin across a reboot, so the in-session cooldown
        // starts fresh; checked_at_unix is what the UI actually shows.
        entry.fetched_at = std::chrono::steady_clock::time_point{};
        g_cache[url] = entry;
    }
}

// One upstream request, written straight through to the cache file.
CatalogVersionStatus FetchAndCache(const std::string &url) {
    auto result = FetchLatestReleaseTag(url);
    CatalogVersionStatus status{result.tag, result.state, result.error, static_cast<int64_t>(std::time(nullptr))};
    std::lock_guard<std::mutex> lock(g_cache_mu);
    g_cache[url] = CacheEntry{status, std::chrono::steady_clock::now()};
    PersistCacheLocked();
    return status;
}

std::mutex g_refresh_mu;
CatalogRefreshStatus g_refresh;

} // namespace

void InitializePayloadCatalog(const Config &cfg) {
    {
        std::lock_guard<std::mutex> lock(g_cache_mu);
        g_cache_path = cfg.root_dir + "/catalog_versions.json";
        LoadCacheLocked();
    }
    std::lock_guard<std::mutex> lock(g_custom_mu);
    if (!g_custom_path.empty()) return; // already initialized
    g_custom_path = cfg.root_dir + "/custom_repositories.json";
    LoadCustomLocked();
}

CatalogVersionStatus RefreshCatalogEntry(const std::string &url) { return FetchAndCache(url); }

std::vector<PayloadCatalogEntry> GetPayloadCatalog() {
    std::vector<PayloadCatalogEntry> out(kCatalog.begin(), kCatalog.end());
    std::lock_guard<std::mutex> lock(g_custom_mu);
    out.insert(out.end(), g_custom.begin(), g_custom.end());
    return out;
}

bool AddCustomRepository(const std::string &raw_url, PayloadCatalogEntry *out_entry, std::string *err) {
    RepoRef ref;
    std::string parse_err;
    if (!ParseRepositoryUrl(raw_url, &ref, &parse_err)) {
        if (err) *err = parse_err;
        return false;
    }
    // Reconstructed, not the raw input — trailing slashes/".git"/casing all
    // collapse to the same normalized form so they can't become duplicates.
    std::string normalized = "https://" + ref.host + "/" + ref.owner + "/" + ref.repo;

    std::lock_guard<std::mutex> lock(g_custom_mu);
    for (const auto &entry : kCatalog) {
        if (SameUrl(entry.url, normalized)) {
            if (err) *err = "This repository is already in your directory.";
            return false;
        }
    }
    for (const auto &entry : g_custom) {
        if (SameUrl(entry.url, normalized)) {
            if (err) *err = "This repository is already in your directory.";
            return false;
        }
    }

    PayloadCatalogEntry entry;
    entry.url = normalized;
    entry.name = ref.repo;
    entry.developer = CanonicalDeveloper(ref.owner);
    entry.description = "";
    entry.custom = true;
    FetchRepoMetadata(ref, &entry.name, &entry.description);

    g_custom.push_back(entry);
    if (!PersistCustomLocked(err)) {
        g_custom.pop_back();
        return false;
    }
    LogEvent("Custom payload repository added: " + entry.developer + "/" + entry.name);
    if (out_entry) *out_entry = entry;
    return true;
}

bool RemoveCustomRepository(const std::string &url, std::string *err) {
    std::lock_guard<std::mutex> lock(g_custom_mu);
    for (auto it = g_custom.begin(); it != g_custom.end(); ++it) {
        if (!SameUrl(it->url, url)) continue;
        PayloadCatalogEntry removed = *it;
        g_custom.erase(it);
        if (!PersistCustomLocked(err)) return false;
        LogEvent("Custom payload repository removed: " + removed.developer + "/" + removed.name);
        return true;
    }
    if (err) *err = "not a custom repository (built-in entries cannot be removed)";
    return false;
}

std::unordered_map<std::string, CatalogVersionStatus> GetCachedCatalogVersions() {
    std::lock_guard<std::mutex> lock(g_cache_mu);
    std::unordered_map<std::string, CatalogVersionStatus> out;
    for (const auto &[url, entry] : g_cache) out[url] = entry.status;
    return out;
}

CatalogRefreshStatus GetCatalogRefreshStatus() {
    CatalogRefreshStatus status;
    {
        std::lock_guard<std::mutex> lock(g_refresh_mu);
        status = g_refresh;
    }
    {
        std::lock_guard<std::mutex> lock(g_cache_mu);
        status.have_cache = !g_cache.empty();
    }
    return status;
}

bool StartCatalogRefreshJob(bool force, std::string *err) {
    auto catalog = GetPayloadCatalog(); // snapshot — built-in + custom, as of now

    {
        std::lock_guard<std::mutex> lock(g_refresh_mu);
        if (g_refresh.running) return true;
        g_refresh = CatalogRefreshStatus{};
        g_refresh.running = true;
        g_refresh.total = static_cast<int>(catalog.size());
    }

    RunOnWorker([force, catalog = std::move(catalog)]() {
        auto now = std::chrono::steady_clock::now();
        int completed = 0;

        // Checked independently so one unreachable repo doesn't block the rest;
        // repos sharing a URL are fetched once.
        for (const auto &entry : catalog) {
            CacheEntry cached;
            bool have_cached = false;
            {
                std::lock_guard<std::mutex> lock(g_cache_mu);
                auto it = g_cache.find(entry.url);
                if (it != g_cache.end()) {
                    cached = it->second;
                    have_cached = true;
                }
            }

            bool within_cooldown = have_cached && (now - cached.fetched_at) < kMinRefreshInterval;
            bool within_ttl = have_cached && (now - cached.fetched_at) < kCacheTtl;
            if (!(within_cooldown || (!force && within_ttl))) FetchAndCache(entry.url);

            completed++;
            std::lock_guard<std::mutex> lock(g_refresh_mu);
            g_refresh.completed = completed;
        }

        LogEvent("Payload catalog version check complete: " + std::to_string(completed) + " entr" +
                 (completed == 1 ? "y" : "ies"));
        std::lock_guard<std::mutex> lock(g_refresh_mu);
        g_refresh.running = false;
    });

    return true;
}

} // namespace prospero
