#include "log.hpp"

#include <signal.h>
#include <sys/syscall.h>

#include <ctime>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unistd.h>

namespace prospero {

namespace {

constexpr size_t kMaxLines = 300;
constexpr uintmax_t kMaxPersistentBytes = 2u * 1024u * 1024u;

std::mutex g_mu;
std::deque<std::string> g_lines;
std::time_t g_start = 0;
std::string g_log_path;
std::ofstream g_log_out; // Kept open, since reopening per line slowed bulk uploads.
std::string g_instance_id;

std::string Timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

// sendsyslog (syscall 0x259) -> klogsrv reads /dev/klog live, so our events show up in the
// same stream every payload writes to. <118> is the kernel.info syslog priority prefix.
void KlogSend(const std::string &line) {
    std::string trimmed = line.size() > 400 ? line.substr(0, 400) + "…" : line;
    std::string msg = "<118>[ProsperoMgr] " + trimmed + "\n";
    syscall(0x259, 7, msg.c_str(), 0);
}

} // namespace

void LogEvent(const std::string &line) {
    std::lock_guard<std::mutex> lock(g_mu);
    std::string formatted = "[" + Timestamp() + "] " + line;
    g_lines.push_back(formatted);
    if (g_lines.size() > kMaxLines) g_lines.pop_front();

    if (g_log_out.is_open()) {
        g_log_out << formatted << '\n';
        g_log_out.flush();
    }

    KlogSend(line);
}

std::vector<std::string> GetLogs() {
    std::lock_guard<std::mutex> lock(g_mu);
    return std::vector<std::string>(g_lines.begin(), g_lines.end());
}

void ClearLogs() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_lines.clear();
}

void ConfigurePersistentLog(const std::string &path) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_log_path = path;

    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(path, ec);
    if (!ec && size > kMaxPersistentBytes) {
        std::string previous = path + ".previous";
        std::filesystem::remove(previous, ec);
        ec.clear();
        std::filesystem::rename(path, previous, ec);
    }

    if (g_log_out.is_open()) g_log_out.close();
    g_log_out.open(path, std::ios::app);
    if (!g_log_out) {
        g_log_path.clear();
        return;
    }
    g_log_out << "\n--- Prospero Manager session " << (g_instance_id.empty() ? "initializing" : g_instance_id)
              << " pid=" << getpid() << " ---\n";
    // Preserve events emitted before Config was available (privilege and
    // replacement diagnostics) in the durable log as well.
    for (const auto &line : g_lines) g_log_out << line << '\n';
    g_log_out.flush();
}

std::string PersistentLogPath() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_log_path;
}

void InstallCrashStack() {
    static thread_local char stack[SIGSTKSZ];
    stack_t ss{};
    ss.ss_sp = stack;
    ss.ss_size = sizeof(stack);
    sigaltstack(&ss, nullptr);
}

void MarkStartTime() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_start = std::time(nullptr);
    uint64_t mixed = (static_cast<uint64_t>(g_start) << 24) ^
                     (static_cast<uint64_t>(getpid()) << 8) ^
                     static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&g_start));
    std::ostringstream id;
    id << std::hex << std::setfill('0') << std::setw(16) << mixed;
    g_instance_id = id.str();
}

long UptimeSeconds() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_start ? static_cast<long>(std::time(nullptr) - g_start) : 0;
}

std::string ProcessInstanceId() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_instance_id;
}

} // namespace prospero
