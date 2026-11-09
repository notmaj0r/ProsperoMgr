#pragma once
// Tracks .elf payloads from bundled_dir (first-party, read-only) and
// payloads_dir (user-imported). Launching goes through ElfldrClient.

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"

namespace prospero {

// No path separators/traversal, and a recognized payload extension. Shared
// by PayloadRegistry and the repository installer so both reject the same things.
bool IsSafePayloadFilename(const std::string &name);

// Records where a payload came from ("repository" | "uploaded" | "usb") so the
// Payloads tab can group by origin. Downloads call this from repository.cpp.
void RecordPayloadSource(const Config &cfg, const std::string &name, const std::string &source);

// Records the catalog repo URL a payload was downloaded from, so the Payloads tab can
// later match it against the catalog's cached latest-version check to flag an update.
void RecordPayloadSourceUrl(const Config &cfg, const std::string &name, const std::string &url);

struct PayloadInfo {
    std::string name;   // filename, e.g. "ftpsrv.elf" — used as the lookup key
    std::string path;    // absolute path on disk
    uint64_t size_bytes = 0;
    int64_t mtime_unix = 0;
    bool bundled = false; // true if from bundled_dir (read-only, first-party)
    bool enabled = true;  // false if the user toggled it off — Launch() refuses these
    bool favorite = false;
    uint64_t launch_count = 0;
    int64_t last_launched_unix = 0;
    std::string installed_version;
    std::string source; // "repository" | "uploaded" | "usb" | "payloadManager", empty if unrecorded
    std::string source_url; // catalog repo URL this came from, empty if unknown/not from the catalog
};

class PayloadRegistry {
  public:
    explicit PayloadRegistry(Config cfg);

    // Scans both source directories. Bundled payloads are listed first.
    std::vector<PayloadInfo> List() const;

    // Finds a payload by filename across both directories.
    bool Resolve(const std::string &name, std::string *out_path) const;

    // Resolves `name` and streams it to elfldr. Returns false + fills
    // `err` on failure (not found, elfldr unreachable, I/O error).
    bool Launch(const std::string &name, std::string *err) const;

    // Removes a payload from `payloads_dir` only — bundled payloads can't
    // be deleted through the registry.
    bool Delete(const std::string &name, std::string *err) const;

    // Writes `data` as a new payload under `payloads_dir` (drag-and-drop import).
    bool Import(const std::string &name, const std::string &data, std::string *err) const;
    bool ImportFile(const std::string &name, const std::string &source_path, const std::string &source,
                    std::string *err) const;

    // Toggles whether a payload can be launched/autoloaded, without deleting
    // it. Applies to bundled payloads too (e.g. temporarily turn off ftpsrv).
    bool SetEnabled(const std::string &name, bool enabled, std::string *err) const;

    // Persistent library organization and launch history.
    bool SetFavorite(const std::string &name, bool favorite, std::string *err) const;
    bool Rename(const std::string &name, const std::string &new_name, std::string *err) const;

  private:
    Config cfg_;
};

} // namespace prospero
