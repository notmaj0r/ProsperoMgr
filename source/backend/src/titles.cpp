#include "titles.hpp"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>

#include "exfat_image.hpp"
#include "file_manager.hpp"
#include "https_client.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "notify.hpp"
#include "notify_text.hpp"
#include "ps5_services.hpp"

#include "sqlite3.h"

namespace prospero {

namespace {
bool IsSafeTitleId(const std::string &title_id) {
    if (title_id.size() < 5 || title_id.size() > 32) return false;
    for (unsigned char c : title_id) {
        if (!std::isalnum(c)) return false;
    }
    return true;
}

bool IsNonEmptyFile(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

// The console writes to app.db during installs; waiting briefly beats an empty list.
int OpenAppDb(sqlite3 **db) {
    int rc = sqlite3_open_v2("/system_data/priv/mms/app.db", db, SQLITE_OPEN_READONLY, nullptr);
    if (rc == SQLITE_OK) sqlite3_busy_timeout(*db, 1500);
    return rc;
}
} // namespace

bool TitleInstallSize(const std::string &title_id, uint64_t *out) {
    if (!IsSafeTitleId(title_id)) return false;

    static std::mutex cache_mu;
    static std::unordered_map<std::string, uint64_t> cache;
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        auto found = cache.find(title_id);
        if (found != cache.end()) { *out = found->second; return true; }
    }

    static const char *const kRoots[] = {
        "/user/app/", "/user/patch/", "/mnt/ext0/user/app/", "/mnt/ext1/user/app/", nullptr,
    };
    uint64_t total = 0;
    bool any = false;
    for (int i = 0; kRoots[i]; i++) {
        uint64_t size = 0;
        std::string err;
        if (NativeDirectorySize(kRoots[i] + title_id, &size, &err)) { total += size; any = true; }
    }
    if (!any) return false;
    std::lock_guard<std::mutex> lock(cache_mu);
    cache[title_id] = total;
    *out = total;
    return true;
}

std::string TitleIconPath(const std::string &title_id) {
    if (!IsSafeTitleId(title_id)) return "";

    static std::mutex cache_mu;
    static std::unordered_map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        auto found = cache.find(title_id);
        if (found != cache.end() && IsNonEmptyFile(found->second)) return found->second;
    }

    std::string result;
    sqlite3 *db = nullptr;
    if (OpenAppDb(&db) == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        const char *sql =
            "SELECT icon0Info FROM tbl_contentinfo WHERE titleId=?1 AND icon0Info IS NOT NULL "
            "AND icon0Info!='' LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, title_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *value = sqlite3_column_text(stmt, 0);
                if (value) {
                    result = reinterpret_cast<const char *>(value);
                    size_t end = result.rfind(".png");
                    if (end != std::string::npos) result.resize(end + 4); else result.clear();
                    if (result.rfind("/mnt/ext0/", 0) == 0 || result.rfind("/mnt/ext1/", 0) == 0 ||
                        !IsNonEmptyFile(result)) result.clear();
                }
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_close(db);
    } else if (db) {
        sqlite3_close(db);
    }

    static const char *const kCandidates[] = {
        "/user/appmeta/%s/icon0.png",
        "/user/appmeta/%s/sce_sys/icon0.png",
        "/user/appmeta/external/%s/icon0.png",
        "/user/appmeta/external/%s/sce_sys/icon0.png",
        "/system_data/priv/appmeta/%s/icon0.png",
        "/system_data/priv/appmeta/%s/sce_sys/icon0.png",
        "/system_data/priv/appmeta/external/%s/icon0.png",
        "/system_data/priv/appmeta/external/%s/sce_sys/icon0.png",
        "/user/app/%s/sce_sys/icon0.png",
        "/user/patch/%s/sce_sys/icon0.png",
        nullptr,
    };
    char path[512];
    if (result.empty()) {
        for (int i = 0; kCandidates[i]; i++) {
            std::snprintf(path, sizeof(path), kCandidates[i], title_id.c_str());
            if (IsNonEmptyFile(path)) { result = path; break; }
        }
    }

    static const char *const kRoots[] = {
        "/user/appmeta", "/user/appmeta/external", "/system_data/priv/appmeta",
        "/system_data/priv/appmeta/external", nullptr,
    };
    for (int i = 0; result.empty() && kRoots[i]; i++) {
        DIR *root = opendir(kRoots[i]);
        if (!root) continue;
        std::string found;
        while (dirent *entry = readdir(root)) {
            if (entry->d_name[0] == '.' || !std::strstr(entry->d_name, title_id.c_str())) continue;
            const std::string base = std::string(kRoots[i]) + "/" + entry->d_name;
            if (IsNonEmptyFile(base + "/icon0.png")) found = base + "/icon0.png";
            else if (IsNonEmptyFile(base + "/sce_sys/icon0.png")) found = base + "/sce_sys/icon0.png";
            if (!found.empty()) break;
        }
        closedir(root);
        if (!found.empty()) result = found;
    }
    if (!result.empty()) {
        std::lock_guard<std::mutex> lock(cache_mu);
        cache[title_id] = result;
    }
    return result;
}

// Sony's own title-id prefix scheme, resolved once here so every
// consumer of ListTitles() gets it.
std::string DerivePlatform(const std::string &title_id) {
    if (title_id.rfind("PPSA", 0) == 0) return "PS5";
    if (title_id.rfind("CUSA", 0) == 0) return "PS4";
    return "APP";
}

namespace {


// app.db's tbl_appbrowse.titleName is legitimately blank for a lot of real titles (fpkg/PS4
// especially) — falls back to param.sfo ("\0PSF" magic), the authoritative source every title ships.
uint16_t ReadLe16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t ReadLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool SfoReadString(const std::string &path, const char *key, std::string *out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto *data = reinterpret_cast<const uint8_t *>(buf.data());
    size_t len = buf.size();

    if (len < 20 || std::memcmp(data, "\0PSF", 4) != 0) return false;

    uint32_t key_table = ReadLe32(data + 8);
    uint32_t data_table = ReadLe32(data + 12);
    uint32_t count = ReadLe32(data + 16);
    if (key_table >= len || data_table >= len || count > 1024 || 20 + static_cast<uint64_t>(count) * 16 > len) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = data + 20 + i * 16;
        uint16_t key_off = ReadLe16(entry + 0);
        uint32_t data_len = ReadLe32(entry + 4);
        uint32_t data_off = ReadLe32(entry + 12);
        if (key_table + key_off >= len || data_table + data_off >= len) continue;

        const char *k = reinterpret_cast<const char *>(data + key_table + key_off);
        size_t kmax = len - (key_table + key_off);
        if (!memchr(k, '\0', kmax)) continue;
        if (std::strcmp(k, key) != 0) continue;

        size_t avail = len - (data_table + data_off);
        size_t n = data_len;
        if (n > avail) n = avail;
        while (n > 0 && data[data_table + data_off + n - 1] == '\0') n--;
        if (n == 0) return false;

        out->assign(reinterpret_cast<const char *>(data + data_table + data_off), n);
        return true;
    }
    return false;
}

bool IsUnresolvedName(const std::string &title_id, const std::string &name) {
    size_t begin = 0;
    while (begin < name.size() && std::isspace(static_cast<unsigned char>(name[begin]))) begin++;
    size_t end = name.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(name[end - 1]))) end--;
    if (begin == end) return true;

    std::string trimmed = name.substr(begin, end - begin);
    if (trimmed.size() == title_id.size()) {
        bool same = true;
        for (size_t i = 0; i < trimmed.size(); i++) {
            if (std::toupper(static_cast<unsigned char>(trimmed[i])) !=
                std::toupper(static_cast<unsigned char>(title_id[i]))) {
                same = false;
                break;
            }
        }
        if (same) return true;
    }
    return trimmed == "Unknown" || trimmed == "Unknown title";
}

bool ReadSmallTextFile(const std::string &path, std::string *out) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out->empty();
}

// Native PS5 applications use param.json, not param.sfo, for localized metadata. `titleName`
// lives below localizedParameters; JsonExtractString() finds that first string in this small, trusted doc.
bool JsonReadValue(const std::string &path, const char *json_key, std::string *out) {
    std::string json;
    if (!ReadSmallTextFile(path, &json)) return false;
    *out = JsonExtractString(json, json_key);
    return !out->empty();
}

bool ReadMetadataDir(const std::string &dir, const char *json_key, const char *sfo_key, std::string *out) {
    if (JsonReadValue(dir + "/param.json", json_key, out) ||
        JsonReadValue(dir + "/sce_sys/param.json", json_key, out) ||
        SfoReadString(dir + "/param.sfo", sfo_key, out) ||
        SfoReadString(dir + "/sce_sys/param.sfo", sfo_key, out)) {
        return !out->empty();
    }
    return false;
}

// External storage and some native installs use a content ID as the appmeta dir name (e.g.
// UP....-PPSA28180_00-...), invisible to a direct /appmeta/PPSA28180 lookup — search children instead.
bool ScanAppMetaForTitle(const std::string &title_id, const char *json_key, const char *sfo_key,
                         std::string *out) {
    static const char *const kBases[] = {"/user/appmeta", "/user/appmeta/external", nullptr};
    for (int i = 0; kBases[i]; i++) {
        DIR *dir = opendir(kBases[i]);
        if (!dir) continue;

        bool found = false;
        while (dirent *entry = readdir(dir)) {
            if (entry->d_name[0] == '.' || !std::strstr(entry->d_name, title_id.c_str())) continue;
            std::string metadata_dir = std::string(kBases[i]) + "/" + entry->d_name;
            if (ReadMetadataDir(metadata_dir, json_key, sfo_key, out)) {
                found = true;
                break;
            }

            std::string sandbox = std::string("/mnt/sandbox/") + entry->d_name + "_000";
            if (ReadMetadataDir(sandbox + "/patch0", json_key, sfo_key, out) ||
                ReadMetadataDir(sandbox + "/app0", json_key, sfo_key, out)) {
                found = true;
                break;
            }
        }
        closedir(dir);
        if (found) return true;
    }
    return false;
}

bool ReadInstalledMetadata(const std::string &title_id, const char *json_key, const char *sfo_key,
                           std::string *out) {
    static const char *const kJsonPaths[] = {
        "/system_data/priv/appmeta/%s/param.json",
        "/system_data/priv/appmeta/external/%s/param.json",
        "/user/appmeta/%s/param.json",
        "/user/appmeta/%s/sce_sys/param.json",
        "/user/appmeta/external/%s/param.json",
        "/user/appmeta/external/%s/sce_sys/param.json",
        "/user/patch/%s/sce_sys/param.json",
        "/user/app/%s/sce_sys/param.json",
        nullptr,
    };
    static const char *const kPaths[] = {
        "/system_data/priv/appmeta/%s/param.sfo",
        "/system_data/priv/appmeta/external/%s/param.sfo",
        "/user/patch/%s/sce_sys/param.sfo",
        "/user/appmeta/%s/param.sfo",
        "/user/appmeta/%s/sce_sys/param.sfo",
        "/user/appmeta/external/%s/param.sfo",
        "/user/appmeta/external/%s/sce_sys/param.sfo",
        "/user/app/%s/sce_sys/param.sfo",
        nullptr,
    };
    char path[512];

    for (int i = 0; kJsonPaths[i]; i++) {
        std::snprintf(path, sizeof(path), kJsonPaths[i], title_id.c_str());
        if (JsonReadValue(path, json_key, out) && !out->empty()) return true;
    }
    for (int i = 0; kPaths[i]; i++) {
        std::snprintf(path, sizeof(path), kPaths[i], title_id.c_str());
        if (SfoReadString(path, sfo_key, out) && !out->empty()) return true;
    }
    return ScanAppMetaForTitle(title_id, json_key, sfo_key, out);
}

bool ReadInstalledTitleName(const std::string &title_id, std::string *out) {
    return ReadInstalledMetadata(title_id, "titleName", "TITLE", out) && !IsUnresolvedName(title_id, *out);
}

bool ReadInstalledContentId(const std::string &title_id, std::string *out) {
    return ReadInstalledMetadata(title_id, "contentId", "CONTENT_ID", out);
}

// Cache dir for names resolved online (see FetchStoreTitleName), so a title
// only ever needs one network round trip, not one per Home-tab load forever after.
std::string TitleCachePath(const Config &cfg, const std::string &title_id) {
    return cfg.root_dir + "/.title_cache/" + title_id + ".txt";
}

bool ReadTitleCache(const Config &cfg, const std::string &title_id, std::string *out) {
    std::ifstream f(TitleCachePath(cfg, title_id));
    if (!f.is_open()) return false;
    std::getline(f, *out);
    return !out->empty();
}

void WriteTitleCache(const Config &cfg, const std::string &title_id, const std::string &name) {
    mkdir((cfg.root_dir + "/.title_cache").c_str(), 0755);
    std::ofstream f(TitleCachePath(cfg, title_id), std::ios::trunc);
    if (f.is_open()) f << name;
}

// Last resort before falling back to just the title ID: an unauthenticated GET of the game's
// public Store page, reading the name out of its embedded JSON-LD block.
bool FetchStoreTitleName(const std::string &content_id, std::string *out) {
    if (content_id.empty()) return false;

    HttpsResponse resp;
    std::string err;
    if (!HttpsClient::Get("https://store.playstation.com/en-us/product/" + content_id, &resp, &err)) return false;

    const std::string open_tag = "<script id=\"mfe-jsonld-tags\" type=\"application/ld+json\">";
    size_t start = resp.body.find(open_tag);
    if (start == std::string::npos) return false;
    start += open_tag.size();

    size_t end = resp.body.find("</script>", start);
    if (end == std::string::npos) return false;

    std::string name = JsonExtractString(resp.body.substr(start, end - start), "name");
    if (name.empty()) return false;
    *out = name;
    return true;
}

bool TableExists(sqlite3 *db, const char *table) {
    const char *sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?1";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

bool TableHasColumn(sqlite3 *db, const char *table, const char *column) {
    std::string sql = std::string("PRAGMA table_info(") + table + ")";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name && std::strcmp(column, reinterpret_cast<const char *>(name)) == 0) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

bool ReadAppInfoTitleName(sqlite3 *db, const std::string &title_id, std::string *out) {
    if (!TableExists(db, "tbl_appinfo")) return false;

    static const char *const kKeys[] = {
        "TITLE_01", "TITLE_02", "TITLE_03", "TITLE_04", "TITLE_05", "TITLE_06", "TITLE_07", "TITLE_08",
        "TITLE_09", "TITLE_10", "TITLE_11", "TITLE_12", "TITLE_13", "TITLE_14", "TITLE_15", "TITLE_16",
        "TITLE_17", "TITLE_18", "TITLE_19", "TITLE_20", "TITLE_21", "TITLE_22", "TITLE_00", "TITLE", nullptr,
    };
    const char *sql =
        "SELECT val FROM tbl_appinfo WHERE titleId=?1 AND key=?2 AND val IS NOT NULL AND val!='' LIMIT 1";
    for (int i = 0; kKeys[i]; i++) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, title_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, kKeys[i], -1, SQLITE_TRANSIENT);
        bool found = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(stmt, 0);
            if (value) {
                *out = reinterpret_cast<const char *>(value);
                found = !IsUnresolvedName(title_id, *out);
            }
        }
        sqlite3_finalize(stmt);
        if (found) return true;
    }
    return false;
}

constexpr const char *kGamePrefixList[] = {"CUSA", "PPSA", "ULUS", "ULES", "ULJS", "ULKS", "SLUS", "SCUS",
                                           "SLES", "SCES", "SLPS", "SLPM", "SCED", "SLED", "SCPS"};

bool IsGameTitleId(const std::string &title_id) {
    for (const char *prefix : kGamePrefixList) {
        if (title_id.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

std::string GamePrefixSql() {
    std::string sql;
    for (const char *prefix : kGamePrefixList) {
        if (!sql.empty()) sql += " OR ";
        sql += std::string("titleId LIKE '") + prefix + "%'";
    }
    return sql;
}

// An app is anything that is not a game: third-party media apps, homebrew and other tools. app.db marks them
// with is3rdPartyVideoApp or the app category bit; installed games carry neither.
void MarkApps(sqlite3 *db, std::vector<TitleInfo> *titles) {
    std::unordered_map<std::string, bool> is_app;
    if (TableExists(db, "tbl_contentinfo")) {
        bool has_video_flag = TableHasColumn(db, "tbl_contentinfo", "is3rdPartyVideoApp");
        bool has_category = TableHasColumn(db, "tbl_contentinfo", "categoryType");
        std::string sql = std::string("SELECT titleId, ") + (has_video_flag ? "COALESCE(is3rdPartyVideoApp,0)" : "0") + ", " +
                          (has_category ? "COALESCE(categoryType,0)" : "0") + " FROM tbl_contentinfo";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *id = sqlite3_column_text(stmt, 0);
                if (!id) continue;
                bool app = sqlite3_column_int(stmt, 1) != 0 || (sqlite3_column_int64(stmt, 2) & 0x10000) != 0;
                is_app[reinterpret_cast<const char *>(id)] = app;
            }
        }
        sqlite3_finalize(stmt);
    }
    for (TitleInfo &title : *titles) {
        auto found = is_app.find(title.title_id);
        if ((found != is_app.end() && found->second) || !IsGameTitleId(title.title_id)) {
            title.kind = "app";
            title.platform = "APP";
        }
    }
}

void RunTitleQuery(sqlite3 *db, const char *sql, std::vector<TitleInfo> *out) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *tid = sqlite3_column_text(stmt, 0);
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (!tid) continue;

        TitleInfo info;
        info.title_id = reinterpret_cast<const char *>(tid);
        // COALESCE gives an empty string, not null, so fpkg titles need a
        // length check rather than a pointer check.
        const char *name_str = name ? reinterpret_cast<const char *>(name) : "";
        info.name = *name_str ? name_str : info.title_id;
        info.platform = DerivePlatform(info.title_id);
        info.kind = "game";
        info.icon_path = TitleIconPath(info.title_id);
        out->push_back(std::move(info));
    }
    sqlite3_finalize(stmt);
}

struct Activity {
    long played_seconds = 0;
    std::string last_played;
};

// Playtime/last-played live in a per-profile table (tbl_iconinfo_<user hex>) — merge across
// every profile found so a shared console still shows something sensible per title.
void MergeActivity(sqlite3 *db, std::unordered_map<std::string, Activity> *activity) {
    sqlite3_stmt *tables_stmt = nullptr;
    const char *tables_sql =
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%'";
    if (sqlite3_prepare_v2(db, tables_sql, -1, &tables_stmt, nullptr) != SQLITE_OK) return;

    std::vector<std::string> tables;
    while (sqlite3_step(tables_stmt) == SQLITE_ROW) {
        const unsigned char *n = sqlite3_column_text(tables_stmt, 0);
        if (n) tables.push_back(reinterpret_cast<const char *>(n));
    }
    sqlite3_finalize(tables_stmt);

    for (const auto &table : tables) {
        if (!TableHasColumn(db, table.c_str(), "playedTimeOnConsole")) continue;

        std::string sql = "SELECT titleId, playedTimeOnConsole, lastAccessTime FROM " + table;
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) continue;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *tid = sqlite3_column_text(stmt, 0);
            if (!tid) continue;
            long played = sqlite3_column_int(stmt, 1);
            const unsigned char *last = sqlite3_column_text(stmt, 2);

            Activity &a = (*activity)[reinterpret_cast<const char *>(tid)];
            if (played > a.played_seconds) a.played_seconds = played;
            if (last && reinterpret_cast<const char *>(last) > a.last_played) {
                a.last_played = reinterpret_cast<const char *>(last);
            }
        }
        sqlite3_finalize(stmt);
    }
}

} // namespace

std::vector<TitleInfo> ListTitles(const Config &cfg, bool include_apps) {
    // Home and Save Data can ask for this at the same time; serializing keeps
    // the app.db snapshot and the name cache coherent.
    static std::mutex list_mu;
    std::lock_guard<std::mutex> list_lock(list_mu);

    std::vector<TitleInfo> out;

    sqlite3 *db = nullptr;
    if (OpenAppDb(&db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }

    // Real game title IDs only — the same table also holds system services,
    // media apps and store listings.
    const std::string kGamePrefixes = GamePrefixSql();
    const std::string kPrefixFilter = std::string(" AND (") + kGamePrefixes + ")";

    // A DB rebuild can drop a PS4 fpkg's browse row, so union both tables and
    // dedup by titleId, keeping whichever name isn't empty.
    bool has_appbrowse = TableExists(db, "tbl_appbrowse");
    bool has_appbrowse_name = has_appbrowse && TableHasColumn(db, "tbl_appbrowse", "titleName");
    bool has_cinfo = TableExists(db, "tbl_contentinfo");
    bool has_cinfo_name = has_cinfo && TableHasColumn(db, "tbl_contentinfo", "titleName");
    bool has_uninstallable = has_cinfo && TableHasColumn(db, "tbl_contentinfo", "uninstallable");
    // Installed apps and homebrew have their own title IDs; system entries (NPXS) cannot be uninstalled.
    std::string cinfo_filter = kPrefixFilter;
    if (include_apps && has_uninstallable) {
        cinfo_filter = std::string(" AND (") + kGamePrefixes +
                       " OR (uninstallable = 1 AND titleId NOT LIKE 'NPXS%'))";
    }

    std::string sql;
    if (has_appbrowse) {
        std::string ab = std::string("SELECT titleId, ") + (has_appbrowse_name ? "COALESCE(titleName,'')" : "''") +
                          " AS n FROM tbl_appbrowse WHERE titleId IS NOT NULL AND titleId != ''" + kPrefixFilter;
        std::string ci;
        if (has_cinfo) {
            ci = std::string(" UNION ALL SELECT titleId, ") + (has_cinfo_name ? "COALESCE(titleName,'')" : "''") +
                 " AS n FROM tbl_contentinfo WHERE titleId IS NOT NULL AND titleId != ''" + cinfo_filter;
        }
        sql = "SELECT titleId, MAX(n) AS n FROM (" + ab + ci + ") GROUP BY titleId ORDER BY n COLLATE NOCASE";
    } else if (has_cinfo) {
        sql = std::string("SELECT DISTINCT titleId, ") + (has_cinfo_name ? "COALESCE(titleName,'')" : "''") +
              " AS n FROM tbl_contentinfo WHERE titleId IS NOT NULL AND titleId != ''" + cinfo_filter +
              " ORDER BY n COLLATE NOCASE";
    }

    if (!sql.empty()) RunTitleQuery(db, sql.c_str(), &out);
    if (include_apps) MarkApps(db, &out);

    // Resolve localized names while app.db is still open; tbl_appinfo covers
    // rows that carry only a title ID.
    for (auto &t : out) {
        if (!IsUnresolvedName(t.title_id, t.name)) continue;
        std::string appinfo_name;
        if (ReadAppInfoTitleName(db, t.title_id, &appinfo_name)) t.name = appinfo_name;
    }

    std::unordered_map<std::string, Activity> activity;
    MergeActivity(db, &activity);
    for (auto &t : out) {
        auto it = activity.find(t.title_id);
        if (it == activity.end()) continue;
        t.played_seconds = it->second.played_seconds;
        t.last_played = it->second.last_played;
    }

    sqlite3_close(db);

    // PS5 param.json calls it "contentVersion", PS4 SFO calls it "APP_VER".
    // Patch metadata is read before the base app's, so this reflects patches.
    for (auto &t : out) {
        ReadInstalledMetadata(t.title_id, "contentVersion", "APP_VER", &t.version);
    }

    // app.db had no usable localized name: read native PS5 param.json first,
    // then the PSF/SFO metadata used by PS4 titles and some homebrew.
    for (auto &t : out) {
        if (!IsUnresolvedName(t.title_id, t.name)) continue;
        std::string local_name;
        if (ReadInstalledTitleName(t.title_id, &local_name)) t.name = local_name;
    }

    // Last resort for content with a blank TITLE: look the name up on the PS
    // Store, cached to disk and capped so one page load can't stack requests.
    int online_lookups_left = 5;
    for (auto &t : out) {
        if (!IsUnresolvedName(t.title_id, t.name)) continue;

        std::string cached;
        if (ReadTitleCache(cfg, t.title_id, &cached)) {
            t.name = cached;
            continue;
        }
        if (online_lookups_left <= 0) continue;

        std::string content_id;
        if (!ReadInstalledContentId(t.title_id, &content_id)) continue;

        std::string store_name;
        online_lookups_left--;
        if (FetchStoreTitleName(content_id, &store_name)) {
            t.name = store_name;
            WriteTitleCache(cfg, t.title_id, store_name);
        }
    }

    return out;
}

bool IdentifyGameFolder(const std::string &dir, GameFolderInfo *out) {
    // An .exfat image is read in place, so its metadata never gets copied anywhere.
    std::string image_param;
    bool in_image = dir.size() > 6 && strcasecmp(dir.c_str() + dir.size() - 6, ".exfat") == 0 &&
                    ReadExfatFile(dir, "sce_sys/param.json", 1024 * 1024, &image_param);
    size_t slash = dir.find_last_of('/');
    std::string base = slash == std::string::npos ? dir : dir.substr(slash + 1);
    auto id_at = [](const std::string &text, size_t pos) {
        if (text.size() < pos + 9) return false;
        for (size_t i = 0; i < 9; i++) {
            unsigned char c = static_cast<unsigned char>(text[pos + i]);
            if (i < 4 ? !std::isalpha(c) : !std::isdigit(c)) return false;
        }
        return true;
    };
    // Dump folders are often named "Game 01.000.000 PPSA01234 LIZARD", so the ID can sit mid-name.
    size_t start = std::string::npos;
    if (id_at(base, 0)) start = 0;
    for (size_t pos = 1; start == std::string::npos && pos + 9 <= base.size(); pos++) {
        bool bounded = !std::isalnum(static_cast<unsigned char>(base[pos - 1])) &&
                       (pos + 9 == base.size() || !std::isalnum(static_cast<unsigned char>(base[pos + 9])));
        if (bounded && id_at(base, pos) && IsGameTitleId(base.substr(pos, 9))) start = pos;
    }
    if (start != std::string::npos) {
        out->title_id = base.substr(start, 9);
    } else {
        // A renamed dump still carries its own title ID in sce_sys/param.json. sce_sys itself is not a game.
        std::string id;
        if (in_image) {
            id = JsonExtractString(image_param, "titleId");
        } else {
            struct stat st {};
            if (stat((dir + "/sce_sys").c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
            if (!ReadMetadataDir(dir, "titleId", "TITLE_ID", &id)) return false;
        }
        if (!id_at(id, 0) || !IsGameTitleId(id.substr(0, 9))) return false;
        out->title_id = id.substr(0, 9);
    }
    for (char &c : out->title_id) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    std::string name;
    bool found = in_image ? !(name = JsonExtractString(image_param, "titleName")).empty()
                          : ReadMetadataDir(dir, "titleName", "TITLE", &name);
    found = found && !IsUnresolvedName(out->title_id, name);
    if (!found) found = ReadInstalledTitleName(out->title_id, &name);
    if (!found) return false;
    out->name = name;

    out->in_image = in_image;
    out->own_icon = in_image || IsNonEmptyFile(dir + "/sce_sys/icon0.png") || IsNonEmptyFile(dir + "/icon0.png");
    out->icon_path = in_image                                       ? ""
                     : IsNonEmptyFile(dir + "/sce_sys/icon0.png") ? dir + "/sce_sys/icon0.png"
                     : IsNonEmptyFile(dir + "/icon0.png")         ? dir + "/icon0.png"
                                                                  : TitleIconPath(out->title_id);
    return true;
}

namespace {

// The address a title's home-screen tile opens, empty when it has none.
std::string DeeplinkOf(const std::string &title_id) {
    std::string uri;
    sqlite3 *db = nullptr;
    if (OpenAppDb(&db) == SQLITE_OK &&
        TableHasColumn(db, "tbl_contentinfo", "pprDeeplinkUri")) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT pprDeeplinkUri FROM tbl_contentinfo WHERE titleId=?1 LIMIT 1", -1, &stmt, nullptr) ==
            SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, title_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *value = sqlite3_column_text(stmt, 0);
                if (value) uri = reinterpret_cast<const char *>(value);
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    if (db) sqlite3_close(db);
    return uri;
}

std::string NameOfTitle(const std::string &title_id) {
    std::string name;
    sqlite3 *db = nullptr;
    if (OpenAppDb(&db) == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT titleName FROM tbl_contentinfo WHERE titleId=?1 LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, title_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *value = sqlite3_column_text(stmt, 0);
                if (value) name = reinterpret_cast<const char *>(value);
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    if (db) sqlite3_close(db);
    return name.empty() ? title_id : name;
}

} // namespace

bool LaunchTitle(const std::string &title_id, std::string *err, bool *via_notification) {
    sceUserServiceInitialize(nullptr);

    uint32_t user_id = 0;
    if (sceUserServiceGetForegroundUser(&user_id) != 0 || user_id == 0) {
        if (err) *err = "no foreground user — sign in to a profile first";
        return false;
    }

    // Web-based homebrew tiles are bookmarks: the home screen opens their address in the browser, and launching
    // them as an app is refused. Tiles of other kinds (psgm: apps and games) launch as apps.
    std::string deeplink = DeeplinkOf(title_id);
    bool is_web = deeplink.rfind("http://", 0) == 0 || deeplink.rfind("https://", 0) == 0;
    if (is_web) {
        int web_rc = sceSystemServiceLaunchWebBrowser(deeplink.c_str());
        if (web_rc < 0) {
            char code[16];
            std::snprintf(code, sizeof(code), "%08x", static_cast<unsigned>(web_rc));
            if (err) *err = std::string("sceSystemServiceLaunchWebBrowser failed (0x") + code + ")";
            return false;
        }
        LogEvent("Opened " + title_id + " in the browser: " + deeplink);
        return true;
    }
    // Other system addresses (pssettings:, ...) can only be opened by the shell itself, from a notification button.
    if (!deeplink.empty() && deeplink.rfind("psgm:", 0) != 0) {
        std::string name = NameOfTitle(title_id);
        NotifyWithAction("Prospero Manager\n" + NotifyText(Msg::kOpenTitle, {{"name", name}}), NotifyText(Msg::kOpenTitle, {{"name", name}}),
                         deeplink);
        LogEvent("Sent a notification that opens " + title_id + " (" + deeplink + ")");
        if (via_notification) *via_notification = true;
        return true;
    }

    AppLaunchCtx ctx{};
    ctx.structsize = sizeof(ctx);
    ctx.user_id = user_id;
    ctx.argv = nullptr;

    int rc = sceSystemServiceLaunchApp(title_id.c_str(), nullptr, &ctx);
    // A success can return the app id rather than zero; SCE errors have the
    // high bit set and arrive negative.
    if (rc < 0) {
        char code[16];
        std::snprintf(code, sizeof(code), "%08x", static_cast<unsigned>(rc));
        if (err) *err = std::string("sceSystemServiceLaunchApp failed (0x") + code + ")";
        return false;
    }
    return true;
}

std::string GetRunningTitleId() {
    int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id <= 0) return "";

    char title_id[16] = {0};
    if (sceSystemServiceGetAppTitleId(app_id, title_id) != 0) return "";
    return title_id;
}

std::string DescribeTitleRemoval(const std::string &title_id) {
    std::string text;
    sqlite3 *db = nullptr;
    if (OpenAppDb(&db) == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT uninstallable, contentStatus, dispLocation FROM tbl_contentinfo WHERE titleId=?1 LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, title_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                text = "app.db: uninstallable=" + std::to_string(sqlite3_column_int(stmt, 0)) +
                       ", contentStatus=" + std::to_string(sqlite3_column_int(stmt, 1)) +
                       ", dispLocation=" + std::to_string(sqlite3_column_int(stmt, 2));
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    if (db) sqlite3_close(db);
    return text;
}

bool CloseRunningTitle(std::string *err) {
    int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id <= 0) {
        if (err) *err = "no game is running";
        return false;
    }
    int rc = sceSystemServiceKillApp(app_id, -1, 0, 0);
    if (rc != 0) {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08x", static_cast<unsigned>(rc));
        if (err) *err = std::string("the system refused to close the game (") + code + ")";
        return false;
    }
    return true;
}

} // namespace prospero
