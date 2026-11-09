#pragma once
// Countdown-then-launch sequence read from autoloader.ini, run in a
// background thread. See source/README.md for the .ini format.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "payload_registry.hpp"

namespace prospero {

struct AutoloaderSequenceStep {
    std::string type;  // "load" or "delay"
    std::string value; // filename, or milliseconds as a string
};

struct AutoloaderConfig {
    bool enabled = false;
    int delay = 5;
    int settle_ms = 500;
    int retries = 0;
    bool continue_on_error = true;
    std::vector<AutoloaderSequenceStep> sequence;
};

// Read/write autoloader.ini directly, for the Settings UI editor — separate
// from the Autoloader class, which only ever reads it at run time.
AutoloaderConfig ReadAutoloaderConfig(const Config &cfg);
bool WriteAutoloaderConfig(const Config &cfg, const AutoloaderConfig &config, std::string *err);
bool RemovePayloadFromAutoloaderConfig(const Config &cfg, const std::string &name, std::string *err);
bool RenamePayloadInAutoloaderConfig(const Config &cfg, const std::string &name,
                                     const std::string &new_name, std::string *err);

// Separate from AutoloaderConfig.enabled: this master switch skips only *bundled*
// payloads (ftpsrv, klogsrv, ...) in a `load=` step; custom steps still run. Defaults to on.
bool GetBundledAutostart(const Config &cfg);
bool SetBundledAutostart(const Config &cfg, bool enabled, std::string *err);

struct AutoloaderStatus {
    bool enabled = false;
    bool running = false;      // countdown or sequence currently in progress
    int remaining_seconds = -1; // -1 = not counting down
    int total = 0;
    int done = 0;
    int failed = 0;
    std::string current;       // name of the payload currently launching, or "" / "DONE"
    std::string last_error;

    // Session guard, see autoloader.cpp. Not a failure: true when a StartupGuarded
    // run found every payload already active; skipped_count covers partial runs too.
    bool already_loaded = false;
    int skipped_count = 0;
};

// StartupGuarded skips payloads already running this boot (resending the .elf doesn't
// reload them). ManualForce always runs the full sequence — a deliberate user action.
enum class AutoloaderRunMode { StartupGuarded, ManualForce };

class Autoloader {
  public:
    Autoloader(Config cfg, const PayloadRegistry *registry);
    ~Autoloader();

    // Reads [general] enabled= from autoloader.ini; if true, spawns the
    // background countdown+sequence thread. Safe to call once at startup.
    void Start();
    bool RunNow(std::string *err);

    // Cancels either the countdown or the sequence between launch attempts.
    void Abort();

    AutoloaderStatus GetStatus() const;

  private:
    void StartWorker(AutoloaderRunMode mode);
    void Worker(AutoloaderRunMode mode);
    bool InterruptibleSleep(int milliseconds);
    bool PageOpen() const;

    Config cfg_;
    const PayloadRegistry *registry_;

    std::thread thread_;
    std::atomic<bool> abort_flag_{false};
    std::atomic<bool> is_executing_{false};
    std::atomic<int> remaining_seconds_{-1};
    std::atomic<int> total_{0};
    std::atomic<int> done_{0};
    std::atomic<int> failed_{0};
    std::atomic<bool> already_loaded_{false};
    std::atomic<int> skipped_{0};
    mutable std::atomic<int64_t> last_poll_ms_{0}; // when the web page last asked for the status

    mutable std::mutex current_mu_;
    std::string current_;
    std::string last_error_;
};

} // namespace prospero
