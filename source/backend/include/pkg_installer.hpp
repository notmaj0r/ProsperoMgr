#pragma once
// PKG installer — hands packages to a private elevated helper that calls the official
// AppInstUtil API. On failure, surfaces the real PS5 error code, not a generic one.

#include <functional>
#include <cstdint>
#include <string>

#include "config.hpp"

namespace prospero {

struct PkgInstallResult {
    // `ok` only means AppInstUtil accepted the request — the install runs on in the
    // system service and can still fail. Describe it as queued, never as installed.
    bool ok = false;
    std::string content_id;
    std::string error;
};

// Reads the 36-byte ContentID from a local .pkg's header — pure file
// parsing (public PKG format layout), works identically on host.
std::string ReadPkgContentId(const std::string &path);

struct PkgMetadata {
    std::string content_id;
    std::string title_id;
    std::string title_name;
    std::string version;
    std::string category; // "base", "update" or "dlc"; empty when undetermined
    long long icon_offset = 0; // icon0.png inside the file; 0 when the package has none
    uint32_t icon_size = 0;
};

// Walks the package's own entry table for param.sfo/param.json, so the UI can name
// a package instead of showing its filename. Read-only; never touches the installer.
PkgMetadata ReadPkgMetadata(const std::string &path);

struct InstalledTitle {
    bool installed = false; // the base game is on the console
    std::string version;    // highest base or patch version found, may be empty
};

// Looks for the game in the console's app folders, so the UI can warn before an upload.
InstalledTitle FindInstalledTitle(const std::string &title_id);

// Holds the cover art the browser read from a package until its live upload starts installing.
void StagePkgCover(const std::string &png);

// path must already be a local file (e.g. downloaded via PkgDownloadToTemp).
PkgInstallResult InstallPkgFile(const Config &cfg, const std::string &path);

// Queues a package the installer fetches itself from `uri`. There is no local file,
// so the header check InstallPkgFile does cannot run here.
PkgInstallResult InstallPkgUri(const Config &cfg, const std::string &uri);

// Removes expired files owned by the PKG staging directory. User-selected
// packages outside tmp_dir are never touched.
void CleanupStalePkgFiles(const Config &cfg);

// Stops a helper left by a previous ProsperoMgr build so the next install
// launches the freshly embedded version. Host builds are a no-op.
void ResetPkgInstallHelper();

// Downloads a .pkg URL into a scratch directory under tmp_dir — this step alone never
// touches AppInstUtil, InstallPkgFile still has to run after. `progress` is optional.
bool PkgDownloadToTemp(const Config &cfg, const std::string &url, std::string *local_path, std::string *err,
                       const std::function<void(long long, long long)> &progress = nullptr);

// ---- Background job status, for polling a progress bar ----
// AppInstUtil's install phase has no progress callback (ABI-level) — only the download phase has real byte counts.

enum class PkgJobPhase { kIdle, kDownloading, kInstalling, kQueued, kError };

struct PkgJobStatus {
    PkgJobPhase phase = PkgJobPhase::kIdle;
    long long loaded = 0;
    long long total = -1;   // -1 = unknown (no Content-Length, or not downloading)
    double speed_bps = 0;
    std::string content_id;
    std::string title; // the package's own name, when its metadata carries one
    std::string error;
    std::string staged_path; // the downloaded/uploaded file in tmp/pkg_downloads, for manual cleanup
};

PkgJobStatus GetPkgJobStatus();

// Starts a background download-then-install job (or reports busy if one's already running)
// and returns immediately; poll GetPkgJobStatus(). Empty `url` means `path` is already local.
bool StartPkgInstallJob(const Config &cfg, const std::string &url, const std::string &path, std::string *err);

// Deletes the most recent job's staged file. AppInstUtil may still be reading it after
// "queued", so this stays user-requested rather than automatic.
bool DeleteStagedPkgFile(const Config &cfg, std::string *err);

// Hands a finished live RAM upload (pkg_live_cache.hpp) to the installer — no file
// involved. A monitor thread polls real install status before releasing the RAM cache.
bool StartPkgInstallJobFromLiveSession(const Config &cfg, const std::string &session_id, std::string *err);

// Aborts an in-progress or already-queued live-session install. Safe to call with none
// active. The session's own monitor thread does the actual teardown once it notices.
void CancelLivePkgInstall();

// Stops a running download (or one waiting to validate) and tears down any live session.
// Once the console has accepted the package the install is out of our hands.
void CancelPkgInstall();

// True while a download/install job or live session is still ours to finish.
bool PkgInstallBusy();

// Removes an installed game through the console's own installer. The removal itself finishes in the
// background; this only reports whether the console accepted the request.
bool UninstallTitle(const Config &cfg, const std::string &title_id, std::string *err);

} // namespace prospero
