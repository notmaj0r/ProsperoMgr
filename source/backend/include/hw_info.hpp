#pragma once
// Clocks, CPU load and memory use. Every field is -1 (or empty) when the kernel does not report it.

#include <string>
#include <vector>

namespace prospero {

struct HwInfo {
    int cpu_mhz = -1;
    std::vector<int> core_mhz;         // 0 for a core that reports nothing
    int gpu_mhz = -1;
    int memory_mhz = -1;
    int fabric_mhz = -1;
    int load_percent = -1;             // average over all logical CPUs
    std::vector<int> cpu_load_percent; // one entry per CPU core, -1 while unknown
    int ram_total_mb = -1;
    int ram_used_mb = -1;
};

// CPU load is the change since the previous call, so the first call has none.
HwInfo ReadHwInfo();

} // namespace prospero
