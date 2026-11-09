#pragma once
// Rolling temperature samples for the Thermal tab's chart.

#include <vector>

namespace prospero {

struct ThermalSample {
    long long time = 0; // unix seconds
    int cpu_c = 0;
    int soc_c = 0;
    int hottest_c = 0;
};

// Starts the sampler thread once; later calls do nothing.
void StartThermalHistory();
std::vector<ThermalSample> GetThermalHistory();

} // namespace prospero
