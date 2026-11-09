#include "pkg_ws_upload.hpp"

#include <atomic>
#include <chrono>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "job_queue.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "pkg_installer.hpp"
#include "pkg_live_cache.hpp"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace prospero {

namespace {

// Time the upload thread spent blocked in recv(), i.e. waiting for the browser.
std::atomic<uint64_t> g_recv_wait_us{0};

// Reachable from the LAN (a PC browser), unlike the loopback-only pkg_stream port —
// same trust model as the rest of ProsperoMgr's unauthenticated local API.
constexpr uint16_t kPort = 9046;
constexpr const char *kPath = "/ws/upload";
constexpr const char *kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr size_t kTextMax = 4096;
constexpr size_t kBinMax = kPkgLiveSegSize + 64 * 1024; // one segment plus frame slack
constexpr int kIoTimeoutSec = 30;
constexpr int kListenRcvBuf = 4 * 1024 * 1024;
constexpr size_t kRecvChunk = 1024 * 1024; // fewer syscalls draining a pipelined burst

Config g_cfg;
std::mutex g_uplink_mu;
int g_uplink_fd = -1;

bool JsonU64(const std::string &json, const std::string &key, uint64_t *out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) return false;
    *out = std::strtoull(json.c_str() + pos, nullptr, 10);
    return true;
}

// ---- SHA1 (public-domain style, compact — handshake key hashing only) ----

struct Sha1Ctx {
    uint32_t h[5];
    uint64_t len = 0;
    unsigned char buf[64];
    size_t buflen = 0;
};

uint32_t Rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void Sha1Block(Sha1Ctx *c, const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
               uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) w[i] = Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = Rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = Rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void Sha1Init(Sha1Ctx *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
}

void Sha1Update(Sha1Ctx *c, const void *data, size_t n) {
    const auto *p = static_cast<const unsigned char *>(data);
    c->len += n;
    while (n > 0) {
        size_t take = std::min(n, 64 - c->buflen);
        std::memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64) { Sha1Block(c, c->buf); c->buflen = 0; }
    }
}

void Sha1Final(Sha1Ctx *c, unsigned char out[20]) {
    uint64_t bitlen = c->len * 8;
    unsigned char one = 0x80;
    Sha1Update(c, &one, 1);
    unsigned char zero = 0;
    while (c->buflen != 56) Sha1Update(c, &zero, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = static_cast<unsigned char>(bitlen >> (56 - i * 8));
    std::memcpy(c->buf + 56, lb, 8);
    Sha1Block(c, c->buf);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = static_cast<unsigned char>(c->h[i] >> 24);
        out[i * 4 + 1] = static_cast<unsigned char>(c->h[i] >> 16);
        out[i * 4 + 2] = static_cast<unsigned char>(c->h[i] >> 8);
        out[i * 4 + 3] = static_cast<unsigned char>(c->h[i]);
    }
}

std::string Base64(const unsigned char *in, size_t n) {
    static const char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = unsigned(in[i]) << 16;
        if (i + 1 < n) v |= unsigned(in[i + 1]) << 8;
        if (i + 2 < n) v |= in[i + 2];
        out += kTab[(v >> 18) & 63];
        out += kTab[(v >> 12) & 63];
        out += (i + 1 < n) ? kTab[(v >> 6) & 63] : '=';
        out += (i + 2 < n) ? kTab[v & 63] : '=';
    }
    return out;
}

std::string HandshakeAccept(const std::string &client_key) {
    std::string combo = client_key + kGuid;
    Sha1Ctx c;
    Sha1Init(&c);
    Sha1Update(&c, combo.data(), combo.size());
    unsigned char digest[20];
    Sha1Final(&c, digest);
    return Base64(digest, sizeof(digest));
}

// ---- frame codec (server sends unmasked frames only; a browser only ever sends masked ones) ----

void EncodeFrame(unsigned char opcode, const void *payload, size_t len, std::vector<unsigned char> *out) {
    out->clear();
    out->push_back(static_cast<unsigned char>(0x80 | (opcode & 0x0F)));
    if (len < 126) {
        out->push_back(static_cast<unsigned char>(len));
    } else if (len <= 0xFFFF) {
        out->push_back(126);
        out->push_back(static_cast<unsigned char>(len >> 8));
        out->push_back(static_cast<unsigned char>(len));
    } else {
        out->push_back(127);
        for (int i = 7; i >= 0; i--) out->push_back(static_cast<unsigned char>(len >> (i * 8)));
    }
    if (len > 0) {
        size_t at = out->size();
        out->resize(at + len);
        std::memcpy(out->data() + at, payload, len);
    }
}

// Browsers mask every client frame; XOR eight bytes at a time instead of one.
void Unmask(unsigned char *data, size_t len, const unsigned char *mask) {
    unsigned char wide_bytes[8];
    for (int i = 0; i < 8; i++) wide_bytes[i] = mask[i & 3];
    uint64_t wide = 0;
    std::memcpy(&wide, wide_bytes, sizeof(wide));
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, sizeof(word));
        word ^= wide;
        std::memcpy(data + i, &word, sizeof(word));
    }
    for (; i < len; i++) data[i] ^= mask[i & 3];
}

// Returns bytes consumed, 0 if the buffer holds an incomplete frame, -1 on a protocol error.
// The payload is unmasked where it sits in `buf`; `pay_off` says where it starts.
int DecodeFrame(unsigned char *buf, size_t buflen, unsigned char *opcode, bool *fin, size_t *pay_off,
                size_t *pay_len) {
    if (buflen < 2) return 0;
    unsigned char b0 = buf[0], b1 = buf[1];
    if (b0 & 0x70) return -1; // RSV1-3 must be 0
    unsigned char op = b0 & 0x0F;
    if (op > 0x0A || (op > 0x02 && op < 0x08)) return -1;
    bool is_fin = (b0 & 0x80) != 0;
    bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;
    size_t hdr = 2;
    if (len == 126) {
        if (buflen < 4) return 0;
        len = (uint64_t(buf[2]) << 8) | buf[3];
        hdr = 4;
    } else if (len == 127) {
        if (buflen < 10) return 0;
        if (buf[2] & 0x80) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | buf[2 + i];
        hdr = 10;
    }
    if (op >= 0x08 && (!is_fin || len > 125)) return -1; // control frames: FIN + <=125 bytes
    if (!masked) return -1;                              // a client frame is always masked
    if (len > kBinMax + 16) return -1;
    if (buflen < hdr + 4 + static_cast<size_t>(len)) return 0;

    Unmask(buf + hdr + 4, static_cast<size_t>(len), buf + hdr);

    *opcode = op;
    *fin = is_fin;
    *pay_off = hdr + 4;
    *pay_len = static_cast<size_t>(len);
    return static_cast<int>(hdr + 4 + len);
}

bool HasHeader(const std::string &headers, const char *name, std::string *value) {
    std::string needle = std::string(name) + ":";
    size_t at = 0;
    while (at < headers.size()) {
        size_t eol = headers.find("\r\n", at);
        if (eol == std::string::npos) eol = headers.size();
        if (eol - at > needle.size()) {
            bool match = true;
            for (size_t i = 0; i < needle.size(); i++) {
                if (std::tolower(static_cast<unsigned char>(headers[at + i])) !=
                    std::tolower(static_cast<unsigned char>(needle[i]))) { match = false; break; }
            }
            if (match) {
                size_t v = at + needle.size();
                while (v < eol && headers[v] == ' ') v++;
                *value = headers.substr(v, eol - v);
                return true;
            }
        }
        at = eol + 2;
    }
    return false;
}

bool SendAll(int fd, const void *data, size_t len) {
    const auto *p = static_cast<const char *>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool SendText(int fd, const std::string &text) {
    std::vector<unsigned char> frame;
    EncodeFrame(0x1, text.data(), text.size(), &frame);
    return SendAll(fd, frame.data(), frame.size());
}

bool SendTextLocked(int fd, const std::string &text) {
    std::lock_guard<std::mutex> lock(g_uplink_mu);
    return SendText(fd, text);
}

// Tells the browser where the installer is reading, so it can stay just ahead.
void EmitPos(uint64_t seg) {
    std::lock_guard<std::mutex> lock(g_uplink_mu);
    if (g_uplink_fd < 0) return;
    SendText(g_uplink_fd, "{\"op\":\"pos\",\"r\":" + std::to_string(seg) + "}");
}

// Called from whichever thread is blocked in PkgLiveCache::Read() on a segment the browser has
// not sent yet.
void EmitSeek(uint64_t seg) {
    std::lock_guard<std::mutex> lock(g_uplink_mu);
    if (g_uplink_fd < 0) return;
    std::string msg = "{\"op\":\"seek\",\"seg\":" + std::to_string(seg) + "}";
    if (!SendText(g_uplink_fd, msg)) LogEvent("[pkgws] seek send failed");
}

// Registered once with the cache; called once the session is torn down so the browser
// isn't left waiting out its own idle timeout to notice the install is done.
void OnSessionDone() {
    std::lock_guard<std::mutex> lock(g_uplink_mu);
    if (g_uplink_fd >= 0) shutdown(g_uplink_fd, SHUT_RDWR);
}

bool IsUplink(int fd) {
    std::lock_guard<std::mutex> lock(g_uplink_mu);
    return g_uplink_fd == fd;
}

void HandleTextMessage(int fd, const std::string &msg, int64_t *pending_seg, bool *finish_ok) {
    std::string op = JsonExtractString(msg, "op");
    if (op.empty()) { SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"missing op\"}"); return; }

    if (op == "init") {
        std::string filename = JsonExtractString(msg, "filename");
        uint64_t total = 0;
        if (filename.empty() || !JsonU64(msg, "total", &total) || total == 0) {
            SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"init needs filename and total\"}");
            return;
        }
        std::string session_id;
        int rc = PkgLiveCache::Create(filename, total, &session_id);
        // A session the console already read in full is only waiting on the final status.
        PkgLiveStatus old = PkgLiveCache::GetStatus();
        if (rc == -2 && old.active && old.total > 0 && old.served >= old.total) {
            PkgLiveCache::Abort();
            PkgLiveCache::Destroy();
            for (int i = 0; i < 20 && rc == -2; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                rc = PkgLiveCache::Create(filename, total, &session_id);
            }
        }
        if (rc == -2) {
            SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"an upload is already in progress\"}");
            return;
        }
        if (rc != 0) {
            SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"could not start the session\"}");
            return;
        }
        PkgLiveCache::SetTitle(JsonExtractString(msg, "title"));
        {
            std::lock_guard<std::mutex> lock(g_uplink_mu);
            if (g_uplink_fd >= 0 && g_uplink_fd != fd) {
                SendText(fd, "{\"op\":\"error\",\"error\":\"another connection is already uploading\"}");
                return;
            }
            g_uplink_fd = fd;
        }
        uint64_t offset = PkgLiveCache::GetResumeOffset();
        SendTextLocked(fd, "{\"op\":\"ready\",\"session_id\":\"" + session_id + "\",\"offset\":" +
                                std::to_string(offset) + ",\"slots\":" + std::to_string(PkgLiveCache::SlotCount()) + "}");
        // Starts the installer as soon as the header lands, not once the upload finishes —
        // the cache is smaller than most packages, so it needs a reader draining it.
        StartBackgroundThread([session_id]() {
            if (PkgLiveCache::WaitHeader(60) != 0 || !PkgLiveCache::CheckId(session_id)) return;
            std::string err;
            if (!StartPkgInstallJobFromLiveSession(g_cfg, session_id, &err)) {
                LogEvent("[pkgws] could not start install: " + err);
                PkgLiveCache::Abort();
                PkgLiveCache::Destroy();
            }
        });
    } else if (op == "seg") {
        if (!IsUplink(fd)) { SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"unauthorized\"}"); return; }
        uint64_t seg = 0;
        *pending_seg = JsonU64(msg, "seg", &seg) ? static_cast<int64_t>(seg) : -1;
    } else if (op == "finish") {
        if (!IsUplink(fd)) { SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"unauthorized\"}"); return; }
        if (PkgLiveCache::Finish() != 0) {
            PkgLiveStatus st = PkgLiveCache::GetStatus();
            SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"incomplete\",\"received\":" +
                                    std::to_string(st.received) + ",\"total\":" + std::to_string(st.total) + "}");
            return;
        }
        // The install already started once the header arrived (see "init"); this just
        // confirms every byte has now landed at least once.
        *finish_ok = true;
        SendTextLocked(fd, "{\"op\":\"complete\"}");
    } else if (op == "stats") {
        if (!IsUplink(fd)) return;
        LogEvent("[pkgws] browser: read " + JsonExtractString(msg, "read_ms") + " ms/seg, ack " + JsonExtractString(msg, "ack_ms") +
                 " ms/seg, socket queue avg " + JsonExtractString(msg, "queued_avg_kb") + " / peak " + JsonExtractString(msg, "queued_kb") +
                 " KB, in flight " + JsonExtractString(msg, "in_flight"));
    } else if (op == "cancel") {
        if (!IsUplink(fd)) { SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"unauthorized\"}"); return; }
        CancelLivePkgInstall();
        SendTextLocked(fd, "{\"op\":\"cancelled\"}");
    } else {
        SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"unknown op\"}");
    }
}

void ConnWorker(int fd) {
    timeval timeout{kIoTimeoutSec, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    std::string headers;
    char buf[4096];
    while (headers.find("\r\n\r\n") == std::string::npos && headers.size() < 16384) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { close(fd); return; }
        headers.append(buf, static_cast<size_t>(n));
    }
    size_t hend = headers.find("\r\n\r\n");
    std::vector<unsigned char> pending(headers.begin() + static_cast<long>(hend) + 4, headers.end());
    headers.resize(hend);

    std::string first_line = headers.substr(0, headers.find("\r\n"));
    char method[16] = {0}, path[512] = {0};
    std::sscanf(first_line.c_str(), "%15s %511s", method, path);
    std::string upgrade, key;
    bool ok_upgrade = HasHeader(headers, "Upgrade", &upgrade) && upgrade.find("websocket") != std::string::npos;
    bool ok_key = HasHeader(headers, "Sec-WebSocket-Key", &key);
    bool ok_path = std::strcmp(path, kPath) == 0;
    if (std::strcmp(method, "GET") != 0 || !ok_path || !ok_upgrade || !ok_key) {
        static const char kBad[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        SendAll(fd, kBad, sizeof(kBad) - 1);
        close(fd);
        return;
    }
    std::string accept = HandshakeAccept(key);
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    if (!SendAll(fd, resp.data(), resp.size())) { close(fd); return; }

    // One fixed buffer filled in place: no per-read zero-fill, consumed frames advance `start`.
    std::vector<unsigned char> store(kBinMax + kRecvChunk + 8192);
    size_t start = 0, end = pending.size();
    if (end > store.size()) end = 0;
    if (end) std::memcpy(store.data(), pending.data(), end);
    std::string frag_text;
    std::vector<unsigned char> frag_bin;
    unsigned char frag_op = 0;
    bool frag_on = false;
    int64_t pending_seg = -1;
    bool finish_ok = false;

    for (;;) {
        bool progressed = true;
        while (progressed && end > start) {
            progressed = false;
            unsigned char opcode = 0;
            bool fin = false;
            size_t poff = 0, plen = 0;
            int used = DecodeFrame(store.data() + start, end - start, &opcode, &fin, &poff, &plen);
            if (used == 0) break;
            if (used < 0) { LogEvent("[pkgws] frame decode failed, closing"); goto conn_done; }
            progressed = true;
            const unsigned char *payload = store.data() + start + poff;

            if (opcode == 0x8) { // close
                std::vector<unsigned char> frame;
                EncodeFrame(0x8, nullptr, 0, &frame);
                SendAll(fd, frame.data(), frame.size());
                goto conn_done;
            } else if (opcode == 0x9) { // ping -> pong
                std::vector<unsigned char> frame;
                EncodeFrame(0xA, payload, std::min(plen, size_t(125)), &frame);
                SendAll(fd, frame.data(), frame.size());
            } else if (opcode == 0xA) {
                // pong: ignore
            } else if (opcode == 0x1 || (opcode == 0x0 && frag_op == 0x1)) {
                bool is_start = opcode == 0x1;
                if (is_start && frag_on) { LogEvent("[pkgws] interleaved text frame, closing"); goto conn_done; }
                if (!is_start && !frag_on) { LogEvent("[pkgws] stray text continuation, closing"); goto conn_done; }
                if (is_start) { frag_text.clear(); frag_op = 0x1; frag_on = true; }
                if (frag_text.size() + plen > kTextMax) { LogEvent("[pkgws] text message too large, closing"); goto conn_done; }
                frag_text.append(reinterpret_cast<const char *>(payload), plen);
                if (fin) {
                    HandleTextMessage(fd, frag_text, &pending_seg, &finish_ok);
                    frag_on = false;
                }
            } else if (opcode == 0x2 || (opcode == 0x0 && frag_op == 0x2)) {
                bool is_start = opcode == 0x2;
                if (!is_start && !frag_on) { LogEvent("[pkgws] stray binary continuation, closing"); goto conn_done; }
                if (is_start) {
                    frag_op = 0x2;
                    frag_on = true;
                    frag_bin.clear();
                    if (pending_seg < 0) { LogEvent("[pkgws] binary frame without a segment header, closing"); goto conn_done; }
                }
                // A whole segment in one frame (the normal case) goes to the cache straight from here.
                const unsigned char *bin = payload;
                size_t bin_len = plen;
                if (!(is_start && fin)) {
                    if (frag_bin.size() + plen > kBinMax) { LogEvent("[pkgws] binary message too large, closing"); goto conn_done; }
                    frag_bin.insert(frag_bin.end(), payload, payload + plen);
                    bin = frag_bin.data();
                    bin_len = frag_bin.size();
                }
                if (fin) {
                    uint64_t seg = static_cast<uint64_t>(pending_seg);
                    if (!IsUplink(fd)) {
                        SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"unauthorized\"}");
                    } else {
                        int wr = PkgLiveCache::TryWrite(seg * kPkgLiveSegSize, bin, bin_len);
                        if (wr == 0) {
                            SendTextLocked(fd, "{\"op\":\"ack\",\"seg\":" + std::to_string(seg) + "}");
                        } else if (wr == -2) {
                            SendTextLocked(fd, "{\"op\":\"busy\",\"seg\":" + std::to_string(seg) + "}");
                        } else {
                            SendTextLocked(fd, "{\"op\":\"error\",\"error\":\"rejected\"}");
                        }
                    }
                    pending_seg = -1;
                    frag_on = false;
                }
            } else {
                LogEvent("[pkgws] unexpected frame, closing");
                goto conn_done;
            }
            start += static_cast<size_t>(used);
        }

        if (start == end) start = end = 0;
        // Slide a partial frame back to the front once the tail is too small for a full read.
        if (store.size() - end < kRecvChunk) {
            std::memmove(store.data(), store.data() + start, end - start);
            end -= start;
            start = 0;
        }
        if (end - start >= kBinMax + 4096) { LogEvent("[pkgws] message exceeds limit, closing"); goto conn_done; }
        {
            auto recv_started = std::chrono::steady_clock::now();
            ssize_t n = recv(fd, store.data() + end, store.size() - end, 0);
            g_recv_wait_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - recv_started).count());
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // The installer may be parked on a resend while the browser has
                    // nothing new to say — that is not an idle connection.
                    if (PkgLiveCache::WaiterCount() > 0) continue;
                    goto conn_done;
                }
                goto conn_done;
            }
            if (n == 0) goto conn_done;
            end += static_cast<size_t>(n);
        }
    }

conn_done: {
    bool was_uplink = false;
    {
        std::lock_guard<std::mutex> lock(g_uplink_mu);
        if (g_uplink_fd == fd) { was_uplink = true; g_uplink_fd = -1; }
    }
    if (was_uplink && !finish_ok) {
        LogEvent("[pkgws] uploader disconnected before finishing; releasing the live cache");
        PkgLiveCache::Abort();
        PkgLiveCache::Destroy();
    }
    close(fd);
}
}

void ListenLoop() {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return;
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // Must be set on the listener: OrbisOS ignores it on an accepted socket and keeps the small default.
    int rcvbuf = kListenRcvBuf;
    setsockopt(server, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int effective = -1;
    socklen_t effective_len = sizeof(effective);
    if (getsockopt(server, SOL_SOCKET, SO_RCVBUF, &effective, &effective_len) != 0) effective = -1;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kPort);
    if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(server, 8) != 0) {
        close(server);
        LogEvent("PKG direct-install listener could not bind port " + std::to_string(kPort));
        return;
    }
    LogEvent("PKG direct-install listener ready on port " + std::to_string(kPort) +
             " (SO_RCVBUF=" + std::to_string(effective) + ")");
    PkgLiveCache::SetSeekFn(EmitSeek);
    PkgLiveCache::SetPosFn(EmitPos);
    PkgLiveCache::SetDoneFn(OnSessionDone);

    for (;;) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;
        StartBackgroundThread([client]() { ConnWorker(client); });
    }
}

} // namespace

uint64_t PkgWsRecvWaitMicros() { return g_recv_wait_us.load(); }

void StartPkgWsUpload(const Config &cfg) {
    static std::mutex mu;
    static bool started = false;
    std::lock_guard<std::mutex> lock(mu);
    if (started) return;
    started = true;
    g_cfg = cfg;
    StartBackgroundThread(ListenLoop);
}

} // namespace prospero
