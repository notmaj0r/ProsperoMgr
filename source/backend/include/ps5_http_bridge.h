#pragma once
// Native PS5 HTTPS transport (sceNet/sceSsl/sceHttp), used for both metadata fetches and
// payload downloads. Nothing here calls back into app code from inside sceHttp/sceSsl internals.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which lifecycle stage was reached when a fatal signal lands.
int prosperomgr_ps5_http_stage(void);

// Coarse, honest classification — SCE error codes are an undocumented, proprietary taxonomy
// in this SDK, so this is derived from *which stage* failed, not decoded from the numeric result.
enum prosperomgr_http_error_kind {
    PROSPEROMGR_HTTP_OK = 0,
    PROSPEROMGR_HTTP_ERR_INIT,     // sceNet/sceSsl/sceHttp/template/connection setup failed
    PROSPEROMGR_HTTP_ERR_CONNECT,  // could not reach/negotiate with the host (covers DNS + TCP + TLS —
                                    // sceHttp does all three inside one call, so they aren't distinguishable here)
    PROSPEROMGR_HTTP_ERR_SEND,     // request could not be sent
    PROSPEROMGR_HTTP_ERR_HTTP,     // got a response, but a non-2xx status (see http_status)
    PROSPEROMGR_HTTP_ERR_READ,     // response body read failed mid-transfer
    PROSPEROMGR_HTTP_ERR_WRITE,    // local disk write failed
    PROSPEROMGR_HTTP_ERR_TIMEOUT,  // stall guard tripped
    PROSPEROMGR_HTTP_ERR_CANCELED,
    PROSPEROMGR_HTTP_ERR_OTHER,
};

typedef void (*prosperomgr_ps5_http_progress_fn)(long long loaded, long long total, void *opaque);
// Return non-zero to abort the transfer cleanly (used for cancellation).
typedef int (*prosperomgr_ps5_http_cancel_fn)(void *opaque);

struct prosperomgr_http_result {
    int kind;         // prosperomgr_http_error_kind
    long http_status;  // 0 if no response was ever received
    long long sce_code; // raw SCE return code, for logs only — never shown to the user
    // Download timings, so a slow transfer can be blamed on the network or the
    // disk instead of guessed at. Zero for non-download requests.
    long long bytes;
    long long total_ms; // wall time of the read/write loop, setup excluded
    long long read_ms;  // time spent inside sceHttpReadData
    long long write_ms; // time spent inside write()
};

// Buffers the whole body in memory (up to max_body_bytes), so only use it for small responses.
// *out_body is malloc'd and the caller frees it; header_name/value add one extra header, or NULL.
int prosperomgr_ps5_http_get(const char *url, char **out_body, size_t *out_len, size_t max_body_bytes,
                             const char *header_name, const char *header_value,
                             struct prosperomgr_http_result *result);

// Content-Length of `url` without reading the body. Fails when the server sends no
// length, which also means it can't be read in ranges.
int prosperomgr_ps5_http_size(const char *url, long long *out_size,
                              struct prosperomgr_http_result *result);

// Reads one byte range into `buf`. `out_len` is what actually arrived, which can be
// short if the server ignored the Range header.
int prosperomgr_ps5_http_range(const char *url, long long start, long long end, char *buf, size_t buf_size,
                               size_t *out_len, struct prosperomgr_http_result *result);

// Streams the response body straight to `out_path` in fixed chunks — never fully in RAM.
// `progress` (if non-NULL) fires roughly every 80ms plus once at the end; `cancel` is polled the same way.
int prosperomgr_ps5_http_download(const char *url, const char *out_path, struct prosperomgr_http_result *result,
                                  prosperomgr_ps5_http_progress_fn progress, void *progress_opaque,
                                  prosperomgr_ps5_http_cancel_fn cancel, void *cancel_opaque);

#ifdef __cplusplus
}
#endif
