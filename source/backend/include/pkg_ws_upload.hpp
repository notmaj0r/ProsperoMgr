#pragma once
// Direct-install WebSocket transport: the browser pushes .pkg bytes here, the PS5
// installer pulls them concurrently through pkg_stream.cpp — nothing touches disk.

#include "config.hpp"

namespace prospero {

// Starts the listener once (idempotent). Needs cfg up front because a finished upload
// hands itself straight to StartPkgInstallJobFromLiveSession from inside this listener.
void StartPkgWsUpload(const Config &cfg);

// Total microseconds the upload thread has spent waiting for the browser to send.
uint64_t PkgWsRecvWaitMicros();

} // namespace prospero
