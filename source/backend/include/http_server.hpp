#pragma once
// Thin C++ wrapper around libmicrohttpd. Unmatched GET routes fall through
// to the static handler so the React router owns client-side paths.

#include <microhttpd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "upload_writer.hpp"

namespace prospero {

// Pull-based response body for transfers that must not be staged on disk or buffered in
// payload memory — libmicrohttpd asks for the next block as the browser is ready.
class HttpBodyStream {
  public:
    virtual ~HttpBodyStream() = default;
    virtual ssize_t Read(uint64_t position, char *buffer, size_t max_bytes) = 0;
};

struct HttpRequest {
    std::string method;
    std::string url;
    std::string body; // fully buffered request body (routes in this API are small-bodied)
    // Large upload routes stream into this temp file instead of `body`. Valid for the
    // route handler's duration; removed automatically afterward unless the handler moves it.
    std::string body_file;
    uint64_t body_size = 0;
    std::map<std::string, std::string> query; // ?key=value GET arguments
    std::string range;                        // Range header, for media players that seek
    // Set when the connection was accepted, before any body bytes arrived — (now -
    // received_at) spans the receive phase. Only used by net-sink debug routes.
    std::chrono::steady_clock::time_point received_at;

    std::string Query(const std::string &key, const std::string &fallback = "") const {
        auto it = query.find(key);
        return it == query.end() ? fallback : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::map<std::string, std::string> extra_headers; // e.g. Content-Disposition for downloads
    // Disk-backed responses avoid loading large downloads into payload RAM. When
    // remove_body_file is true, the path is unlinked after MHD opens it (fd stays valid).
    std::string body_file;
    bool remove_body_file = false;
    std::shared_ptr<HttpBodyStream> body_stream;
    uint64_t body_stream_size = MHD_SIZE_UNKNOWN;

    static HttpResponse Json(const std::string &json_body, int status = 200) {
        return HttpResponse{status, "application/json", json_body};
    }
    static HttpResponse Error(const std::string &message, int status = 400) {
        return HttpResponse{status, "application/json", "{\"error\":\"" + message + "\"}"};
    }

    // `code` is the machine-readable companion to `message`, so the UI can show a
    // translated string instead of the English one.
    static HttpResponse CodedError(const std::string &code, const std::string &message, int status = 400) {
        return HttpResponse{status, "application/json",
                            "{\"error\":\"" + message + "\",\"code\":\"" + code + "\"}"};
    }
};

using RouteHandler = std::function<HttpResponse(const HttpRequest &)>;
using StaticHandler = std::function<HttpResponse(const std::string &url)>;

class HttpServer {
  public:
    explicit HttpServer(uint16_t port);
    ~HttpServer();

    void Route(const std::string &method, const std::string &path, RouteHandler handler);

    // Raw-body upload route streamed to a temp file under `temp_dir` — the only safe
    // shape for PKGs and large file-manager uploads on this payload's limited address space.
    void UploadRoute(const std::string &method, const std::string &path, const std::string &temp_dir,
                     uint64_t max_bytes, RouteHandler handler);

    // Streams into a unique sibling of the destination path in `query_key`; the handler
    // renames it into place after the last byte. Avoids a second copy for huge uploads.
    void DirectUploadRoute(const std::string &method, const std::string &path, const std::string &query_key,
                           uint64_t max_bytes, RouteHandler handler);

    // Resumable uploads append directly to <target>.pmgr-part-<upload_id> at the declared
    // offset — no second staging copy, so large PS5 transfers keep full disk throughput.
    void AppendUploadRoute(const std::string &method, const std::string &path,
                           const std::string &target_query, const std::string &upload_id_query,
                           const std::string &offset_query, const std::string &total_query,
                           uint64_t max_bytes, RouteHandler handler);

    // Debug-only route (PROSPEROMGR_DEBUG_NET): counts body bytes without writing them,
    // so upload throughput can be measured with the filesystem/sync path excluded.
    void CountingUploadRoute(const std::string &method, const std::string &path, uint64_t max_bytes,
                             RouteHandler handler);

    // Called for any GET that doesn't match a registered route — used to
    // serve the embedded React app and its static assets.
    void SetStaticFallback(StaticHandler handler);

    // Starts libmicrohttpd on the configured port. A watchdog recreates the
    // complete daemon after standby or an IP/network transition.
    bool Start();
    void Stop();
    void RequestRestart();

  private:
    struct ConnState {
        ~ConnState();

        std::string body;
        std::string body_file;
        int upload_fd = -1;
        std::unique_ptr<UploadWriter> upload_writer; // created with the first bytes of a streamed upload
        uint64_t body_size = 0;
        uint64_t max_body_size = 0;
        bool streamed = false;
        bool direct_upload = false;
        bool preserve_body_file = false;
        bool in_place_upload = false; // body_file is the final destination, not a staging file
        bool discard_upload = false;
        bool count_only = false; // CountingUploadRoute — bytes are counted, never written anywhere
        std::chrono::steady_clock::time_point received_at = std::chrono::steady_clock::now();
        bool rejected = false; // true once a 413 has been queued for this connection
        bool response_queued = false;
        std::string write_error;
        int write_error_status = 507;
        std::string upload_reservation_path;
        std::string upload_reservation_token;
        bool persistent_upload_reservation = false;

        // A non-final chunk before a checkpoint skips the finalize fsync: visible, not yet
        // durable. Every route except resumable append keeps always-sync.
        bool sync_on_finalize = true;
    };

    static bool AppendUpload(ConnState *state, const char *data, size_t size);
    static bool FlushUpload(ConnState *state);

    struct RegisteredRoute {
        RouteHandler handler;
        std::string upload_temp_dir;
        std::string upload_target_query;
        std::string upload_id_query;
        std::string upload_offset_query;
        std::string upload_total_query;
        uint64_t max_upload_bytes = 0;
        bool upload_count_only = false;
    };

    HttpResponse Dispatch(const HttpRequest &req) const;

    static MHD_Result OnRequest(void *cls, MHD_Connection *conn, const char *url, const char *method,
                                const char *version, const char *upload_data, size_t *upload_data_size,
                                void **con_cls);
    static MHD_Result OnRequestImpl(void *cls, MHD_Connection *conn, const char *url, const char *method,
                                    const char *version, const char *upload_data, size_t *upload_data_size,
                                    void **con_cls);
    static void OnCompleted(void *cls, MHD_Connection *conn, void **con_cls,
                            MHD_RequestTerminationCode toe);

    bool StartDaemon();
    void ListenerLoop();
    void WatchdogLoop();

    uint16_t port_;

    std::thread listener_thread_;
    std::thread watchdog_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> restart_requested_{false};

    MHD_Daemon *daemon_ = nullptr;
    int listen_fd_ = -1;
    std::string last_ip_;

    std::map<std::string, RegisteredRoute> routes_; // key: "METHOD path"
    StaticHandler static_handler_;
};

} // namespace prospero
