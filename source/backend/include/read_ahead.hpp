#pragma once
// Reads a big file ahead of the network, so a download is not paced by the disk. The reader
// thread fills a few large blocks while the connection drains the earlier ones.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace prospero {

class ReadAhead {
  public:
    // Null when the memory is better spent elsewhere: only a couple of streams read ahead at once.
    static std::unique_ptr<ReadAhead> Start(int fd, uint64_t size);
    ~ReadAhead();

    // True when the data came from the read-ahead: *got is the byte count, 0 at the end, -1 on error.
    // False when the position is not the next one in line, and the caller should read it directly.
    bool Read(uint64_t position, char *out, size_t max, ssize_t *got);

  private:
    ReadAhead(int fd, uint64_t size);
    void Run();

    struct Slot {
        std::vector<char> data;
        uint64_t offset = 0;
        size_t length = 0;
        bool ready = false;
    };

    int fd_;
    uint64_t size_;
    std::vector<Slot> slots_;
    size_t head_ = 0; // next slot the connection reads from
    size_t tail_ = 0; // next slot the reader fills
    uint64_t next_ = 0;   // file offset the reader fetches next
    uint64_t cursor_ = 0; // file offset the connection reads next
    bool started_ = false;
    bool stopped_ = false;
    bool error_ = false;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
};

} // namespace prospero
