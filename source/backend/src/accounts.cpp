#include "accounts.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "avatar.hpp"
#include "notify.hpp"
#include "notify_text.hpp"
#include "ps5_services.hpp"

namespace prospero {

namespace {

constexpr int kMaxSlots = 16;
constexpr int kActivateFlags = 4098;

// Reverse-engineered RegMgr entity layout.
int EntityNumber(int slot, int base, int wrap) {
    if (slot < 1 || slot > kMaxSlots) return wrap;
    return (slot - 1) * 65536 + base;
}

int NameEntity(int slot) { return EntityNumber(slot, 125829632, 127140352); }
int IdEntity(int slot) { return EntityNumber(slot, 125830400, 127141120); }
int TypeEntity(int slot) { return EntityNumber(slot, 125874183, 127184903); }
int FlagsEntity(int slot) { return EntityNumber(slot, 125831168, 127141888); }

std::string HexId(uint64_t id) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(id));
    return buf;
}

// What the user sees and types: the 8 bytes in memory order, as Garlic SaveMgr and a hex editor
// show them. The registry and the saves hold those same bytes, so the two always match.
std::string ByteOrderHex(uint64_t id) {
    char buf[24] = "0x";
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&id);
    for (int i = 0; i < 8; i++) std::snprintf(buf + 2 + i * 2, 3, "%02x", bytes[i]);
    return buf;
}

bool ParseByteOrderHex(const std::string &text, uint64_t *out) {
    size_t start = text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0 ? 2 : 0;
    if (text.size() - start != 16) return false;
    unsigned char bytes[8];
    for (int i = 0; i < 8; i++) {
        char pair[3] = {text[start + i * 2], text[start + i * 2 + 1], 0};
        char *end = nullptr;
        unsigned long value = std::strtoul(pair, &end, 16);
        if (!end || *end != '\0') return false;
        bytes[i] = static_cast<unsigned char>(value);
    }
    std::memcpy(out, bytes, sizeof(bytes));
    return true;
}

// The numeric form older account backups were written in; kept so they still restore.
bool ParseHexId(const std::string &text, uint64_t *out) {
    if (text.empty()) return false;
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(text.c_str(), &end, 16);
    if (!end || *end != '\0') return false;
    *out = parsed;
    return true;
}

AccountSlot ReadSlot(int slot, int foreground_slot) {
    AccountSlot s;
    s.slot = slot;

    char name[32] = {0};
    sceRegMgrGetStr(NameEntity(slot), name, sizeof(name));
    s.name = name;

    uint64_t id = 0;
    sceRegMgrGetBin(IdEntity(slot), &id, sizeof(id));
    s.id_hex = ByteOrderHex(id);

    char type[17] = {0};
    sceRegMgrGetStr(TypeEntity(slot), type, sizeof(type));
    s.type = type;

    int flags = 0;
    sceRegMgrGetInt(FlagsEntity(slot), &flags);
    s.flags = flags;

    s.is_foreground = foreground_slot != 0 && slot == foreground_slot;
    return s;
}

std::string BackupPath(const Config &cfg, int slot) {
    return cfg.root_dir + "/account_backups/slot_" + std::to_string(slot) + ".txt";
}

void PatchStr(std::vector<uint8_t> &buf, size_t offset, const std::string &str, size_t max_len) {
    std::memset(&buf[offset], 0, max_len);
    size_t len = std::min(str.size(), max_len - 1);
    std::memcpy(&buf[offset], str.data(), len);
}

// ---- PS5 sign-in flow: no HMAC-signed account.dat/token.dat or ShellCore patching needed,
// just these two vendored dat files (third_party/npdat_*.h) plus the registry fields below.
#include "npdat_auth.h"
#include "npdat_config.h"

// Registry offsets for the config.dat layout — these fields (np_env, account_id, username,
// flags) share the same per-user registry block the account-activation entities above use.
struct StrField { uint32_t base; size_t size; size_t cfg_offset; };
struct IntField { uint32_t base; size_t cfg_offset; };

constexpr StrField kStrFields[] = {
    {125830656, 65, 0x108}, // email
    {125874188, 17, 0x1AD}, // online_id
    {125874183, 17, 0x177}, // np_env
    {125874190, 3, 0x1BE},  // country
    {125874191, 6, 0x1C1},  // language
    {125874192, 36, 0x1C7}, // locale
    {125874189, 65, 0x1100},
    {125874193, 11, 0x1141},
    {125874195, 65, 0x114C},
};

constexpr IntField kIntFields[] = {
    {125830144, 0x48},
    {125831424, 0x4C},
    {125831168, 0x50}, // account flags
    {125832960, 0x5C},
    {125874194, 0x1F4},
    {125874185, 0x1F8}, // sign-in flag
    {125874186, 0x1FC},
    {125830912, 0xA4},
    {125831936, 0xB4},
    {125832704, 0xD0},
    {125882625, 0xD4},
    {125854723, 0xDC},
    {125833216, 0xF4},
};

struct Region { const char *country; const char *lang; const char *locale; };

// sceSystemServiceParamGetInt(1, ...) system-language codes -> region.
// Falls back to English (US).
Region RegionFor(int32_t sys_lang) {
    static constexpr Region kRegions[] = {
        {"jp", "ja", "ja-JP"}, {"us", "en", "en-US"}, {"fr", "fr", "fr-FR"}, {"es", "es", "es-ES"},
        {"de", "de", "de-DE"}, {"it", "it", "it-IT"}, {"nl", "nl", "nl-NL"}, {"pt", "pt", "pt-PT"},
        {"ru", "ru", "ru-RU"}, {"kr", "ko", "ko-KR"}, {"tw", "zh", "zh-TW"}, {"cn", "zh", "zh-CN"},
        {"fi", "fi", "fi-FI"}, {"se", "sv", "sv-SE"}, {"dk", "da", "da-DK"}, {"no", "no", "no-NO"},
        {"pl", "pl", "pl-PL"}, {"br", "pt", "pt-BR"}, {"gb", "en", "en-GB"}, {"tr", "tr", "tr-TR"},
        {"mx", "es", "es-MX"}, {"sa", "ar", "ar-SA"}, {"ca", "fr", "fr-CA"}, {"cz", "cs", "cs-CZ"},
        {"hu", "hu", "hu-HU"}, {"gr", "el", "el-GR"}, {"ro", "ro", "ro-RO"}, {"th", "th", "th-TH"},
        {"vn", "vi", "vi-VN"}, {"id", "id", "id-ID"},
    };
    if (sys_lang < 0 || static_cast<size_t>(sys_lang) >= std::size(kRegions)) return kRegions[1];
    return kRegions[sys_lang];
}

// Patches username/account_id/online_id and builds np_email fields — country/language/locale
// must already be patched into `cfg` (via RegionFor) first, since email is built from country.
void BuildConfigDat(std::vector<uint8_t> &cfg, const std::string &username, uint64_t account_id) {
    PatchStr(cfg, 0x04, username, 17);
    std::memcpy(&cfg[0x100], &account_id, 8);
    PatchStr(cfg, 0x1AD, username, 17);

    char country[3] = {0};
    std::memcpy(country, &cfg[0x1BE], 2);
    if (country[0] == 0) { country[0] = 'u'; country[1] = 's'; }
    char np_email[65];
    std::snprintf(np_email, sizeof(np_email), "%s@a8.%s.np.playstation.net", username.c_str(), country);
    PatchStr(cfg, 0x108, np_email, 65);
    PatchStr(cfg, 0x1100, np_email, 65);
}

bool WriteFile(const std::string &path, const void *data, size_t len) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(static_cast<const char *>(data), static_cast<std::streamsize>(len));
    return out.good();
}

bool WriteNpFiles(uint32_t user_id, const std::vector<uint8_t> &cfg, std::string *err) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%x", user_id);
    std::error_code ec;
    std::filesystem::create_directories(std::string("/system_data/priv/home/") + hex + "/np", ec);

    if (!WriteFile(std::string("/system_data/priv/home/") + hex + "/np/auth.dat", auth_dat, auth_dat_len)) {
        if (err) *err = "could not write auth.dat";
        return false;
    }
    if (!WriteFile(std::string("/system_data/priv/home/") + hex + "/config.dat", cfg.data(), cfg.size())) {
        if (err) *err = "could not write config.dat";
        return false;
    }
    return true;
}

void WriteRegistryState(const std::vector<uint8_t> &cfg, uint32_t off) {
    auto readInt = [&](size_t offset) {
        int32_t v;
        std::memcpy(&v, &cfg[offset], 4);
        return v;
    };
    auto str = [&](size_t offset) { return reinterpret_cast<const char *>(&cfg[offset]); };

    if (cfg[0x108] != 0) sceRegMgrSetStr(static_cast<int>(125830656 + off), str(0x108), 65);
    for (const auto &f : kStrFields) {
        if (f.base == 125830656) continue; // email — conditional, handled above
        sceRegMgrSetStr(static_cast<int>(f.base + off), str(f.cfg_offset), f.size);
    }
    for (const auto &f : kIntFields) {
        sceRegMgrSetInt(static_cast<int>(f.base + off), readInt(f.cfg_offset));
    }
}

} // namespace

std::vector<AccountSlot> ListAccountSlots(const Config &cfg) {
    AvatarUser fg = GetAvatarUser();
    int fg_slot = 0;
    if (fg.ok) {
        for (int s = 1; s <= kMaxSlots; s++) {
            char name[32] = {0};
            sceRegMgrGetStr(NameEntity(s), name, sizeof(name));
            if (name[0] && fg.user_name == name) { fg_slot = s; break; }
        }
    }

    std::vector<AccountSlot> out;
    for (int slot = 1; slot <= kMaxSlots; slot++) {
        AccountSlot s = ReadSlot(slot, fg_slot);
        if (s.name.empty()) continue; // unused slot — nothing to show
        std::ifstream backup(BackupPath(cfg, slot));
        s.has_backup = backup.is_open();
        out.push_back(std::move(s));
    }
    return out;
}

uint64_t GenerateAccountId(const std::string &name) {
    // A seeded FNV variant — a stable, reproducible suggestion, not a requirement.
    uint64_t base = 0;
    for (unsigned char c : name) {
        base = 0x100000001B3ULL * (base ^ c);
    }
    return base;
}

bool BackupAccountSlot(const Config &cfg, int slot, std::string *err) {
    if (slot < 1 || slot > kMaxSlots) {
        if (err) *err = "slot must be 1-16";
        return false;
    }
    AccountSlot current = ReadSlot(slot, 0);
    uint32_t slot_off = static_cast<uint32_t>(slot - 1) * 65536u;

    std::error_code ec;
    std::filesystem::create_directories(cfg.root_dir + "/account_backups", ec);

    std::ofstream out(BackupPath(cfg, slot), std::ios::trunc);
    if (!out.is_open()) {
        if (err) *err = "could not write account backup";
        return false;
    }
    uint64_t raw_id = 0;
    sceRegMgrGetBin(IdEntity(slot), &raw_id, sizeof(raw_id));
    out << current.name << '\n' << HexId(raw_id) << '\n' << current.type << '\n' << current.flags << '\n';

    for (const auto &f : kStrFields) {
        char base_val[65] = {0}, slot_val[65] = {0};
        sceRegMgrGetStr(static_cast<int>(f.base), base_val, sizeof(base_val));
        sceRegMgrGetStr(static_cast<int>(f.base + slot_off), slot_val, sizeof(slot_val));
        out << base_val << '\n' << slot_val << '\n';
    }
    for (const auto &f : kIntFields) {
        int base_val = 0, slot_val = 0;
        sceRegMgrGetInt(static_cast<int>(f.base), &base_val);
        sceRegMgrGetInt(static_cast<int>(f.base + slot_off), &slot_val);
        out << base_val << '\n' << slot_val << '\n';
    }
    return true;
}

bool ApplyOfflineActivation(const Config &cfg, int slot, const std::string &account_id_hex, std::string *err) {
    if (slot < 1 || slot > kMaxSlots) {
        if (err) *err = "slot must be 1-16";
        return false;
    }
    uint64_t id = 0;
    if (!ParseByteOrderHex(account_id_hex, &id) || id == 0) {
        if (err) *err = "account_id must be 16 hex digits and not all zero";
        return false;
    }
    AccountSlot current = ReadSlot(slot, 0);
    if (current.name.empty()) {
        if (err) *err = "that slot has no local account to activate";
        return false;
    }

    if (!BackupAccountSlot(cfg, slot, err)) return false;

    bool ok = sceRegMgrSetBin(IdEntity(slot), &id, sizeof(id)) == 0;
    ok = sceRegMgrSetStr(TypeEntity(slot), "np", 3) == 0 && ok;
    ok = sceRegMgrSetInt(FlagsEntity(slot), kActivateFlags) == 0 && ok;

    if (!ok && err) *err = "one or more RegMgr writes failed — restore the backup and retry";
    return ok;
}

bool ApplySignIn(const Config &cfg, const std::string &account_id_hex, std::string *err) {
    AvatarUser user = GetAvatarUser();
    if (!user.ok) {
        if (err) *err = "no foreground user — sign in to a profile first";
        return false;
    }

    int slot = 0;
    for (int s = 1; s <= kMaxSlots; s++) {
        char name[32] = {0};
        sceRegMgrGetStr(NameEntity(s), name, sizeof(name));
        if (name[0] && user.user_name == name) { slot = s; break; }
    }
    if (slot == 0) {
        if (err) *err = "could not find a registry slot for the current user";
        return false;
    }

    uint64_t id = 0;
    if (!ParseByteOrderHex(account_id_hex, &id) || id == 0) {
        if (err) *err = "account_id must be 16 hex digits and not all zero";
        return false;
    }

    if (!BackupAccountSlot(cfg, slot, err)) return false;

    bool ok = sceRegMgrSetBin(IdEntity(slot), &id, sizeof(id)) == 0;
    ok = sceRegMgrSetStr(TypeEntity(slot), "np", 3) == 0 && ok;
    if (!ok) {
        if (err) *err = "could not write account_id/type";
        return false;
    }

    int32_t sys_lang = 1; // default English (US) if the read fails
    sceSystemServiceParamGetInt(1, &sys_lang);
    Region region = RegionFor(sys_lang);

    std::vector<uint8_t> cfgbuf(config_dat, config_dat + config_dat_len);
    PatchStr(cfgbuf, 0x1BE, region.country, 3);
    PatchStr(cfgbuf, 0x1C1, region.lang, 6);
    PatchStr(cfgbuf, 0x1C7, region.locale, 36);
    BuildConfigDat(cfgbuf, user.user_name, id);

    if (!WriteNpFiles(user.user_id, cfgbuf, err)) return false;

    uint32_t slot_off = static_cast<uint32_t>(slot - 1) * 65536u;
    WriteRegistryState(cfgbuf, 0);
    if (slot > 1) WriteRegistryState(cfgbuf, slot_off);

    NotifyDebug("\xF0\x9F\x91\x8D " + NotifyText(Msg::kSignedIn));
    return true;
}

bool RestoreAccountSlot(const Config &cfg, int slot, std::string *err) {
    if (slot < 1 || slot > kMaxSlots) {
        if (err) *err = "slot must be 1-16";
        return false;
    }
    std::ifstream in(BackupPath(cfg, slot));
    if (!in.is_open()) {
        if (err) *err = "no backup found for that slot";
        return false;
    }
    std::string name, id_hex, type, flags_line;
    std::getline(in, name);
    std::getline(in, id_hex);
    std::getline(in, type);
    std::getline(in, flags_line);

    uint64_t id = 0;
    if (!ParseHexId(id_hex, &id)) {
        if (err) *err = "backup file is corrupt";
        return false;
    }
    int flags = std::atoi(flags_line.c_str());

    bool ok = sceRegMgrSetBin(IdEntity(slot), &id, sizeof(id)) == 0;
    ok = sceRegMgrSetStr(TypeEntity(slot), type.c_str(), type.size() + 1) == 0 && ok;
    ok = sceRegMgrSetInt(FlagsEntity(slot), flags) == 0 && ok;

    uint32_t slot_off = static_cast<uint32_t>(slot - 1) * 65536u;
    for (const auto &f : kStrFields) {
        std::string base_val, slot_val;
        std::getline(in, base_val);
        std::getline(in, slot_val);
        ok = sceRegMgrSetStr(static_cast<int>(f.base), base_val.c_str(), base_val.size() + 1) == 0 && ok;
        ok = sceRegMgrSetStr(static_cast<int>(f.base + slot_off), slot_val.c_str(), slot_val.size() + 1) == 0 && ok;
    }
    for (const auto &f : kIntFields) {
        std::string base_line, slot_line;
        std::getline(in, base_line);
        std::getline(in, slot_line);
        ok = sceRegMgrSetInt(static_cast<int>(f.base), std::atoi(base_line.c_str())) == 0 && ok;
        ok = sceRegMgrSetInt(static_cast<int>(f.base + slot_off), std::atoi(slot_line.c_str())) == 0 && ok;
    }

    if (!ok && err) *err = "one or more RegMgr writes failed while restoring";
    return ok;
}

} // namespace prospero
