#pragma once
// Checks the project's GitHub releases and swaps the running ELF for a newer one.

#include <functional>
#include <string>

#include "config.hpp"

namespace prospero {

struct UpdateStatus {
    std::string current;
    std::string latest;
    std::string notes;
    std::string state; // idle, checking, downloading, launching, error
    std::string error;
    bool available = false;
    bool auto_update = true;
    long long loaded = 0;
    long long total = -1;
};

// `busy` says whether the console is mid-transfer; automatic updates wait for it to be false.
void InitializeSelfUpdate(const Config &cfg, std::function<bool()> busy);
UpdateStatus GetUpdateStatus();
void CheckForUpdateAsync();
bool StartUpdateInstall(std::string *err);
void SetAutoUpdate(bool enabled);

} // namespace prospero
