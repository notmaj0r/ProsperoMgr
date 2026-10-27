#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"

namespace prospero {

struct FileJobInfo {
    uint64_t id = 0;
    std::string type;   // copy, move, delete, zip, unzip
    std::string state;  // queued, scanning, running, done, attention, canceled, error
    std::string source;
    std::string destination;
    std::string current;
    uint64_t current_index = 0; // 1-based position of `current` within total_items
    uint64_t total_items = 0;   // file count (not bytes) — zip/unzip only, 0 for copy/move
    uint64_t completed_bytes = 0;
    uint64_t total_bytes = 0;
    std::string error;
    // Machine-readable classification of `error` (e.g. "no_space") so the UI
    // can show a specific message instead of parsing free text.
    std::string error_code;
    std::string compression = "smart";    // zip only: smart, store, fast, balanced
    std::string conflict_policy = "skip"; // skip, replace, keep_both; copy/move also accept cancel
    bool delete_source_on_success = false; // unzip only
    std::string archive_password; // unzip only, encrypted .rar/.zip sources — never serialized to JSON
    // Download-as-ZIP writes into the temp archive area, not the visible
    // tree — /api/ftp/download unlinks it the moment it opens the file.
    bool ephemeral = false;
};

void InitializeFileJobs(const Config &cfg);
bool BeginImmediateFileMutation(std::string *err);
void EndImmediateFileMutation();
bool FileMutationInProgress();
bool BeginFileRead(const std::string &path, std::string *err);
void EndFileRead(const std::string &path);
bool ReserveFileUpload(const std::string &path, const std::string &token, bool expires_when_idle,
                       std::string *err);
void IdleFileUpload(const std::string &path, const std::string &token);
void ReleaseFileUpload(const std::string &path, const std::string &token);
bool QueueFileJob(const std::string &type, const std::string &source, const std::string &destination,
                  const std::string &conflict_policy, uint64_t *job_id, std::string *err);
bool QueueDeleteJob(const std::string &path, uint64_t *job_id, std::string *err);
// One archive from several files/folders — a single source behaves like a
// plain archive; more than one keeps each top-level name as a prefix.
bool QueueZipJob(const std::vector<std::string> &sources, const std::string &destination,
                 const std::string &compression, uint64_t *job_id, std::string *err);
// Same as QueueZipJob but written under the Prospero temp directory for a
// browser download. `out_path` is the archive path once queued.
bool QueueDownloadZipJob(const std::vector<std::string> &sources, const std::string &name,
                         uint64_t *job_id, std::string *out_path, std::string *err);
// `destination` may already exist — entries merge into it per `conflict_policy`.
// `password` is used for encrypted .rar and .zip sources (ZipCrypto and WinZip AES);
// it's ignored for archives that aren't encrypted.
bool QueueUnzipJob(const std::string &source, const std::string &destination, const std::string &conflict_policy,
                   bool delete_source_on_success, const std::string &password, uint64_t *job_id, std::string *err);
std::vector<FileJobInfo> GetFileJobs();
bool CancelFileJob(uint64_t id, std::string *err);
void ClearFinishedFileJobs();

// Case-insensitive ".zip" check, so archive.ZIP matches too.
bool IsZipFilename(const std::string &name);

// Same, for ".rar" — extraction only, RAR archives can't be created here.
bool IsRarFilename(const std::string &name);

struct ArchiveEntryInfo {
    std::string name;
    bool is_dir = false;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
};

struct ArchiveInfo {
    uint64_t items = 0;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    std::vector<ArchiveEntryInfo> entries; // capped — see kArchiveInfoEntryCap in file_jobs.cpp
    bool entries_truncated = false;
};

// Reads only the ZIP central directory, never decompresses entries. `code` is set
// to "password_required" for an encrypted .rar or .zip with no/wrong password given.
bool ReadArchiveInfo(const std::string &path, const std::string &password, ArchiveInfo *out,
                     std::string *err, std::string *code = nullptr);

// Collision-safe rename: returns `path` unchanged if free, otherwise "name (1).ext",
// "name (2).ext"... Shared with the payload downloader's version-conflict handling.
std::string UniqueSiblingPath(const std::string &path);

} // namespace prospero
