#include "payload_registry.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "elfldr_client.hpp"

namespace fs = std::filesystem;

namespace prospero {

namespace {

bool IsElf(const fs::path &p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".elf" || ext == ".bin" || ext == ".self";
}

bool IsPrivateBundledHelper(const fs::path &p) {
    return p.filename() == "prospero-pkgd.elf" || p.filename() == "prospero-netd.elf";
}

} // namespace

bool IsSafePayloadFilename(const std::string &name) {
    if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) return false;
    return IsElf(name);
}

namespace {

// One flat file listing disabled filenames — applies across both bundled
// and imported payloads, same convention as .versions.txt.
std::string DisabledListPath(const Config &cfg) { return cfg.payloads_dir + "/.disabled.txt"; }
std::string FavoritesPath(const Config &cfg) { return cfg.payloads_dir + "/.favorites.txt"; }
std::string LaunchHistoryPath(const Config &cfg) { return cfg.payloads_dir + "/.launches.txt"; }
std::string VersionsPath(const Config &cfg) { return cfg.payloads_dir + "/.versions.txt"; }
std::string SourcesPath(const Config &cfg) { return cfg.payloads_dir + "/.sources.txt"; }
std::string SourceUrlsPath(const Config &cfg) { return cfg.payloads_dir + "/.source_urls.txt"; }
std::mutex g_state_mu;

std::vector<std::string> LoadDisabled(const Config &cfg) {
    std::vector<std::string> out;
    std::ifstream f(DisabledListPath(cfg));
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

std::vector<std::string> LoadLines(const std::string &path) {
    std::vector<std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) if (!line.empty()) out.push_back(line);
    return out;
}

bool SaveLines(const std::string &path, const std::vector<std::string> &lines, std::string *err) {
    std::string staged = path + ".tmp";
    std::ofstream out(staged, std::ios::trunc);
    if (!out.is_open()) { if (err) *err = "could not write payload state"; return false; }
    for (const auto &line : lines) out << line << '\n';
    out.flush();
    if (!out) { if (err) *err = "could not flush payload state"; return false; }
    out.close();
    std::error_code ec;
    fs::rename(staged, path, ec);
    if (ec) { fs::remove(staged, ec); if (err) *err = "could not publish payload state: " + ec.message(); return false; }
    return true;
}

struct LaunchStats { uint64_t count = 0; int64_t last = 0; };

std::unordered_map<std::string, LaunchStats> LoadLaunchStats(const Config &cfg) {
    std::unordered_map<std::string, LaunchStats> out;
    std::ifstream f(LaunchHistoryPath(cfg));
    std::string line;
    while (std::getline(f, line)) {
        size_t first = line.find('\t');
        size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        LaunchStats stats;
        std::istringstream(line.substr(first + 1, second - first - 1)) >> stats.count;
        std::istringstream(line.substr(second + 1)) >> stats.last;
        out[line.substr(0, first)] = stats;
    }
    return out;
}

bool SaveLaunchStats(const Config &cfg, const std::unordered_map<std::string, LaunchStats> &stats, std::string *err) {
    std::vector<std::string> lines;
    for (const auto &[name, value] : stats) lines.push_back(name + "\t" + std::to_string(value.count) + "\t" + std::to_string(value.last));
    return SaveLines(LaunchHistoryPath(cfg), lines, err);
}

// Shared by the "name=value" sidecars (.versions.txt, .sources.txt).
std::unordered_map<std::string, std::string> LoadKeyValues(const std::string &path) {
    std::unordered_map<std::string, std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t equals = line.find('=');
        if (equals != std::string::npos) out[line.substr(0, equals)] = line.substr(equals + 1);
    }
    return out;
}

std::unordered_map<std::string, std::string> LoadVersions(const Config &cfg) {
    return LoadKeyValues(VersionsPath(cfg));
}

bool SaveVersions(const Config &cfg, const std::unordered_map<std::string, std::string> & versions, std::string *err) {
    std::vector<std::string> lines;
    for (const auto &[name, version] : versions) lines.push_back(name + "=" + version);
    return SaveLines(VersionsPath(cfg), lines, err);
}

void ReplaceOrRemoveLine(const std::string &path, const std::string &name, const std::string &replacement) {
    auto lines = LoadLines(path);
    auto it = std::find(lines.begin(), lines.end(), name);
    if (it == lines.end()) return;
    if (replacement.empty()) lines.erase(it); else *it = replacement;
    std::string ignored;
    (void)SaveLines(path, lines, &ignored);
}

void AddPayload(const fs::path &file, bool bundled, std::vector<PayloadInfo> *out) {
    PayloadInfo info;
    info.name = file.filename().string();
    info.path = fs::absolute(file).string();

    // stat() instead of fs::last_write_time: file_time_type's epoch
    // isn't guaranteed Unix time on this libc++ build, gave garbage mtimes.
    struct stat st{};
    if (stat(info.path.c_str(), &st) == 0) {
        info.size_bytes = static_cast<uint64_t>(st.st_size);
        info.mtime_unix = static_cast<int64_t>(st.st_mtime);
    }

    info.bundled = bundled;
    out->push_back(std::move(info));
}

// `nested` also picks up payloads in subfolders, one level (<dir>/<name>/<file>.elf) or two
// (<dir>/local/<slot>/<file>.elf), the layouts Payload Manager uses. A loose file wins over a nested one.
void ScanDir(const std::string &dir, bool bundled, std::vector<PayloadInfo> *out, bool nested = false) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;

    auto subfolders = [&](const fs::path &parent) {
        std::vector<fs::path> found;
        for (const auto &entry : fs::directory_iterator(parent, ec)) {
            if (entry.is_directory(ec) && entry.path().filename().string()[0] != '.') found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end());
        return found;
    };
    auto addNested = [&](const fs::path &folder) {
        for (const auto &entry : fs::directory_iterator(folder, ec)) {
            if (!entry.is_regular_file(ec) || !IsElf(entry.path())) continue;
            std::string name = entry.path().filename().string();
            bool taken = std::any_of(out->begin(), out->end(), [&](const PayloadInfo &item) { return item.name == name; });
            if (!taken) AddPayload(entry.path(), bundled, out);
        }
    };

    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (!IsElf(entry.path())) continue;
        if (bundled && IsPrivateBundledHelper(entry.path())) continue;
        AddPayload(entry.path(), bundled, out);
    }
    if (!nested) return;

    for (const auto &first : subfolders(dir)) {
        addNested(first);
        for (const auto &second : subfolders(first)) addNested(second);
    }
}

// Where Payload Manager keeps what it downloads.
constexpr const char *kPayloadManagerDir = "/data/pldmgr/payloads";

bool InPayloadManagerDir(const std::string &path) { return path.rfind(std::string(kPayloadManagerDir) + "/", 0) == 0; }

// Uploaded payloads, plus the ones Payload Manager installed. On a name clash the newer file wins,
// so an update made by either manager is the copy that shows up.
std::vector<PayloadInfo> ImportedPayloads(const Config &cfg) {
    std::vector<PayloadInfo> imported;
    ScanDir(cfg.payloads_dir, /*bundled=*/false, &imported, /*nested=*/true);
    std::vector<PayloadInfo> shared;
    ScanDir(kPayloadManagerDir, /*bundled=*/false, &shared, /*nested=*/true);
    for (auto &candidate : shared) {
        auto existing = std::find_if(imported.begin(), imported.end(), [&](const PayloadInfo &item) { return item.name == candidate.name; });
        if (existing == imported.end()) imported.push_back(std::move(candidate));
        else if (candidate.mtime_unix > existing->mtime_unix) *existing = std::move(candidate);
    }
    return imported;
}

} // namespace

PayloadRegistry::PayloadRegistry(Config cfg) : cfg_(std::move(cfg)) {}

std::vector<PayloadInfo> PayloadRegistry::List() const {
    std::vector<PayloadInfo> out;
    ScanDir(cfg_.bundled_dir, /*bundled=*/true, &out);
    std::vector<PayloadInfo> imported = ImportedPayloads(cfg_);
    // An uploaded payload deliberately shadows a bundled one with the same name;
    // deleting the override reveals the bundled copy again.
    for (auto &candidate : imported) {
        auto existing = std::find_if(out.begin(), out.end(), [&](const PayloadInfo &item) {
            return item.name == candidate.name;
        });
        if (existing == out.end()) out.push_back(std::move(candidate));
        else *existing = std::move(candidate);
    }

    auto disabled = LoadDisabled(cfg_);
    auto favorites = LoadLines(FavoritesPath(cfg_));
    auto launches = LoadLaunchStats(cfg_);
    auto versions = LoadVersions(cfg_);
    auto sources = LoadKeyValues(SourcesPath(cfg_));
    auto source_urls = LoadKeyValues(SourceUrlsPath(cfg_));
    for (auto &info : out) {
        info.enabled = std::find(disabled.begin(), disabled.end(), info.name) == disabled.end();
        info.favorite = std::find(favorites.begin(), favorites.end(), info.name) != favorites.end();
        auto history = launches.find(info.name);
        if (history != launches.end()) {
            info.launch_count = history->second.count;
            info.last_launched_unix = history->second.last;
        }
        auto version = versions.find(info.name);
        if (version != versions.end()) info.installed_version = version->second;
        auto source = sources.find(info.name);
        if (source != sources.end()) info.source = source->second;
        auto source_url = source_urls.find(info.name);
        if (source_url != source_urls.end()) info.source_url = source_url->second;
        if (InPayloadManagerDir(info.path)) info.source = "payloadManager";
    }
    return out;
}

void RecordPayloadSource(const Config &cfg, const std::string &name, const std::string &source) {
    std::lock_guard<std::mutex> lock(g_state_mu);
    auto sources = LoadKeyValues(SourcesPath(cfg));
    sources[name] = source;
    std::vector<std::string> lines;
    for (const auto &[key, value] : sources) lines.push_back(key + "=" + value);
    std::string ignored;
    (void)SaveLines(SourcesPath(cfg), lines, &ignored);
}

void RecordPayloadSourceUrl(const Config &cfg, const std::string &name, const std::string &url) {
    std::lock_guard<std::mutex> lock(g_state_mu);
    auto urls = LoadKeyValues(SourceUrlsPath(cfg));
    urls[name] = url;
    std::vector<std::string> lines;
    for (const auto &[key, value] : urls) lines.push_back(key + "=" + value);
    std::string ignored;
    (void)SaveLines(SourceUrlsPath(cfg), lines, &ignored);
}

bool PayloadRegistry::Resolve(const std::string &name, std::string *out_path) const {
    for (const auto &info : List()) {
        if (info.name == name) {
            *out_path = info.path;
            return true;
        }
    }
    return false;
}

bool PayloadRegistry::Launch(const std::string &name, std::string *err) const {
    std::string path;
    if (!Resolve(name, &path)) {
        if (err) *err = "payload not found: " + name;
        return false;
    }

    auto disabled = LoadDisabled(cfg_);
    if (std::find(disabled.begin(), disabled.end(), name) != disabled.end()) {
        if (err) *err = name + " is disabled — enable it first";
        return false;
    }

    ElfldrClient client(Config::kElfldrHost, Config::kElfldrPort);
    if (!client.LaunchFile(path, err)) return false;
    {
        std::lock_guard<std::mutex> lock(g_state_mu);
        auto stats = LoadLaunchStats(cfg_);
        stats[name].count++;
        stats[name].last = static_cast<int64_t>(std::time(nullptr));
        std::string ignored;
        (void)SaveLaunchStats(cfg_, stats, &ignored);
    }
    return true;
}

bool PayloadRegistry::Delete(const std::string &name, std::string *err) const {
    if (!IsSafePayloadFilename(name)) {
        if (err) *err = "invalid payload name";
        return false;
    }

    // The listing knows where it lives, including inside a subfolder of a shared library.
    std::vector<PayloadInfo> imported = ImportedPayloads(cfg_);
    auto found = std::find_if(imported.begin(), imported.end(), [&](const PayloadInfo &item) { return item.name == name; });
    if (found == imported.end()) {
        if (err) *err = "payload not found in payloads_dir (bundled payloads can't be deleted): " + name;
        return false;
    }

    fs::path path = found->path;
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        if (err) *err = "remove() failed: " + ec.message();
        return false;
    }
    // A subfolder left with nothing but Payload Manager's meta.json goes too, so no empty slot stays behind.
    fs::path root = fs::absolute(cfg_.payloads_dir).lexically_normal();
    if (InPayloadManagerDir(path.string())) root = kPayloadManagerDir;
    if (!root.has_filename()) root = root.parent_path();
    fs::path parent = path.parent_path();
    if (parent != root && parent.string().rfind(root.string() + "/", 0) == 0) {
        bool only_meta = true;
        for (const auto &entry : fs::directory_iterator(parent, ec)) {
            if (entry.path().filename() != "meta.json") only_meta = false;
        }
        // Only meta.json can be left here, so two plain removes do it without a recursive removal.
        if (only_meta) {
            fs::remove(parent / "meta.json", ec);
            fs::remove(parent, ec);
        }
    }
    std::lock_guard<std::mutex> lock(g_state_mu);
    ReplaceOrRemoveLine(DisabledListPath(cfg_), name, "");
    ReplaceOrRemoveLine(FavoritesPath(cfg_), name, "");
    auto launches = LoadLaunchStats(cfg_);
    launches.erase(name);
    std::string ignored;
    (void)SaveLaunchStats(cfg_, launches, &ignored);
    auto versions = LoadVersions(cfg_);
    versions.erase(name);
    (void)SaveVersions(cfg_, versions, &ignored);
    return true;
}

bool PayloadRegistry::Import(const std::string &name, const std::string &data, std::string *err) const {
    if (!IsSafePayloadFilename(name)) {
        if (err) *err = "invalid payload name (expected a safe .elf/.bin/.self filename)";
        return false;
    }

    fs::path path = fs::path(cfg_.payloads_dir) / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (err) *err = "cannot open destination for writing";
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
        if (err) *err = "failed while writing payload";
        return false;
    }
    return true;
}

bool PayloadRegistry::ImportFile(const std::string &name, const std::string &source_path,
                                 const std::string &source, std::string *err) const {
    if (!IsSafePayloadFilename(name)) {
        if (err) *err = "invalid payload name (expected a safe .elf/.bin/.self filename)";
        return false;
    }
    if (source_path.empty()) {
        if (err) *err = "empty upload";
        return false;
    }

    fs::path destination = fs::path(cfg_.payloads_dir) / name;
    fs::path staged = destination;
    staged += ".uploading";

    std::error_code ec;
    fs::copy_file(source_path, staged, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (err) *err = "failed to stage payload: " + ec.message();
        return false;
    }

    fs::rename(staged, destination, ec);
    if (ec) {
        // rename() may not replace an existing destination on every /data filesystem,
        // so remove first and keep the partial copy out of the live registry.
        std::error_code remove_ec;
        fs::remove(destination, remove_ec);
        ec.clear();
        fs::rename(staged, destination, ec);
    }
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove(staged, cleanup_ec);
        if (err) *err = "failed to install payload: " + ec.message();
        return false;
    }
    if (!source.empty()) RecordPayloadSource(cfg_, name, source);
    return true;
}

bool PayloadRegistry::SetEnabled(const std::string &name, bool enabled, std::string *err) const {
    if (!IsSafePayloadFilename(name)) {
        if (err) *err = "invalid payload name";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_state_mu);
    auto disabled = LoadDisabled(cfg_);
    auto it = std::find(disabled.begin(), disabled.end(), name);

    if (enabled) {
        if (it != disabled.end()) disabled.erase(it);
    } else if (it == disabled.end()) {
        disabled.push_back(name);
    }

    return SaveLines(DisabledListPath(cfg_), disabled, err);
}

bool PayloadRegistry::SetFavorite(const std::string &name, bool favorite, std::string *err) const {
    if (!IsSafePayloadFilename(name)) { if (err) *err = "invalid payload name"; return false; }
    std::string path;
    if (!Resolve(name, &path)) { if (err) *err = "payload not found: " + name; return false; }
    std::lock_guard<std::mutex> lock(g_state_mu);
    auto favorites = LoadLines(FavoritesPath(cfg_));
    auto it = std::find(favorites.begin(), favorites.end(), name);
    if (favorite && it == favorites.end()) favorites.push_back(name);
    if (!favorite && it != favorites.end()) favorites.erase(it);
    return SaveLines(FavoritesPath(cfg_), favorites, err);
}

bool PayloadRegistry::Rename(const std::string &name, const std::string &new_name, std::string *err) const {
    if (!IsSafePayloadFilename(name) || !IsSafePayloadFilename(new_name)) {
        if (err) *err = "invalid payload name (expected .elf/.bin/.self without path separators)";
        return false;
    }
    if (name == new_name) return true;
    // The renamed file stays in its own folder, including a subfolder of a shared library.
    std::vector<PayloadInfo> imported = ImportedPayloads(cfg_);
    auto found = std::find_if(imported.begin(), imported.end(), [&](const PayloadInfo &item) { return item.name == name; });
    if (found == imported.end()) { if (err) *err = "only uploaded payloads can be renamed"; return false; }
    fs::path source = found->path;
    fs::path destination = source.parent_path() / new_name;
    std::error_code ec;
    bool taken = std::any_of(imported.begin(), imported.end(), [&](const PayloadInfo &item) { return item.name == new_name; });
    if (taken || fs::exists(destination, ec)) { if (err) *err = "a payload with that name already exists"; return false; }
    fs::rename(source, destination, ec);
    if (ec) { if (err) *err = "could not rename payload: " + ec.message(); return false; }

    std::lock_guard<std::mutex> lock(g_state_mu);
    ReplaceOrRemoveLine(DisabledListPath(cfg_), name, new_name);
    ReplaceOrRemoveLine(FavoritesPath(cfg_), name, new_name);
    std::string ignored;
    auto launches = LoadLaunchStats(cfg_);
    auto history = launches.find(name);
    if (history != launches.end()) { launches[new_name] = history->second; launches.erase(history); }
    (void)SaveLaunchStats(cfg_, launches, &ignored);
    auto versions = LoadVersions(cfg_);
    auto version = versions.find(name);
    if (version != versions.end()) { versions[new_name] = version->second; versions.erase(version); }
    (void)SaveVersions(cfg_, versions, &ignored);
    return true;
}

} // namespace prospero
