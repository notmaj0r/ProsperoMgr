#include "hw_info.hpp"

#include <sys/sysctl.h>
#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

extern "C" {
long sceKernelGetCpuFrequency(void);
int sceKernelGetCpuCoreClock(int *out_mhz);
int sceKernelGetSocClock(uint32_t *out_mhz);
int sceKernelGetCpuUsage(void *out, int32_t *count);
int sceKernelGetThreadName(uint32_t tid, char *out);
int get_page_table_stats(int vm, int type, int *total_mb, int *free_mb);
}

namespace prospero {
namespace {

constexpr int kMaxCores = 16;
constexpr int kLoadCores = 8;
constexpr int kSocClockCount = 32;
constexpr int kClockGpu = 20;
constexpr int kClockMemory = 23;
constexpr int kClockFabric = 24;
constexpr int kMaxThreads = 3072;
constexpr double kMinLoadWindow = 0.5;

struct Timeval {
    int64_t sec;
    int64_t usec;
};

struct ThreadUsage {
    uint32_t pid;
    uint32_t tid;
    Timeval user;
    Timeval system;
};

double Seconds(const ThreadUsage &t) {
    return static_cast<double>(t.user.sec + t.system.sec) + static_cast<double>(t.user.usec + t.system.usec) / 1e6;
}

struct LoadState {
    std::mutex mu;
    std::vector<ThreadUsage> prev;
    std::vector<ThreadUsage> cur;
    double prev_time = 0;
    std::map<int, uint32_t> idle_tid; // logical CPU -> its idle thread
    std::vector<int> last;
};

LoadState &Load() {
    static LoadState state;
    return state;
}

double Now() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

bool Sample(std::vector<ThreadUsage> *out) {
    out->resize(kMaxThreads);
    int32_t count = kMaxThreads;
    if (sceKernelGetCpuUsage(out->data(), &count) != 0 || count <= 0 || count > kMaxThreads) return false;
    out->resize(static_cast<size_t>(count));
    return true;
}

// Each core has an idle thread; the time it did not run is the core's load.
void FindIdleThreads(const std::vector<ThreadUsage> &threads, std::map<int, uint32_t> *idle) {
    for (const ThreadUsage &t : threads) {
        char name[0x40] {};
        if (sceKernelGetThreadName(t.tid, name) != 0) continue;
        int lane = -1;
        if (std::sscanf(name, "SceIdleCpu%d", &lane) != 1) continue;
        if (lane >= 0 && lane < kLoadCores) idle->emplace(lane, t.tid);
    }
}

const ThreadUsage *Find(const std::vector<ThreadUsage> &threads, uint32_t tid) {
    for (const ThreadUsage &t : threads) {
        if (t.tid == tid) return &t;
    }
    return nullptr;
}

std::vector<int> CpuLoads() {
    LoadState &s = Load();
    std::lock_guard<std::mutex> lock(s.mu);
    double now = Now();
    if (!s.prev.empty() && now - s.prev_time < kMinLoadWindow) return s.last;
    if (!Sample(&s.cur)) return s.last;

    if (s.idle_tid.empty()) FindIdleThreads(s.cur, &s.idle_tid);
    if (!s.prev.empty() && !s.idle_tid.empty()) {
        std::vector<int> loads;
        double window = now - s.prev_time;
        for (const auto &lane : s.idle_tid) {
            const ThreadUsage *before = Find(s.prev, lane.second);
            const ThreadUsage *after = Find(s.cur, lane.second);
            if (!before || !after) { loads.push_back(-1); continue; }
            double idle = std::clamp((Seconds(*after) - Seconds(*before)) / window, 0.0, 1.0);
            loads.push_back(static_cast<int>((1.0 - idle) * 100.0 + 0.5));
        }
        s.last = std::move(loads);
    }
    s.prev.swap(s.cur);
    s.prev_time = now;
    return s.last;
}

// Counters are 4 bytes on the console, so a zeroed 8-byte target holds either width.
bool ReadCounter(const char *name, uint64_t *out) {
    uint64_t value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname(name, &value, &len, nullptr, 0) != 0) return false;
    *out = value;
    return true;
}

// Main memory from the VM counters, for when the page table has no entry (no game is running).
bool RamFromCounters(int *total_mb, int *used_mb) {
    uint64_t page_size = 0, pages = 0, free_pages = 0, inactive = 0, cache = 0;
    if (!ReadCounter("hw.pagesize", &page_size) || page_size == 0) return false;
    if (!ReadCounter("vm.stats.vm.v_page_count", &pages) || pages == 0) return false;
    ReadCounter("vm.stats.vm.v_free_count", &free_pages);
    ReadCounter("vm.stats.vm.v_inactive_count", &inactive);
    ReadCounter("vm.stats.vm.v_cache_count", &cache);
    uint64_t reclaimable = std::min(pages, free_pages + inactive + cache);
    *total_mb = static_cast<int>(pages * page_size / (1024 * 1024));
    *used_mb = static_cast<int>((pages - reclaimable) * page_size / (1024 * 1024));
    return true;
}

} // namespace

HwInfo ReadHwInfo() {
    HwInfo info;

    long hz = sceKernelGetCpuFrequency();
    if (hz > 0) info.cpu_mhz = static_cast<int>(hz / 1000000);

    int cores[kMaxCores] {};
    if (sceKernelGetCpuCoreClock(cores) == 0) {
        int count = 0;
        for (int i = 0; i < kMaxCores; i++) {
            if (cores[i] > 0) count = i + 1;
        }
        info.core_mhz.assign(cores, cores + count);
    }

    uint32_t soc[kSocClockCount] {};
    if (sceKernelGetSocClock(soc) == 0) {
        auto clock = [&](int index) { return soc[index] > 0 ? static_cast<int>(soc[index]) : -1; };
        info.gpu_mhz = clock(kClockGpu);
        info.memory_mhz = clock(kClockMemory);
        info.fabric_mhz = clock(kClockFabric);
    }

    info.cpu_load_percent = CpuLoads();
    int sum = 0, known = 0;
    for (int load : info.cpu_load_percent) {
        if (load >= 0) { sum += load; known++; }
    }
    if (known > 0) info.load_percent = sum / known;

    int total = 0, free_mb = 0;
    if (get_page_table_stats(1, 1, &total, &free_mb) == 0 && total > 0) {
        info.ram_total_mb = total;
        info.ram_used_mb = total - std::clamp(free_mb, 0, total);
    }
    if (info.ram_total_mb <= 0) RamFromCounters(&info.ram_total_mb, &info.ram_used_mb);
    return info;
}

} // namespace prospero
