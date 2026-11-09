#pragma once
// Installed PS5 title library — reads app.db (read-only) for the Home page's
// games grid, and launches a title through the system's own app launcher.

#include <string>
#include <vector>

#include "config.hpp"

namespace prospero {

struct TitleInfo {
    std::string title_id;
    std::string name;
    std::string platform;     // "PS5" | "PS4" | "APP" — derived from the title-ID prefix
    std::string kind;         // "game" | "app" (installed apps and homebrew)
    std::string icon_path;    // absolute path to icon0.png, empty if none found
    long played_seconds = 0;  // 0 if never played / no activity record found
    std::string last_played;  // as app.db stores it (ISO-ish string), empty if unknown
    std::string version;      // installed content version (e.g. "1.034.000"), empty if unknown
};

// `cfg` is only used to place the on-disk title-name cache (see titles.cpp's
// FetchStoreTitleName).
// With include_apps the list also holds installed apps and homebrew, marked with kind "app".
std::vector<TitleInfo> ListTitles(const Config &cfg, bool include_apps = false);

// Resolves a title's icon0.png path for serving as an image response.
std::string TitleIconPath(const std::string &title_id);

struct GameFolderInfo {
    std::string title_id;
    std::string name;
    std::string icon_path; // The folder's own icon0.png, else the installed title's.
    bool own_icon = false;
    bool in_image = false; // Metadata and icon live inside an .exfat image.
};

// Recognizes a game folder or .exfat image and finds its name and cover, from its title ID or its sce_sys metadata.
bool IdentifyGameFolder(const std::string &dir, GameFolderInfo *out);

// Bytes the game takes under the app and patch folders; false while unknown or a file job runs.
bool TitleInstallSize(const std::string &title_id, uint64_t *out);

// `via_notification` is set when the title can only be opened from a notification on the console.
bool LaunchTitle(const std::string &title_id, std::string *err, bool *via_notification = nullptr);

// Title ID of whatever big-app is in the foreground right now, or empty if none — cheap
// sceSystemService polling with no SQL round-trip, so the Home page can mark the running tile.
std::string GetRunningTitleId();

// What app.db records about a title's removal flags, for explaining a refused uninstall.
std::string DescribeTitleRemoval(const std::string &title_id);

// Asks the system to close the running game, the same way the home menu does (how = -1).
bool CloseRunningTitle(std::string *err);

} // namespace prospero
