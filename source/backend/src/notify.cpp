#include "notify.hpp"

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "assets.hpp"
#include "log.hpp"

extern "C" {
// Plain-text toast, the shape every payload uses. Also Notify()'s fallback when the
// rich call fails.
struct NotifyRequest {
    char reserved[45];
    char message[3075];
};
int sceKernelSendNotificationRequest(int, NotifyRequest *, unsigned long, int);

// Rich toast IPC takes a JSON payload rather than a fixed struct. This is the call that
// actually renders a custom icon.
int sceNotificationSend(int user_id, bool is_logged_in, const char *payload);
}

namespace prospero {

namespace {

// Toasts go through one long-lived thread instead of one thread per call:
// the autoloader fires several in a burst, and this syscall can stall.
constexpr size_t kMaxQueued = 32;

// Under /user/data, not /data: the notification service resolves icon.url from its own
// mount view, the same way AppInstUtil does.
constexpr const char *kIconDir = "/user/data/ProsperoMgr";
constexpr const char *kIconPath = "/user/data/ProsperoMgr/notify_icon.png";
constexpr int kSystemUserId = 0xFE; // SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM

struct QueuedToast {
    std::string message;
    bool rich;
    std::string icon_path; // empty: our own app icon
    std::string action_name, action_url; // optional button that opens action_url
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<QueuedToast> g_queue;
bool g_started = false;

// The rich toast's icon.url wants a real file on disk, not embedded bytes — write our own
// app icon out once.
bool EnsureIconOnDisk() {
    struct stat st {};
    if (stat(kIconPath, &st) != 0 || st.st_size == 0) {
        std::error_code ec;
        std::filesystem::create_directories(kIconDir, ec);
        HttpResponse icon = ServeEmbeddedAsset("/icon-64.png");
        if (icon.status != 200) {
            LogEvent("[notify] embedded icon asset missing");
            return false;
        }
        std::ofstream out(kIconPath, std::ios::binary | std::ios::trunc);
        out.write(icon.body.data(), static_cast<std::streamsize>(icon.body.size()));
        if (!out.good()) {
            LogEvent(std::string("[notify] could not write ") + kIconPath);
            return false;
        }
        out.close();
        if (stat(kIconPath, &st) != 0) return false;
    }

    // Re-applied every run, not just after writing: an icon left by an earlier
    // build keeps whatever permissions it was created with.
    chmod(kIconDir, 0755);
    chmod(kIconPath, 0644);
    return true;
}

std::string JsonEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += static_cast<char>(c);
        }
    }
    return out;
}

std::string IsoTimestampUtc() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
    return buf;
}

// Sony's InteractiveToastTemplateB schema, sent through sceNotificationSend to get a
// real app icon on the toast.
bool SendRichToast(const std::string &message, const std::string &icon_path, const std::string &action_name = "",
                   const std::string &action_url = "") {
    if (!EnsureIconOnDisk()) {
        LogEvent("[notify] could not write notification icon, falling back to plain toast");
        return false;
    }
    // Omitting soundEffect is what lets the default toast sound play; setting it to
    // "none" mutes the toast.
    std::string actions;
    if (!action_url.empty()) {
        actions = ",\"actions\":[{\"actionName\":\"" + JsonEscape(action_name) +
                  "\",\"actionType\":\"DeepLink\",\"defaultFocus\":true,\"parameters\":{\"actionUrl\":\"" +
                  JsonEscape(action_url) + "\"}}]";
    }
    std::string payload =
        "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
        "\"channelType\":\"ServiceFeedback\",\"bundleName\":\"ProsperoMgr\","
        "\"useCaseId\":\"IDC\","
        "\"toastOverwriteType\":\"InQueue\",\"isImmediate\":true,\"priority\":100,"
        "\"viewData\":{\"icon\":{\"type\":\"Url\",\"parameters\":{\"url\":\"" +
        (icon_path.empty() ? std::string(kIconPath) : icon_path) + "\"}},\"message\":{\"body\":\"" + JsonEscape(message) +
        "\"}" + actions + "}},\"createdDateTime\":\"" + IsoTimestampUtc() + "\",\"localNotificationId\":\"" +
        std::to_string((static_cast<uint32_t>(std::time(nullptr))) ^ static_cast<uint32_t>(getpid())) + "\"}";
    int rc = sceNotificationSend(kSystemUserId, true, payload.c_str());
    // Once per session: tells a rejected toast apart from one the system accepted
    // but drew without our icon.
    static bool reported = false;
    if (!reported) {
        reported = true;
        struct stat icon {};
        stat(kIconPath, &icon);
        char line[160];
        std::snprintf(line, sizeof(line), "[notify] rich toast rc=0x%08x, icon %lld bytes, mode %o",
                      static_cast<unsigned>(rc), static_cast<long long>(icon.st_size),
                      static_cast<unsigned>(icon.st_mode & 07777));
        LogEvent(line);
    }
    if (rc != 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[notify] sceNotificationSend failed (rc=0x%08x)", static_cast<unsigned>(rc));
        LogEvent(buf);
    }
    return rc == 0;
}

void SendPlainToast(const std::string &message) {
    NotifyRequest req{};
    std::snprintf(req.message, sizeof(req.message), "%s", message.c_str());
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

void *NotifierLoop(void *) {
    for (;;) {
        QueuedToast toast;
        {
            std::unique_lock<std::mutex> lock(g_mu);
            g_cv.wait(lock, [] { return !g_queue.empty(); });
            toast = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (!toast.rich || !SendRichToast(toast.message, toast.icon_path, toast.action_name, toast.action_url)) {
            SendPlainToast(toast.message);
        }
    }
    return nullptr;
}

void QueueToast(const std::string &message, bool rich, const std::string &icon_path = "",
                const std::string &action_name = "", const std::string &action_url = "") {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_started) {
        g_started = true;
        // Explicit stack: the default here is small and NotifyRequest alone is 3 KB.
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        pthread_t tid;
        pthread_create(&tid, &attr, NotifierLoop, nullptr);
        pthread_attr_destroy(&attr);
    }
    // Drop the oldest rather than grow without bound if the syscall wedges.
    if (g_queue.size() >= kMaxQueued) g_queue.pop_front();
    g_queue.push_back({message, rich, icon_path, action_name, action_url});
    g_cv.notify_one();
}

} // namespace

void Notify(const std::string &message) { QueueToast(message, /*rich=*/true); }

void NotifyDebug(const std::string &message) { QueueToast(message, /*rich=*/false); }

void NotifyWithAction(const std::string &message, const std::string &action_name, const std::string &action_url) {
    QueueToast(message, /*rich=*/true, "", action_name, action_url);
}

void NotifyWithImage(const std::string &message, const std::string &png) {
    static unsigned counter = 0;
    static std::string previous;
    if (png.empty() || !EnsureIconOnDisk()) {
        Notify(message);
        return;
    }
    // A new name each time, so the notification service never shows an older cover from its cache.
    std::string path = std::string(kIconDir) + "/cover-" + std::to_string(++counter) + ".png";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(png.data(), static_cast<std::streamsize>(png.size()));
        if (!out.good()) {
            Notify(message);
            return;
        }
    }
    chmod(path.c_str(), 0644);
    if (!previous.empty()) std::remove(previous.c_str());
    previous = path;
    QueueToast(message, /*rich=*/true, path);
}

} // namespace prospero
