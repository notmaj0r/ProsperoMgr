#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace prospero {

struct FileManagerEntry {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
    uint32_t mode = 0;
    std::string modified; // UTC YYYYMMDDHHMMSS, matching the existing UI contract
};

struct StorageVolume {
    std::string label;   // English fallback for clients that don't translate
    std::string kind;    // "internal" | "user" | "usb" | "ext_usb" | "ext_m2"
    int index = -1;      // USB port number, -1 when the kind has only one slot
    std::string path;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
};

enum class RenameDeviceRelation {
    same,
    crosses,
    unknown,
};

// Paths that panic the PS5 kernel when walked or stat'd. Every entry point
// that touches the filesystem must refuse these before opening anything.
bool IsKernelUnsafePath(const std::string &path);
bool IsProtectedNativeRoot(const std::string &path);
bool IsExternalStoragePath(const std::string &path);

// Only a confirmed same-device result is safe for rename().
RenameDeviceRelation CheckRenameDeviceRelation(const std::string &from, const std::string &to);

// All public operations require normalized absolute paths and reject parent traversal.
// Mutation additionally refuses filesystem roots that should never be removable via a UI typo.
bool ListNativeDirectory(const std::string &path, std::vector<FileManagerEntry> *entries, std::string *err);
// Recursive size is explicit and never part of a normal directory listing.
bool NativeDirectorySize(const std::string &path, uint64_t *out_size, std::string *err);
bool CreateNativeDirectory(const std::string &path, std::string *err);
bool DeleteNativePath(const std::string &path, std::string *err);
bool SetNativePermissions(const std::string &path, uint32_t mode, std::string *err);
bool SetNativePermissionsTree(const std::string &path, uint32_t mode, std::string *err);
// Non-following POSIX tree removal used by background staging cleanup too — unlike libc++
// remove_all on Orbis, won't misclassify files as dirs or follow symlinks. Callers own root policy.
bool RemoveNativeTree(const std::string &path, std::string *err);
bool RenameNativePath(const std::string &from, const std::string &to, std::string *err);

std::vector<StorageVolume> ListStorageVolumes();
// `target_path` is the file that will be written — checks free space on *its parent's*
// filesystem (statvfs(dirname(target_path))). Don't pass a directory; see CheckFreeSpaceAtDirectory.
bool CheckUploadSpace(const std::string &target_path, uint64_t bytes, uint64_t *available, std::string *err);
// `dir_path` is the directory content will be written into — checks free space on its own
// filesystem, walking up to the nearest existing ancestor. Avoids landing one level too high at a mount boundary.
bool CheckFreeSpaceAtDirectory(const std::string &dir_path, uint64_t bytes, uint64_t *available, std::string *err);

} // namespace prospero
