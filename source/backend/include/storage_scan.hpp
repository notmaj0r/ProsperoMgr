#pragma once
// WinDirStat-style storage analyzer: one background walk of a filesystem builds an
// in-memory size tree that the UI then browses (treemap, folder tree, "what is Other").
//
// The walk never leaves the filesystem it started on (like `du -x`), so /user is not
// inflated by whatever happens to be mounted under it. Small files are folded into a
// per-folder aggregate instead of a node each, which bounds memory on the console.

#include <cstdint>
#include <string>

namespace prospero {

struct StorageScanProgress {
    std::string state = "idle"; // idle, scanning, paused, done, canceled, error
    std::string root;
    std::string current;
    std::string error;
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t bytes = 0;      // logical (st_size) bytes seen so far
    uint64_t errors = 0;     // entries that could not be read
    int64_t started_at = 0;  // unix seconds
    int64_t finished_at = 0; // unix seconds, 0 while running
};

// Starts a scan of `root` (an absolute directory). Replaces any finished result; refuses
// while another scan is running.
bool StartStorageScan(const std::string &root, std::string *err);
bool CancelStorageScan(std::string *err);
StorageScanProgress GetStorageScanProgress();

// JSON for the finished scan, or false with `err` when there is none / the path is unknown.
// `path` must be the scan root or a folder below it. `depth` levels of children are
// included (1 = direct children only) and each folder lists at most `limit` children,
// the rest folded into one "rest" item.
bool StorageScanTreeJson(const std::string &path, int depth, int limit, std::string *json, std::string *err);
// Category breakdown (Games / Media / Saves / Other and what Other is made of) plus
// the volume's own used/free numbers so the UI can show what the walk could not see.
bool StorageScanSummaryJson(std::string *json, std::string *err);
// The largest individual files and the per-extension totals.
bool StorageScanFilesJson(int limit, std::string *json, std::string *err);

} // namespace prospero
