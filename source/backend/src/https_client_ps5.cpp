#include "https_client.hpp"

#if defined(PROSPEROMGR_PS5)

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "log.hpp"
#include "process_manager.hpp"
#include "ps5_http_bridge.h"

namespace prospero {
namespace {

constexpr size_t kMaxBufferedResponse = 8 * 1024 * 1024;

// The PS5 resolver/TLS/HTTP context is built/torn down per request (ps5_http_bridge.c) —
// one transfer at a time keeps repository/payload/PKG requests from racing that setup.
std::mutex g_transfer_mu;

std::string RequestHost(const std::string &url) {
    size_t host_start = url.find("://");
    host_start = host_start == std::string::npos ? 0 : host_start + 3;
    size_t path_start = url.find('/', host_start);
    return url.substr(host_start, path_start - host_start);
}

std::string RequestLabel(const std::string &url) {
    std::string host = RequestHost(url);
    std::string leaf;
    size_t leaf_start = url.find_last_of('/');
    if (leaf_start != std::string::npos) {
        leaf = url.substr(leaf_start + 1);
        size_t query = leaf.find('?');
        if (query != std::string::npos) leaf.resize(query);
    }
    return leaf.empty() ? host : host + "/" + leaf;
}

NetErrorKind ClassifyHttpResult(const prosperomgr_http_result &result) {
    switch (result.kind) {
        case PROSPEROMGR_HTTP_OK: return NetErrorKind::kNone;
        case PROSPEROMGR_HTTP_ERR_CONNECT: return NetErrorKind::kConnect;
        case PROSPEROMGR_HTTP_ERR_HTTP: return NetErrorKind::kHttp;
        case PROSPEROMGR_HTTP_ERR_TIMEOUT: return NetErrorKind::kTimeout;
        case PROSPEROMGR_HTTP_ERR_INIT:
        case PROSPEROMGR_HTTP_ERR_SEND:
        case PROSPEROMGR_HTTP_ERR_READ:
        case PROSPEROMGR_HTTP_ERR_WRITE:
        case PROSPEROMGR_HTTP_ERR_CANCELED:
        case PROSPEROMGR_HTTP_ERR_OTHER:
        default: return NetErrorKind::kOther;
    }
}

std::string FriendlyMessage(NetErrorKind kind, const std::string &host, int http_status,
                            const std::string &fallback) {
    switch (kind) {
        case NetErrorKind::kConnect:
            return "Could not reach " + host + ". Check the PS5's network and DNS settings.";
        case NetErrorKind::kTimeout:
            return "Connection to " + host + " timed out.";
        case NetErrorKind::kHttp:
            if (http_status == 404) return host + " returned 404 (not found).";
            if (http_status == 403 || http_status == 429) {
                return host + " temporarily rejected the request (HTTP " + std::to_string(http_status) +
                       ") — possibly rate limited.";
            }
            return host + " returned HTTP " + std::to_string(http_status) + ".";
        case NetErrorKind::kNone: return "";
        case NetErrorKind::kOther:
        default:
            return fallback.empty() ? ("Request to " + host + " failed.") : fallback;
    }
}

void LogResult(const std::string &label, const prosperomgr_http_result &result) {
    if (result.kind == PROSPEROMGR_HTTP_OK) {
        LogEvent("PS5 HTTP complete: " + label + " (HTTP " + std::to_string(result.http_status) + ")");
        return;
    }
    LogEvent("PS5 HTTP failed: " + label + " (kind=" + std::to_string(result.kind) +
             " http_status=" + std::to_string(result.http_status) + " sce_code=" +
             std::to_string(result.sce_code) + ")");
}

} // namespace

bool HttpsClient::Get(const std::string &url, HttpsResponse *out, std::string *err, NetErrorKind *kind,
                      const std::string &bearer_token) {
    if (!out) {
        if (err) *err = "missing response destination";
        return false;
    }
    out->body.clear();
    out->status = 0;
    if (kind) *kind = NetErrorKind::kNone;

    std::string label = RequestLabel(url);
    std::lock_guard<std::mutex> lock(g_transfer_mu);
    LogEvent("PS5 HTTP GET begin: " + label);

    std::string auth_value = bearer_token.empty() ? "" : ("Bearer " + bearer_token);
    prosperomgr_http_result result{};
    char *body = nullptr;
    size_t body_len = 0;
    int rc = prosperomgr_ps5_http_get(url.c_str(), &body, &body_len, kMaxBufferedResponse,
                                      bearer_token.empty() ? nullptr : "Authorization",
                                      bearer_token.empty() ? nullptr : auth_value.c_str(), &result);
    LogResult(label, result);

    out->status = static_cast<int>(result.http_status);

    if (rc != 0) {
        if (body) free(body);
        NetErrorKind detected = ClassifyHttpResult(result);
        if (kind) *kind = detected;
        if (err) *err = FriendlyMessage(detected, RequestHost(url), out->status, "");
        return false;
    }

    if (body) {
        out->body.assign(body, body_len);
        free(body);
    }
    return true;
}

bool HttpsClient::DownloadToFile(const std::string &url, const std::string &local_path,
                                 std::string *err, const DownloadProgressFn &progress, NetErrorKind *kind,
                                 const DownloadCancelFn &cancel) {
    if (kind) *kind = NetErrorKind::kNone;
    std::string label = RequestLabel(url);

    std::lock_guard<std::mutex> lock(g_transfer_mu);
    LogEvent("PS5 HTTP download begin: " + label);

    // Called from the bridge's own read loop, never from inside sceHttp/sceSsl,
    // so touching application state here is safe.
    auto trampoline = [](long long loaded, long long total, void *opaque) {
        auto *fn = static_cast<const DownloadProgressFn *>(opaque);
        if (!fn || !*fn) return;
        try {
            (*fn)(loaded, total);
        } catch (...) {
        }
    };

    auto cancelTrampoline = [](void *opaque) -> int {
        auto *fn = static_cast<const DownloadCancelFn *>(opaque);
        return fn && *fn && (*fn)() ? 1 : 0;
    };

    prosperomgr_http_result result{};
    int rc = prosperomgr_ps5_http_download(url.c_str(), local_path.c_str(), &result,
                                           progress ? +trampoline : nullptr,
                                           progress ? const_cast<DownloadProgressFn *>(&progress) : nullptr,
                                           cancel ? +cancelTrampoline : nullptr,
                                           cancel ? const_cast<DownloadCancelFn *>(&cancel) : nullptr);
    LogResult(label, result);
    if (result.bytes > 0 && result.total_ms > 0) {
        long long kb_s = result.bytes / result.total_ms; // bytes/ms == KB/s
        LogEvent("Transfer: " + label + " " + std::to_string(result.bytes / 1024) + "KB in " +
                 std::to_string(result.total_ms) + "ms (" + std::to_string(kb_s) + " KB/s) net=" +
                 std::to_string(result.read_ms) + "ms disk=" + std::to_string(result.write_ms) + "ms");
    }

    if (rc != 0) {
        NetErrorKind detected = ClassifyHttpResult(result);
        if (kind) *kind = detected;
        if (err) *err = FriendlyMessage(detected, RequestHost(url), static_cast<int>(result.http_status), "");
        return false;
    }
    return true;
}

void ResetNetworkHelper() {
    for (const auto &process : ListProcesses()) {
        if (process.name != "prospero-netd.elf") continue;
        std::string ignored;
        (void)KillProcess(process.pid, &ignored);
    }
}

} // namespace prospero

#endif
