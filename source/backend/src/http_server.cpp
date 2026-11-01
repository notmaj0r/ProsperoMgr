#include "http_server.hpp"

#include <fcntl.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#include "json_writer.hpp"
#include "file_jobs.hpp"
#include "log.hpp"

namespace prospero {

namespace {

// Non-streamed request bodies are fully buffered in RAM — cap it so a bad
// request fails predictably instead of growing until allocation fails.
constexpr size_t kMaxBodyBytes = 1024u * 1024;

// MHD hands upload data over in small fragments. Batch them into
// storage-sized writes instead of hitting the filesystem per fragment.
constexpr size_t kUploadWriteChunk = 2 * 1024u * 1024u;

// Also the size FileReadStream reads in (api_routes.cpp) — keep both in
// sync or MHD clamps every pread() down to whatever's smaller.
constexpr size_t kResponseStreamChunk = 2u * 1024u * 1024u;

// Set on the listening socket before bind()/listen(). Setting SO_RCVBUF
// after accept() is too late on OrbisOS — it stays at the small default.
constexpr int kListenRcvBuf = 4 * 1024 * 1024;

// OrbisOS's default SNDBUF caps outbound throughput well under GbE for a
// plain HTTP socket — set explicitly on accept instead.
constexpr int kAcceptedSndBuf = 4 * 1024 * 1024;

// Sync only on the chunk that finishes the file or crosses this boundary,
// not every chunk — bounds how much a crash could lose either way.
constexpr uint64_t kUploadSyncCheckpoint = 256ull * 1024ull * 1024ull;

ssize_t ReadResponseStream(void *cls, uint64_t position, char *buffer, size_t max_bytes) noexcept {
    auto *stream = static_cast<std::shared_ptr<HttpBodyStream> *>(cls);
    if (!stream || !*stream) return MHD_CONTENT_READER_END_WITH_ERROR;
    try {
        return (*stream)->Read(position, buffer, max_bytes);
    } catch (const std::exception &e) {
        LogEvent(std::string("Response stream failed safely: ") + e.what());
    } catch (...) {
        LogEvent("Response stream failed safely");
    }
    return MHD_CONTENT_READER_END_WITH_ERROR;
}

void FreeResponseStream(void *cls) {
    delete static_cast<std::shared_ptr<HttpBodyStream> *>(cls);
}

bool ParseUnsigned(const char *text, uint64_t *value) {
    if (!text || !*text || !value) return false;
    errno = 0;
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
}

bool SafeUploadId(const char *text) {
    if (!text || !*text || std::strlen(text) > 64) return false;
    for (const unsigned char c : std::string(text)) {
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}

bool WriteAll(int fd, const char *data, size_t size) {
    size_t written = 0;
    while (written < size) {
        ssize_t rc = write(fd, data + written, size - written);
        if (rc > 0) {
            written += static_cast<size_t>(rc);
            continue;
        }
        if (rc < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

MHD_Result RejectTooLarge(MHD_Connection *conn) {
    static const char kMsg[] = "{\"error\":\"request body too large\"}";
    MHD_Response *resp = MHD_create_response_from_buffer(sizeof(kMsg) - 1, const_cast<char *>(kMsg),
                                                           MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONNECTION, "close");
    MHD_Result ret = MHD_queue_response(conn, 413, resp);
    MHD_destroy_response(resp);
    return ret;
}

std::string CurrentIpv4() {
    ifaddrs *addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return {};
    std::string result;
    for (ifaddrs *item = addresses; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            std::strncmp(item->ifa_name, "lo", 2) == 0) continue;
        char text[INET_ADDRSTRLEN]{};
        auto *address = reinterpret_cast<sockaddr_in *>(item->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) {
            result = text;
            break;
        }
    }
    freeifaddrs(addresses);
    return result;
}

bool ProbeHttp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int result = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (result != 0 && errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    pollfd event{fd, POLLOUT, 0};
    if (result != 0 && poll(&event, 1, 1500) <= 0) {
        close(fd);
        return false;
    }
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0 || socket_error != 0) {
        close(fd);
        return false;
    }

    static const char request[] = "GET /api/status HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    if (send(fd, request, sizeof(request) - 1, 0) != static_cast<ssize_t>(sizeof(request) - 1)) {
        close(fd);
        return false;
    }
    event.events = POLLIN;
    event.revents = 0;
    char response[16]{};
    bool ok = poll(&event, 1, 1500) > 0 && recv(fd, response, sizeof(response), 0) > 7 &&
              std::memcmp(response, "HTTP/1.", 7) == 0;
    close(fd);
    return ok;
}

void TuneAcceptedSocket(int fd) {
    int nodelay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sndbuf = kAcceptedSndBuf;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
}

int CreateTunedListenSocket(uint16_t port, int *out_effective_rcvbuf) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    int rcvbuf = kListenRcvBuf;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    if (out_effective_rcvbuf) {
        int effective = 0;
        socklen_t effective_len = sizeof(effective);
        *out_effective_rcvbuf =
            getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &effective, &effective_len) == 0 ? effective : -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    if (listen(fd, 32) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

bool ListenSocketAlive(int fd) {
    int type = 0;
    socklen_t size = sizeof(type);
    if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_STREAM) return false;
#ifdef SO_ACCEPTCONN
    int accepting = 0;
    size = sizeof(accepting);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &size) == 0) return accepting != 0;
#endif
    return true;
}

unsigned int ListenerRetryDelayMs(unsigned int attempt) {
    static constexpr unsigned int delays[] = {250, 500, 1000, 2000, 4000, 8000, 10000};
    size_t index = std::min<size_t>(attempt, sizeof(delays) / sizeof(delays[0]) - 1);
    return delays[index];
}

bool InterruptibleSleep(const std::atomic<bool> &stopping, unsigned int milliseconds) {
    while (milliseconds > 0 && !stopping) {
        unsigned int slice = std::min(milliseconds, 100u);
        usleep(slice * 1000u);
        milliseconds -= slice;
    }
    return !stopping;
}

} // namespace

HttpServer::HttpServer(uint16_t port) : port_(port) {}

HttpServer::~HttpServer() { Stop(); }

HttpServer::ConnState::~ConnState() {
    if (upload_fd >= 0) close(upload_fd);
    if (!preserve_body_file && !body_file.empty()) std::remove(body_file.c_str());
    if (!upload_reservation_path.empty()) {
        if (persistent_upload_reservation) IdleFileUpload(upload_reservation_path, upload_reservation_token);
        else ReleaseFileUpload(upload_reservation_path, upload_reservation_token);
    }
}

bool HttpServer::AppendUpload(ConnState *state, const char *data, size_t size) {
    if (!state || state->upload_fd < 0) return false;
    if (state->upload_buffer.empty()) state->upload_buffer.resize(kUploadWriteChunk);

    while (size > 0) {
        // Avoid an extra copy when MHD does hand us one or more complete
        // storage chunks and there is no partial chunk pending.
        if (state->upload_buffer_used == 0 && size >= kUploadWriteChunk) {
            size_t direct = size - (size % kUploadWriteChunk);
            if (!WriteAll(state->upload_fd, data, direct)) return false;
            data += direct;
            size -= direct;
            continue;
        }

        size_t take = std::min(size, kUploadWriteChunk - state->upload_buffer_used);
        std::memcpy(state->upload_buffer.data() + state->upload_buffer_used, data, take);
        state->upload_buffer_used += take;
        data += take;
        size -= take;
        if (state->upload_buffer_used == kUploadWriteChunk) {
            if (!WriteAll(state->upload_fd, state->upload_buffer.data(), state->upload_buffer_used)) return false;
            state->upload_buffer_used = 0;
        }
    }
    return true;
}

bool HttpServer::FlushUpload(ConnState *state) {
    if (!state || state->upload_fd < 0) return false;
    if (state->upload_buffer_used == 0) return true;
    if (!WriteAll(state->upload_fd, state->upload_buffer.data(), state->upload_buffer_used)) return false;
    state->upload_buffer_used = 0;
    return true;
}

void HttpServer::Route(const std::string &method, const std::string &path, RouteHandler handler) {
    RegisteredRoute route;
    route.handler = std::move(handler);
    routes_[method + " " + path] = std::move(route);
}

void HttpServer::UploadRoute(const std::string &method, const std::string &path, const std::string &temp_dir,
                             uint64_t max_bytes, RouteHandler handler) {
    RegisteredRoute route;
    route.handler = std::move(handler);
    route.upload_temp_dir = temp_dir;
    route.max_upload_bytes = max_bytes;
    routes_[method + " " + path] = std::move(route);
}

void HttpServer::DirectUploadRoute(const std::string &method, const std::string &path,
                                   const std::string &query_key, uint64_t max_bytes, RouteHandler handler) {
    RegisteredRoute route;
    route.handler = std::move(handler);
    route.upload_target_query = query_key;
    route.max_upload_bytes = max_bytes;
    routes_[method + " " + path] = std::move(route);
}

void HttpServer::AppendUploadRoute(const std::string &method, const std::string &path,
                                   const std::string &target_query, const std::string &upload_id_query,
                                   const std::string &offset_query, const std::string &total_query,
                                   uint64_t max_bytes, RouteHandler handler) {
    RegisteredRoute route;
    route.handler = std::move(handler);
    route.upload_target_query = target_query;
    route.upload_id_query = upload_id_query;
    route.upload_offset_query = offset_query;
    route.upload_total_query = total_query;
    route.max_upload_bytes = max_bytes;
    routes_[method + " " + path] = std::move(route);
}

void HttpServer::CountingUploadRoute(const std::string &method, const std::string &path, uint64_t max_bytes,
                                     RouteHandler handler) {
    RegisteredRoute route;
    route.handler = std::move(handler);
    route.upload_count_only = true;
    route.max_upload_bytes = max_bytes;
    routes_[method + " " + path] = std::move(route);
}

void HttpServer::SetStaticFallback(StaticHandler handler) { static_handler_ = std::move(handler); }

namespace {

// Serves a disk-backed response through pread() instead of sendfile().
class FdBodyStream final : public HttpBodyStream {
  public:
    explicit FdBodyStream(int fd) : fd_(fd) {}
    ~FdBodyStream() override { if (fd_ >= 0) close(fd_); }

    ssize_t Read(uint64_t position, char *buffer, size_t max_bytes) override {
        ssize_t got;
        do {
            got = pread(fd_, buffer, max_bytes, static_cast<off_t>(position));
        } while (got < 0 && errno == EINTR);
        return got;
    }

  private:
    int fd_;
};

// A few GETs are real user actions rather than polling, so they get logged too.
bool IsLoggedGet(const std::string &url) {
    return url == "/api/payloads/download" || url == "/api/catalog/refresh";
}

// Only these keys are ever pulled out of a request body — never the raw body,
// which would put the user's GitHub token in a plaintext log.
std::string ActionDetail(const HttpRequest &req) {
    std::string detail;
    for (const char *key : {"name", "path", "url", "pid"}) {
        // Query first: upload routes stream their body to a file and carry the
        // filename as a query argument instead.
        std::string value = req.Query(key);
        if (value.empty()) value = JsonExtractString(req.body, key);
        if (value.empty()) continue;
        if (value.size() > 60) value.resize(60);
        detail += " " + std::string(key) + "=" + value;
    }
    return detail;
}

// User-action trail. Polling GETs are skipped so the log stays readable and
// idle costs nothing — see LogEvent for why volume matters here.
void LogUserAction(const HttpRequest &req, const HttpResponse &resp) {
    if (req.method == "GET" && !IsLoggedGet(req.url)) return;
    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                    req.received_at)
                  .count();
    LogEvent(req.method + " " + req.url + ActionDetail(req) + " -> " + std::to_string(resp.status) + " (" +
             std::to_string(ms) + "ms)");
}

} // namespace

HttpResponse HttpServer::Dispatch(const HttpRequest &req) const {
    auto it = routes_.find(req.method + " " + req.url);
    if (it != routes_.end()) return it->second.handler(req);

    if (req.method == "GET" && static_handler_) return static_handler_(req.url);

    return HttpResponse::Error("not found", 404);
}

MHD_Result HttpServer::OnRequest(void *cls, MHD_Connection *conn, const char *url, const char *method,
                                 const char * /*version*/, const char *upload_data, size_t *upload_data_size,
                                 void **con_cls) {
    try {
        return OnRequestImpl(cls, conn, url, method, nullptr, upload_data, upload_data_size, con_cls);
    } catch (const std::exception &e) {
        try {
            LogEvent(std::string("HTTP callback failed safely: ") + e.what());
        } catch (...) {
        }
        return MHD_NO;
    } catch (...) {
        try {
            LogEvent("HTTP callback failed safely with an unknown exception");
        } catch (...) {
        }
        return MHD_NO;
    }
}

MHD_Result HttpServer::OnRequestImpl(void *cls, MHD_Connection *conn, const char *url, const char *method,
                                     const char * /*version*/, const char *upload_data, size_t *upload_data_size,
                                     void **con_cls) {
    auto *self = static_cast<HttpServer *>(cls);

    // MHD calls this handler multiple times per request: once to let us
    // set up per-connection state, then again for each body chunk.
    if (*con_cls == nullptr) {
        auto *state = new (std::nothrow) ConnState();
        if (!state) return MHD_NO;

        auto route = self->routes_.find(std::string(method) + " " + url);
        if (route != self->routes_.end() && route->second.upload_count_only) {
            // Debug net-sink: bytes are counted (state->body_size, below)
            // and immediately discarded — no temp file, no disk write at all.
            state->streamed = true;
            state->count_only = true;
            state->max_body_size = route->second.max_upload_bytes;
        } else if (route != self->routes_.end() &&
            (!route->second.upload_temp_dir.empty() || !route->second.upload_target_query.empty())) {
            state->streamed = true;
            state->max_body_size = route->second.max_upload_bytes;

            std::string pattern;
            if (!route->second.upload_target_query.empty()) {
                state->direct_upload = true;
                const char *target = MHD_lookup_connection_value(
                    conn, MHD_GET_ARGUMENT_KIND, route->second.upload_target_query.c_str());
                if (!target || target[0] != '/' || std::strstr(target, "/../") ||
                    (std::strlen(target) >= 3 && std::strcmp(target + std::strlen(target) - 3, "/..") == 0)) {
                    state->write_error = "upload destination must be a valid absolute path";
                    state->write_error_status = 400;
                } else if (!route->second.upload_id_query.empty()) {
                    const char *upload_id = MHD_lookup_connection_value(
                        conn, MHD_GET_ARGUMENT_KIND, route->second.upload_id_query.c_str());
                    const char *offset_text = MHD_lookup_connection_value(
                        conn, MHD_GET_ARGUMENT_KIND, route->second.upload_offset_query.c_str());
                    const char *total_text = MHD_lookup_connection_value(
                        conn, MHD_GET_ARGUMENT_KIND, route->second.upload_total_query.c_str());
                    const char *length_text = MHD_lookup_connection_value(
                        conn, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_LENGTH);
                    uint64_t offset = 0, total = 0, content_length = 0;
                    if (!SafeUploadId(upload_id) || !ParseUnsigned(offset_text, &offset) ||
                        !ParseUnsigned(total_text, &total) || !ParseUnsigned(length_text, &content_length) ||
                        offset > total || content_length > total - offset) {
                        state->write_error = "invalid resumable-upload checkpoint";
                        state->write_error_status = 400;
                    } else {
                        std::string reservation_error;
                        if (!ReserveFileUpload(target, upload_id, true, &reservation_error)) {
                            state->write_error = reservation_error;
                            state->write_error_status = 409;
                        } else {
                            state->upload_reservation_path = target;
                            state->upload_reservation_token = upload_id;
                            state->persistent_upload_reservation = true;
                            std::string partial = std::string(target) + ".pmgr-part-" + upload_id;
                            struct stat current {};
                            bool partial_exists = stat(partial.c_str(), &current) == 0;
                            struct stat published {};
                            // offset==0 is also a legitimate same-sized overwrite;
                            // only a later chunk can confirm a completed retry.
                            bool already_published = offset > 0 && !partial_exists && stat(target, &published) == 0 &&
                                                     static_cast<uint64_t>(published.st_size) == total &&
                                                     offset + content_length == total;
                            if (already_published) {
                                state->body_file = target;
                                state->preserve_body_file = true;
                                state->discard_upload = true;
                            } else if (partial_exists && current.st_size >= 0 &&
                                       static_cast<uint64_t>(current.st_size) == offset + content_length) {
                                // Drain a retry when the prior reply was lost after commit.
                                state->body_file = partial;
                                state->preserve_body_file = true;
                                state->discard_upload = true;
                            } else if ((partial_exists && (current.st_size < 0 ||
                                                           static_cast<uint64_t>(current.st_size) != offset)) ||
                                       (!partial_exists && offset != 0)) {
                                state->write_error = "upload offset does not match the saved checkpoint";
                            } else {
                                int flags = O_CREAT | O_WRONLY;
                                if (offset == 0) flags |= O_TRUNC;
                                int fd = open(partial.c_str(), flags, 0666);
                                if (fd < 0 || lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
                                    if (fd >= 0) close(fd);
                                    state->write_error = "could not open resumable upload checkpoint";
                                } else {
                                    state->body_file = partial;
                                    state->preserve_body_file = true;
                                    state->upload_fd = fd;
                                    uint64_t new_size = offset + content_length;
                                    bool completes_file = new_size == total;
                                    bool crosses_checkpoint =
                                        new_size / kUploadSyncCheckpoint > offset / kUploadSyncCheckpoint;
                                    state->sync_on_finalize = completes_file || crosses_checkpoint;
                                }
                            }
                        }
                    }
                } else {
                    state->upload_reservation_path = target;
                    state->upload_reservation_token = std::to_string(reinterpret_cast<uintptr_t>(state));
                    std::string reservation_error;
                    if (!ReserveFileUpload(target, state->upload_reservation_token, false, &reservation_error)) {
                        state->write_error = reservation_error;
                        state->write_error_status = 409;
                        state->upload_reservation_path.clear();
                        state->upload_reservation_token.clear();
                    } else {
                        pattern = std::string(target) + ".pmgr-upload-XXXXXX";
                    }
                }
            } else {
                std::error_code ec;
                std::filesystem::create_directories(route->second.upload_temp_dir, ec);
                pattern = route->second.upload_temp_dir + "/http_upload_XXXXXX";
            }

            if (!pattern.empty()) {
                std::vector<char> writable(pattern.begin(), pattern.end());
                writable.push_back('\0');
                int fd = mkstemp(writable.data());
                if (fd < 0) {
                    state->write_error = "could not create upload staging file";
                } else {
                    state->body_file = writable.data();
                    state->upload_fd = fd;
                }
            }
        } else {
            state->max_body_size = kMaxBodyBytes;
        }

        *con_cls = state;
        return MHD_YES;
    }

    auto *state = static_cast<ConnState *>(*con_cls);
    auto idle_persistent_upload = [state]() {
        if (!state->persistent_upload_reservation || state->upload_reservation_path.empty()) return;
        IdleFileUpload(state->upload_reservation_path, state->upload_reservation_token);
        state->upload_reservation_path.clear();
        state->upload_reservation_token.clear();
    };

    if (state->response_queued) {
        *upload_data_size = 0;
        return MHD_YES;
    }

    if (*upload_data_size != 0) {
        if (state->rejected) {
            *upload_data_size = 0; // already queued a 413 — just drain the rest
            return MHD_YES;
        }

        const uint64_t incoming = static_cast<uint64_t>(*upload_data_size);
        if (incoming > state->max_body_size || state->body_size > state->max_body_size - incoming) {
            LogEvent("Rejected an oversized request body (over the " +
                     std::to_string(state->max_body_size / (1024 * 1024)) + "MB cap) instead of buffering it");
            state->rejected = true;
            state->body.clear();
            state->body.shrink_to_fit();
            if (state->upload_fd >= 0) {
                close(state->upload_fd);
                state->upload_fd = -1;
            }
            *upload_data_size = 0;
            // MHD only reliably flushes a response once it calls us with a
            // zero-sized final chunk. Keep draining, then queue the 413 below.
            return MHD_YES;
        }

        if (state->streamed) {
            if (!state->count_only && !state->discard_upload && state->write_error.empty() &&
                !AppendUpload(state, upload_data, *upload_data_size)) {
                state->write_error = "failed while writing upload staging file";
            }
        } else {
            // Small JSON bodies stay in memory. A failed allocation is still
            // contained to this request rather than escaping MHD's C callback.
            try {
                state->body.append(upload_data, *upload_data_size);
            } catch (const std::bad_alloc &) {
                LogEvent("Dropped a request body after an allocation failure instead of crashing");
                return MHD_NO;
            }
        }
        state->body_size += incoming;
        *upload_data_size = 0;
        return MHD_YES;
    }

    if (state->rejected) {
        state->response_queued = true;
        return RejectTooLarge(conn);
    }

    if (state->upload_fd >= 0) {
        int fd = state->upload_fd;
        if (!FlushUpload(state) && state->write_error.empty()) {
            state->write_error = "failed while writing upload staging file";
        }
        if (state->direct_upload && fchmod(fd, 0777) != 0 && state->write_error.empty() &&
            errno != ENOTSUP && errno != EPERM && errno != EINVAL && errno != EROFS) {
            state->write_error = "failed to set uploaded file permissions";
        }
        // An intermediate chunk that hasn't crossed a checkpoint skips this. fdatasync
        // still flushes the new file size, just not mtime/atime.
        if (state->sync_on_finalize) {
#ifdef PROSPEROMGR_DEBUG_NET
            auto sync_start = std::chrono::steady_clock::now();
#endif
#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
            bool synced = fdatasync(fd) == 0;
#else
            bool synced = fsync(fd) == 0;
#endif
#ifdef PROSPEROMGR_DEBUG_NET
            auto sync_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sync_start).count();
            auto total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - state->received_at).count();
            LogEvent("[net] upload chunk: " + std::to_string(state->body_size) + " bytes, receive+write " +
                     std::to_string(total_ms - sync_ms) + "ms, sync " + std::to_string(sync_ms) + "ms");
#endif
            if (!synced && state->write_error.empty()) {
                state->write_error = "failed to sync upload staging file";
            }
        }
        if (close(fd) != 0 && state->write_error.empty()) {
            state->write_error = "failed to finish upload staging file";
        }
        state->upload_fd = -1;
    }

    if (!state->write_error.empty()) {
        idle_persistent_upload();
        HttpResponse failed = HttpResponse::Error(state->write_error, state->write_error_status);
        MHD_Response *mhd_resp =
            MHD_create_response_from_buffer_copy(failed.body.size(), failed.body.data());
        if (!mhd_resp) return MHD_NO;
        MHD_add_response_header(mhd_resp, MHD_HTTP_HEADER_CONTENT_TYPE, failed.content_type.c_str());
        MHD_add_response_header(mhd_resp, MHD_HTTP_HEADER_CONNECTION, "close");
        MHD_Result ret = MHD_queue_response(conn, static_cast<unsigned int>(state->write_error_status), mhd_resp);
        MHD_destroy_response(mhd_resp);
        state->response_queued = true;
        return ret;
    }

    HttpRequest req;
    req.method = method;
    req.url = url;
    req.body = std::move(state->body);
    req.body_file = state->body_file;
    req.body_size = state->body_size;
    req.received_at = state->received_at;
    if (const char *range = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Range"))
        req.range = range;

    MHD_get_connection_values(
        conn, MHD_GET_ARGUMENT_KIND,
        [](void *cls, MHD_ValueKind, const char *key, const char *value) -> MHD_Result {
            auto *q = static_cast<std::map<std::string, std::string> *>(cls);
            (*q)[key] = value ? value : "";
            return MHD_YES;
        },
        &req.query);

    // Runs on MHD's per-connection thread, where an uncaught handler exception
    // would take down the process rather than just this request.
    HttpResponse resp;
    try {
        resp = self->Dispatch(req);
    } catch (const std::exception &e) {
        LogEvent(std::string("Route handler threw: ") + e.what());
        resp = HttpResponse::Error("internal error", 500);
    } catch (...) {
        LogEvent("Route handler threw a non-std::exception");
        resp = HttpResponse::Error("internal error", 500);
    }
    idle_persistent_upload();
    LogUserAction(req, resp);

    MHD_Response *mhd_resp = nullptr;
    if (resp.body_stream) {
        auto *stream = new (std::nothrow) std::shared_ptr<HttpBodyStream>(std::move(resp.body_stream));
        if (stream) {
            mhd_resp = MHD_create_response_from_callback(resp.body_stream_size, kResponseStreamChunk,
                                                         ReadResponseStream, stream, FreeResponseStream);
            if (!mhd_resp) delete stream;
        }
        if (!mhd_resp) resp = HttpResponse::Error("could not create response stream", 500);
    } else if (!resp.body_file.empty()) {
        // pread(), never MHD_create_response_from_fd: that path hands the fd to
        // sendfile(), which panics this kernel on exFAT/FAT32 and corrupts PFS.
        int fd = open(resp.body_file.c_str(), O_RDONLY);
        struct stat st {};
        if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= 0) {
            // The stream owns the fd from here, including down the failure paths below.
            auto body = std::make_shared<FdBodyStream>(fd);
            fd = -1;
            auto *stream = new (std::nothrow) std::shared_ptr<HttpBodyStream>(std::move(body));
            if (stream) {
                mhd_resp = MHD_create_response_from_callback(static_cast<uint64_t>(st.st_size),
                                                             kResponseStreamChunk, ReadResponseStream, stream,
                                                             FreeResponseStream);
                if (!mhd_resp) delete stream;
            }
            if (mhd_resp && resp.remove_body_file) std::remove(resp.body_file.c_str());
        }
        if (fd >= 0) close(fd);
        if (!mhd_resp) {
            resp = HttpResponse::Error("could not open response file", 500);
        }
    }
    if (!mhd_resp) {
        mhd_resp = MHD_create_response_from_buffer_copy(resp.body.size(), resp.body.data());
    }
    if (!mhd_resp) return MHD_NO;
    MHD_add_response_header(mhd_resp, MHD_HTTP_HEADER_CONTENT_TYPE, resp.content_type.c_str());
    // The PS5 browser holds idle HTTP/1.1 sockets open too long, so one response per
    // socket keeps its small connection pool bounded.
    MHD_add_response_header(mhd_resp, MHD_HTTP_HEADER_CONNECTION, "close");
    for (const auto &[name, value] : resp.extra_headers) {
        MHD_add_response_header(mhd_resp, name.c_str(), value.c_str());
    }
    MHD_Result ret = MHD_queue_response(conn, static_cast<unsigned int>(resp.status), mhd_resp);
    MHD_destroy_response(mhd_resp);
    state->response_queued = true;
    return ret;
}

void HttpServer::OnCompleted(void * /*cls*/, MHD_Connection * /*conn*/, void **con_cls,
                             MHD_RequestTerminationCode /*toe*/) {
    auto *state = static_cast<ConnState *>(*con_cls);
    delete state;
    *con_cls = nullptr;
}

bool HttpServer::StartDaemon() {
    daemon_ = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ITC |
            MHD_USE_NO_LISTEN_SOCKET | MHD_USE_DEBUG | MHD_USE_TURBO,
        0,
        nullptr, nullptr, &HttpServer::OnRequest, this,
        MHD_OPTION_CONNECTION_LIMIT, 24u,
        MHD_OPTION_CONNECTION_TIMEOUT, 60u,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, static_cast<size_t>(8 * 1024 * 1024),
        MHD_OPTION_CONNECTION_MEMORY_INCREMENT, static_cast<size_t>(2 * 1024 * 1024),
        MHD_OPTION_NOTIFY_COMPLETED, &HttpServer::OnCompleted, this,
        MHD_OPTION_END);
    return daemon_ != nullptr;
}

void HttpServer::ListenerLoop() {
    unsigned int retry_attempt = 0;
    bool replacing = false;

    while (!stop_requested_) {
        if (restart_requested_.exchange(false)) {
            if (listen_fd_ >= 0) close(listen_fd_);
            listen_fd_ = -1;
            replacing = true;
        }

        if (listen_fd_ < 0) {
            int effective_rcvbuf = -1;
            listen_fd_ = CreateTunedListenSocket(port_, &effective_rcvbuf);
            if (listen_fd_ < 0) {
                unsigned int delay = ListenerRetryDelayMs(retry_attempt++);
                LogEvent("HTTP listener unavailable (errno " + std::to_string(errno) + ") — retrying in " +
                         std::to_string(delay) + "ms");
                if (!InterruptibleSleep(stop_requested_, delay)) break;
                continue;
            }
            retry_attempt = 0;
            LogEvent("HTTP listener ready on port " + std::to_string(port_) +
                     " (loopback and LAN, SO_RCVBUF=" + std::to_string(effective_rcvbuf) + ")");
            if (replacing) LogEvent("HTTP listener restored");
            replacing = false;
        }

        pollfd event{listen_fd_, POLLIN, 0};
        int ready = poll(&event, 1, 250);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL)) ||
            (ready == 0 && !ListenSocketAlive(listen_fd_))) {
            int error = ready < 0 ? errno : 0;
            close(listen_fd_);
            listen_fd_ = -1;
            replacing = true;
            LogEvent("HTTP listener lost" + (error ? " (errno " + std::to_string(error) + ")" : "") +
                     " — reopening");
            continue;
        }
        if (ready == 0 || !(event.revents & POLLIN)) continue;

        for (;;) {
            sockaddr_storage client_address{};
            socklen_t client_size = sizeof(client_address);
            int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_address), &client_size);
            if (client_fd >= 0) {
                TuneAcceptedSocket(client_fd);
                if (MHD_add_connection(daemon_, client_fd, reinterpret_cast<sockaddr *>(&client_address),
                                       client_size) != MHD_YES) {
                    LogEvent("HTTP connection handoff failed");
                }
                continue;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            int error = errno;
            close(listen_fd_);
            listen_fd_ = -1;
            replacing = true;
            LogEvent("HTTP accept failed (errno " + std::to_string(error) + ") — reopening listener");
            break;
        }
    }

    if (listen_fd_ >= 0) close(listen_fd_);
    listen_fd_ = -1;
}

void HttpServer::WatchdogLoop() {
    int network_ticks = 0;
    int probe_ticks = 0;
    auto last_tick = std::chrono::steady_clock::now();

    while (!stop_requested_) {
        usleep(100000);
        if (stop_requested_) break;

        auto now = std::chrono::steady_clock::now();
        bool suspend_gap = now - last_tick > std::chrono::seconds(3);
        last_tick = now;
        if (suspend_gap) {
            LogEvent("HTTP listener refresh requested after console resume");
            restart_requested_ = true;
            probe_ticks = 0;
            continue;
        }

        if (++network_ticks >= 10) {
            network_ticks = 0;
            std::string current = CurrentIpv4();
            if (current != last_ip_) {
                const char *reason = last_ip_.empty() ? "network available" :
                                     current.empty() ? "offline mode" : "IP address change";
                LogEvent(std::string("HTTP listener refresh requested for ") + reason);
                last_ip_ = std::move(current);
                restart_requested_ = true;
                probe_ticks = 0;
                continue;
            }
        }

        if (++probe_ticks < 50) continue;
        probe_ticks = 0;
        if (ProbeHttp(port_)) continue;
        LogEvent("HTTP listener refresh requested after failed loopback probes");
        restart_requested_ = true;
    }
}

bool HttpServer::Start() {
    stop_requested_ = false;
    restart_requested_ = false;
    last_ip_ = CurrentIpv4();
    if (!StartDaemon()) return false;
    listener_thread_ = std::thread([this] { ListenerLoop(); });
    watchdog_thread_ = std::thread([this] { WatchdogLoop(); });
    return true;
}

void HttpServer::RequestRestart() { restart_requested_ = true; }

void HttpServer::Stop() {
    stop_requested_ = true;
    if (watchdog_thread_.joinable()) watchdog_thread_.join();
    if (listener_thread_.joinable()) listener_thread_.join();
    if (daemon_) {
        MHD_stop_daemon(daemon_);
        daemon_ = nullptr;
    }
}

} // namespace prospero
