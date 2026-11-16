#pragma once
// Google Drive as a save-backup destination.
//
// Sign-in uses Google's OAuth "device" flow: the console shows a short code, the user types it on
// google.com/device from any phone or PC, and the console polls until Google hands back a token.
// Only the narrow drive.file scope is requested, so ProsperoMgr can see just the files it created
// itself (the "ProsperoPS5" folder and the zips in it) and nothing else in the user's Drive.
//
// Layout in Drive:  ProsperoPS5/<title_id>/<timestamp>.zip   (same zips as the USB/internal backups)
//
// The user supplies their own OAuth client (client id + secret, type "TVs and Limited Input
// devices") from the Google Cloud console, so no credential ships inside the ELF.

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"

namespace prospero {

// Backup paths handed to the UI for Drive entries: "gdrive://<file id>".
inline constexpr const char *kGdrivePathPrefix = "gdrive://";
// Destination label used in backup results and the list.
inline constexpr const char *kGdriveDestination = "gdrive:";

struct GdriveStatus {
    bool configured = false; // client id and secret saved
    bool linked = false;     // a refresh token is stored
    bool enabled = false;    // automatic backups also go to Drive
    bool drive_only = false; // ...and nothing is written locally unless the upload fails
    std::string client_id;   // not a secret; the secret is never sent back to the UI
    std::string folder_name; // "ProsperoPS5"
    // Sign-in in progress: idle | waiting | linked | error
    std::string link_phase = "idle";
    std::string user_code;
    std::string verification_url;
    std::string link_error;
};

GdriveStatus GetGdriveStatus(const Config &cfg);

// `client_secret` empty keeps the stored one. Changing the client id drops the current sign-in.
bool SetGdriveConfig(const Config &cfg, const std::string &client_id, const std::string &client_secret, bool enabled,
                     bool drive_only, std::string *err);

// Asks Google for a device code and starts polling in the background; progress shows in GetGdriveStatus().
bool StartGdriveLink(const Config &cfg, std::string *err);
void CancelGdriveLink();
// Forgets the sign-in on the console and asks Google to revoke it.
void UnlinkGdrive(const Config &cfg);

// True when Drive is linked and switched on, i.e. a backup should also be uploaded there.
bool GdriveBackupActive(const Config &cfg);
// True when Drive is active AND the user asked for Drive to be the only destination.
bool GdriveDriveOnly(const Config &cfg);

// A Drive file id is [A-Za-z0-9_-]+ ; anything else coming from a client is refused.
bool IsValidGdriveId(const std::string &id);

// `code` (optional) is a short machine-readable reason for the UI: gdrive_not_linked, gdrive_auth,
// gdrive_network, gdrive_api, gdrive_upload.
bool GdriveUploadBackup(const Config &cfg, const std::string &title_id, const std::string &file_name,
                        const std::string &local_zip, std::string *err, std::string *code = nullptr);

struct GdriveBackup {
    std::string id;
    std::string title_id;
    std::string name;
    uint64_t size_bytes = 0;
    int64_t created_unix = 0;
};

bool GdriveListBackups(const Config &cfg, std::vector<GdriveBackup> *out, std::string *err,
                       std::string *code = nullptr);
bool GdriveDownload(const Config &cfg, const std::string &file_id, const std::string &local_path, std::string *err,
                    std::string *code = nullptr);
bool GdriveDelete(const Config &cfg, const std::string &file_id, std::string *err, std::string *code = nullptr);

} // namespace prospero
