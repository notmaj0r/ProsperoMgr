#include "read_ahead.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <new>

#include <unistd.h>

namespace prospero {

namespace {

constexpr size_t kBlock = 8u * 1024u * 1024u;
constexpr size_t kSlotCount = 3;
// 24 MiB per stream, so two at a time is a fair price for fast downloads.
constexpr int kMaxStreams = 2;

std::atomic<int> g_streams{0};

} // namespace

std::unique_ptr<ReadAhead> ReadAhead::Start(int fd, uint64_t size) {
    if (g_streams.fetch_add(1) >= kMaxStreams) {
        g_streams.fetch_sub(1);
        return nullptr;
    }
    return std::unique_ptr<ReadAhead>(new ReadAhead(fd, size));
}

ReadAhead::ReadAhead(int fd, uint64_t size) : fd_(fd), size_(size), slots_(kSlotCount) {}

ReadAhead::~ReadAhead() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopped_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    g_streams.fetch_sub(1);
}

void ReadAhead::Run() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [this] { return stopped_ || (!slots_[tail_].ready && next_ < size_); });
        if (stopped_) return;
        Slot &slot = slots_[tail_];
        uint64_t offset = next_;
        size_t want = static_cast<size_t>(std::min<uint64_t>(kBlock, size_ - offset));
        lock.unlock();

        size_t got = 0;
        bool failed = false;
        while (got < want) {
            ssize_t rc = pread(fd_, slot.data.data() + got, want - got, static_cast<off_t>(offset + got));
            if (rc > 0) {
                got += static_cast<size_t>(rc);
            } else if (rc < 0 && errno == EINTR) {
                continue;
            } else {
                failed = rc < 0;
                break;
            }
        }

        lock.lock();
        if (failed) {
            error_ = true;
        } else {
            // The file got shorter while it was being read, so the end moves up.
            if (got < want) size_ = offset + got;
            slot.offset = offset;
            slot.length = got;
            slot.ready = got > 0;
            next_ = offset + got;
            if (got > 0) tail_ = (tail_ + 1) % kSlotCount;
        }
        cv_.notify_all();
        if (error_) return;
    }
}

bool ReadAhead::Read(uint64_t position, char *out, size_t max, ssize_t *got) {
    std::unique_lock<std::mutex> lock(mu_);
    if (stopped_) return false;
    if (!started_) {
        try {
            for (Slot &slot : slots_) slot.data.resize(kBlock);
            started_ = true;
            cursor_ = position;
            next_ = position;
            thread_ = std::thread([this] { Run(); });
        } catch (const std::exception &) {
            stopped_ = true;
            return false;
        }
    }
    // Only a straight read-through is served. A jump means the caller is seeking, so it reads directly.
    if (position != cursor_) {
        stopped_ = true;
        cv_.notify_all();
        return false;
    }

    for (;;) {
        if (error_) {
            *got = -1;
            return true;
        }
        if (cursor_ >= size_) {
            *got = 0;
            return true;
        }
        Slot &slot = slots_[head_];
        if (slot.ready) {
            size_t into = static_cast<size_t>(cursor_ - slot.offset);
            size_t take = std::min(max, slot.length - into);
            std::memcpy(out, slot.data.data() + into, take);
            cursor_ += take;
            if (into + take == slot.length) {
                slot.ready = false;
                head_ = (head_ + 1) % kSlotCount;
                cv_.notify_all();
            }
            *got = static_cast<ssize_t>(take);
            return true;
        }
        cv_.wait(lock);
    }
}

} // namespace prospero
