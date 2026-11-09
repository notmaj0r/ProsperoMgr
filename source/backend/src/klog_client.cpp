#include "job_queue.hpp"
#include "klog_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include "config.hpp"

namespace prospero {

namespace {

constexpr size_t kMaxLines = 500;

std::mutex g_mu;
std::deque<std::string> g_lines;
uint64_t g_seq = 0; // lines ever appended
std::atomic<bool> g_connected{false};
std::atomic<bool> g_started{false};

void AppendLine(const std::string &line) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_lines.push_back(line);
    g_seq++;
    if (g_lines.size() > kMaxLines) g_lines.pop_front();
}

void TailerLoop() {
    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(Config::kKlogsrvPort);
        inet_pton(AF_INET, Config::kKlogsrvHost, &addr.sin_addr);

        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            close(fd);
            g_connected = false;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        g_connected = true;

        // klogsrv broadcasts raw /dev/klog bytes with no framing, so split on newlines
        // and carry a partial line across reads.
        std::string pending;
        char buf[4096];
        for (;;) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break; // klogsrv closed, or we lost the connection

            pending.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) AppendLine(line);
                pending.erase(0, nl + 1);
            }
        }

        close(fd);
        g_connected = false;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

} // namespace

void StartKlogTailer() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return; // already running
    StartBackgroundThread(TailerLoop);
}

uint64_t KlogMark() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_seq;
}

bool KlogSeenSince(uint64_t mark, const std::string &needle) {
    std::lock_guard<std::mutex> lock(g_mu);
    size_t fresh = static_cast<size_t>(std::min<uint64_t>(g_seq - mark, g_lines.size()));
    for (size_t i = g_lines.size() - fresh; i < g_lines.size(); i++) {
        if (g_lines[i].find(needle) != std::string::npos) return true;
    }
    return false;
}

std::vector<std::string> GetKlogs() {
    std::lock_guard<std::mutex> lock(g_mu);
    return std::vector<std::string>(g_lines.begin(), g_lines.end());
}

void ClearKlogs() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_lines.clear();
}

bool IsKlogsrvConnected() { return g_connected; }

} // namespace prospero
