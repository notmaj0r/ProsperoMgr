#include "upload_writer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <unistd.h>

namespace prospero {

namespace {

bool WriteAll(int fd, const char *data, size_t size) {
    size_t written = 0;
    while (written < size) {
        ssize_t rc = write(fd, data + written, size - written);
        if (rc > 0) {
            written += static_cast<size_t>(rc);
        } else if (rc < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

UploadWriter::UploadWriter(int fd, size_t chunk) : fd_(fd), chunk_(chunk) { fill_.resize(chunk_); }

UploadWriter::~UploadWriter() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void UploadWriter::Run() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [this] { return has_pending_ || stop_; });
        if (!has_pending_) break;
        has_pending_ = false;
        busy_ = true;
        lock.unlock();
        bool ok = WriteAll(fd_, pending_.data(), pending_size_);
        lock.lock();
        if (!ok) error_ = true;
        busy_ = false;
        cv_.notify_all();
    }
}

// Hands the filled buffer to the writer. Waits first for the previous write, so at most one is queued.
bool UploadWriter::Handoff() {
    if (used_ == 0) return true;
    std::unique_lock<std::mutex> lock(mu_);
    if (!thread_.joinable()) {
        pending_.resize(chunk_);
        thread_ = std::thread([this] { Run(); });
    }
    cv_.wait(lock, [this] { return !has_pending_ && !busy_; });
    if (error_) return false;
    fill_.swap(pending_);
    pending_size_ = used_;
    used_ = 0;
    has_pending_ = true;
    cv_.notify_all();
    return true;
}

bool UploadWriter::Append(const char *data, size_t size) {
    while (size > 0) {
        size_t take = std::min(size, chunk_ - used_);
        std::memcpy(fill_.data() + used_, data, take);
        used_ += take;
        data += take;
        size -= take;
        if (used_ == chunk_ && !Handoff()) return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    return !error_;
}

bool UploadWriter::Flush() {
    // A small upload never started the thread, so write it here.
    if (!thread_.joinable()) {
        bool ok = WriteAll(fd_, fill_.data(), used_);
        used_ = 0;
        return ok;
    }
    if (!Handoff()) return false;
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return !has_pending_ && !busy_; });
    return !error_;
}

} // namespace prospero
