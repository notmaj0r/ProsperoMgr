#pragma once
// HTTPS GET client, built on sceNet/sceSsl/sceHttp (see ps5_http_bridge.c).

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace prospero {

struct HttpsResponse {
    int status = 0;
    std::string body;
};

// Answer to HttpsClient::Request. Any HTTP status counts as an answer — the caller decides what a
// 4xx or 308 means — so `status` is what to branch on.
struct HttpsReply {
    int status = 0;
    std::string body;
    std::string location; // Location response header, "" when absent
    std::string range;    // Range response header, "" when absent (a resumable upload reports progress here)
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// loaded/total in bytes — total is -1 if the server never sent a length
// (chunked transfer or no Content-Length header at all).
using DownloadProgressFn = std::function<void(long long loaded, long long total)>;
using DownloadCancelFn = std::function<bool()>;

// Coarse classification, enough to give the user a real reason instead of one
// generic string. sceHttp reports DNS, TCP and TLS failures as one kind.
enum class NetErrorKind {
    kNone,     // request succeeded
    kConnect,  // could not resolve, connect or negotiate TLS
    kTimeout,  // connected but stalled/timed out
    kHttp,     // got a response, but a non-2xx status
    kOther,    // any other transport failure
};

class HttpsClient {
  public:
    // Buffers the whole body in memory — fine for repo index JSON. `out->status` is
    // set even when this fails, so callers can tell an HTTP 404 from a dead transport.
    static bool Get(const std::string &url, HttpsResponse *out, std::string *err, NetErrorKind *kind = nullptr,
                    const std::string &bearer_token = "");

    // Streams the body to a local file with a stall timeout, unlike Get() —
    // always prefer this over buffering a download in memory.
    // `bearer_token` (when set) is sent as an Authorization header on the first request only.
    static bool DownloadToFile(const std::string &url, const std::string &local_path, std::string *err,
                               const DownloadProgressFn &progress = nullptr, NetErrorKind *kind = nullptr,
                               const DownloadCancelFn &cancel = nullptr, const std::string &bearer_token = "");

    // HTTPS only. `method` is GET, POST, PUT or DELETE. The body is held in memory, so send large
    // payloads in pieces. Returns false only when no HTTP answer arrived at all (`err` says why);
    // a 4xx/5xx answer returns true with the status in `out->status`. Never follows redirects.
    static bool Request(const std::string &method, const std::string &url, const HttpHeaders &headers,
                        const void *body, size_t body_len, HttpsReply *out, std::string *err,
                        NetErrorKind *kind = nullptr);
};

// Stops the network helper that older builds left running.
void ResetNetworkHelper();

} // namespace prospero
