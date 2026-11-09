#pragma once
// Tails the bundled klogsrv payload (/dev/klog over TCP) into a ring buffer alongside
// log.hpp's own event log. One persistent connection — klogsrv only streams data sent after connect.

#include <cstdint>
#include <string>
#include <vector>

namespace prospero {

// Starts the background tailer thread — connects with retry/backoff, then reads forever.
// Call once at startup; safe on host too (never connects there).
void StartKlogTailer();

std::vector<std::string> GetKlogs();
void ClearKlogs();

// Lines already seen are counted, so a caller can ask whether something showed up since a mark.
uint64_t KlogMark();
bool KlogSeenSince(uint64_t mark, const std::string &needle);

// Whether the tailer currently has a live connection to klogsrv.
bool IsKlogsrvConnected();

} // namespace prospero
