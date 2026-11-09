#include "autoloader.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <thread>

#include "ini.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "klog_client.hpp"
#include "notify.hpp"
#include "notify_text.hpp"
#include "process_manager.hpp"

namespace prospero {

namespace {

constexpr int kMinAbortWindowSeconds = 5;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr int kPageOpenMs = 5000; // a page that asked for the status this recently is open

// The shell logs this line to the kernel log whenever the PS button is pressed.
class PsButtonWatch {
  public:
    explicit PsButtonWatch(bool active) {
        if (!active) return;
        mark_ = KlogMark();
        fd_ = open("/dev/klog", O_RDONLY | O_NONBLOCK);
        LogEvent(std::string("PS button watch: /dev/klog ") + (fd_ >= 0 ? "open" : "unavailable (errno " + std::to_string(errno) + ")") +
                 ", klogsrv " + (IsKlogsrvConnected() ? "connected" : "not connected"));
        // Lines logged before the countdown must not count as a press.
        char buffer[4096];
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (fd_ >= 0 && std::chrono::steady_clock::now() < until && read(fd_, buffer, sizeof(buffer)) > 0) {}
    }
    ~PsButtonWatch() {
        if (fd_ >= 0) close(fd_);
    }
    PsButtonWatch(const PsButtonWatch &) = delete;
    PsButtonWatch &operator=(const PsButtonWatch &) = delete;

    bool Pressed() {
        // klogsrv may be reading the kernel log too, and then it gets some of the lines.
        if (KlogSeenSince(mark_, kPressLine)) return true;
        if (fd_ < 0) return false;
        char buffer[2048];
        ssize_t n;
        while ((n = read(fd_, buffer, sizeof(buffer))) > 0) {
            recent_.append(buffer, static_cast<size_t>(n));
            if (recent_.find(kPressLine) != std::string::npos) return true;
            if (recent_.size() > 32) recent_.erase(0, recent_.size() - 32); // a match can span two reads
        }
        return false;
    }

  private:
    static constexpr const char *kPressLine = "onPSButtonPressed";
    int fd_ = -1;
    uint64_t mark_ = 0;
    std::string recent_;
};
std::string BundledAutostartPath(const Config &cfg) { return cfg.root_dir + "/bundled_autostart.txt"; }

// ---- Same-boot session state: survives a killed+redeployed ProsperoMgr.elf but resets
// on a real reboot via boot_id, compared against GetBootId() on every StartupGuarded run.

struct AutoloaderLoadedPayload {
    std::string name;
    uint64_t size = 0;
    int64_t mtime = 0;
    bool resident = false; // still running as its own process after settling, vs. one-shot
};

struct AutoloaderSessionState {
    std::string boot_id;
    std::vector<AutoloaderLoadedPayload> loaded;
};

std::string SessionStatePath(const Config &cfg) { return cfg.root_dir + "/autoloader_state.json"; }

AutoloaderSessionState LoadSessionStateRaw(const Config &cfg) {
    AutoloaderSessionState out;
    std::ifstream in(SessionStatePath(cfg), std::ios::binary);
    if (!in) return out;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.boot_id = JsonExtractString(json, "boot_id");
    for (const auto &item : JsonExtractArrayItems(json, "loaded")) {
        AutoloaderLoadedPayload p;
        p.name = JsonExtractString(item, "name");
        p.size = static_cast<uint64_t>(JsonExtractInt(item, "size", 0));
        p.mtime = static_cast<int64_t>(JsonExtractInt(item, "mtime", 0));
        p.resident = JsonExtractBool(item, "resident", false);
        if (!p.name.empty()) out.loaded.push_back(std::move(p));
    }
    return out;
}

bool PersistSessionState(const Config &cfg, const AutoloaderSessionState &state, std::string *err) {
    std::vector<std::string> items;
    for (const auto &p : state.loaded) {
        items.push_back(JsonWriter()
                             .Str("name", p.name)
                             .Int("size", static_cast<long long>(p.size))
                             .Int("mtime", static_cast<long long>(p.mtime))
                             .Bool("resident", p.resident)
                             .Done());
    }
    std::string path = SessionStatePath(cfg);
    std::string temp = path + ".tmp";
    std::ofstream out(temp, std::ios::trunc | std::ios::binary);
    if (!out) {
        if (err) *err = "could not write autoloader session state";
        return false;
    }
    out << JsonWriter().Str("boot_id", state.boot_id).Raw("loaded", JsonWriter::Array(items)).Done();
    out.flush();
    if (!out) {
        if (err) *err = "could not flush autoloader session state";
        return false;
    }
    out.close();
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        if (err) *err = "could not commit autoloader session state: " + ec.message();
        return false;
    }
    return true;
}

// Current on-disk identity of a resolved payload file (size + mtime) — used to check if a
// session-state record still matches the file on disk, and to write a fresh record after launch.
bool StatPayload(const PayloadRegistry *registry, const std::string &name, uint64_t *out_size, int64_t *out_mtime) {
    std::string path;
    if (!registry->Resolve(name, &path)) return false;
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return false;
    if (out_size) *out_size = static_cast<uint64_t>(st.st_size);
    if (out_mtime) *out_mtime = static_cast<int64_t>(st.st_mtime);
    return true;
}

void UpsertLoadedRecord(AutoloaderSessionState *state, const AutoloaderLoadedPayload &record) {
    for (auto &existing : state->loaded) {
        if (existing.name == record.name) {
            existing = record;
            return;
        }
    }
    state->loaded.push_back(record);
}

} // namespace

bool GetBundledAutostart(const Config &cfg) {
    std::ifstream f(BundledAutostartPath(cfg));
    if (!f.is_open()) return true; // default: on, unchanged behavior
    std::string value;
    f >> value;
    return value != "0";
}

bool SetBundledAutostart(const Config &cfg, bool enabled, std::string *err) {
    std::ofstream f(BundledAutostartPath(cfg), std::ios::trunc);
    if (!f.is_open()) {
        if (err) *err = "could not write bundled_autostart.txt";
        return false;
    }
    f << (enabled ? "1" : "0");
    return true;
}

AutoloaderConfig ReadAutoloaderConfig(const Config &cfg) {
    Ini ini = Ini::Load(cfg.autoloader_ini);

    AutoloaderConfig out;
    out.enabled = ini.GetBool("general", "enabled", false);
    out.delay = std::clamp(ini.GetInt("general", "delay", 5), 0, 300);
    out.settle_ms = std::clamp(ini.GetInt("general", "settle_ms", 500), 0, 30000);
    out.retries = std::clamp(ini.GetInt("general", "retries", 0), 0, 3);
    out.continue_on_error = ini.GetBool("general", "continue_on_error", true);

    for (const auto &e : ini.Entries("sequence")) {
        if (e.key == "load" || e.key == "delay") out.sequence.push_back({e.key, e.value});
    }
    return out;
}

bool WriteAutoloaderConfig(const Config &cfg, const AutoloaderConfig &config, std::string *err) {
    std::string staged = cfg.autoloader_ini + ".tmp";
    std::ofstream f(staged, std::ios::trunc);
    if (!f.is_open()) {
        if (err) *err = "cannot write autoloader.ini";
        return false;
    }

    f << "[general]\n";
    f << "enabled=" << (config.enabled ? 1 : 0) << '\n';
    f << "delay=" << std::clamp(config.delay, 0, 300) << '\n';
    f << "settle_ms=" << std::clamp(config.settle_ms, 0, 30000) << '\n';
    f << "retries=" << std::clamp(config.retries, 0, 3) << '\n';
    f << "continue_on_error=" << (config.continue_on_error ? 1 : 0) << '\n';
    f << "\n[sequence]\n";
    for (const auto &step : config.sequence) {
        if (step.type != "load" && step.type != "delay") continue;
        f << step.type << '=' << step.value << '\n';
    }
    f.flush();
    if (!f) {
        if (err) *err = "could not flush autoloader config";
        return false;
    }
    f.close();
    std::error_code ec;
    std::filesystem::rename(staged, cfg.autoloader_ini, ec);
    if (ec) {
        std::filesystem::remove(staged, ec);
        if (err) *err = "could not publish autoloader config: " + ec.message();
        return false;
    }
    return true;
}

bool RemovePayloadFromAutoloaderConfig(const Config &cfg, const std::string &name, std::string *err) {
    AutoloaderConfig config = ReadAutoloaderConfig(cfg);
    auto old_size = config.sequence.size();
    config.sequence.erase(std::remove_if(config.sequence.begin(), config.sequence.end(), [&](const auto &step) {
        return step.type == "load" && step.value == name;
    }), config.sequence.end());
    if (config.sequence.size() == old_size) return true;
    return WriteAutoloaderConfig(cfg, config, err);
}

bool RenamePayloadInAutoloaderConfig(const Config &cfg, const std::string &name,
                                     const std::string &new_name, std::string *err) {
    AutoloaderConfig config = ReadAutoloaderConfig(cfg);
    bool changed = false;
    for (auto &step : config.sequence) {
        if (step.type == "load" && step.value == name) {
            step.value = new_name;
            changed = true;
        }
    }
    return !changed || WriteAutoloaderConfig(cfg, config, err);
}

Autoloader::Autoloader(Config cfg, const PayloadRegistry *registry)
    : cfg_(std::move(cfg)), registry_(registry) {}

Autoloader::~Autoloader() {
    abort_flag_ = true;
    if (thread_.joinable()) thread_.join();
}

void Autoloader::Start() {
    Ini ini = Ini::Load(cfg_.autoloader_ini);
    if (!ini.GetBool("general", "enabled", false)) return;

    StartWorker(AutoloaderRunMode::StartupGuarded);
}

void Autoloader::Abort() {
    abort_flag_ = true;
}

bool Autoloader::RunNow(std::string *err) {
    if (is_executing_ || remaining_seconds_ >= 0) {
        if (err) *err = "autoloader is already running";
        return false;
    }
    StartWorker(AutoloaderRunMode::ManualForce);
    return true;
}

void Autoloader::StartWorker(AutoloaderRunMode mode) {
    if (thread_.joinable()) thread_.join();
    abort_flag_ = false;
    is_executing_ = false;
    remaining_seconds_ = (mode == AutoloaderRunMode::ManualForce) ? 0 : -1;
    total_ = 0;
    done_ = 0;
    failed_ = 0;
    already_loaded_ = false;
    skipped_ = 0;
    {
        std::lock_guard<std::mutex> lock(current_mu_);
        current_.clear();
        last_error_.clear();
    }
    thread_ = std::thread([this, mode] {
        try {
            Worker(mode);
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(current_mu_);
            last_error_ = std::string("autoloader failed safely: ") + e.what();
            current_ = "ERROR";
            failed_++;
            is_executing_ = false;
            remaining_seconds_ = -1;
        } catch (...) {
            std::lock_guard<std::mutex> lock(current_mu_);
            last_error_ = "autoloader failed safely";
            current_ = "ERROR";
            failed_++;
            is_executing_ = false;
            remaining_seconds_ = -1;
        }
    });
}

AutoloaderStatus Autoloader::GetStatus() const {
    last_poll_ms_ = NowMs();
    Ini ini = Ini::Load(cfg_.autoloader_ini);

    AutoloaderStatus s;
    s.enabled = ini.GetBool("general", "enabled", false);
    s.running = is_executing_ || remaining_seconds_ >= 0;
    s.remaining_seconds = remaining_seconds_;
    s.total = total_;
    s.done = done_;
    s.failed = failed_;
    s.already_loaded = already_loaded_;
    s.skipped_count = skipped_;

    std::lock_guard<std::mutex> lock(current_mu_);
    s.current = current_;
    s.last_error = last_error_;
    return s;
}

bool Autoloader::PageOpen() const {
    return NowMs() - last_poll_ms_ < kPageOpenMs;
}

bool Autoloader::InterruptibleSleep(int milliseconds) {
    int remaining = std::max(0, milliseconds);
    while (remaining > 0 && !abort_flag_) {
        int slice = std::min(remaining, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
    return !abort_flag_;
}

void Autoloader::Worker(AutoloaderRunMode mode) {
    bool skip_countdown = (mode == AutoloaderRunMode::ManualForce);
    AutoloaderConfig config = ReadAutoloaderConfig(cfg_);
    // Leaves time to press the PS button before anything loads.
    int delay_seconds = skip_countdown ? 0 : std::clamp(std::max(config.delay, kMinAbortWindowSeconds), 0, 300);
    int retries = std::clamp(config.retries, 0, 3);
    auto sequence = config.sequence;

    bool bundled_autostart = GetBundledAutostart(cfg_);
    std::vector<std::string> bundled_names;
    if (!bundled_autostart) {
        for (const auto &p : registry_->List()) {
            if (p.bundled) bundled_names.push_back(p.name);
        }
    }
    auto is_bundled_excluded = [&](const std::string &name) {
        return !bundled_autostart && std::find(bundled_names.begin(), bundled_names.end(), name) != bundled_names.end();
    };

    // A redeploy replaces this process but not the payloads it already launched.
    // Skip whatever is still resident or recorded as launched this boot.
    std::string boot_id = GetBootId();
    AutoloaderSessionState session = LoadSessionStateRaw(cfg_);
    if (boot_id.empty() || session.boot_id != boot_id) {
        if (!session.boot_id.empty() && !boot_id.empty()) {
            LogEvent("Autoloader session guard: new PS5 boot detected — previous session state cleared");
        }
        session.loaded.clear();
        session.boot_id = boot_id;
    }

    std::vector<bool> already_loaded(sequence.size(), false);
    int required_total = 0;
    int already_loaded_count = 0;
    if (mode == AutoloaderRunMode::StartupGuarded && !boot_id.empty()) {
        std::vector<ProcessInfo> processes = ListProcesses();
        for (size_t i = 0; i < sequence.size(); i++) {
            const auto &e = sequence[i];
            if (e.type != "load" || is_bundled_excluded(e.value)) continue;
            required_total++;

            uint64_t cur_size = 0;
            int64_t cur_mtime = 0;
            bool resolved = StatPayload(registry_, e.value, &cur_size, &cur_mtime);

            bool session_match = false;
            for (const auto &lp : session.loaded) {
                if (lp.name != e.value) continue;
                // Size or mtime drift means the binary was replaced, so the
                // recorded launch no longer counts.
                if (resolved && (lp.size != cur_size || lp.mtime != cur_mtime)) continue;
                session_match = true;
                break;
            }
            if (session_match || IsPayloadResident(e.value, processes)) {
                already_loaded[i] = true;
                already_loaded_count++;
            }
        }
    }

    bool fully_loaded = required_total > 0 && already_loaded_count == required_total;
    if (fully_loaded) {
        std::string msg = "Prospero Manager: Payloads already loaded!";
        NotifyDebug("Prospero Manager: " + NotifyText(Msg::kPayloadsAlreadyLoaded));
        LogEvent(msg + " (" + std::to_string(already_loaded_count) + " payload(s) skipped, autoloader sequence not re-run)");
        std::lock_guard<std::mutex> lock(current_mu_);
        current_ = "DONE";
        already_loaded_ = true;
        skipped_ = already_loaded_count;
        total_ = required_total;
        done_ = required_total;
        is_executing_ = false;
        remaining_seconds_ = -1;
        return;
    }

    remaining_seconds_ = skip_countdown ? 0 : delay_seconds;
    bool counting_down = !skip_countdown && delay_seconds > 0;
    PsButtonWatch ps_button(counting_down);
    if (counting_down) {
        NotifyDebug("Prospero Manager: " + NotifyText(Msg::kAutoloadCountdown, {{"seconds", std::to_string(delay_seconds)}}));
    }
    for (int elapsed = 0; !skip_countdown && elapsed < delay_seconds; elapsed++) {
        if (abort_flag_) {
            remaining_seconds_ = -1;
            return;
        }
        bool pressed = false;
        for (int slice = 0; slice < 10 && !pressed; slice++) {
            if (!InterruptibleSleep(100)) {
                remaining_seconds_ = -1;
                return;
            }
            pressed = ps_button.Pressed();
        }
        if (pressed) {
            abort_flag_ = true;
            remaining_seconds_ = -1;
            LogEvent("Autoloader aborted with the PS button");
            NotifyDebug("Prospero Manager: " + NotifyText(Msg::kAutoloadAborted));
            return;
        }
        remaining_seconds_ = delay_seconds - elapsed - 1;
    }

    if (abort_flag_) {
        remaining_seconds_ = -1;
        return;
    }

    is_executing_ = true;
    remaining_seconds_ = 0;

    total_ = 0;
    for (const auto &e : sequence) {
        if (e.type == "load") total_++;
    }

    int kstuff_count = 0;
    bool has_etahen = false;
    for (const auto &e : sequence) {
        if (e.type != "load") continue;
        std::string lowered = e.value;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered.find("kstuff") != std::string::npos) kstuff_count++;
        if (lowered.find("etahen") != std::string::npos) has_etahen = true;
    }
    if (kstuff_count > 1 || (kstuff_count > 0 && has_etahen)) {
        std::string conflict = kstuff_count > 1
                                   ? "autoloader stopped: multiple kstuff payloads conflict"
                                   : "autoloader stopped: etaHEN and standalone kstuff conflict";
        NotifyDebug("Prospero Manager: " + NotifyText(kstuff_count > 1 ? Msg::kConflictKstuff : Msg::kConflictEtahen));
        LogEvent(conflict);
        std::lock_guard<std::mutex> lock(current_mu_);
        current_ = "ERROR";
        last_error_ = conflict;
        failed_ = 1;
        is_executing_ = false;
        remaining_seconds_ = -1;
        return;
    }

    int launching_count = required_total > 0 ? required_total - already_loaded_count : total_.load();
    if (already_loaded_count > 0) {
        std::string msg = "Prospero Manager: Auto-Loading (" + std::to_string(launching_count) +
                           ") Payloads — " + std::to_string(already_loaded_count) + " already loaded";
        if (!PageOpen()) { // the page already shows the progress
            NotifyDebug("Prospero Manager: " + NotifyText(Msg::kAutoLoadingSkipping, {{"count", std::to_string(launching_count)}, {"loaded", std::to_string(already_loaded_count)}}));
        }
        LogEvent(msg);
    } else if (total_ > 0) {
        std::string msg = "Prospero Manager: Auto-Loading (" + std::to_string(total_) + ") Payloads";
        if (!PageOpen()) NotifyDebug("Prospero Manager: " + NotifyText(Msg::kAutoLoading, {{"count", std::to_string(total_.load())}}));
        LogEvent(msg);
    }

    // Stop after the last step that still needs launching, so trailing delays
    // don't run for launches that were skipped.
    int last_needed_index = -1;
    for (size_t i = 0; i < sequence.size(); i++) {
        if (sequence[i].type == "load" && !already_loaded[i] && !is_bundled_excluded(sequence[i].value)) {
            last_needed_index = static_cast<int>(i);
        }
    }

    for (size_t i = 0; i < sequence.size(); i++) {
        if (abort_flag_) break;
        if (last_needed_index >= 0 && static_cast<int>(i) > last_needed_index) break;
        const auto &e = sequence[i];
        if (e.type == "delay") {
            int ms = std::clamp(std::atoi(e.value.c_str()), 0, 600000);
            if (ms > 0 && !InterruptibleSleep(ms)) break;
            continue;
        }
        if (e.type != "load") continue;

        // Bundled autostart is off: skip only bundled payloads, not the
        // user's own imported ones.
        if (is_bundled_excluded(e.value)) {
            std::string toast = "Prospero Manager: " + e.value + " skipped (bundled autostart is off)";
            LogEvent(toast);
            done_++;
            if (!InterruptibleSleep(std::clamp(config.settle_ms, 0, 30000))) break;
            continue;
        }

        if (already_loaded[i]) {
            LogEvent("Prospero Manager: " + e.value + " already loaded — skipped");
            done_++;
            continue; // nothing launched, no settle wait needed
        }

        {
            std::lock_guard<std::mutex> lock(current_mu_);
            current_ = e.value;
        }

        std::string err;
        bool ok = false;
        for (int attempt = 0; attempt <= retries && !abort_flag_; attempt++) {
            err.clear();
            ok = registry_->Launch(e.value, &err);
            if (ok || attempt == retries) break;
            InterruptibleSleep(750);
        }
        std::string toast = ok ? "Prospero Manager: " + e.value + " Loaded!" : "Prospero Manager: " + e.value + " failed to load";
        LogEvent(ok ? toast : toast + " (" + err + ")"); // per-payload toast removed; the "Auto-Loading"/"Loaded all" pair covers it

        done_++;
        if (!ok) {
            failed_++;
            std::lock_guard<std::mutex> lock(current_mu_);
            last_error_ = e.value + ": " + err;
        }

        // Record it against this boot so the next redeploy doesn't relaunch
        // what this run just started.
        if (ok && !boot_id.empty()) {
            AutoloaderLoadedPayload record;
            record.name = e.value;
            StatPayload(registry_, e.value, &record.size, &record.mtime);
            record.resident = false; // refined below, after the settle window
            UpsertLoadedRecord(&session, record);
            std::string persist_err;
            if (!PersistSessionState(cfg_, session, &persist_err)) {
                LogEvent("Autoloader session guard: could not persist state: " + persist_err);
            }
        }

        if (!ok && !config.continue_on_error) break;
        if (!InterruptibleSleep(std::clamp(config.settle_ms, 0, 30000))) break;

        if (ok && !boot_id.empty()) {
            bool resident_now = IsPayloadResident(e.value, ListProcesses());
            for (auto &lp : session.loaded) {
                if (lp.name == e.value) {
                    lp.resident = resident_now;
                    break;
                }
            }
            std::string persist_err;
            if (!PersistSessionState(cfg_, session, &persist_err)) {
                LogEvent("Autoloader session guard: could not persist state: " + persist_err);
            }
        }
    }

    // Pairs with the "Auto-Loading (N) Payloads" toast above. Silent on partial failure —
    // LogEvent already has the per-payload detail, and "Loaded all" would be false there.
    if (!abort_flag_ && launching_count > 0 && failed_ == 0) {
        if (!PageOpen()) NotifyDebug("Prospero Manager: " + NotifyText(Msg::kLoadedAll, {{"count", std::to_string(launching_count)}}));
    }

    {
        std::lock_guard<std::mutex> lock(current_mu_);
        current_ = abort_flag_ ? "ABORTED" : "DONE";
        skipped_ = already_loaded_count;
        is_executing_ = false;
        remaining_seconds_ = -1;
    }
}

} // namespace prospero
