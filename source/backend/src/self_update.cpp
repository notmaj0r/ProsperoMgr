#include "self_update.hpp"

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "elfldr_client.hpp"
#include "https_client.hpp"
#include "job_queue.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "notify.hpp"
#include "notify_text.hpp"

namespace prospero {
namespace {

constexpr const char *kLatestReleaseUrl = "https://api.github.com/repos/notmaj0r/ProsperoMgr/releases/latest";
constexpr const char *kAssetUrlPrefix = "https://github.com/notmaj0r/ProsperoMgr/releases/download/";
constexpr const char *kAssetName = "ProsperoMgr.elf";
constexpr auto kStartupDelay = std::chrono::seconds(90);
constexpr int kCheckEveryTicks = 6 * 60; // one tick per minute
constexpr long long kMinElfBytes = 1024 * 1024;

std::mutex g_mu;
UpdateStatus g_status;
std::string g_asset_url;
std::string g_failed_latest; // a version that already failed to install, so auto mode doesn't loop on it
std::string g_root;
std::function<bool()> g_busy;
std::atomic<bool> g_checking{false};

std::string AutoFlagPath() { return g_root + "/auto_update.txt"; }
std::string UpdateDir() { return g_root + "/update"; }
std::string UpdateElfPath() { return UpdateDir() + "/ProsperoMgr.elf"; }
// Sits next to the downloaded ELF so an older copy can read its version without running it.
std::string PendingVersionPath() { return UpdateDir() + "/ProsperoMgr.version"; }
std::string UsedVersionPath() { return UpdateDir() + "/ProsperoMgr.version.used"; }

std::string ReadFirstLine(const std::string &path) {
    std::ifstream f(path);
    std::string line;
    if (f.is_open()) std::getline(f, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

std::vector<int> ParseVersion(const std::string &text) {
    std::vector<int> parts;
    int value = 0;
    bool in_number = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            in_number = true;
        } else if (c == '.' && in_number) {
            parts.push_back(value);
            value = 0;
            in_number = false;
        } else if (in_number) {
            break;
        }
    }
    if (in_number) parts.push_back(value);
    return parts;
}

bool IsNewer(const std::string &latest, const std::string &current) {
    return ParseVersion(latest) > ParseVersion(current);
}

bool CheckForUpdate() {
    if (g_checking.exchange(true)) return false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_status.state == "downloading" || g_status.state == "launching") {
            g_checking = false;
            return false;
        }
        g_status.state = "checking";
        g_status.error.clear();
    }

    HttpsResponse resp;
    std::string err;
    bool ok = HttpsClient::Get(kLatestReleaseUrl, &resp, &err);
    std::string tag, notes, asset_url;
    if (ok) {
        tag = JsonExtractString(resp.body, "tag_name");
        notes = JsonExtractString(resp.body, "body");
        for (const auto &asset : JsonExtractArrayItems(resp.body, "assets")) {
            if (JsonExtractString(asset, "name") != kAssetName) continue;
            asset_url = JsonExtractString(asset, "browser_download_url");
            break;
        }
    }

    std::lock_guard<std::mutex> lock(g_mu);
    g_status.state = "idle";
    if (!ok) {
        g_status.error = err.empty() ? "could not reach GitHub" : err;
    } else if (tag.empty()) {
        g_status.error = "unexpected release data from GitHub";
    } else {
        g_status.latest = tag.size() && tag[0] == 'v' ? tag.substr(1) : tag;
        g_status.notes = notes;
        bool usable = asset_url.rfind(kAssetUrlPrefix, 0) == 0;
        g_asset_url = usable ? asset_url : "";
        g_status.available = usable && IsNewer(g_status.latest, g_status.current);
        if (!usable && IsNewer(g_status.latest, g_status.current)) g_status.error = "the release has no ProsperoMgr.elf file";
    }
    g_checking = false;
    return ok;
}

bool HasElfMagic(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    char magic[4] = {};
    f.read(magic, 4);
    return f.gcount() == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

void FailInstall(const std::string &message) {
    LogEvent("Self-update failed: " + message);
    std::lock_guard<std::mutex> lock(g_mu);
    g_failed_latest = g_status.latest;
    g_status.state = "error";
    g_status.error = message;
}

void RunInstall(std::string url, std::string version) {
    std::string dir = UpdateDir();
    mkdir(dir.c_str(), 0755);
    std::string part = dir + "/ProsperoMgr.elf.part";
    std::string final_path = dir + "/ProsperoMgr.elf";

    std::string err;
    bool ok = HttpsClient::DownloadToFile(url, part, &err, [](long long loaded, long long total) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.loaded = loaded;
        g_status.total = total;
    });
    if (!ok) {
        std::remove(part.c_str());
        FailInstall(err.empty() ? "download failed" : err);
        return;
    }

    struct stat st {};
    if (stat(part.c_str(), &st) != 0 || st.st_size < kMinElfBytes || !HasElfMagic(part) ||
        std::rename(part.c_str(), final_path.c_str()) != 0) {
        std::remove(part.c_str());
        FailInstall("the downloaded file is not a valid ProsperoMgr.elf");
        return;
    }

    {
        std::ofstream marker(PendingVersionPath(), std::ios::trunc);
        marker << version << "\n";
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = "launching";
    }
    Notify("Prospero Manager\n" + NotifyText(Msg::kUpdating, {{"version", version}}));
    LogEvent("Self-update: launching v" + version);
    // The new copy replaces this process at startup, so a success never returns here.
    std::rename(PendingVersionPath().c_str(), UsedVersionPath().c_str());
    ElfldrClient client(Config::kElfldrHost, Config::kElfldrPort);
    if (!client.LaunchFile(final_path, &err)) {
        std::rename(UsedVersionPath().c_str(), PendingVersionPath().c_str());
        FailInstall(err.empty() ? "could not start the new version" : err);
    }
}

// On startup: the updated copy keeps its marker for later boots, and an older copy
// hands over to a newer ELF that is already on disk, without needing the network.
void AdoptPendingUpdate() {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (ReadFirstLine(UsedVersionPath()) == kVersion) {
        std::rename(UsedVersionPath().c_str(), PendingVersionPath().c_str());
        return;
    }

    std::string version = ReadFirstLine(PendingVersionPath());
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (version.empty() || !g_status.auto_update || !IsNewer(version, g_status.current)) return;
    }
    struct stat st {};
    std::string elf = UpdateElfPath();
    if (stat(elf.c_str(), &st) != 0 || st.st_size < kMinElfBytes || !HasElfMagic(elf)) return;
    if (g_busy && g_busy()) return;

    LogEvent("Self-update: starting v" + version + " found in the update folder");
    Notify("Prospero Manager\n" + NotifyText(Msg::kUpdating, {{"version", version}}));
    // The marker moves aside first, so a copy that turns out not to be this version cannot loop.
    std::rename(PendingVersionPath().c_str(), UsedVersionPath().c_str());
    std::string err;
    ElfldrClient client(Config::kElfldrHost, Config::kElfldrPort);
    if (!client.LaunchFile(elf, &err)) {
        std::rename(UsedVersionPath().c_str(), PendingVersionPath().c_str());
        LogEvent("Self-update: could not start the saved update: " + err);
    }
}

void UpdateLoop() {
    std::this_thread::sleep_for(kStartupDelay);
    int tick = kCheckEveryTicks;
    for (;;) {
        if (++tick >= kCheckEveryTicks) {
            tick = 0;
            CheckForUpdate();
        }
        std::string err;
        bool run_auto = false;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            run_auto = g_status.auto_update && g_status.available && g_status.state == "idle" &&
                       g_status.latest != g_failed_latest;
        }
        if (run_auto && !(g_busy && g_busy())) StartUpdateInstall(&err);
        std::this_thread::sleep_for(std::chrono::minutes(1));
    }
}

} // namespace

void InitializeSelfUpdate(const Config &cfg, std::function<bool()> busy) {
    g_root = cfg.root_dir;
    g_busy = std::move(busy);
    g_status.current = kVersion;
    g_status.state = "idle";
    std::ifstream f(AutoFlagPath());
    std::string flag;
    if (f.is_open() && std::getline(f, flag)) g_status.auto_update = flag != "0";
    StartBackgroundThread(AdoptPendingUpdate);
    StartBackgroundThread(UpdateLoop);
}

UpdateStatus GetUpdateStatus() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_status;
}

void CheckForUpdateAsync() {
    StartBackgroundThread([]() { CheckForUpdate(); });
}

bool StartUpdateInstall(std::string *err) {
    std::string url, version;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_status.state == "downloading" || g_status.state == "launching" || g_status.state == "checking") {
            if (err) *err = "an update is already in progress";
            return false;
        }
        if (!g_status.available || g_asset_url.empty()) {
            if (err) *err = "no update is available";
            return false;
        }
        url = g_asset_url;
        version = g_status.latest;
        g_status.state = "downloading";
        g_status.error.clear();
        g_status.loaded = 0;
        g_status.total = -1;
    }
    StartBackgroundThread([url, version]() { RunInstall(url, version); });
    return true;
}

void SetAutoUpdate(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.auto_update = enabled;
    }
    std::ofstream f(AutoFlagPath(), std::ios::trunc);
    f << (enabled ? "1" : "0") << "\n";
}

} // namespace prospero
