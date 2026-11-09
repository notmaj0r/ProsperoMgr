// ProsperoMgr entry point — wires everything up, then blocks forever
// (HttpServer runs its own accept-loop thread, see http_server.cpp).

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>

#include "api_routes.hpp"
#include "autoloader.hpp"
#include "bundled_payloads.hpp"
#include "config.hpp"
#include "file_jobs.hpp"
#include "http_server.hpp"
#include "https_client.hpp"
#include "jailbreak.hpp"
#include "job_queue.hpp"
#include "klog_client.hpp"
#include "log.hpp"
#include "notify.hpp"
#include "notify_text.hpp"
#include "payload_catalog.hpp"
#include "payload_registry.hpp"
#include "pkg_installer.hpp"
#include "process_manager.hpp"
#include "ps5_http_bridge.h"
#include "repository.hpp"
#include "savedata.hpp"
#include "thermal.hpp"
#include "tile_pkg.hpp"

namespace {

volatile sig_atomic_t g_resume_requested = 0;

void HandleResume(int) { g_resume_requested = 1; }

void HandleFatalSignal(int value) {
    // Async-signal-safe breadcrumb, written on the thread's signal stack so
    // even a blown stack still gets reported.
    char line[] = "[fatal] signal 00 http-stage 00\n";
    int signal_value = value < 0 ? -value : value;
    line[15] = static_cast<char>('0' + ((signal_value / 10) % 10));
    line[16] = static_cast<char>('0' + (signal_value % 10));
    int http_stage = prosperomgr_ps5_http_stage();
    line[29] = static_cast<char>('0' + ((http_stage / 10) % 10));
    line[30] = static_cast<char>('0' + (http_stage % 10));
    int fd = open("/data/ProsperoMgr/prosperomgr.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)write(fd, line, sizeof(line) - 1);
        (void)close(fd);
    }
    signal(value, SIG_DFL);
    (void)kill(getpid(), value);
    _exit(128 + value);
}

void InstallFatalSignalHandlers() {
    prospero::InstallCrashStack();

    struct sigaction sa {};
    sa.sa_handler = HandleFatalSignal;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    for (int value : {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGQUIT, SIGSEGV, SIGTERM}) {
        sigaction(value, &sa, nullptr);
    }
}

extern "C" int sceNetCtlInit(void);

// Redeploying should replace the running instance, not stack a second one — finds the previous
// copy by name and SIGKILLs it. Must run after EscalatePrivileges(), or kill() fails silently with EPERM.
void KillPreviousInstance() {
    constexpr const char *kProcessName = "ProsperoMgr.elf";
    pid_t self = getpid();
    std::set<int32_t> killed_pids;
    bool still_running = false;

    for (int spin = 0; spin < 60; spin++) {
        bool found = false;
        for (const auto &p : prospero::ListProcesses()) {
            if (p.pid == self || p.name != kProcessName) continue;
            found = true;
            std::string err;
            if (prospero::KillProcess(p.pid, &err)) {
                killed_pids.insert(p.pid);
            } else if (spin == 0) {
                prospero::LogEvent("Could not signal previous instance (pid " + std::to_string(p.pid) +
                                    "): " + err);
            }
        }
        still_running = found;
        if (!found) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    if (!killed_pids.empty()) {
        prospero::LogEvent("Replaced " + std::to_string(killed_pids.size()) + " previous instance(s) (new pid " +
                            std::to_string(self) + ")");
    }
    if (still_running) {
        prospero::LogEvent("A previous ProsperoMgr.elf instance is still running after repeated kill attempts");
    }

    syscall(SYS_thr_set_name, -1, kProcessName);
}

// Show loopback when the console has no LAN address.
std::string LocalIp() {
    std::string ip = "127.0.0.1";

    ifaddrs *list;
    if (getifaddrs(&list) != 0) return ip;

    for (ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (std::strncmp(ifa->ifa_name, "lo", 2) == 0) continue;

        char buf[INET_ADDRSTRLEN];
        auto *addr = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
            ip = buf;
            break;
        }
    }

    freeifaddrs(list);
    return ip;
}

} // namespace

int main() {
    using namespace prospero;

    // Writing to a half-closed socket raises SIGPIPE, which kills the process
    // outright. Ignoring it leaves send() returning EPIPE, which callers handle.
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    MarkStartTime();

    // The loader hands every payload kernel R/W before main(); this spends it
    // on our own credentials. See jailbreak.hpp.
    std::string jb_err;
    if (EscalatePrivileges(&jb_err)) {
        LogEvent("Privilege escalation OK — running as root/full caps");
    } else if (!jb_err.empty()) {
        LogEvent("Privilege escalation skipped: " + jb_err);
    }

    // Must follow escalation: the old instance runs as uid 0, so kill() fails
    // silently under the credentials elfldr starts us with.
    KillPreviousInstance();

    // Start networking only once the credentials are final — Sony's services
    // latch onto the process context they were initialized under.
    int netctl_result = sceNetCtlInit();
    if (netctl_result == 0) {
        LogEvent("PS5 network controller initialized");
    } else {
        LogEvent("sceNetCtlInit returned " + std::to_string(netctl_result));
    }

    // sceHttp contexts are built per request, so there is no global client
    // to bring up here.
    Config cfg = Config::Load();
    ConfigurePersistentLog(cfg.log_file);
    InstallFatalSignalHandlers();
    InitializeJobQueue();
    InitializeFileJobs(cfg);
    InitializePayloadCatalog(cfg);
    InitializeGithubToken(cfg);
    CleanupStalePkgFiles(cfg);
    std::printf("[Prospero Manager] root: %s\n", cfg.root_dir.c_str());

    InstallBundledPayloads(cfg.bundled_dir); // keeps ftpsrv/klogsrv in sync with this build
    ResetPkgInstallHelper(); // next install starts this build's private AppInst service
    std::remove((cfg.bundled_dir + "/prospero-netd.elf").c_str()); // Leftover from older builds.
    ResetNetworkHelper(); // Stops that helper if it is still running.
    InstallTilePkgIfNeeded(cfg); // one-time: home-screen tile deeplinking to :7070

    PayloadRegistry registry(cfg);
    Autoloader autoloader(cfg, &registry);
    autoloader.Start();
    StartKlogTailer(); // no-op-ish until klogsrv is actually launched — retries quietly
    StartSaveAutoBackupWatcher(cfg); // no-op until enabled in Settings → Saves
    RestorePinnedFanThreshold(cfg); // no-op if nothing was pinned last session

    HttpServer server(Config::kHttpPort);
    RegisterApiRoutes(&server, cfg, &registry, &autoloader);

    // Install after PS5 services and worker initialization so nothing can
    // replace the standby-resume handler during startup.
    signal(SIGCONT, HandleResume);

    if (!server.Start()) {
        std::fprintf(stderr, "[Prospero Manager] failed to start HTTP server on port %d\n",
                     Config::kHttpPort);
        return 1;
    }

    std::printf("[Prospero Manager] listening on :%d\n", Config::kHttpPort);
    std::string address = "http://" + LocalIp() + ":" + std::to_string(Config::kHttpPort);
    // Two separate toasts instead of one combined multi-line one — each is
    // its own piece of news (which build is running, where to reach it).
    Notify(NotifyText(Msg::kStartup, {{"version", kVersion}, {"author", kAuthor}}));
    NotifyDebug("Prospero Manager: " + NotifyText(Msg::kListening, {{"address", address}}));
    LogEvent(std::string("Prospero Manager v") + kVersion + " (built " + __DATE__ + " " + __TIME__ + ") listening on " + address);

    // SIGCONT asks the HTTP listener to rebuild after standby.
    auto next_pkg_cleanup = std::chrono::steady_clock::now() + std::chrono::hours(1);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (g_resume_requested) {
            g_resume_requested = 0;
            server.RequestRestart();
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= next_pkg_cleanup) {
            CleanupStalePkgFiles(cfg);
            next_pkg_cleanup = now + std::chrono::hours(1);
        }
    }
}
