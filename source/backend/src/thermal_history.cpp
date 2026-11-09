#include "thermal_history.hpp"

#include <chrono>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

#include "job_queue.hpp"
#include "thermal.hpp"

namespace prospero {
namespace {

constexpr auto kSampleEvery = std::chrono::seconds(15);
constexpr size_t kMaxSamples = 240; // one hour

std::mutex g_mu;
std::deque<ThermalSample> g_samples;
bool g_started = false;

void SampleLoop() {
    for (;;) {
        ThermalInfo info = GetThermalInfo();
        if (info.ok) {
            ThermalSample sample;
            sample.time = static_cast<long long>(std::time(nullptr));
            sample.cpu_c = info.cpu_c;
            sample.soc_c = info.soc_c;
            sample.hottest_c = info.hottest_c;
            std::lock_guard<std::mutex> lock(g_mu);
            g_samples.push_back(sample);
            while (g_samples.size() > kMaxSamples) g_samples.pop_front();
        }
        std::this_thread::sleep_for(kSampleEvery);
    }
}

} // namespace

void StartThermalHistory() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_started) return;
        g_started = true;
    }
    StartBackgroundThread(SampleLoop);
}

std::vector<ThermalSample> GetThermalHistory() {
    std::lock_guard<std::mutex> lock(g_mu);
    return {g_samples.begin(), g_samples.end()};
}

} // namespace prospero
