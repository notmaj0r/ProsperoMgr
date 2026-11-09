#include "exfat_image.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

namespace prospero {
namespace {

constexpr size_t kMaxDirBytes = 4 * 1024 * 1024;
constexpr uint32_t kEndOfChain = 0xFFFFFFF8u;

uint32_t Le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t Le64(const unsigned char *p) { return Le32(p) | (static_cast<uint64_t>(Le32(p + 4)) << 32); }

struct Volume {
    int fd = -1;
    uint64_t fat_offset = 0;
    uint64_t heap_offset = 0;
    uint32_t cluster_count = 0;
    uint32_t root_cluster = 0;
    uint32_t cluster_size = 0;
};

struct Entry {
    uint32_t first_cluster = 0;
    uint64_t length = 0;
    bool no_fat_chain = false;
    bool is_dir = false;
};

bool ReadAt(int fd, uint64_t offset, void *buffer, size_t size) {
    return pread(fd, buffer, size, static_cast<off_t>(offset)) == static_cast<ssize_t>(size);
}

// Follows a cluster chain (or a contiguous run) and returns up to `length` bytes, or up to `limit` when the length is unknown.
bool ReadClusters(const Volume &vol, const Entry &entry, size_t limit, std::string *out) {
    uint64_t want = entry.length ? entry.length : limit;
    if (want > limit) return false;
    std::string data;
    uint32_t cluster = entry.first_cluster;
    for (uint32_t hops = 0; data.size() < want && hops <= vol.cluster_count; hops++) {
        if (cluster < 2 || cluster >= vol.cluster_count + 2) break;
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(vol.cluster_size, want - data.size()));
        size_t at = data.size();
        data.resize(at + chunk);
        if (!ReadAt(vol.fd, vol.heap_offset + static_cast<uint64_t>(cluster - 2) * vol.cluster_size, &data[at], chunk)) return false;
        if (entry.no_fat_chain) {
            cluster++;
        } else {
            unsigned char next[4];
            if (!ReadAt(vol.fd, vol.fat_offset + static_cast<uint64_t>(cluster) * 4, next, 4)) return false;
            cluster = Le32(next);
            if (cluster >= kEndOfChain) break;
        }
    }
    if (entry.length && data.size() != entry.length) return false;
    *out = std::move(data);
    return true;
}

bool SameName(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

bool FindEntry(const Volume &vol, const Entry &dir, const std::string &name, Entry *found) {
    std::string data;
    if (!ReadClusters(vol, dir, kMaxDirBytes, &data)) return false;
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data.data());
    size_t count = data.size() / 32;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *file = bytes + i * 32;
        if (file[0] == 0x00) break;
        if (file[0] != 0x85) continue;
        size_t secondary = file[1];
        bool is_dir = (file[4] | (file[5] << 8)) & 0x10;
        Entry candidate;
        candidate.is_dir = is_dir;
        std::string entry_name;
        size_t name_length = 0;
        for (size_t j = 1; j <= secondary && i + j < count; j++) {
            const unsigned char *part = bytes + (i + j) * 32;
            if (part[0] == 0xC0) {
                candidate.no_fat_chain = part[1] & 0x02;
                name_length = part[3];
                candidate.first_cluster = Le32(part + 20);
                candidate.length = Le64(part + 24);
            } else if (part[0] == 0xC1) {
                for (int k = 0; k < 15 && entry_name.size() < name_length; k++) {
                    unsigned ch = part[2 + k * 2] | (part[3 + k * 2] << 8);
                    entry_name += ch < 0x80 ? static_cast<char>(ch) : '?';
                }
            }
        }
        if (SameName(entry_name, name)) {
            *found = candidate;
            return true;
        }
        i += secondary;
    }
    return false;
}

} // namespace

bool ReadExfatFile(const std::string &image_path, const std::string &inner_path, size_t max_bytes, std::string *out) {
    Volume vol;
    vol.fd = open(image_path.c_str(), O_RDONLY);
    if (vol.fd < 0) return false;
    struct Closer {
        int fd;
        ~Closer() { close(fd); }
    } closer{vol.fd};

    unsigned char boot[512];
    if (!ReadAt(vol.fd, 0, boot, sizeof(boot)) || std::memcmp(boot + 3, "EXFAT   ", 8) != 0) return false;
    unsigned sector_shift = boot[0x6C];
    unsigned cluster_shift = boot[0x6D];
    if (sector_shift < 9 || sector_shift > 12 || sector_shift + cluster_shift > 25) return false;
    vol.fat_offset = static_cast<uint64_t>(Le32(boot + 0x50)) << sector_shift;
    vol.heap_offset = static_cast<uint64_t>(Le32(boot + 0x58)) << sector_shift;
    vol.cluster_count = Le32(boot + 0x5C);
    vol.root_cluster = Le32(boot + 0x60);
    vol.cluster_size = 1u << (sector_shift + cluster_shift);

    Entry current;
    current.first_cluster = vol.root_cluster;
    current.is_dir = true;
    size_t start = 0;
    while (start < inner_path.size()) {
        size_t end = inner_path.find('/', start);
        if (end == std::string::npos) end = inner_path.size();
        if (!current.is_dir || !FindEntry(vol, current, inner_path.substr(start, end - start), &current)) return false;
        start = end + 1;
    }
    if (current.is_dir || current.length == 0) return false;
    return ReadClusters(vol, current, max_bytes, out);
}

} // namespace prospero
