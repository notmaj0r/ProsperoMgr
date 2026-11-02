#include "storage_scan.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>
#include <vector>

#include "file_jobs.hpp"
#include "file_manager.hpp"
#include "job_queue.hpp"
#include "json_writer.hpp"
#include "log.hpp"

namespace fs = std::filesystem;

namespace prospero {
namespace {

// A node is ~130 bytes, so the cap keeps a pathological tree near 30 MB. Folders past it
// are still walked and counted, just folded into their nearest recorded ancestor.
constexpr size_t kMaxNodes = 250000;
// Files below this size never get a node of their own — shader caches and save folders
// hold tens of thousands of them, and in a treemap they are invisible anyway.
constexpr uint64_t kMinFileNodeBytes = 1024ull * 1024ull;
constexpr size_t kMaxFileNodesPerDir = 64;
constexpr size_t kTopFiles = 200;
constexpr size_t kMaxExtensions = 512;
constexpr size_t kCategoryPaths = 8;

enum NodeKind : uint8_t { kDir, kFile, kMount };

struct Node {
    std::string name;
    int32_t parent = -1;
    uint8_t kind = kDir;
    bool truncated = false;  // some subfolders were folded in because of kMaxNodes
    bool unreadable = false; // opendir() failed
    bool title = false;      // looks like an unpacked game: sce_sys/param.json or param.sfo
    // Recursive totals, filled in after the walk.
    uint64_t size = 0;
    uint64_t alloc = 0;
    uint64_t files = 0;
    uint64_t dirs = 0;
    // Files (and folded folders) counted here without a node of their own.
    uint64_t loose_size = 0;
    uint64_t loose_alloc = 0;
    uint64_t loose_files = 0;
    uint64_t loose_dirs = 0;
    std::vector<int32_t> children;
};

struct ExtStat {
    uint64_t files = 0;
    uint64_t size = 0;
    uint64_t alloc = 0;
};

struct TopFile {
    uint64_t size = 0;
    uint64_t alloc = 0;
    std::string path;
    bool operator>(const TopFile &other) const { return size > other.size; }
};

struct ScanResult {
    std::string root;
    std::vector<Node> nodes; // nodes[0] is the root; a child's index is always above its parent's
    std::vector<TopFile> top_files; // largest first
    std::map<std::string, ExtStat> extensions;
    uint64_t errors = 0;
    bool truncated = false;
    std::string summary_json;
};

std::mutex g_mu;
StorageScanProgress g_progress;
std::shared_ptr<const ScanResult> g_result;
std::atomic<bool> g_cancel {false};

int64_t NowSeconds() { return static_cast<int64_t>(std::time(nullptr)); }

std::string JoinPath(const std::string &dir, const std::string &name) {
    std::string out = dir;
    if (out.empty() || out.back() != '/') out.push_back('/');
    out += name;
    return out;
}

std::string LowerExtension(const std::string &name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size() || name.size() - dot > 11) return "";
    std::string ext = name.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool IsTitleFolder(const std::string &dir) {
    struct stat st {};
    return stat(JoinPath(dir, "sce_sys/param.json").c_str(), &st) == 0 ||
           stat(JoinPath(dir, "sce_sys/param.sfo").c_str(), &st) == 0;
}

std::string NodePath(const ScanResult &result, int32_t index) {
    std::vector<const std::string *> parts;
    for (int32_t i = index; i > 0; i = result.nodes[i].parent) parts.push_back(&result.nodes[i].name);
    std::string out = result.root;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out = JoinPath(out, **it);
    return out;
}

void SetCurrent(const std::string &path) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_progress.current = path;
}

// Waits out a copy/move/upload instead of competing with it for the disk. Returns false
// when the scan was canceled while waiting.
bool WaitForFileOperations() {
    if (!FileMutationInProgress()) return !g_cancel.load();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_progress.state = "paused";
    }
    while (FileMutationInProgress()) {
        if (g_cancel.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_progress.state = "scanning";
    return !g_cancel.load();
}

struct PendingDir {
    std::string path;
    int32_t node = 0;
    bool own = true; // false: folded into an ancestor because of kMaxNodes
};

struct FileCandidate {
    std::string name;
    uint64_t size = 0;
    uint64_t alloc = 0;
};

// Walks `root` without leaving its filesystem. Returns false on cancel.
bool Walk(ScanResult *result, dev_t root_dev) {
    std::vector<PendingDir> stack;
    stack.push_back({result->root, 0, true});
    std::unordered_set<ino_t> linked_inodes;
    std::priority_queue<TopFile, std::vector<TopFile>, std::greater<TopFile>> top;
    uint64_t files = 0, dirs = 0, bytes = 0;
    size_t since_check = 0;

    while (!stack.empty()) {
        PendingDir dir = std::move(stack.back());
        stack.pop_back();
        if (!WaitForFileOperations()) return false;
        SetCurrent(dir.path);

        DIR *handle = opendir(dir.path.c_str());
        if (!handle) {
            if (dir.own) result->nodes[dir.node].unreadable = true;
            result->errors++;
            continue;
        }
        std::vector<std::string> subdirs;
        std::vector<FileCandidate> candidates;
        errno = 0;
        while (dirent *entry = readdir(handle)) {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            if ((++since_check & 1023u) == 0 && g_cancel.load()) {
                closedir(handle);
                return false;
            }
            std::string name = entry->d_name;
            std::string child = JoinPath(dir.path, name);
            struct stat st {};
            if (lstat(child.c_str(), &st) != 0) {
                result->errors++;
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (IsKernelUnsafePath(child)) continue;
                if (st.st_dev != root_dev) {
                    // Another filesystem mounted here — shown, never entered.
                    if (dir.own && result->nodes.size() < kMaxNodes) {
                        Node mount;
                        mount.name = name;
                        mount.parent = dir.node;
                        mount.kind = kMount;
                        result->nodes[dir.node].children.push_back(static_cast<int32_t>(result->nodes.size()));
                        result->nodes.push_back(std::move(mount));
                    }
                    continue;
                }
                if (dir.own && name == "sce_sys" && IsTitleFolder(dir.path)) result->nodes[dir.node].title = true;
                subdirs.push_back(std::move(name));
                continue;
            }
            if (!S_ISREG(st.st_mode)) continue; // symlinks, sockets, fifos: no data of their own
            if (st.st_nlink > 1 && !linked_inodes.insert(st.st_ino).second) continue;

            uint64_t size = st.st_size > 0 ? static_cast<uint64_t>(st.st_size) : 0;
            uint64_t alloc = st.st_blocks > 0 ? static_cast<uint64_t>(st.st_blocks) * 512ull : 0;
            files++;
            bytes += size;

            std::string ext = LowerExtension(name);
            auto ext_it = result->extensions.find(ext);
            if (ext_it == result->extensions.end()) {
                if (result->extensions.size() >= kMaxExtensions) ext = "*";
                ext_it = result->extensions.emplace(ext, ExtStat {}).first;
            }
            ext_it->second.files++;
            ext_it->second.size += size;
            ext_it->second.alloc += alloc;

            if (top.size() < kTopFiles || size > top.top().size) {
                top.push(TopFile {size, alloc, child});
                if (top.size() > kTopFiles) top.pop();
            }

            if (dir.own && size >= kMinFileNodeBytes) {
                candidates.push_back(FileCandidate {std::move(name), size, alloc});
            } else {
                Node &target = result->nodes[dir.node];
                target.loose_size += size;
                target.loose_alloc += alloc;
                target.loose_files++;
            }
            errno = 0;
        }
        if (errno != 0) result->errors++;
        closedir(handle);
        dirs++;

        if (candidates.size() > kMaxFileNodesPerDir) {
            std::partial_sort(candidates.begin(), candidates.begin() + kMaxFileNodesPerDir, candidates.end(),
                              [](const FileCandidate &a, const FileCandidate &b) { return a.size > b.size; });
        }
        for (size_t i = 0; i < candidates.size(); i++) {
            if (i < kMaxFileNodesPerDir && result->nodes.size() < kMaxNodes) {
                Node file;
                file.name = std::move(candidates[i].name);
                file.parent = dir.node;
                file.kind = kFile;
                file.size = candidates[i].size;
                file.alloc = candidates[i].alloc;
                result->nodes[dir.node].children.push_back(static_cast<int32_t>(result->nodes.size()));
                result->nodes.push_back(std::move(file));
            } else {
                Node &target = result->nodes[dir.node];
                target.loose_size += candidates[i].size;
                target.loose_alloc += candidates[i].alloc;
                target.loose_files++;
            }
        }

        for (auto &name : subdirs) {
            std::string path = JoinPath(dir.path, name);
            if (dir.own && result->nodes.size() < kMaxNodes) {
                Node sub;
                sub.name = std::move(name);
                sub.parent = dir.node;
                int32_t index = static_cast<int32_t>(result->nodes.size());
                result->nodes[dir.node].children.push_back(index);
                result->nodes.push_back(std::move(sub));
                stack.push_back({std::move(path), index, true});
            } else {
                result->nodes[dir.node].truncated = true;
                result->nodes[dir.node].loose_dirs++;
                result->truncated = true;
                stack.push_back({std::move(path), dir.node, false});
            }
        }

        std::lock_guard<std::mutex> lock(g_mu);
        g_progress.files = files;
        g_progress.dirs = dirs;
        g_progress.bytes = bytes;
        g_progress.errors = result->errors;
    }

    result->top_files.reserve(top.size());
    while (!top.empty()) {
        result->top_files.push_back(top.top());
        top.pop();
    }
    std::reverse(result->top_files.begin(), result->top_files.end());
    return true;
}

// Children always sit at a higher index than their parent, so one backwards pass
// rolls every total up without recursion.
void Aggregate(ScanResult *result) {
    auto &nodes = result->nodes;
    for (auto &node : nodes) {
        if (node.kind == kDir) {
            node.size = node.loose_size;
            node.alloc = node.loose_alloc;
            node.files = node.loose_files;
            node.dirs = node.loose_dirs;
        } else if (node.kind == kFile) {
            node.files = 1;
        }
    }
    for (size_t i = nodes.size(); i-- > 1;) {
        Node &child = nodes[i];
        Node &parent = nodes[child.parent];
        parent.size += child.size;
        parent.alloc += child.alloc;
        parent.files += child.files;
        parent.dirs += child.dirs + (child.kind == kDir ? 1 : 0);
    }
}

// ---- Categories ------------------------------------------------------------
// Paths are relative to the console's user partition (/user). The PS5's own Storage
// screen only knows Games and Apps / Media Gallery / Saved Data / Other — everything
// below that isn't one of the first three is what it lumps into "Other".

struct CategoryRule {
    const char *pattern;
    const char *id;
};

const CategoryRule kRules[] = {
    {"app", "games"},
    {"patch", "patches"},
    {"addcont", "addons"},
    {"appmeta", "app_metadata"},
    {"av_contents", "media"},
    {"home/*/savedata_prospero", "saves"},
    {"home/*/savedata", "saves"},
    {"home/*/savedata_prospero_meta", "saves"},
    {"home/*/savedata_meta", "saves"},
    {"home", "profiles"},
    {"download", "downloads"},
    {"update", "updates"},
    {"data", "app_data"},
    {"temp", "temp"},
    {"tmp", "temp"},
    {"license", "licenses"},
};

const char *BucketFor(const std::string &id) {
    if (id == "games" || id == "patches" || id == "addons") return "games";
    if (id == "media") return "media";
    if (id == "saves") return "saves";
    return "other";
}

// Only these give way to content-based detection (game dumps, loose PKGs, disk images):
// a title folder inside /user/app is a game, inside /data it's a dump someone copied there.
bool Overridable(const std::string &id) {
    return id == "app_data" || id == "unclassified" || id == "temp";
}

const char *CategoryForFile(const std::string &name) {
    std::string ext = LowerExtension(name);
    if (ext == "pkg") return "pkg_files";
    if (ext == "exfat" || ext == "ffpkg" || ext == "ffpfs" || ext == "img" || ext == "iso") return "disk_images";
    return nullptr;
}

std::vector<std::string> SplitPath(const std::string &path) {
    std::vector<std::string> out;
    for (const auto &part : fs::path(path)) {
        std::string text = part.string();
        if (!text.empty() && text != "/") out.push_back(text);
    }
    return out;
}

struct SplitRule {
    std::vector<std::string> parts;
    std::string id;
};

bool PrefixMatches(const std::vector<std::string> &pattern, const std::vector<std::string> &rel) {
    if (rel.size() > pattern.size()) return false;
    for (size_t i = 0; i < rel.size(); i++) {
        if (pattern[i] != "*" && pattern[i] != rel[i]) return false;
    }
    return true;
}

// Where the scan root sits relative to /user, or false when rules don't apply (USB, etc.).
bool RootRelativeToUser(const std::string &root, std::vector<std::string> *rel, bool *strip_user) {
    std::vector<std::string> parts = SplitPath(root);
    *strip_user = false;
    if (!parts.empty() && parts[0] == "user") {
        rel->assign(parts.begin() + 1, parts.end());
        return true;
    }
    // /data is the same content as /user/data on the console.
    if (!parts.empty() && parts[0] == "data") {
        rel->assign(parts.begin(), parts.end());
        return true;
    }
    // Extended storage keeps its own user/ tree for games installed there.
    if (parts.size() >= 2 && parts[0] == "mnt" && (parts[1] == "ext0" || parts[1] == "ext1")) {
        rel->assign(parts.begin() + 2, parts.end());
        *strip_user = true;
        return true;
    }
    return false;
}

// A place in the tree where a category starts, with only the bytes that belong to that
// category — /user/home is "profiles" but the save folders below it are not.
struct CategoryEntry {
    int32_t node = 0;
    uint64_t size = 0;
    uint64_t alloc = 0;
};

struct CategoryTotal {
    uint64_t size = 0;
    uint64_t alloc = 0;
    uint64_t files = 0;
    std::vector<CategoryEntry> entries;
};

std::string BuildSummary(const ScanResult &result) {
    std::vector<SplitRule> rules;
    for (const auto &rule : kRules) rules.push_back({SplitPath(rule.pattern), rule.id});

    std::vector<std::string> root_rel;
    bool strip_user = false;
    bool rules_on = RootRelativeToUser(result.root, &root_rel, &strip_user);

    std::map<std::string, CategoryTotal> totals;
    std::vector<int32_t> mounts;

    struct Item {
        int32_t node;
        std::string category;
        int32_t entry; // index into totals[category].entries, -1 for the root's own files
        std::vector<std::string> rel; // only kept while some rule could still match below
        bool live;
    };
    std::vector<Item> stack;
    stack.push_back({0, "unclassified", -1, root_rel, rules_on});
    const auto &nodes = result.nodes;

    while (!stack.empty()) {
        Item item = std::move(stack.back());
        stack.pop_back();
        const Node &node = nodes[item.node];
        if (node.kind == kMount) {
            mounts.push_back(item.node);
            continue;
        }

        std::string category = item.category;
        bool live_children = false;
        if (item.live) {
            std::vector<std::string> rel = item.rel;
            if (strip_user && !rel.empty() && rel[0] == "user") rel.erase(rel.begin());
            for (const auto &rule : rules) {
                if (rule.parts.size() == rel.size() && PrefixMatches(rule.parts, rel)) {
                    category = rule.id;
                    break;
                }
            }
            for (const auto &rule : rules) {
                if (rule.parts.size() > rel.size() && PrefixMatches(rule.parts, rel)) live_children = true;
            }
        }
        if (Overridable(category)) {
            if (node.kind == kDir && node.title) category = "game_dumps";
            if (node.kind == kFile) {
                if (const char *by_type = CategoryForFile(node.name)) category = by_type;
            }
        }

        CategoryTotal &total = totals[category];
        int32_t entry = item.entry;
        if (item.node != 0 && (node.parent == 0 || category != item.category)) {
            entry = static_cast<int32_t>(total.entries.size());
            total.entries.push_back({item.node, 0, 0});
        }
        uint64_t own_size = node.kind == kFile ? node.size : node.loose_size;
        uint64_t own_alloc = node.kind == kFile ? node.alloc : node.loose_alloc;
        total.size += own_size;
        total.alloc += own_alloc;
        total.files += node.kind == kFile ? 1 : node.loose_files;
        if (entry >= 0) {
            total.entries[entry].size += own_size;
            total.entries[entry].alloc += own_alloc;
        }
        if (node.kind == kFile) continue;
        for (int32_t child : node.children) {
            Item next {child, category, entry, {}, live_children};
            if (live_children) {
                next.rel = item.rel;
                next.rel.push_back(nodes[child].name);
            }
            stack.push_back(std::move(next));
        }
    }

    std::vector<std::pair<std::string, CategoryTotal *>> ordered;
    for (auto &entry : totals) {
        if (entry.second.size == 0 && entry.second.alloc == 0 && entry.second.entries.empty()) continue;
        ordered.push_back({entry.first, &entry.second});
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &a, const auto &b) { return a.second->alloc > b.second->alloc; });

    std::vector<std::string> categories;
    for (auto &[id, total] : ordered) {
        std::sort(total->entries.begin(), total->entries.end(),
                  [](const CategoryEntry &a, const CategoryEntry &b) { return a.alloc > b.alloc; });
        std::vector<std::string> paths;
        for (size_t i = 0; i < total->entries.size() && i < kCategoryPaths; i++) {
            const CategoryEntry &entry = total->entries[i];
            paths.push_back(JsonWriter()
                                .Str("path", NodePath(result, entry.node))
                                .Str("type", nodes[entry.node].kind == kFile ? "file" : "dir")
                                .Int("size", static_cast<long long>(entry.size))
                                .Int("alloc", static_cast<long long>(entry.alloc))
                                .Done());
        }
        categories.push_back(JsonWriter()
                                 .Str("id", id)
                                 .Str("bucket", BucketFor(id))
                                 .Int("size", static_cast<long long>(total->size))
                                 .Int("alloc", static_cast<long long>(total->alloc))
                                 .Int("files", static_cast<long long>(total->files))
                                 .Int("entries", static_cast<long long>(total->entries.size()))
                                 .Raw("paths", JsonWriter::Array(paths))
                                 .Done());
    }

    std::vector<std::string> mount_paths;
    for (int32_t index : mounts) mount_paths.push_back(JsonWriter::Quote(NodePath(result, index)));

    struct statvfs usage {};
    uint64_t total_bytes = 0, free_bytes = 0, used_bytes = 0;
    if (statvfs(result.root.c_str(), &usage) == 0) {
        total_bytes = static_cast<uint64_t>(usage.f_blocks) * usage.f_frsize;
        free_bytes = static_cast<uint64_t>(usage.f_bavail) * usage.f_frsize;
        used_bytes = static_cast<uint64_t>(usage.f_blocks - usage.f_bfree) * usage.f_frsize;
    }
    // Only a mount root's used space is fully explained by its own tree.
    bool mount_root = result.root == "/";
    struct stat self {}, parent {};
    if (!mount_root && stat(result.root.c_str(), &self) == 0 &&
        stat(fs::path(result.root).parent_path().c_str(), &parent) == 0) {
        mount_root = self.st_dev != parent.st_dev;
    }

    const Node &root = nodes[0];
    return JsonWriter()
        .Str("root", result.root)
        .Raw("scanned", JsonWriter()
                            .Int("size", static_cast<long long>(root.size))
                            .Int("alloc", static_cast<long long>(root.alloc))
                            .Int("files", static_cast<long long>(root.files))
                            .Int("dirs", static_cast<long long>(root.dirs))
                            .Int("errors", static_cast<long long>(result.errors))
                            .Done())
        .Bool("truncated", result.truncated)
        .Bool("rules", rules_on)
        .Raw("volume", JsonWriter()
                           .Int("total", static_cast<long long>(total_bytes))
                           .Int("free", static_cast<long long>(free_bytes))
                           .Int("used", static_cast<long long>(used_bytes))
                           .Bool("mount_root", mount_root)
                           .Done())
        .Raw("categories", JsonWriter::Array(categories))
        .Raw("mounts", JsonWriter::Array(mount_paths))
        .Done();
}

// ---- Scan job --------------------------------------------------------------

void RunScan(std::string root) {
    auto started = std::chrono::steady_clock::now();
    auto result = std::make_shared<ScanResult>();
    result->root = root;
    result->nodes.emplace_back();
    result->nodes[0].name = root;

    // USB extended storage is fragile under concurrent access; the file-job layer
    // hands out a read lease for it and refuses writes while one is held.
    std::string lease_error;
    bool leased = BeginFileRead(root, &lease_error);
    struct stat st {};
    bool ok = leased && lstat(root.c_str(), &st) == 0;
    int stat_errno = errno;
    bool finished = ok && Walk(result.get(), st.st_dev);
    if (leased) EndFileRead(root);

    if (finished) {
        Aggregate(result.get());
        result->summary_json = BuildSummary(*result);
    }

    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::lock_guard<std::mutex> lock(g_mu);
    g_progress.finished_at = NowSeconds();
    g_progress.current.clear();
    if (finished) {
        g_progress.state = "done";
        g_result = result;
        LogEvent("Storage scan of " + root + " finished: " + std::to_string(result->nodes[0].files) + " files, " +
                 std::to_string(result->nodes[0].dirs) + " folders in " + std::to_string(static_cast<int>(seconds)) + "s");
    } else if (!ok) {
        g_progress.state = "error";
        g_progress.error = leased ? std::string("could not read scan root: ") + std::strerror(stat_errno) : lease_error;
    } else {
        g_progress.state = "canceled";
    }
}

std::string NodeJson(const ScanResult &result, int32_t index, const std::string *path, int depth, int limit) {
    const Node &node = result.nodes[index];
    JsonWriter json;
    json.Str("name", index == 0 ? result.root : node.name);
    if (path) json.Str("path", *path);
    json.Str("type", node.kind == kFile ? "file" : node.kind == kMount ? "mount" : "dir")
        .Int("size", static_cast<long long>(node.size))
        .Int("alloc", static_cast<long long>(node.alloc));
    if (node.kind == kFile) return json.Done();
    json.Int("files", static_cast<long long>(node.files)).Int("dirs", static_cast<long long>(node.dirs));
    if (node.title) json.Bool("title", true);
    if (node.truncated) json.Bool("truncated", true);
    if (node.unreadable) json.Bool("unreadable", true);
    bool has_children = !node.children.empty() || node.loose_files > 0;
    json.Bool("has_children", has_children);
    if (depth <= 0 || !has_children) return json.Done();

    std::vector<int32_t> order = node.children;
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        return result.nodes[a].alloc != result.nodes[b].alloc ? result.nodes[a].alloc > result.nodes[b].alloc
                                                              : result.nodes[a].size > result.nodes[b].size;
    });
    std::vector<std::string> children;
    size_t shown = std::min(order.size(), static_cast<size_t>(std::max(limit, 1)));
    for (size_t i = 0; i < shown; i++) children.push_back(NodeJson(result, order[i], nullptr, depth - 1, limit));
    if (shown < order.size()) {
        uint64_t size = 0, alloc = 0, files = 0;
        for (size_t i = shown; i < order.size(); i++) {
            size += result.nodes[order[i]].size;
            alloc += result.nodes[order[i]].alloc;
            files += result.nodes[order[i]].files;
        }
        children.push_back(JsonWriter()
                               .Str("type", "rest")
                               .Int("count", static_cast<long long>(order.size() - shown))
                               .Int("files", static_cast<long long>(files))
                               .Int("size", static_cast<long long>(size))
                               .Int("alloc", static_cast<long long>(alloc))
                               .Done());
    }
    if (node.loose_files > 0 || node.loose_dirs > 0) {
        children.push_back(JsonWriter()
                               .Str("type", "files")
                               .Int("files", static_cast<long long>(node.loose_files))
                               .Int("dirs", static_cast<long long>(node.loose_dirs))
                               .Int("size", static_cast<long long>(node.loose_size))
                               .Int("alloc", static_cast<long long>(node.loose_alloc))
                               .Done());
    }
    json.Raw("children", JsonWriter::Array(children));
    return json.Done();
}

std::shared_ptr<const ScanResult> CurrentResult(std::string *err) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_result && err) *err = "no storage scan has finished yet";
    return g_result;
}

} // namespace

bool StartStorageScan(const std::string &raw_root, std::string *err) {
    if (raw_root.empty() || raw_root.front() != '/') {
        if (err) *err = "scan path must be absolute";
        return false;
    }
    for (const auto &part : fs::path(raw_root)) {
        if (part == "..") {
            if (err) *err = "parent traversal is not allowed";
            return false;
        }
    }
    std::string root = fs::path(raw_root).lexically_normal().string();
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    if (IsKernelUnsafePath(root)) {
        if (err) *err = "this path is a kernel device tree and cannot be scanned safely";
        return false;
    }
    struct stat st {};
    if (lstat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (err) *err = "scan path is not a folder";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_progress.state == "scanning" || g_progress.state == "paused") {
            if (err) *err = "a storage scan is already running";
            return false;
        }
        g_cancel.store(false);
        g_progress = StorageScanProgress {};
        g_progress.state = "scanning";
        g_progress.root = root;
        g_progress.started_at = NowSeconds();
    }
    if (!StartBackgroundThread([root] { RunScan(root); })) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_progress.state = "error";
        g_progress.error = "could not start the scan thread";
        if (err) *err = g_progress.error;
        return false;
    }
    return true;
}

bool CancelStorageScan(std::string *err) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_progress.state != "scanning" && g_progress.state != "paused") {
        if (err) *err = "no storage scan is running";
        return false;
    }
    g_cancel.store(true);
    return true;
}

StorageScanProgress GetStorageScanProgress() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_progress;
}

bool StorageScanTreeJson(const std::string &raw_path, int depth, int limit, std::string *json, std::string *err) {
    auto result = CurrentResult(err);
    if (!result) return false;
    std::string path = raw_path.empty() ? result->root : fs::path(raw_path).lexically_normal().string();
    while (path.size() > 1 && path.back() == '/') path.pop_back();

    int32_t index = 0;
    if (path != result->root) {
        std::string prefix = result->root == "/" ? "/" : result->root + "/";
        if (path.compare(0, prefix.size(), prefix) != 0) {
            if (err) *err = "path is outside the scanned folder";
            return false;
        }
        for (const auto &part : SplitPath(path.substr(prefix.size()))) {
            int32_t next = -1;
            for (int32_t child : result->nodes[index].children) {
                if (result->nodes[child].kind != kFile && result->nodes[child].name == part) {
                    next = child;
                    break;
                }
            }
            if (next < 0) {
                if (err) *err = "folder not found in the scan result";
                return false;
            }
            index = next;
        }
    }
    depth = std::clamp(depth, 0, 4);
    limit = std::clamp(limit, 1, 500);
    *json = JsonWriter()
                .Str("root", result->root)
                .Raw("node", NodeJson(*result, index, &path, depth, limit))
                .Done();
    return true;
}

bool StorageScanSummaryJson(std::string *json, std::string *err) {
    auto result = CurrentResult(err);
    if (!result) return false;
    *json = result->summary_json;
    return true;
}

bool StorageScanFilesJson(int limit, std::string *json, std::string *err) {
    auto result = CurrentResult(err);
    if (!result) return false;
    limit = std::clamp(limit, 1, static_cast<int>(kTopFiles));

    std::vector<std::string> top;
    for (size_t i = 0; i < result->top_files.size() && i < static_cast<size_t>(limit); i++) {
        const auto &file = result->top_files[i];
        top.push_back(JsonWriter()
                          .Str("path", file.path)
                          .Int("size", static_cast<long long>(file.size))
                          .Int("alloc", static_cast<long long>(file.alloc))
                          .Done());
    }

    std::vector<std::pair<std::string, ExtStat>> exts(result->extensions.begin(), result->extensions.end());
    std::sort(exts.begin(), exts.end(), [](const auto &a, const auto &b) { return a.second.alloc > b.second.alloc; });
    std::vector<std::string> types;
    for (const auto &[ext, stat] : exts) {
        if (types.size() >= 100) break;
        types.push_back(JsonWriter()
                            .Str("ext", ext)
                            .Int("files", static_cast<long long>(stat.files))
                            .Int("size", static_cast<long long>(stat.size))
                            .Int("alloc", static_cast<long long>(stat.alloc))
                            .Done());
    }
    *json = JsonWriter()
                .Raw("largest", JsonWriter::Array(top))
                .Raw("types", JsonWriter::Array(types))
                .Int("type_count", static_cast<long long>(exts.size()))
                .Done();
    return true;
}

} // namespace prospero
