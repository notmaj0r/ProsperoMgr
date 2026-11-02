#pragma once
// A tiny persistent worker pool for one-shot background jobs (payload downloads,
// catalog checks) with an explicit stack size, unlike the per-job threads this replaced.

#include <functional>

namespace prospero {

// Starts the worker threads. Call once, before RunOnWorker(). A second call
// is a no-op.
void InitializeJobQueue();

// Queues `job` on one of the worker threads, in submission order per worker. Safe to call
// before InitializeJobQueue() runs elsewhere — jobs just wait in the queue.
void RunOnWorker(std::function<void()> job);

// Starts a detached thread with an explicit stack. Never use std::thread here:
// the SDK's default stack is small enough that one frame can run off the end.
// Returns false (and logs) when the thread could not be created.
bool StartBackgroundThread(std::function<void()> fn);

} // namespace prospero
