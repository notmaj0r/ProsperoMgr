#include "job_queue.hpp"
#include "tile_pkg.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "bundled_tile_pkg.hpp"
#include "jailbreak.hpp"
#include "log.hpp"
#include "miniz.h"
#include "notify.hpp"
#include "notify_text.hpp"

namespace prospero {

namespace {

// Matches pkg-source/sce_sys/param.json's titleId — the system creates this
// appmeta directory once the tile is actually installed.
constexpr const char *kTileAppMeta = "/user/appmeta/PMGR09999";

// Same ABI pkg_installer.cpp uses. Called in-process, no helper subprocess —
// AppInstallPkg is a plain local-file install, unlike InstallByPackage/PPR.
struct PkgInfoAbi {
    char content_id[0x30];
    int content_type;
    int content_platform;
};

struct PkgMetaInfoAbi {
    const char *uri;
    const char *ex_uri;
    const char *playgo_scenario_id;
    const char *content_id;
    const char *content_name;
    const char *icon_url;
};

using PkgLanguageAbi = char[8];
using PkgScenarioAbi = char[3];
using PkgContentIdAbi = char[0x30];

struct PkgPlayGoInfoAbi {
    PkgLanguageAbi languages[30];
    PkgScenarioAbi playgo_scenario_ids[64];
    PkgContentIdAbi content_ids[64];
    long unknown[810];
};

// The installer service has a different mount namespace from payloads —
// its view of payload /data/foo is /user/data/foo.
std::string AppInstVisiblePath(const std::string &path) {
    if (path.rfind("/data/", 0) == 0) return "/user" + path;
    return path;
}

} // namespace

extern "C" {
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallPkg(const char *path, PkgInfoAbi *pkg_info);
int sceAppInstUtilInstallByPackage(PkgMetaInfoAbi *meta, PkgInfoAbi *pkg_info, PkgPlayGoInfoAbi *playgo);
}

void InstallTilePkgIfNeeded(const Config &cfg) {
    struct stat st {};
    if (stat(kTileAppMeta, &st) == 0) return; // already installed, nothing to do

    StartBackgroundThread([cfg]() {
        std::vector<unsigned char> raw(kTilePkgRawSize);
        mz_ulong raw_len = kTilePkgRawSize;
        if (mz_uncompress(raw.data(), &raw_len, kTilePkg, kTilePkgSize) != MZ_OK || raw_len != kTilePkgRawSize) {
            LogEvent("tile PKG: failed to decompress embedded package");
            return;
        }

        std::error_code mkdir_ec;
        std::filesystem::create_directories(cfg.tmp_dir, mkdir_ec);
        std::string path = cfg.tmp_dir + "/prosperomgr-tile.pkg";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
        out.close();
        if (!out) {
            LogEvent("tile PKG: failed to write " + path);
            return;
        }

        // Belt-and-suspenders re-escalation, same as EscalatePrivileges()
        // already ran once at boot — cheap and idempotent.
        std::string jb_err;
        if (!EscalatePrivileges(&jb_err)) {
            LogEvent("tile PKG: privilege escalation failed: " + jb_err);
        }

        std::string sdk_path = AppInstVisiblePath(path);
        char buf[192];

        int init_rc = sceAppInstUtilInitialize();
        if (init_rc != 0) {
            std::snprintf(buf, sizeof(buf), "tile PKG: install failed: sceAppInstUtilInitialize rc=0x%08x",
                          static_cast<unsigned>(init_rc));
            LogEvent(buf);
            return;
        }

        // Primary: direct local-file install.
        PkgInfoAbi info{};
        int rc = sceAppInstUtilAppInstallPkg(sdk_path.c_str(), &info);
        if (rc == 0) {
            LogEvent("tile PKG: home-screen tile installed");
            Notify("Prospero Manager\n" + NotifyText(Msg::kPkgInstalled));
            return;
        }
        std::snprintf(buf, sizeof(buf), "tile PKG: AppInstallPkg failed rc=0x%08x — trying InstallByPackage",
                      static_cast<unsigned>(rc));
        LogEvent(buf);

        // Fallback: InstallByPackage with a file:// URI.
        std::string file_uri = "file://" + sdk_path;
        PkgMetaInfoAbi meta{};
        PkgInfoAbi info2{};
        PkgPlayGoInfoAbi playgo{};
        meta.uri = file_uri.c_str();
        meta.ex_uri = "";
        meta.playgo_scenario_id = "";
        meta.content_id = "";
        meta.content_name = "Prospero Manager";
        meta.icon_url = "";

        int rc2 = sceAppInstUtilInstallByPackage(&meta, &info2, &playgo);
        if (rc2 == 0) {
            LogEvent("tile PKG: home-screen tile installed");
            Notify("Prospero Manager\n" + NotifyText(Msg::kPkgInstalled));
        } else {
            std::snprintf(buf, sizeof(buf), "tile PKG: install failed: AppInstallPkg rc=0x%08x, InstallByPackage rc=0x%08x",
                          static_cast<unsigned>(rc), static_cast<unsigned>(rc2));
            LogEvent(buf);
        }
    });
}

} // namespace prospero
