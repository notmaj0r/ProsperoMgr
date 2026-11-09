#pragma once
// Paths and ports shared across the backend. On console everything lives
// under /data/ProsperoMgr; PROSPEROMGR_ROOT overrides that for host builds.

#include <cstdint>
#include <string>

namespace prospero {

inline constexpr const char *kVersion = "1.2";
inline constexpr const char *kAuthor = "maj0r";

struct Config {
    std::string root_dir;
    std::string payloads_dir;   // <root>/payloads — autoloaded .elf files, autoloader.ini
    std::string autoloader_ini; // <root>/payloads/autoloader.ini
    std::string bundled_dir;    // <root>/bundled — first-party payloads (ftpsrv, etc.)
    std::string tmp_dir;        // <root>/tmp — scratch space for FTP upload/download bridging
    std::string log_file;       // <root>/prosperomgr.log — survives a payload restart

    static constexpr uint16_t kHttpPort = 7070;

    // elfldr wire protocol — same port across the whole ecosystem, don't change.
    static constexpr const char *kElfldrHost = "127.0.0.1";
    static constexpr uint16_t kElfldrPort = 9021;

    // Bundled ftpsrv payload, see payloads/README.md.
    static constexpr const char *kFtpsrvHost = "127.0.0.1";
    static constexpr uint16_t kFtpsrvPort = 2121;

    // Bundled klogsrv payload — raw /dev/klog tail over a plain TCP stream,
    // default port per its own -p flag.
    static constexpr const char *kKlogsrvHost = "127.0.0.1";
    static constexpr uint16_t kKlogsrvPort = 3232;

    static Config Load();
};

// Points payloads_dir at an existing folder (e.g. one shared with another homebrew
// manager) instead of Prospero's own default. Takes effect on the next launch.
bool SetPayloadsDirOverride(const Config &cfg, const std::string &path, std::string *err);

} // namespace prospero
