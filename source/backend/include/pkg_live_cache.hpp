#pragma once
// RAM-only cache for a PKG streamed in live from a browser while the PS5
// installer reads it back out — no bytes ever touch disk, one session at a time.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace prospero {

inline constexpr uint64_t kPkgLiveSegSize = 1024ull * 1024ull;

// Invoked (without the cache's internal lock held) when a blocked reader needs a segment
// the browser hasn't sent yet, so the transport can ask it to resend just that one.
using PkgLiveSeekFn = std::function<void(uint64_t seg)>;

// Invoked (without the lock held) as the installer's sequential read position moves on, so the
// transport can tell the browser how far ahead it may send without overrunning the cache.
using PkgLivePosFn = std::function<void(uint64_t seg)>;

// Invoked when a session that was active gets torn down, so the transport can close the
// browser's connection right away instead of waiting out its own idle timeout.
using PkgLiveDoneFn = std::function<void()>;

struct PkgLiveStatus {
    bool active = false;
    bool aborted = false;
    bool complete = false; // every segment has been stored at least once
    std::string session_id;
    std::string filename;
    std::string title; // game name sent by the browser, may be empty
    uint64_t total = 0;
    uint64_t received = 0; // bytes ever acked (some may since have been evicted)
    uint64_t served = 0;   // bytes actually handed to the installer reader
    uint64_t reader_seg = 0;     // furthest segment the installer has finished reading
    uint64_t resident = 0;       // segments currently held in the cache
    uint64_t reader_wait_us = 0; // total time the installer's reads waited for data
    uint64_t busy_replies = 0;   // times a segment arrived with no free slot
    uint64_t resend_requests = 0;
    uint64_t wire_bytes = 0;      // every segment byte that reached the cache, resends included
    uint64_t duplicates = 0;      // segments that arrived while already resident
    uint64_t evicted_unread = 0;  // segments dropped before the installer read them
    int slots = 0;
};

// All methods are thread-safe; there is exactly one session at a time.
namespace PkgLiveCache {

// 0 ok, -2 a session is already active, -1 invalid arguments.
int Create(const std::string &filename, uint64_t total_size, std::string *out_session_id);
void SetTitle(const std::string &title);

bool SessionActive();
bool CheckId(const std::string &session_id);

void SetSeekFn(PkgLiveSeekFn fn);
void SetPosFn(PkgLivePosFn fn);
// Number of 1 MiB cache slots the current session was given (0 when idle).
int SlotCount();
void SetDoneFn(PkgLiveDoneFn fn);

// Stores one whole segment (or the short final one). Blocks on reader backpressure when
// every slot is pinned. 0 ok, -1 error/aborted/bad range.
int Write(uint64_t offset, const void *data, size_t len);

// Single attempt, never blocks — for a connection worker that must keep servicing seeks.
// 0 stored, -2 busy (no evictable slot right now, caller should retry shortly), -1 fatal.
int TryWrite(uint64_t offset, const void *data, size_t len);

// Parked-reader count across all segments, so a transport can avoid an idle timeout while
// the installer is waiting on a resend.
int WaiterCount();

// 0 ok (every segment stored at least once), -1 not complete yet / aborted.
int Finish();
// Unblocks every waiter with failure. Idempotent.
void Abort();
// Frees the session's RAM. Idempotent; waits out any attached reader first.
void Destroy();

// Blocks until segment 0 (the header) is present, abort, or timeout.
int WaitHeader(int timeout_sec);
size_t GetHeader(uint8_t *out, size_t len);

// Delivers exactly `count` bytes unless at true EOF (short/0 return) or on abort/timeout
// (-1). May block and emit seek requests for segments not yet resident.
long Read(uint64_t off, void *buf, size_t count);

PkgLiveStatus GetStatus();
uint64_t GetTotal();
// Length of the contiguous prefix acknowledged at least once — resume point on reconnect.
uint64_t GetResumeOffset();

// Reader refcount: Destroy() waits for the last Detach() before freeing.
int Attach();
void Detach();

} // namespace PkgLiveCache

} // namespace prospero
