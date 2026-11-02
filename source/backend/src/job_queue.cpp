#include "job_queue.hpp"

#include <pthread.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "log.hpp"

namespace prospero {

namespace {

// Two workers: enough for a download and a catalog refresh to run at the
// same time (the common overlap) without ever creating a third thread.
constexpr int kWorkerCount = 2;
constexpr size_t kThreadStack = 1024 * 1024;

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_queue;
bool g_started = false;

void *BackgroundEntry(void *arg) {
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()> *>(arg));
    InstallCrashStack();
    try {
        (*fn)();
    } catch (const std::exception &e) {
        LogEvent(std::string("Background thread threw: ") + e.what());
    } catch (...) {
        LogEvent("Background thread threw an unknown exception");
    }
    return nullptr;
}

void *WorkerLoop(void *) {
    InstallCrashStack();
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(g_mu);
            g_cv.wait(lock, [] { return !g_queue.empty(); });
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }
        try {
            job();
        } catch (const std::exception &e) {
            LogEvent(std::string("Background job threw: ") + e.what());
        } catch (...) {
            LogEvent("Background job threw an unknown exception");
        }
    }
    return nullptr;
}

} // namespace

void InitializeJobQueue() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_started) return;
    g_started = true;
    // Explicit stack size: the default is small enough that one inlined job
    // frame can run off the end of it and take the process down.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kThreadStack);

    for (int i = 0; i < kWorkerCount; i++) {
        pthread_t tid;
        // Left unjoined on purpose: these live for the process's whole life.
        pthread_create(&tid, &attr, WorkerLoop, nullptr);
    }
    pthread_attr_destroy(&attr);
    LogEvent("Job queue started (" + std::to_string(kWorkerCount) + " workers)");
}

bool StartBackgroundThread(std::function<void()> fn) {
    auto *payload = new (std::nothrow) std::function<void()>(std::move(fn));
    if (!payload) return false;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kThreadStack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    bool started = pthread_create(&tid, &attr, BackgroundEntry, payload) == 0;
    if (!started) {
        LogEvent("Could not start a background thread");
        delete payload;
    }
    pthread_attr_destroy(&attr);
    return started;
}

void RunOnWorker(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_queue.push_back(std::move(job));
    }
    g_cv.notify_one();
}

} // namespace prospero
