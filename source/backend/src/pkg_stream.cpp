#include "pkg_stream.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <ctime>
#include <string>
#include <vector>

#include "job_queue.hpp"
#include "pkg_live_cache.hpp"
#include "ps5_http_bridge.h"
#include "log.hpp"

namespace prospero {

namespace {

// Separate from the dashboard's own listener: the installer pulls at full disk
// speed and would otherwise make the UI crawl.
constexpr uint16_t kStreamPort = 9045;
constexpr size_t kChunk = 2 * 1024 * 1024;

// An install keeps streaming long after it reports "queued", so several can be in
// flight at once. Each keeps its own session until it ages out.
struct Session {
    std::string name;
    std::string path;    // local file
    std::string url;     // remote package
    std::string live_id; // live RAM session (pkg_live_cache); path/url empty when set
    long long size = 0;
};

constexpr size_t kMaxSessions = 8;

std::mutex g_mu;
std::vector<Session> g_sessions;
bool g_listening = false;
// Bounded: a real install makes thousands of range requests, and only the first
// few show whether the installer is talking to us as expected.
std::atomic<int> g_log_budget{0};
std::atomic<uint64_t> g_bytes_served{0};

void StreamLog(const std::string &line) {
    if (g_log_budget.fetch_sub(1) <= 0) return;
    LogEvent("[pkgstream] " + line);
}

bool SendAll(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
        g_bytes_served += static_cast<uint64_t>(n);
    }
    return true;
}

// "Range: bytes=START-[END]". Returns false when the header is absent or unusable,
// which the caller answers with a plain 200.
bool ParseRange(const std::string &request, long long total, long long *start, long long *end) {
    // Matched at a line start so "If-Range:", which the installer also sends, can't
    // be mistaken for the real header.
    size_t at = request.find("\nRange:");
    if (at == std::string::npos) at = request.find("\nrange:");
    if (at == std::string::npos) return false;
    size_t eol = request.find('\n', at + 1);
    size_t eq = request.find("bytes=", at);
    if (eq == std::string::npos || (eol != std::string::npos && eq > eol)) return false;

    const char *p = request.c_str() + eq + 6;
    char *stop = nullptr;
    long long from = std::strtoll(p, &stop, 10);
    if (stop == p || from < 0) return false;
    long long to = total - 1;
    if (stop && *stop == '-' && stop[1] >= '0' && stop[1] <= '9') to = std::strtoll(stop + 1, nullptr, 10);
    if (to >= total) to = total - 1;
    if (from > to) return false;

    *start = from;
    *end = to;
    return true;
}

// Only a filename handed out by this server is served; anything else is a stray probe.
const Session *ResolveRequest(const std::string &request, std::string *method) {
    size_t sp = request.find(' ');
    if (sp == std::string::npos) return nullptr;
    *method = request.substr(0, sp);
    if (*method != "GET" && *method != "HEAD") return nullptr;
    size_t end = request.find(' ', sp + 1);
    if (end == std::string::npos) return nullptr;
    // The installer appends its own query (product, serverIpAddr, ...), so compare
    // the path alone.
    size_t query = request.find('?', sp + 1);
    if (query != std::string::npos && query < end) end = query;

    for (const auto &session : g_sessions) {
        if (request.compare(sp + 1, end - sp - 1, "/stream/install/" + session.name) == 0) return &session;
    }
    return nullptr;
}

std::string g_cover;      // guarded by g_mu
std::string g_cover_name; // the installer fetches it next to the package, under /stream/install/

bool IsCoverRequest(const std::string &request, const std::string &name) {
    size_t sp = request.find(' ');
    if (name.empty() || sp == std::string::npos ||
        (request.compare(0, sp, "GET") != 0 && request.compare(0, sp, "HEAD") != 0)) {
        return false;
    }
    size_t end = request.find_first_of(" ?", sp + 1);
    return end != std::string::npos && request.compare(sp + 1, end - sp - 1, "/stream/install/" + name) == 0;
}

// Answers one request on a connection that closes afterwards.
void ServeRequest(int client, const std::string &request) {
        std::string cover, cover_name;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        cover = g_cover;
        cover_name = g_cover_name;
    }
    if (IsCoverRequest(request, cover_name)) {
        LogEvent("[pkgstream] cover art requested by the console (" + std::to_string(cover.size()) + " bytes)");
        std::string head = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " + std::to_string(cover.size()) +
                           "\r\nConnection: close\r\n\r\n";
        if (SendAll(client, head.data(), head.size()) && request.compare(0, 4, "HEAD") != 0) {
            (void)SendAll(client, cover.data(), cover.size());
        }
        return;
    }
    std::string first_line = request.substr(0, request.find('\r'));
    std::string path, url, live_id, method;
    long long total = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const Session *session = ResolveRequest(request, &method);
        if (!session) {
            StreamLog("404 for: " + (first_line.empty() ? std::string("<no request line>") : first_line));
            (void)SendAll(client, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", 66);
            return;
        }
        path = session->path;
        url = session->url;
        live_id = session->live_id;
        total = session->size;
    }

    long long start = 0, end = total - 1;
    bool ranged = ParseRange(request, total, &start, &end);
    long long length = end - start + 1;
    StreamLog(first_line + " -> " + (ranged ? "206 " : "200 ") + std::to_string(start) + "-" +
              std::to_string(end) + "/" + std::to_string(total));

    char header[512];
    int header_len;
    if (ranged) {
        header_len = std::snprintf(header, sizeof(header),
                                   "HTTP/1.1 206 Partial Content\r\n"
                                   "Content-Type: application/octet-stream\r\n"
                                   "Accept-Ranges: bytes\r\n"
                                   "Content-Length: %lld\r\n"
                                   "Content-Range: bytes %lld-%lld/%lld\r\n"
                                   "Connection: close\r\n\r\n",
                                   length, start, end, total);
    } else {
        header_len = std::snprintf(header, sizeof(header),
                                   "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/octet-stream\r\n"
                                   "Accept-Ranges: bytes\r\n"
                                   "Content-Length: %lld\r\n"
                                   "Connection: close\r\n\r\n",
                                   total);
    }
    if (header_len <= 0 || !SendAll(client, header, static_cast<size_t>(header_len)) || method == "HEAD") return;

    std::string chunk(kChunk, '\0');
    long long at = start;

    if (!live_id.empty()) {
        if (!PkgLiveCache::CheckId(live_id) || PkgLiveCache::Attach() != 0) return;
        while (at <= end) {
            size_t want = static_cast<size_t>(std::min<long long>(static_cast<long long>(kChunk), end - at + 1));
            long got = PkgLiveCache::Read(static_cast<uint64_t>(at), chunk.data(), want);
            if (got <= 0 || !SendAll(client, chunk.data(), static_cast<size_t>(got))) break;
            at += got;
        }
        PkgLiveCache::Detach();
        return;
    }

    if (!url.empty()) {
        while (at <= end) {
            // Ask for exactly the range needed: a server that ignores Range sends the whole
            // file, and filling the buffer would overshoot the promised Content-Length.
            long long stop = std::min<long long>(at + static_cast<long long>(kChunk) - 1, end);
            size_t want = static_cast<size_t>(stop - at + 1);
            size_t got = 0;
            prosperomgr_http_result fetch{};
            if (prosperomgr_ps5_http_range(url.c_str(), at, stop, chunk.data(), want, &got, &fetch) != 0 ||
                got == 0) {
                StreamLog("remote range " + std::to_string(at) + "-" + std::to_string(stop) + " failed");
                break;
            }
            if (!SendAll(client, chunk.data(), got)) break;
            at += static_cast<long long>(got);
        }
        return;
    }

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    while (at <= end) {
        size_t want = static_cast<size_t>(std::min<long long>(static_cast<long long>(kChunk), end - at + 1));
        ssize_t got;
        do { got = pread(fd, chunk.data(), want, static_cast<off_t>(at)); } while (got < 0 && errno == EINTR);
        if (got <= 0 || !SendAll(client, chunk.data(), static_cast<size_t>(got))) break;
        at += got;
    }
    close(fd);
}

void ServeConnection(int client) {
    timeval timeout{30, 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int nodelay = 1;
    (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sndbuf = 1024 * 1024;
    (void)setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
        ssize_t got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        request.append(buffer, static_cast<size_t>(got));
    }
    ServeRequest(client, request);
    close(client);
}

void ListenLoop() {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return;
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(kStreamPort);
    if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(server, 8) != 0) {
        close(server);
        LogEvent("PKG stream server could not bind port " + std::to_string(kStreamPort));
        return;
    }
    LogEvent("PKG stream server ready on port " + std::to_string(kStreamPort));

    for (;;) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;
        // The installer reads several ranges at once; a connection waiting on data must not hold up the rest.
        StartBackgroundThread([client]() { ServeConnection(client); });
    }
}

} // namespace

namespace {

std::string PinSession(const std::string &path, const std::string &url, long long size,
                       const std::string &live_id = "") {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_listening) {
        g_listening = true;
        StartBackgroundThread(ListenLoop);
    }

    // A fresh name per install: the installer caches by URI, so reusing one makes
    // a second install of the same package read the first one's bytes.
    static unsigned sequence = 0;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    Session session;
    session.name = "package-" + std::to_string(now) + "-" + std::to_string(++sequence) + ".pkg";
    session.path = path;
    session.url = url;
    session.live_id = live_id;
    session.size = size;

    g_sessions.push_back(session);
    if (g_sessions.size() > kMaxSessions) g_sessions.erase(g_sessions.begin());
    g_log_budget = 24;
    return "http://127.0.0.1:" + std::to_string(kStreamPort) + "/stream/install/" + session.name;
}

} // namespace

bool StartPkgStream(const std::string &path, std::string *uri, std::string *err) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        if (err) *err = "package file is not readable";
        return false;
    }
    *uri = PinSession(path, "", st.st_size);
    return true;
}

bool StartRemotePkgStream(const std::string &url, std::string *uri, std::string *err) {
    long long size = 0;
    prosperomgr_http_result probe{};
    if (prosperomgr_ps5_http_size(url.c_str(), &size, &probe) != 0 || size <= 0) {
        if (err) {
            *err = probe.http_status > 0
                       ? "the server answered HTTP " + std::to_string(probe.http_status)
                       : "the server sent no size, so the package cannot be read in ranges";
        }
        return false;
    }
    // Plenty of simple servers (python -m http.server among them) ignore Range and
    // send the whole file, which would corrupt every response built from it.
    char probe_byte = 0;
    size_t got = 0;
    prosperomgr_http_result ranged{};
    if (prosperomgr_ps5_http_range(url.c_str(), 0, 0, &probe_byte, 1, &got, &ranged) != 0 ||
        ranged.http_status != 206) {
        if (err) {
            *err = "that server ignores Range requests (answered HTTP " +
                   std::to_string(ranged.http_status) + "), so the package cannot be streamed";
        }
        return false;
    }

    LogEvent("Streaming package from URL (" + std::to_string(size) + " bytes)");
    *uri = PinSession("", url, size);
    return true;
}

bool StartLivePkgStream(const std::string &session_id, std::string *uri, std::string *err) {
    if (!PkgLiveCache::CheckId(session_id)) {
        if (err) *err = "live upload session is no longer active";
        return false;
    }
    long long size = static_cast<long long>(PkgLiveCache::GetTotal());
    if (size <= 0) {
        if (err) *err = "live upload session has no size";
        return false;
    }
    *uri = PinSession("", "", size, session_id);
    return true;
}

std::string SetPkgCover(const std::string &png) {
    static unsigned counter = 0;
    std::lock_guard<std::mutex> lock(g_mu);
    if (png.size() < 8 || png.size() > 10 * 1024 * 1024 || png.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) {
        g_cover.clear();
        g_cover_name.clear();
        return "";
    }
    g_cover = png;
    g_cover_name = "icon-" + std::to_string(static_cast<long long>(time(nullptr))) + "-" + std::to_string(++counter) + ".png";
    return "http://127.0.0.1:" + std::to_string(kStreamPort) + "/stream/install/" + g_cover_name;
}

void StopPkgStream(const std::string &uri) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it) {
        if (uri.size() >= it->name.size() && uri.compare(uri.size() - it->name.size(), it->name.size(), it->name) == 0) {
            g_sessions.erase(it);
            return;
        }
    }
}

uint64_t PkgStreamBytesServed() { return g_bytes_served.load(); }

} // namespace prospero
