#pragma once
// Writes an upload to disk on its own thread, so receiving the next chunk from the network
// overlaps with the previous one hitting the disk.

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace prospero {

class UploadWriter {
  public:
    UploadWriter(int fd, size_t chunk);
    ~UploadWriter();
    UploadWriter(const UploadWriter &) = delete;
    UploadWriter &operator=(const UploadWriter &) = delete;

    // Copies into the buffer being filled. False once a write has failed.
    bool Append(const char *data, size_t size);
    // Returns when everything appended is in the file. False if any write failed.
    bool Flush();

  private:
    bool Handoff();
    void Run();

    int fd_;
    size_t chunk_;
    std::vector<char> fill_;    // owned by the caller's thread
    std::vector<char> pending_; // owned by the writer thread while busy_
    size_t used_ = 0;
    size_t pending_size_ = 0;
    bool has_pending_ = false;
    bool busy_ = false;
    bool stop_ = false;
    bool error_ = false;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
};

} // namespace prospero
