#pragma once

#include <string>
#include <vector>

#include "config.hpp"

namespace prospero {

// Sets up the icon cache under cfg.root_dir and loads what it holds, so logos show without internet.
// Call once at startup, before any other function in this file.
void InitializeRepoIcons(const Config &cfg);

// Looks up each repository's logo in the background (the mirror first, then the repository) and keeps it on disk.
// Calling it again only checks repositories that are not cached or whose cache is old.
void ScanRepoIcons(const std::vector<std::string> &repo_urls);

// True while a scan is still running, so the page knows to ask again.
bool RepoIconScanRunning();

// The icon found for a repository; false when there is none (or none yet).
bool FindRepoIcon(const std::string &repo_url, std::string *mime, std::string *bytes);

} // namespace prospero
