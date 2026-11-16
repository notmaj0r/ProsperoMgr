// Native PS5 HTTPS transport — sceNet + sceSsl + sceHttp.
// Warning: the SSL callback accepts unconditionally — certs aren't verified, traffic is still encrypted.

#if defined(PROSPEROMGR_PS5)

#include "ps5_http_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

// No official headers ship these in this SDK — verified against the real exported symbols
// in libSceNet.so / libSceSsl.so / libSceHttp.so via `nm -D` before writing this file.
extern int sceNetInit(void);
extern int sceNetPoolCreate(const char *name, int size, int flags);
extern int sceNetPoolDestroy(int poolId);

extern int sceSslInit(size_t poolSize);
extern int sceSslTerm(int sslCtxId);

extern int sceHttpInit(int netPoolId, int sslCtxId, size_t poolSize);
extern int sceHttpTerm(int httpCtxId);
extern int sceHttpCreateTemplate(int httpCtxId, const char *userAgent, int httpVer, int autoProxyConf);
extern int sceHttpDeleteTemplate(int tmplId);
extern int sceHttpSetResponseHeaderMaxSize(int id, size_t headerSize);
extern int sceHttpSetAutoRedirect(int id, int enable);
extern int sceHttpsSetSslCallback(int id, void *callback, void *userArg);

extern int sceHttpCreateConnectionWithURL(int tmplId, const char *url, int enableKeepAlive);
extern int sceHttpDeleteConnection(int connId);

extern int sceHttpSetConnectTimeOut(int id, unsigned int usec);
extern int sceHttpSetResolveTimeOut(int id, unsigned int usec);
extern int sceHttpSetSendTimeOut(int id, unsigned int usec);
extern int sceHttpSetRecvTimeOut(int id, unsigned int usec);
extern int sceHttpGetAllResponseHeaders(int reqId, char **header, size_t *headerSize);

extern int sceHttpCreateRequestWithURL(int connId, int method, const char *url, uint64_t contentLength);
extern int sceHttpAddRequestHeader(int reqId, const char *name, const char *value, unsigned int mode);
extern int sceHttpSendRequest(int reqId, const void *postData, size_t size);
extern int sceHttpGetStatusCode(int reqId, int *statusCode);
extern int sceHttpGetResponseContentLength(int reqId, int *contentLengthType, uint64_t *contentLength);
extern int sceHttpReadData(int reqId, void *data, size_t size);
extern int sceHttpDeleteRequest(int reqId);

#define HTTP_METHOD_GET 0
#define HTTP_TIMEOUT_US (20u * 1000u * 1000u)
#define HTTP_MAX_REDIRECTS 5

static volatile sig_atomic_t g_stage = 0;

int prosperomgr_ps5_http_stage(void) { return (int)g_stage; }

// Unconditional accept — see the file-level comment for what that
// trade-off means. Not a silent addition; called out explicitly there.
static int http_ssl_cb(void) { return 0; }

// ---- DNS: sceHttp's own resolver can fail outright on some consoles, so this resolves
// hostnames itself — console's resolver first (honors nanoDNS/overrides), public DNS as fallback.

#define DNS_CACHE_MAX 16
typedef struct { char host[128]; char ip[64]; } dns_cache_entry_t;
static dns_cache_entry_t g_dns_cache[DNS_CACHE_MAX];
static int g_dns_cache_n = 0; // no lock — callers are serialized by g_transfer_mu

// Skips a DNS name, handling label-compression pointers. Returns the new offset.
static size_t dns_skip_name(const uint8_t *buf, size_t len, size_t pos) {
    while (pos < len) {
        uint8_t b = buf[pos];
        if (b == 0) return pos + 1;               // root label
        if ((b & 0xC0) == 0xC0) return pos + 2;    // compression pointer
        pos += 1 + (size_t)b;
    }
    return len;
}

static int build_dns_query(const char *hostname, uint8_t *buf, size_t buf_sz, uint16_t id) {
    if (buf_sz < 18) return -1;
    size_t pos = 0;

    buf[pos++] = (uint8_t)(id >> 8);
    buf[pos++] = (uint8_t)(id & 0xFF);
    buf[pos++] = 0x01; buf[pos++] = 0x00; // flags: RD=1
    buf[pos++] = 0x00; buf[pos++] = 0x01; // QDCOUNT=1
    buf[pos++] = 0x00; buf[pos++] = 0x00; // ANCOUNT=0
    buf[pos++] = 0x00; buf[pos++] = 0x00; // NSCOUNT=0
    buf[pos++] = 0x00; buf[pos++] = 0x00; // ARCOUNT=0

    const char *p = hostname;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        size_t label_len = (size_t)(dot - p);
        if (label_len == 0 || label_len > 63 || pos + label_len + 2 >= buf_sz) return -1;
        buf[pos++] = (uint8_t)label_len;
        memcpy(buf + pos, p, label_len);
        pos += label_len;
        p = *dot ? dot + 1 : dot;
    }
    buf[pos++] = 0x00;                    // root label
    buf[pos++] = 0x00; buf[pos++] = 0x01; // QTYPE=A
    buf[pos++] = 0x00; buf[pos++] = 0x01; // QCLASS=IN
    return (int)pos;
}

static int parse_dns_response(const uint8_t *buf, size_t len, uint16_t id, char *ip_out, size_t ip_out_sz) {
    if (len < 12) return -1;
    if ((((uint16_t)buf[0] << 8) | buf[1]) != id) return -1;
    if (!(buf[2] & 0x80)) return -1; // QR bit not set — not a response

    uint16_t ancount = (uint16_t)(((uint16_t)buf[6] << 8) | buf[7]);
    if (ancount == 0) return -1;

    size_t pos = 12;
    pos = dns_skip_name(buf, len, pos);
    pos += 4; // QTYPE + QCLASS

    for (uint16_t i = 0; i < ancount && pos < len; i++) {
        pos = dns_skip_name(buf, len, pos);
        if (pos + 10 > len) return -1;
        uint16_t type = (uint16_t)(((uint16_t)buf[pos] << 8) | buf[pos + 1]);
        uint16_t rdlength = (uint16_t)(((uint16_t)buf[pos + 8] << 8) | buf[pos + 9]);
        pos += 10; // type(2) + class(2) + ttl(4) + rdlength(2)
        if (type == 1 && rdlength == 4 && pos + 4 <= len) {
            snprintf(ip_out, ip_out_sz, "%u.%u.%u.%u", buf[pos], buf[pos + 1], buf[pos + 2], buf[pos + 3]);
            return 0;
        }
        pos += rdlength;
    }
    return -1;
}

// Resolves through the console's own configured resolver (sceNetResolver, via getaddrinfo) —
// whatever that's pointed at, including a local override like nanoDNS. Returns 0 on success.
static int system_dns_resolve(const char *hostname, char *ip_out, size_t ip_out_sz) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || !res) return -1;

    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    const char *ok = inet_ntop(AF_INET, &addr->sin_addr, ip_out, ip_out_sz);
    freeaddrinfo(res);
    return ok ? 0 : -1;
}

// Resolves via raw UDP to 8.8.8.8 then 1.1.1.1, bypassing the PS5's system DNS. Returns 0 on
// success (ip_out filled), -1 on failure — caller falls back to the original hostname.
static int public_dns_resolve(const char *hostname, char *ip_out, size_t ip_out_sz) {
    static const char *dns_servers[] = { "8.8.8.8", "1.1.1.1", NULL };

    uint16_t qid = (uint16_t)arc4random();
    uint8_t query[512], resp[4096];
    int qlen = build_dns_query(hostname, query, sizeof(query), qid);
    if (qlen < 0) return -1;

    for (int si = 0; dns_servers[si]; si++) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) continue;

        struct timeval tv = { 4, 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in dns_addr;
        memset(&dns_addr, 0, sizeof(dns_addr));
        dns_addr.sin_family = AF_INET;
        dns_addr.sin_port = htons(53);
        dns_addr.sin_addr.s_addr = inet_addr(dns_servers[si]);

        // connect() makes the kernel drop any reply not actually from this
        // server/port instead of accepting a spoofed datagram from anyone.
        if (connect(sock, (struct sockaddr *)&dns_addr, sizeof(dns_addr)) != 0) {
            close(sock);
            continue;
        }

        ssize_t sent = send(sock, query, (size_t)qlen, 0);
        if (sent != qlen) { close(sock); continue; }

        ssize_t n = recv(sock, resp, sizeof(resp), 0);
        close(sock);
        if (n < 12) continue;

        if (parse_dns_response(resp, (size_t)n, qid, ip_out, ip_out_sz) == 0) return 0;
    }
    return -1;
}

// Tries the console's resolver first (honors a local override), then public DNS as fallback.
// Returns 0 on success (ip_out filled), -1 on failure — caller then uses the original hostname.
static int ps5_dns_resolve(const char *hostname, char *ip_out, size_t ip_out_sz) {
    if (!hostname || !hostname[0] || !ip_out || ip_out_sz < 8) return -1;

    int is_ip = 1;
    for (const char *p = hostname; *p; p++) {
        if (*p != '.' && (*p < '0' || *p > '9')) { is_ip = 0; break; }
    }
    if (is_ip) { snprintf(ip_out, ip_out_sz, "%s", hostname); return 0; }

    for (int i = 0; i < g_dns_cache_n; i++) {
        if (strcmp(g_dns_cache[i].host, hostname) == 0) {
            snprintf(ip_out, ip_out_sz, "%s", g_dns_cache[i].ip);
            return 0;
        }
    }

    if (system_dns_resolve(hostname, ip_out, ip_out_sz) != 0 &&
        public_dns_resolve(hostname, ip_out, ip_out_sz) != 0) {
        return -1;
    }

    if (g_dns_cache_n < DNS_CACHE_MAX) {
        snprintf(g_dns_cache[g_dns_cache_n].host, sizeof(g_dns_cache[0].host), "%s", hostname);
        snprintf(g_dns_cache[g_dns_cache_n].ip, sizeof(g_dns_cache[0].ip), "%s", ip_out);
        g_dns_cache_n++;
    }
    return 0;
}

// Substitutes a resolved IP into `url`'s host and fills `orig_host` with the
// real hostname (empty if no substitution happened), for the Host header.
static const char *bypass_dns_ex(const char *url, char *resolved_url, size_t resolved_sz, char *orig_host,
                                 size_t orig_host_sz, int public_dns_only, char *ip_out, size_t ip_out_sz) {
    orig_host[0] = '\0';
    if (ip_out && ip_out_sz) ip_out[0] = '\0';

    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return url;
    scheme_end += 3;

    const char *host_end = scheme_end;
    while (*host_end && *host_end != '/' && *host_end != ':' && *host_end != '?') host_end++;
    size_t host_len = (size_t)(host_end - scheme_end);
    if (host_len == 0 || host_len >= orig_host_sz) return url;

    memcpy(orig_host, scheme_end, host_len);
    orig_host[host_len] = '\0';

    int already_ip = 1;
    for (size_t i = 0; i < host_len; i++) {
        char c = orig_host[i];
        if (c != '.' && (c < '0' || c > '9')) { already_ip = 0; break; }
    }
    if (already_ip) { orig_host[0] = '\0'; return url; }

    char ip[64];
    int resolved = public_dns_only ? public_dns_resolve(orig_host, ip, sizeof(ip))
                                   : ps5_dns_resolve(orig_host, ip, sizeof(ip));
    if (resolved != 0 || !ip[0]) {
        orig_host[0] = '\0';
        return url;
    }
    if (ip_out && ip_out_sz) snprintf(ip_out, ip_out_sz, "%s", ip);

    size_t scheme_len = (size_t)(scheme_end - url);
    snprintf(resolved_url, resolved_sz, "%.*s%s%s", (int)scheme_len, url, ip, host_end);
    return resolved_url;
}

static const char *bypass_dns(const char *url, char *resolved_url, size_t resolved_sz, char *orig_host,
                              size_t orig_host_sz) {
    return bypass_dns_ex(url, resolved_url, resolved_sz, orig_host, orig_host_sz, 0, NULL, 0);
}

// ---- request context, torn down the same way on every exit path ----------

typedef struct {
    int net_pool;
    int ssl_ctx;
    int http_ctx;
    int tmpl;
    int conn;
    int req;
} http_ctx_t;

static void http_ctx_init(http_ctx_t *ctx) {
    ctx->net_pool = -1;
    ctx->ssl_ctx = -1;
    ctx->http_ctx = -1;
    ctx->tmpl = -1;
    ctx->conn = -1;
    ctx->req = -1;
}

// Teardown order (request, connection, template, http, ssl, net pool) is the one actually
// exercised on hardware. sceNetInit() has no matching sceNetTerm() call here, on purpose.
static void http_ctx_close(http_ctx_t *ctx) {
    g_stage = 11;
    if (ctx->req >= 0) sceHttpDeleteRequest(ctx->req);
    if (ctx->conn >= 0) sceHttpDeleteConnection(ctx->conn);
    if (ctx->tmpl >= 0) sceHttpDeleteTemplate(ctx->tmpl);
    if (ctx->http_ctx >= 0) sceHttpTerm(ctx->http_ctx);
    if (ctx->ssl_ctx >= 0) sceSslTerm(ctx->ssl_ctx);
    if (ctx->net_pool >= 0) sceNetPoolDestroy(ctx->net_pool);
    g_stage = 0;
}

static void set_result(struct prosperomgr_http_result *result, int kind, long status, long long code) {
    if (!result) return;
    result->kind = kind;
    result->http_status = status;
    result->sce_code = code;
}

// Deletes just the request/connection pair, so the same net/ssl/http/template
// can be reused for the next redirect hop.
static void http_close_request(http_ctx_t *ctx) {
    if (ctx->req >= 0) sceHttpDeleteRequest(ctx->req);
    if (ctx->conn >= 0) sceHttpDeleteConnection(ctx->conn);
    ctx->req = -1;
    ctx->conn = -1;
}

// Applied at all three levels because some sceHttp builds only honor it on one — an unbounded
// wait here wedges every other transfer behind https_client_ps5.cpp's g_transfer_mu.
static void http_set_timeouts(int id) {
    sceHttpSetResolveTimeOut(id, HTTP_TIMEOUT_US);
    sceHttpSetConnectTimeOut(id, HTTP_TIMEOUT_US);
    sceHttpSetSendTimeOut(id, HTTP_TIMEOUT_US);
    sceHttpSetRecvTimeOut(id, HTTP_TIMEOUT_US);
}

// Copies the response's Location header into `out`, resolving a root-relative
// value (Gitea sends those) against `base_url`. Returns 0 when one was found.
static int http_read_location(int req, const char *base_url, char *out, size_t out_sz) {
    char *headers = NULL;
    size_t headers_len = 0;
    if (sceHttpGetAllResponseHeaders(req, &headers, &headers_len) != 0 || !headers) return -1;

    const char *end = headers + headers_len;
    for (const char *line = headers; line < end;) {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        size_t line_len = eol ? (size_t)(eol - line) : (size_t)(end - line);
        while (line_len && (line[line_len - 1] == '\r' || line[line_len - 1] == ' ')) line_len--;

        if (line_len > 9 && strncasecmp(line, "location:", 9) == 0) {
            const char *value = line + 9;
            size_t value_len = line_len - 9;
            while (value_len && (*value == ' ' || *value == '\t')) { value++; value_len--; }
            if (value_len == 0 || value_len >= out_sz) return -1;

            if (value[0] != '/') {
                snprintf(out, out_sz, "%.*s", (int)value_len, value);
                return 0;
            }
            const char *scheme_end = strstr(base_url, "://");
            if (!scheme_end) return -1;
            const char *host_end = scheme_end + 3;
            while (*host_end && *host_end != '/') host_end++;
            size_t prefix_len = (size_t)(host_end - base_url);
            if (prefix_len + value_len >= out_sz) return -1;
            snprintf(out, out_sz, "%.*s%.*s", (int)prefix_len, base_url, (int)value_len, value);
            return 0;
        }

        if (!eol) break;
        line = eol + 1;
    }
    return -1;
}

// Opens one hop and reads back its status. `bypass` connects by raw IP with
// an explicit Host header, for a console whose system resolver is hijacked.
static int http_send_hop(http_ctx_t *ctx, const char *url, int bypass, int first_hop,
                         const char *header_name, const char *header_value, int *status_out,
                         struct prosperomgr_http_result *result) {
    long long rc;
    char resolved_url[1024];
    char orig_host[256];
    const char *connect_url = url;

    orig_host[0] = '\0';
    if (bypass) {
        connect_url = bypass_dns(url, resolved_url, sizeof(resolved_url), orig_host, sizeof(orig_host));
        if (!orig_host[0]) return -1; // nothing resolved, so this retry adds nothing
    }

    g_stage = 4;
    if ((ctx->conn = sceHttpCreateConnectionWithURL(ctx->tmpl, connect_url, 0)) < 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_CONNECT, 0, ctx->conn);
        return -1;
    }
    http_set_timeouts(ctx->conn);

    g_stage = 5;
    if ((ctx->req = sceHttpCreateRequestWithURL(ctx->conn, HTTP_METHOD_GET, connect_url, 0)) < 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_CONNECT, 0, ctx->req);
        return -1;
    }
    http_set_timeouts(ctx->req);
    if (orig_host[0]) sceHttpAddRequestHeader(ctx->req, "Host", orig_host, 0);
    // First hop only: an api.github.com token must not follow a redirect onto
    // the release-asset CDN.
    if (first_hop && header_name && header_name[0] && header_value) {
        sceHttpAddRequestHeader(ctx->req, header_name, header_value, 0);
    }

    g_stage = 6;
    if ((rc = sceHttpSendRequest(ctx->req, NULL, 0)) != 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_CONNECT, 0, rc);
        return -1;
    }

    g_stage = 7;
    if ((rc = sceHttpGetStatusCode(ctx->req, status_out)) != 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_CONNECT, 0, rc);
        return -1;
    }
    return 0;
}

// Brings up net pool, SSL, HTTP context and the shared template — everything before the first
// connection. Returns 0 on success; on failure the caller must still call http_ctx_close().
static int http_setup(http_ctx_t *ctx, struct prosperomgr_http_result *result) {
    long long rc;

    g_stage = 1;
    if ((rc = sceNetInit()) < 0) { set_result(result, PROSPEROMGR_HTTP_ERR_INIT, 0, rc); return -1; }
    if ((ctx->net_pool = sceNetPoolCreate("prosperomgr_http", 512 * 1024, 0)) < 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_INIT, 0, ctx->net_pool);
        return -1;
    }

    g_stage = 2;
    if ((ctx->ssl_ctx = sceSslInit(384 * 1024)) < 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_INIT, 0, ctx->ssl_ctx);
        return -1;
    }

    g_stage = 3;
    if ((ctx->http_ctx = sceHttpInit(ctx->net_pool, ctx->ssl_ctx, 256 * 1024)) < 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_INIT, 0, ctx->http_ctx);
        return -1;
    }
    if ((ctx->tmpl = sceHttpCreateTemplate(ctx->http_ctx, "ProsperoMgr/1.0", 2, 1)) < 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_INIT, 0, ctx->tmpl);
        return -1;
    }
    sceHttpSetResponseHeaderMaxSize(ctx->tmpl, 0x4000);
    // Off on purpose: sceHttp's own auto-redirect would resolve the Location
    // host through the system resolver — the exact thing bypass_dns() avoids.
    sceHttpSetAutoRedirect(ctx->tmpl, 0);
    http_set_timeouts(ctx->tmpl);
    if ((rc = sceHttpsSetSslCallback(ctx->tmpl, (void *)http_ssl_cb, 0)) != 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_INIT, 0, rc);
        return -1;
    }
    return 0;
}

// Builds the full request pipeline, following any redirects itself. Returns
// 0 on success; on failure the caller must still call http_ctx_close().
static int http_open(http_ctx_t *ctx, const char *url, const char *header_name, const char *header_value,
                     struct prosperomgr_http_result *result) {
    if (http_setup(ctx, result) != 0) return -1;

    char current[1024];
    snprintf(current, sizeof(current), "%s", url);

    for (int hop = 0;; hop++) {
        int plain = strncmp(current, "http://", 7) == 0;
        if (!plain && strncmp(current, "https://", 8) != 0) {
            set_result(result, PROSPEROMGR_HTTP_ERR_INIT, 0, 0);
            return -1;
        }

        // Resolver bypass first, hostname as the retry — a bare-IP connection
        // has no SNI, which some CDNs refuse.
        int status = 0;
        if (http_send_hop(ctx, current, 1, hop == 0, header_name, header_value, &status, result) != 0) {
            http_close_request(ctx);
            if (http_send_hop(ctx, current, 0, hop == 0, header_name, header_value, &status, result) != 0) {
                return -1;
            }
        }

        if (status >= 300 && status < 400 && hop < HTTP_MAX_REDIRECTS) {
            char next[1024];
            if (http_read_location(ctx->req, current, next, sizeof(next)) == 0) {
                snprintf(current, sizeof(current), "%s", next);
                http_close_request(ctx);
                continue;
            }
        }
        if (status < 200 || status >= 300) {
            set_result(result, PROSPEROMGR_HTTP_ERR_HTTP, status, 0);
            return -1;
        }

        set_result(result, PROSPEROMGR_HTTP_OK, status, 0);
        return 0;
    }
}

static long long monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int prosperomgr_ps5_http_size(const char *url, long long *out_size,
                              struct prosperomgr_http_result *result) {
    http_ctx_t ctx;
    http_ctx_init(&ctx);
    if (out_size) *out_size = -1;

    if (http_open(&ctx, url, NULL, NULL, result) != 0) {
        http_ctx_close(&ctx);
        return -1;
    }
    int cl_type = 1;
    uint64_t content_length = 0;
    int have = sceHttpGetResponseContentLength(ctx.req, &cl_type, &content_length) == 0 && cl_type == 0;
    http_ctx_close(&ctx);

    // No length means no random access, so the caller has to stage the file instead.
    if (!have || content_length == 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_HTTP, result ? result->http_status : 0, 0);
        return -1;
    }
    if (out_size) *out_size = (long long)content_length;
    return 0;
}

int prosperomgr_ps5_http_range(const char *url, long long start, long long end, char *buf, size_t buf_size,
                               size_t *out_len, struct prosperomgr_http_result *result) {
    char range[64];
    snprintf(range, sizeof(range), "bytes=%lld-%lld", start, end);

    http_ctx_t ctx;
    http_ctx_init(&ctx);
    if (out_len) *out_len = 0;

    if (http_open(&ctx, url, "Range", range, result) != 0) {
        http_ctx_close(&ctx);
        return -1;
    }

    size_t off = 0;
    int ok = 1;
    while (off < buf_size) {
        int n = sceHttpReadData(ctx.req, buf + off, buf_size - off);
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;
        off += (size_t)n;
    }
    http_ctx_close(&ctx);

    if (!ok) {
        set_result(result, PROSPEROMGR_HTTP_ERR_READ, result ? result->http_status : 0, 0);
        return -1;
    }
    if (out_len) *out_len = off;
    return 0;
}

int prosperomgr_ps5_http_get(const char *url, char **out_body, size_t *out_len, size_t max_body_bytes,
                             const char *header_name, const char *header_value,
                             struct prosperomgr_http_result *result) {
    http_ctx_t ctx;
    http_ctx_init(&ctx);
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;

    if (http_open(&ctx, url, header_name, header_value, result) != 0) {
        http_ctx_close(&ctx);
        return -1;
    }

    g_stage = 8;
    int cl_type = 1;
    uint64_t content_length = 0;
    sceHttpGetResponseContentLength(ctx.req, &cl_type, &content_length);

    size_t cap = (cl_type == 0 && content_length > 0 && content_length < max_body_bytes)
                     ? (size_t)content_length + 1
                     : 65536;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        http_ctx_close(&ctx);
        set_result(result, PROSPEROMGR_HTTP_ERR_OTHER, result ? result->http_status : 0, 0);
        return -1;
    }

    g_stage = 10;
    size_t off = 0;
    int ok = 1;
    for (;;) {
        if (off + 65536 > cap) {
            size_t new_cap = cap * 2;
            if (new_cap > max_body_bytes + 1) new_cap = max_body_bytes + 1;
            if (new_cap <= cap) { ok = 0; break; } // hit the cap
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) { ok = 0; break; }
            buf = nb;
            cap = new_cap;
        }
        int n = sceHttpReadData(ctx.req, buf + off, cap - off);
        if (n < 0) { set_result(result, PROSPEROMGR_HTTP_ERR_READ, result ? result->http_status : 0, n); ok = 0; break; }
        if (n == 0) break;
        off += (size_t)n;
        if (off > max_body_bytes) { ok = 0; break; }
    }

    http_ctx_close(&ctx);

    if (!ok) {
        free(buf);
        if (result && result->kind == PROSPEROMGR_HTTP_OK) set_result(result, PROSPEROMGR_HTTP_ERR_OTHER, result->http_status, 0);
        return -1;
    }
    if (out_body) *out_body = buf; else free(buf);
    if (out_len) *out_len = off;
    return 0;
}

int prosperomgr_ps5_http_download(const char *url, const char *out_path, struct prosperomgr_http_result *result,
                                  prosperomgr_ps5_http_progress_fn progress, void *progress_opaque,
                                  prosperomgr_ps5_http_cancel_fn cancel, void *cancel_opaque,
                                  const char *header_name, const char *header_value) {
    http_ctx_t ctx;
    http_ctx_init(&ctx);

    if (http_open(&ctx, url, header_name, header_value, result) != 0) {
        http_ctx_close(&ctx);
        return -1;
    }

    g_stage = 8;
    int cl_type = 1;
    uint64_t content_length = 0;
    int have_length = (sceHttpGetResponseContentLength(ctx.req, &cl_type, &content_length) == 0 && cl_type == 0);
    long long total = have_length ? (long long)content_length : -1;

    g_stage = 9;
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        http_ctx_close(&ctx);
        set_result(result, PROSPEROMGR_HTTP_ERR_WRITE, result ? result->http_status : 0, 0);
        return -1;
    }

    const size_t kChunk = 256 * 1024;
    unsigned char *chunk = (unsigned char *)malloc(kChunk);
    if (!chunk) {
        close(fd);
        unlink(out_path);
        http_ctx_close(&ctx);
        set_result(result, PROSPEROMGR_HTTP_ERR_OTHER, result ? result->http_status : 0, 0);
        return -1;
    }

    g_stage = 10;
    long long loaded = 0;
    long long last_progress_ms = monotonic_ms();
    long long last_data_ms = last_progress_ms;
    const long long kStallTimeoutMs = 25000;
    int fail_kind = PROSPEROMGR_HTTP_OK;
    long long fail_code = 0;
    long long loop_start_us = monotonic_us();
    long long read_us = 0;
    long long write_us = 0;

    for (;;) {
        if (cancel && cancel(cancel_opaque)) { fail_kind = PROSPEROMGR_HTTP_ERR_CANCELED; break; }

        long long before_read_us = monotonic_us();
        int n = sceHttpReadData(ctx.req, chunk, kChunk);
        long long after_read_us = monotonic_us();
        read_us += after_read_us - before_read_us;
        if (n < 0) { fail_kind = PROSPEROMGR_HTTP_ERR_READ; fail_code = n; break; }
        if (n == 0) break; // clean EOF

        last_data_ms = monotonic_ms();

        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(fd, chunk + written, (size_t)(n - written));
            if (w <= 0) { fail_kind = PROSPEROMGR_HTTP_ERR_WRITE; break; }
            written += w;
        }
        write_us += monotonic_us() - after_read_us;
        if (fail_kind != PROSPEROMGR_HTTP_OK) break;

        loaded += n;

        if (progress) {
            long long now = monotonic_ms();
            if (now - last_progress_ms >= 80) {
                progress(loaded, total, progress_opaque);
                last_progress_ms = now;
            }
        }

        if (monotonic_ms() - last_data_ms > kStallTimeoutMs) { fail_kind = PROSPEROMGR_HTTP_ERR_TIMEOUT; break; }
    }

    free(chunk);
    close(fd);
    http_ctx_close(&ctx);

    if (result) {
        result->bytes = loaded;
        result->total_ms = (monotonic_us() - loop_start_us) / 1000;
        result->read_ms = read_us / 1000;
        result->write_ms = write_us / 1000;
    }

    if (fail_kind != PROSPEROMGR_HTTP_OK) {
        unlink(out_path);
        set_result(result, fail_kind, result ? result->http_status : 0, fail_code);
        return -1;
    }

    if (have_length && loaded != total) {
        unlink(out_path);
        set_result(result, PROSPEROMGR_HTTP_ERR_READ, result ? result->http_status : 0, 0);
        return -1;
    }

    if (progress) progress(loaded, total, progress_opaque); // always report the final state
    return 0;
}

// ---- general request (POST / PUT / DELETE with an in-memory body) -----------------------------

// Copies the value of response header `name` (case-insensitive) into `out`; "" when absent.
static void http_header_value(int req, const char *name, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';

    char *headers = NULL;
    size_t headers_len = 0;
    if (sceHttpGetAllResponseHeaders(req, &headers, &headers_len) != 0 || !headers) return;

    size_t name_len = strlen(name);
    const char *end = headers + headers_len;
    for (const char *line = headers; line < end;) {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        size_t line_len = eol ? (size_t)(eol - line) : (size_t)(end - line);
        while (line_len && (line[line_len - 1] == '\r' || line[line_len - 1] == ' ')) line_len--;

        if (line_len > name_len + 1 && line[name_len] == ':' && strncasecmp(line, name, name_len) == 0) {
            const char *value = line + name_len + 1;
            size_t value_len = line_len - name_len - 1;
            while (value_len && (*value == ' ' || *value == '\t')) { value++; value_len--; }
            if (value_len < out_sz) {
                memcpy(out, value, value_len);
                out[value_len] = '\0';
            }
            return;
        }
        if (!eol) break;
        line = eol + 1;
    }
}

// One attempt at a request. Returns 0 once a status code was read back. `mode` picks how it connects:
//   0: by the IP the console's own resolver gives, with an explicit Host header (like http_send_hop)
//   1: by hostname, leaving resolution to sceHttp
//   2: by an IP from public DNS, for a resolver that answers with an address nothing listens on
static int http_request_hop(http_ctx_t *ctx, int method, const char *url, int mode,
                            const struct prosperomgr_http_header *headers, size_t header_count,
                            const void *body, size_t body_len, int *status_out,
                            struct prosperomgr_http_result *result) {
    long long rc;
    char resolved_url[2048]; // Drive's resumable-upload session URLs are long
    char orig_host[256];
    const char *connect_url = url;

    orig_host[0] = '\0';
    if (result) {
        result->resolved_ip[0] = '\0';
        result->attempts++;
    }
    if (mode != 1) {
        connect_url = bypass_dns_ex(url, resolved_url, sizeof(resolved_url), orig_host, sizeof(orig_host), mode == 2,
                                    result ? result->resolved_ip : NULL, result ? sizeof(result->resolved_ip) : 0);
        if (!orig_host[0]) return -1; // nothing resolved, so this attempt adds nothing
    }

    g_stage = 4;
    if ((ctx->conn = sceHttpCreateConnectionWithURL(ctx->tmpl, connect_url, 0)) < 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_CONNECT, 0, ctx->conn);
        return -1;
    }
    http_set_timeouts(ctx->conn);

    g_stage = 5;
    if ((ctx->req = sceHttpCreateRequestWithURL(ctx->conn, method, connect_url, (uint64_t)body_len)) < 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_CONNECT, 0, ctx->req);
        return -1;
    }
    http_set_timeouts(ctx->req);
    if (orig_host[0]) sceHttpAddRequestHeader(ctx->req, "Host", orig_host, 0);
    for (size_t i = 0; i < header_count; i++) {
        if (headers[i].name && headers[i].name[0] && headers[i].value) {
            sceHttpAddRequestHeader(ctx->req, headers[i].name, headers[i].value, 0);
        }
    }

    g_stage = 6;
    if ((rc = sceHttpSendRequest(ctx->req, body_len ? body : NULL, body_len)) != 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_SEND, 0, rc);
        return -1;
    }

    g_stage = 7;
    if ((rc = sceHttpGetStatusCode(ctx->req, status_out)) != 0) {
        set_result(result, PROSPEROMGR_HTTP_ERR_CONNECT, 0, rc);
        return -1;
    }
    return 0;
}

int prosperomgr_ps5_http_request(int method, const char *url, const struct prosperomgr_http_header *headers,
                                 size_t header_count, const void *body, size_t body_len, char **out_body,
                                 size_t *out_len, size_t max_body_bytes, char *out_location, size_t location_size,
                                 char *out_range, size_t range_size, struct prosperomgr_http_result *result) {
    http_ctx_t ctx;
    http_ctx_init(&ctx);
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;
    if (out_location && location_size) out_location[0] = '\0';
    if (out_range && range_size) out_range[0] = '\0';

    if (strncmp(url, "https://", 8) != 0) { // credentials and saves never travel in clear text
        set_result(result, PROSPEROMGR_HTTP_ERR_INIT, 0, 0);
        return -1;
    }
    if (http_setup(&ctx, result) != 0) {
        http_ctx_close(&ctx);
        return -1;
    }

    int status = 0;
    int connected = 0;
    for (int mode = 0; mode < 3 && !connected; mode++) {
        if (http_request_hop(&ctx, method, url, mode, headers, header_count, body, body_len, &status, result) == 0) {
            connected = 1;
        } else {
            http_close_request(&ctx);
        }
    }
    if (!connected) {
        http_ctx_close(&ctx);
        return -1;
    }
    set_result(result, PROSPEROMGR_HTTP_OK, status, 0);

    if (out_location) http_header_value(ctx.req, "Location", out_location, location_size);
    if (out_range) http_header_value(ctx.req, "Range", out_range, range_size);

    g_stage = 8;
    size_t cap = 4096;
    size_t off = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        http_ctx_close(&ctx);
        set_result(result, PROSPEROMGR_HTTP_ERR_OTHER, status, 0);
        return -1;
    }

    g_stage = 10;
    int ok = 1;
    for (;;) {
        if (off + 4096 > cap) {
            size_t new_cap = cap * 2;
            if (new_cap > max_body_bytes + 1) new_cap = max_body_bytes + 1;
            if (new_cap <= cap) { ok = 0; break; } // hit the cap
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) { ok = 0; break; }
            buf = nb;
            cap = new_cap;
        }
        int n = sceHttpReadData(ctx.req, buf + off, cap - off);
        if (n < 0) { set_result(result, PROSPEROMGR_HTTP_ERR_READ, status, n); ok = 0; break; }
        if (n == 0) break;
        off += (size_t)n;
        if (off > max_body_bytes) { ok = 0; break; }
    }
    http_ctx_close(&ctx);

    if (!ok) {
        free(buf);
        if (result && result->kind == PROSPEROMGR_HTTP_OK) set_result(result, PROSPEROMGR_HTTP_ERR_OTHER, status, 0);
        return -1;
    }
    if (out_body) *out_body = buf; else free(buf);
    if (out_len) *out_len = off;
    return 0;
}

#endif // PROSPEROMGR_PS5
