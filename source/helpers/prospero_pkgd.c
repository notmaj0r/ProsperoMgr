// ProsperoMgr private package-queue service.

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <ps5/kernel.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "pkg_install_policy.h"

#define PROSPERO_PKGD_PORT 9042
#define PROSPERO_PKGD_LOG "/data/ProsperoMgr/pkgd.log"
#define PROSPERO_PKGD_KERNEL_WAIT_STEPS 50
#define PROSPERO_PKGD_KERNEL_WAIT_NS 500000000L
#define PROSPERO_PKGD_INIT_TIMEOUT_SECONDS 10

typedef char pkg_language_t[8];
typedef char pkg_scenario_t[3];
typedef char pkg_content_id_t[0x30];

typedef struct {
    pkg_content_id_t content_id;
    int content_type;
    int content_platform;
} pkg_info_t;

typedef struct {
    const char *uri;
    const char *ex_uri;
    const char *playgo_scenario_id;
    const char *content_id;
    const char *content_name;
    const char *icon_url;
} pkg_meta_t;

typedef struct {
    pkg_language_t languages[30];
    pkg_scenario_t playgo_scenario_ids[64];
    pkg_content_id_t content_ids[64];
    long reserved[810];
} pkg_playgo_t;

_Static_assert(sizeof(pkg_info_t) == 0x38, "package info ABI mismatch");
_Static_assert(sizeof(pkg_meta_t) == 0x30, "package metadata ABI mismatch");
_Static_assert(sizeof(pkg_playgo_t) == 0x2700, "package PlayGo ABI mismatch");

// Layout reverse-engineered by the community for libSceAppInstUtil; used here only to
// read back install progress, never written to.
typedef struct {
    int32_t error_code;
    int32_t version;
    char description[512];
    char type[9];
} pkg_install_error_t;

typedef struct {
    char status[16]; // "downloading", "installing", "playable", "completed", "error", "none", ...
    char src_type[8];
    uint32_t remain_time;
    uint64_t downloaded_size;
    uint64_t initial_chunk_size;
    uint64_t total_size;
    uint32_t promote_progress;
    pkg_install_error_t error_info;
    int32_t local_copy_percent;
    _Bool is_copy_only;
} pkg_install_status_t;

extern int sceNetInit(void);
extern int sceNetCtlInit(void);
extern int sceUserServiceInitialize(int *priority);
extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilInstallByPackage(pkg_meta_t *, pkg_info_t *, pkg_playgo_t *);
extern int sceAppInstUtilGetInstallStatus(const char *content_id, pkg_install_status_t *status);
extern int sceAppInstUtilAppUnInstall(const char *title_id);

typedef enum {
    HELPER_STARTING = 0,
    HELPER_INITIALIZING,
    HELPER_READY,
    HELPER_PRIVILEGE_ERROR,
    HELPER_INITIALIZE_ERROR,
    HELPER_INITIALIZE_TIMEOUT,
} helper_state_t;

typedef struct {
    helper_state_t state;
    int rc;
    int firmware_major;
    int firmware_minor;
    uint64_t authid;
} helper_snapshot_t;

static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static helper_snapshot_t g_state = {HELPER_STARTING, 0, 0, 0, PROSPERO_PKG_SYSTEM_AUTHID};

static void log_event(const char *format, ...) {
    pthread_mutex_lock(&g_log_lock);
    FILE *file = fopen(PROSPERO_PKGD_LOG, "a");
    if (file) {
        time_t now = time(NULL);
        struct tm local_time;
        char timestamp[16] = "--:--:--";
        if (localtime_r(&now, &local_time)) {
            (void)strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &local_time);
        }
        fprintf(file, "[%s] ", timestamp);
        va_list args;
        va_start(args, format);
        vfprintf(file, format, args);
        va_end(args);
        fputc('\n', file);
        fflush(file);
        fclose(file);
    }
    pthread_mutex_unlock(&g_log_lock);
}

static void set_state(helper_state_t state, int rc, prospero_pkg_firmware_t firmware, uint64_t authid) {
    pthread_mutex_lock(&g_state_lock);
    g_state.state = state;
    g_state.rc = rc;
    g_state.firmware_major = firmware.major;
    g_state.firmware_minor = firmware.minor;
    g_state.authid = authid;
    pthread_mutex_unlock(&g_state_lock);
}

static helper_snapshot_t get_state(void) {
    helper_snapshot_t snapshot;
    pthread_mutex_lock(&g_state_lock);
    snapshot = g_state;
    pthread_mutex_unlock(&g_state_lock);
    return snapshot;
}

static int finish_initialization(int rc, prospero_pkg_firmware_t firmware, uint64_t authid) {
    int accepted = 0;
    pthread_mutex_lock(&g_state_lock);
    if (g_state.state == HELPER_INITIALIZING) {
        g_state.state = rc == 0 ? HELPER_READY : HELPER_INITIALIZE_ERROR;
        g_state.rc = rc;
        g_state.firmware_major = firmware.major;
        g_state.firmware_minor = firmware.minor;
        g_state.authid = authid;
        accepted = 1;
    }
    pthread_mutex_unlock(&g_state_lock);
    return accepted;
}

static const char *authid_name(uint64_t authid) {
    return authid == PROSPERO_PKG_SYSTEM_AUTHID ? "SYSTEM" : "ShellCore";
}

static prospero_pkg_firmware_t detect_firmware(void) {
    return prospero_pkg_decode_firmware(kernel_get_fw_version());
}

static void format_firmware(int major, int minor, char *output, size_t capacity) {
    if (major > 0) snprintf(output, capacity, "%d.%02d", major, minor);
    else snprintf(output, capacity, "unknown");
}

static const char *filename_from_path(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static int elevate_self(uint64_t authid) {
    pid_t pid = getpid();
    if (!kernel_get_proc(pid)) return -1;

    int failed = 0;
    failed |= kernel_set_ucred_uid(pid, 0) != 0;
    failed |= kernel_set_ucred_ruid(pid, 0) != 0;
    failed |= kernel_set_ucred_svuid(pid, 0) != 0;
    failed |= kernel_set_ucred_rgid(pid, 0) != 0;
    failed |= kernel_set_ucred_svgid(pid, 0) != 0;

    intptr_t root = kernel_get_root_vnode();
    if (!root) return -1;
    failed |= kernel_set_proc_rootdir(pid, root) != 0;
    failed |= kernel_set_proc_jaildir(pid, root) != 0;
    failed |= kernel_set_ucred_authid(pid, authid) != 0;

    uint8_t caps[16];
    memset(caps, 0xff, sizeof(caps));
    failed |= kernel_set_ucred_caps(pid, caps) != 0;

    uint8_t attrs[32];
    memset(attrs, 0, sizeof(attrs));
    attrs[0] = 0x80;
    failed |= kernel_set_ucred_attrs(pid, attrs) != 0;
    return failed ? -1 : 0;
}

static void *initialize_watchdog(void *unused) {
    (void)unused;
    sleep(PROSPERO_PKGD_INIT_TIMEOUT_SECONDS);
    pthread_mutex_lock(&g_state_lock);
    int timed_out = g_state.state == HELPER_INITIALIZING;
    if (timed_out) g_state.state = HELPER_INITIALIZE_TIMEOUT;
    pthread_mutex_unlock(&g_state_lock);
    if (timed_out) log_event("AppInstUtil initialization timed out after %d seconds",
                             PROSPERO_PKGD_INIT_TIMEOUT_SECONDS);
    return NULL;
}

static void *initialize_service(void *unused) {
    (void)unused;
    prospero_pkg_firmware_t firmware = detect_firmware();
    uint64_t authid = PROSPERO_PKG_SYSTEM_AUTHID;
    set_state(HELPER_STARTING, 0, firmware, authid);
    char firmware_text[24];
    format_firmware(firmware.major, firmware.minor, firmware_text, sizeof(firmware_text));
    log_event("PS5 firmware %s detected — %s authorization selected", firmware_text, authid_name(authid));

    struct timespec interval = {0, PROSPERO_PKGD_KERNEL_WAIT_NS};
    int kernel_ready = 0;
    for (int attempt = 0; attempt < PROSPERO_PKGD_KERNEL_WAIT_STEPS; attempt++) {
        if (kernel_get_proc(getpid()) && kernel_get_root_vnode()) {
            kernel_ready = 1;
            break;
        }
        nanosleep(&interval, NULL);
    }
    if (!kernel_ready) {
        set_state(HELPER_PRIVILEGE_ERROR, -1, firmware, authid);
        log_event("Privilege escalation failed — kernel primitives did not become ready");
        return NULL;
    }

    int privilege_rc = elevate_self(authid);
    if (privilege_rc != 0) {
        set_state(HELPER_PRIVILEGE_ERROR, privilege_rc, firmware, authid);
        log_event("Privilege escalation failed (0x%08x)", (unsigned)privilege_rc);
        return NULL;
    }
    log_event("Privilege escalation OK — running with %s authorization", authid_name(authid));

    set_state(HELPER_INITIALIZING, 0, firmware, authid);
    log_event("Initializing AppInstUtil");
    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, initialize_watchdog, NULL) == 0) pthread_detach(watchdog);
    int init_rc = sceAppInstUtilInitialize();
    if (!finish_initialization(init_rc, firmware, authid)) {
        log_event("AppInstUtil returned after its timeout (0x%08x) — result ignored", (unsigned)init_rc);
        return NULL;
    }
    if (init_rc != 0) {
        log_event("AppInstUtil initialization failed (0x%08x)", (unsigned)init_rc);
        return NULL;
    }

    set_state(HELPER_READY, 0, firmware, authid);
    log_event("AppInstUtil initialized — ready to install packages");
    return NULL;
}

static int send_all(int client, const char *text) {
    size_t length = strlen(text);
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(client, text + sent, length - sent, 0);
        if (count <= 0) return -1;
        sent += (size_t)count;
    }
    return 0;
}

static void reply_error(int client, const char *stage, int rc) {
    char response[128];
    snprintf(response, sizeof(response), "error:%s:0x%08x", stage, (unsigned)rc);
    (void)send_all(client, response);
}

static void reply_status(int client) {
    helper_snapshot_t snapshot = get_state();
    char response[160];
    char firmware[24];
    format_firmware(snapshot.firmware_major, snapshot.firmware_minor, firmware, sizeof(firmware));
    const char *auth = authid_name(snapshot.authid);
    switch (snapshot.state) {
        case HELPER_STARTING:
            snprintf(response, sizeof(response), "starting:kernel:fw=%s:auth=%s", firmware, auth);
            break;
        case HELPER_INITIALIZING:
            snprintf(response, sizeof(response), "starting:initialize:fw=%s:auth=%s", firmware, auth);
            break;
        case HELPER_READY:
            snprintf(response, sizeof(response), "ready:fw=%s:auth=%s", firmware, auth);
            break;
        case HELPER_PRIVILEGE_ERROR:
            snprintf(response, sizeof(response), "error:privilege:0x%08x", (unsigned)snapshot.rc);
            break;
        case HELPER_INITIALIZE_ERROR:
            snprintf(response, sizeof(response), "error:initialize:0x%08x", (unsigned)snapshot.rc);
            break;
        case HELPER_INITIALIZE_TIMEOUT:
            snprintf(response, sizeof(response), "error:initialize:timeout");
            break;
    }
    (void)send_all(client, response);
}

// "IV0000-CUSA00000_00-..." — bounded and printable is enough; this only ever
// reaches sceAppInstUtilGetInstallStatus's fixed-size input, never a shell or path.
static int content_id_is_safe(const char *id) {
    size_t len = strlen(id);
    if (len == 0 || len >= 0x30) return 0;
    for (size_t i = 0; i < len; i++) {
        if (id[i] < 0x20 || id[i] > 0x7e) return 0;
    }
    return 1;
}

static void reply_install_status(int client, const char *content_id) {
    pkg_install_status_t status;
    memset(&status, 0, sizeof(status));
    int rc = sceAppInstUtilGetInstallStatus(content_id, &status);
    if (rc != 0) {
        reply_error(client, "status", rc);
        return;
    }
    char response[64];
    size_t status_len = strnlen(status.status, sizeof(status.status));
    snprintf(response, sizeof(response), "install-status:%.*s:0x%08x", (int)status_len, status.status,
             (unsigned)status.error_info.error_code);
    (void)send_all(client, response);
}

// "PPSA01234" / "CUSA01234": four capitals and five digits, nothing else reaches the uninstall call.
static int title_id_is_safe(const char *id) {
    if (strlen(id) != 9) return 0;
    for (int i = 0; i < 4; i++) if (id[i] < 'A' || id[i] > 'Z') return 0;
    for (int i = 4; i < 9; i++) if (id[i] < '0' || id[i] > '9') return 0;
    return 1;
}

static int path_is_safe(const char *path) {
    if (!path) return 0;
    // A stream URI is handed to the installer verbatim; only local paths get the
    // traversal check, since ".." means nothing in a URL we built ourselves.
    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        return strlen(path) < 4096;
    }
    if (path[0] != '/') return 0;
    if (strstr(path, "..")) return 0;
    return strlen(path) < 4096;
}

static void normalize_path(const char *input, char *output, size_t capacity) {
    if (strncmp(input, "/data/", 6) == 0) {
        snprintf(output, capacity, "/user%s", input);
        return;
    }
    strncpy(output, input, capacity - 1);
    output[capacity - 1] = '\0';
}

static void queue_install(int client, const helper_snapshot_t *snapshot, const char *request,
                          const char *icon_url, const char *name) {
    // The system reads these after the call returns, so they must outlive this function.
    static char uri[4102];
    static char name_copy[128];
    static char icon_copy[256];
    static pkg_meta_t meta;
    static pkg_info_t info;
    static pkg_playgo_t playgo;
    snprintf(name_copy, sizeof(name_copy), "%s", name);
    snprintf(icon_copy, sizeof(icon_copy), "%s", icon_url);
    normalize_path(request, uri, sizeof(uri));
    const char *package_name = filename_from_path(uri);
    log_event("Queueing package: %s", package_name);

    memset(&meta, 0, sizeof(meta));
    memset(&info, 0, sizeof(info));
    memset(&playgo, 0, sizeof(playgo));
    meta.uri = uri;
    meta.ex_uri = "";
    meta.playgo_scenario_id = "";
    meta.content_id = "";
    // Sony's own install card shows this name and cover art.
    meta.content_name = name_copy;
    meta.icon_url = icon_copy;

    // Firmware 11 and newer queue under SYSTEM; older ones only take a local
    // package under ShellCore, so lend the call that authid and hand it back.
    pid_t self = getpid();
    uint64_t saved_authid = prospero_pkg_needs_shellcore_authid(snapshot->firmware_major)
                                ? kernel_get_ucred_authid(self)
                                : 0;
    if (saved_authid) kernel_set_ucred_authid(self, PROSPERO_PKG_SHELLCORE_AUTHID);
    int queue_rc = sceAppInstUtilInstallByPackage(&meta, &info, &playgo);
    if (saved_authid) kernel_set_ucred_authid(self, saved_authid);
    if (queue_rc == 0) {
        char response[80];
        size_t content_len = strnlen(info.content_id, sizeof(info.content_id));
        snprintf(response, sizeof(response), "ok:%.*s", (int)content_len, info.content_id);
        (void)send_all(client, response);
        log_event("Package accepted: %.*s", (int)content_len, info.content_id);
    } else {
        log_event("Package queue failed: %s (0x%08x)", package_name, (unsigned)queue_rc);
        reply_error(client, "queue", queue_rc);
    }
    close(client);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    (void)syscall(SYS_thr_set_name, -1, "prospero-pkgd.elf");

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return 1;
    int enabled = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PROSPERO_PKGD_PORT);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(server);
        return 2;
    }
    if (listen(server, 8) != 0) {
        close(server);
        return 3;
    }
    log_event("Private PKG service started (PID %d)", (int)getpid());
    log_event("Listening for package requests on port %d", PROSPERO_PKGD_PORT);

    // The console's install card fetches its cover art over the network, so these come up first.
    int user_priority = 256;
    int netctl_rc = sceNetCtlInit();
    int user_rc = sceUserServiceInitialize(&user_priority);
    int net_rc = sceNetInit();
    log_event("Network services initialized (0x%08x, 0x%08x, 0x%08x)", (unsigned)netctl_rc, (unsigned)user_rc, (unsigned)net_rc);

    pthread_t initializer;
    int thread_rc = pthread_create(&initializer, NULL, initialize_service, NULL);
    if (thread_rc != 0) {
        prospero_pkg_firmware_t unknown = {0, 0};
        set_state(HELPER_INITIALIZE_ERROR, thread_rc, unknown, PROSPERO_PKG_SYSTEM_AUTHID);
        log_event("Could not start the AppInstUtil initializer (0x%08x)", (unsigned)thread_rc);
    } else {
        pthread_detach(initializer);
    }

    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) continue;

        struct timeval timeout = {40, 0};
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        int nodelay = 1;
        (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        char request[4096];
        size_t used = 0;
        while (used + 1 < sizeof(request)) {
            ssize_t got = recv(client, request + used, sizeof(request) - used - 1, 0);
            if (got <= 0) break;
            used += (size_t)got;
            if (memchr(request, '\n', used)) break;
        }
        request[used] = '\0';
        while (used > 0 && (request[used - 1] == '\n' || request[used - 1] == '\r' || request[used - 1] == ' ')) {
            request[--used] = '\0';
        }

        if (strcmp(request, "status") == 0) {
            reply_status(client);
            close(client);
            continue;
        }
        if (strcmp(request, "stop") == 0) {
            (void)send_all(client, "ok:stopping");
            close(client);
            break;
        }

        helper_snapshot_t snapshot = get_state();
        if (snapshot.state != HELPER_READY) {
            reply_status(client);
            close(client);
            continue;
        }
        if (strncmp(request, "status-install:", 15) == 0) {
            const char *content_id = request + 15;
            if (!content_id_is_safe(content_id)) {
                reply_error(client, "status", -1);
            } else {
                reply_install_status(client, content_id);
            }
            close(client);
            continue;
        }
        if (strncmp(request, "uninstall:", 10) == 0) {
            const char *title_id = request + 10;
            if (!title_id_is_safe(title_id)) {
                reply_error(client, "uninstall", -1);
            } else {
                int uninstall_rc = sceAppInstUtilAppUnInstall(title_id);
                log_event("Uninstall requested: %s (0x%08x)", title_id, (unsigned)uninstall_rc);
                if (uninstall_rc == 0) (void)send_all(client, "ok:uninstall");
                else reply_error(client, "uninstall", uninstall_rc);
            }
            close(client);
            continue;
        }
        if (strncmp(request, "install-meta:", 13) == 0) {
            // install-meta:<icon url>\t<name>\t<uri>, the cover art and name for the install card.
            char *icon = request + 13;
            char *name = strchr(icon, '\t');
            char *uri = name ? strchr(name + 1, '\t') : NULL;
            if (name) *name++ = '\0';
            if (uri) *uri++ = '\0';
            int valid = uri && path_is_safe(uri) && strlen(name) <= 100 && strlen(icon) < 200 &&
                        (icon[0] == '\0' || strncmp(icon, "http://127.0.0.1:9045/stream/install/icon-", 42) == 0);
            for (const char *c = valid ? icon : ""; *c; c++) if ((unsigned char)*c < 0x20) valid = 0;
            for (const char *c = valid ? name : ""; *c; c++) if ((unsigned char)*c < 0x20) valid = 0;
            if (!valid) {
                reply_error(client, "request", -1);
                close(client);
                continue;
            }
            queue_install(client, &snapshot, uri, icon, name[0] ? name : "Prospero Manager");
            continue;
        }
        if (!path_is_safe(request)) {
            reply_error(client, "request", -1);
            log_event("Package request rejected — unsafe path");
            close(client);
            continue;
        }

        queue_install(client, &snapshot, request, "", "Prospero Manager");
    }

    log_event("Private PKG service stopped");
    close(server);
    return 0;
}
