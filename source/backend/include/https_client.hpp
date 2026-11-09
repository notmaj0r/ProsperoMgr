#pragma once
// HTTPS GET client, built on sceNet/sceSsl/sceHttp (see ps5_http_bridge.c).

#include <functional>
#include <string>

namespace prospero {

struct HttpsResponse {
    int status = 0;
    std::string body;
};

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
    static bool DownloadToFile(const std::string &url, const std::string &local_path, std::string *err,
                               const DownloadProgressFn &progress = nullptr, NetErrorKind *kind = nullptr,
                               const DownloadCancelFn &cancel = nullptr);
};

// Stops the network helper that older builds left running.
void ResetNetworkHelper();

} // namespace prospero
