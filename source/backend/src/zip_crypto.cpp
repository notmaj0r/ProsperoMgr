#include "zip_crypto.hpp"

#include "miniz.h"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace prospero::zipcrypto {
namespace {

constexpr size_t kInBuffer = 64 * 1024;
constexpr size_t kDictSize = TINFL_LZ_DICT_SIZE; // 32 KiB inflate window
constexpr uint32_t kAesPbkdf2Iterations = 1000;
constexpr size_t kAesMacLen = 10;

// ---- SHA-1 / HMAC-SHA1 / PBKDF2 ------------------------------------------------

inline uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

struct Sha1State {
    uint32_t h[5];
    uint64_t total;
    uint8_t buf[64];
    size_t used;

    Sha1State() { Reset(); }

    void Reset() {
        h[0] = 0x67452301u; h[1] = 0xEFCDAB89u; h[2] = 0x98BADCFEu; h[3] = 0x10325476u; h[4] = 0xC3D2E1F0u;
        total = 0;
        used = 0;
    }

    void Block(const uint8_t *p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
            uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void Update(const uint8_t *p, size_t n) {
        total += n;
        while (n) {
            size_t take = std::min(n, sizeof buf - used);
            memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == sizeof buf) { Block(buf); used = 0; }
        }
    }

    void Final(uint8_t out[20]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        pad = 0;
        while (used != 56) Update(&pad, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        Update(len, 8);
        for (int i = 0; i < 5; i++) {
            out[4 * i] = uint8_t(h[i] >> 24); out[4 * i + 1] = uint8_t(h[i] >> 16);
            out[4 * i + 2] = uint8_t(h[i] >> 8); out[4 * i + 3] = uint8_t(h[i]);
        }
    }
};

struct HmacSha1 {
    Sha1State inner, outer;

    void Init(const uint8_t *key, size_t key_len) {
        uint8_t k[64] = {};
        if (key_len > sizeof k) {
            Sha1State s;
            s.Update(key, key_len);
            s.Final(k);
        } else if (key_len) {
            memcpy(k, key, key_len);
        }
        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
        inner.Reset(); inner.Update(ipad, sizeof ipad);
        outer.Reset(); outer.Update(opad, sizeof opad);
    }
    void Update(const uint8_t *p, size_t n) { inner.Update(p, n); }
    void Final(uint8_t out[20]) {
        uint8_t ih[20];
        inner.Final(ih);
        Sha1State o = outer;
        o.Update(ih, sizeof ih);
        o.Final(out);
    }
};

// ---- AES (encrypt direction only — WinZip AES runs the cipher in CTR mode) -------

struct AesTables {
    uint8_t sbox[256];
    AesTables() {
        uint8_t p = 1, q = 1;
        do {
            p = uint8_t(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= uint8_t(q << 1); q ^= uint8_t(q << 2); q ^= uint8_t(q << 4);
            if (q & 0x80) q ^= 0x09;
            auto rotl8 = [](uint8_t x, int s) { return uint8_t((x << s) | (x >> (8 - s))); };
            sbox[p] = uint8_t(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
    }
};

const AesTables &Tables() {
    static const AesTables t;
    return t;
}

class Aes {
public:
    // key_len: 16, 24 or 32.
    void SetKey(const uint8_t *key, size_t key_len) {
        const auto &sb = Tables().sbox;
        int nk = int(key_len / 4);
        rounds_ = nk + 6;
        int words = 4 * (rounds_ + 1);
        memcpy(rk_, key, key_len);
        uint8_t rcon = 1;
        for (int i = nk; i < words; i++) {
            uint8_t t[4];
            memcpy(t, rk_ + 4 * (i - 1), 4);
            if (i % nk == 0) {
                uint8_t r = t[0];
                t[0] = uint8_t(sb[t[1]] ^ rcon); t[1] = sb[t[2]]; t[2] = sb[t[3]]; t[3] = sb[r];
                rcon = uint8_t((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
            } else if (nk > 6 && i % nk == 4) {
                for (auto &b : t) b = sb[b];
            }
            for (int j = 0; j < 4; j++) rk_[4 * i + j] = uint8_t(rk_[4 * (i - nk) + j] ^ t[j]);
        }
    }

    void EncryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        const auto &sb = Tables().sbox;
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = uint8_t(in[i] ^ rk_[i]);
        for (int round = 1; round <= rounds_; round++) {
            uint8_t t[16];
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) t[r + 4 * c] = sb[s[r + 4 * ((c + r) & 3)]]; // SubBytes + ShiftRows
            if (round < rounds_) {
                for (int c = 0; c < 4; c++) {
                    uint8_t *col = t + 4 * c;
                    uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    auto xt = [](uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); };
                    col[0] = uint8_t(xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3);
                    col[1] = uint8_t(a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3);
                    col[2] = uint8_t(a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3));
                    col[3] = uint8_t((xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3));
                }
            }
            for (int i = 0; i < 16; i++) s[i] = uint8_t(t[i] ^ rk_[16 * round + i]);
        }
        memcpy(out, s, 16);
    }

private:
    uint8_t rk_[240] = {};
    int rounds_ = 10;
};

// ---- ZipCrypto -----------------------------------------------------------------

struct CrcTable {
    uint32_t t[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    }
};

const CrcTable &Crc() {
    static const CrcTable t;
    return t;
}

struct ZipCryptoKeys {
    uint32_t k0 = 0x12345678u, k1 = 0x23456789u, k2 = 0x34567890u;

    static uint32_t CrcStep(uint32_t crc, uint8_t b) { return Crc().t[(crc ^ b) & 0xFF] ^ (crc >> 8); }

    void Update(uint8_t b) {
        k0 = CrcStep(k0, b);
        k1 = (k1 + (k0 & 0xFF)) * 134775813u + 1;
        k2 = CrcStep(k2, uint8_t(k1 >> 24));
    }
    void Init(const std::string &password) {
        for (unsigned char c : password) Update(c);
    }
    uint8_t Decrypt(uint8_t c) {
        uint32_t t = (k2 & 0xFFFF) | 2;
        c ^= uint8_t((t * (t ^ 1)) >> 8);
        Update(c);
        return c;
    }
};

// ---- Entry reader --------------------------------------------------------------

uint16_t Le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

bool PreadFull(int fd, void *buf, size_t n, uint64_t ofs) {
    auto *p = static_cast<uint8_t *>(buf);
    while (n) {
        ssize_t r = pread(fd, p, n, static_cast<off_t>(ofs));
        if (r <= 0) return false;
        p += r; ofs += uint64_t(r); n -= size_t(r);
    }
    return true;
}

enum class Scheme { kZipCrypto, kAes };

class Reader final : public EntryReader {
public:
    OpenResult Init(int fd, const EntryInfo &info, const std::string &password) {
        fd_ = fd;
        expected_crc_ = info.crc;

        if (info.bit_flag & ((1u << 6) | (1u << 13))) return OpenResult::kUnsupported; // strong encryption / masked headers

        uint8_t lh[30];
        if (!PreadFull(fd, lh, sizeof lh, info.local_header_ofs) || Le32(lh) != 0x04034B50u) return OpenResult::kCorrupt;
        uint16_t name_len = Le16(lh + 26), extra_len = Le16(lh + 28);
        uint64_t data_ofs = info.local_header_ofs + sizeof lh + name_len + extra_len;
        uint16_t dos_time = Le16(lh + 10);

        uint64_t prefix = 0; // bytes of per-entry crypto header before the ciphertext
        uint64_t suffix = 0; // bytes of trailer after it
        uint16_t real_method = info.method;

        if (info.method == 99) {
            scheme_ = Scheme::kAes;
            std::vector<uint8_t> extra(extra_len);
            if (extra_len && !PreadFull(fd, extra.data(), extra_len, info.local_header_ofs + sizeof lh + name_len)) return OpenResult::kCorrupt;
            bool found = false;
            uint8_t strength = 0;
            uint16_t version = 0;
            for (size_t off = 0; off + 4 <= extra.size();) {
                uint16_t id = Le16(&extra[off]), sz = Le16(&extra[off + 2]);
                if (off + 4 + sz > extra.size()) break;
                if (id == 0x9901 && sz >= 7) {
                    version = Le16(&extra[off + 4]);
                    strength = extra[off + 8];
                    real_method = Le16(&extra[off + 9]);
                    found = true;
                    break;
                }
                off += 4 + size_t(sz);
            }
            if (!found || strength < 1 || strength > 3 || (version != 1 && version != 2)) return OpenResult::kUnsupported;
            check_crc_ = (version == 1); // AE-2 stores 0 — the HMAC alone vouches for integrity
            key_len_ = size_t(8 * (strength + 1));
            size_t salt_len = key_len_ / 2;
            prefix = salt_len + 2;
            suffix = kAesMacLen;
            if (info.comp_size < prefix + suffix) return OpenResult::kCorrupt;
        } else {
            scheme_ = Scheme::kZipCrypto;
            prefix = 12;
            if (info.comp_size < prefix) return OpenResult::kCorrupt;
        }
        if (real_method != 0 && real_method != 8) return OpenResult::kUnsupported;
        method_ = real_method;

        if (password.empty()) return OpenResult::kBadPassword;

        if (scheme_ == Scheme::kZipCrypto) {
            uint8_t header[12];
            if (!PreadFull(fd, header, sizeof header, data_ofs)) return OpenResult::kCorrupt;
            zc_.Init(password);
            uint8_t last = 0;
            for (uint8_t b : header) last = zc_.Decrypt(b);
            // With a data descriptor (bit 3) the CRC wasn't known when the header was
            // written, so writers check against the mod time instead.
            uint8_t by_crc = uint8_t(info.crc >> 24), by_time = uint8_t(dos_time >> 8);
            bool ok = (info.bit_flag & 0x8) ? (last == by_time || last == by_crc) : (last == by_crc);
            if (!ok) return OpenResult::kBadPassword;
        } else {
            size_t salt_len = key_len_ / 2;
            uint8_t head[16 + 2];
            if (!PreadFull(fd, head, salt_len + 2, data_ofs)) return OpenResult::kCorrupt;
            uint8_t derived[32 + 32 + 2];
            Pbkdf2HmacSha1(reinterpret_cast<const uint8_t *>(password.data()), password.size(), head, salt_len,
                           kAesPbkdf2Iterations, derived, 2 * key_len_ + 2);
            if (derived[2 * key_len_] != head[salt_len] || derived[2 * key_len_ + 1] != head[salt_len + 1]) return OpenResult::kBadPassword;
            aes_.SetKey(derived, key_len_);
            hmac_.Init(derived + key_len_, key_len_);
            memset(counter_, 0, sizeof counter_);
        }

        pos_ = data_ofs + prefix;
        remaining_ = info.comp_size - prefix - suffix;
        trailer_ofs_ = pos_ + remaining_;
        in_.resize(kInBuffer);

        if (method_ == 8) {
            dict_.resize(kDictSize);
            tinfl_init(&inflate_);
        }
        return OpenResult::kOk;
    }

    Chunk Next(const uint8_t **data, size_t *len) override {
        if (error_) return Chunk::kError;
        if (method_ == 0) {
            if (!Refill()) return error_ ? Chunk::kError : Chunk::kEnd;
            crc_ = uint32_t(mz_crc32(crc_, in_.data(), in_len_));
            *data = in_.data();
            *len = in_len_;
            in_pos_ = in_len_; // handed out whole
            return Chunk::kData;
        }

        for (;;) {
            if (inflate_done_) return Chunk::kEnd;
            if (in_pos_ == in_len_ && remaining_ > 0 && !Refill()) return Chunk::kError;

            size_t in_size = in_len_ - in_pos_;
            size_t out_size = kDictSize - dict_ofs_;
            mz_uint32 flags = remaining_ > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0;
            tinfl_status st = tinfl_decompress(&inflate_, in_.data() + in_pos_, &in_size, dict_.data(), dict_.data() + dict_ofs_, &out_size, flags);
            in_pos_ += in_size;

            if (st < 0) { error_ = true; return Chunk::kError; } // TINFL_STATUS_FAILED*
            if (st == TINFL_STATUS_DONE) inflate_done_ = true;
            if (st == TINFL_STATUS_NEEDS_MORE_INPUT && remaining_ == 0 && in_pos_ == in_len_) { error_ = true; return Chunk::kError; } // truncated stream

            if (out_size > 0) {
                crc_ = uint32_t(mz_crc32(crc_, dict_.data() + dict_ofs_, out_size));
                *data = dict_.data() + dict_ofs_;
                *len = out_size;
                dict_ofs_ = (dict_ofs_ + out_size) & (kDictSize - 1);
                return Chunk::kData;
            }
            if (inflate_done_) return Chunk::kEnd;
        }
    }

    VerifyResult Verify() override {
        // The MAC covers every ciphertext byte, including any the inflater didn't need.
        while (remaining_ > 0) {
            if (!Refill()) return VerifyResult::kIoError;
        }
        if (scheme_ == Scheme::kAes) {
            uint8_t mac[20], stored[kAesMacLen];
            hmac_.Final(mac);
            if (!PreadFull(fd_, stored, sizeof stored, trailer_ofs_)) return VerifyResult::kIoError;
            uint8_t diff = 0;
            for (size_t i = 0; i < kAesMacLen; i++) diff |= uint8_t(mac[i] ^ stored[i]);
            if (diff) return VerifyResult::kBadAuth;
        }
        if (check_crc_ && crc_ != expected_crc_) return VerifyResult::kBadCrc;
        return VerifyResult::kOk;
    }

    bool WeakPasswordCheck() const override { return scheme_ == Scheme::kZipCrypto; }

private:
    // Reads and decrypts the next run of ciphertext into in_. False at the end
    // of the entry (error_ stays false) or on a read failure (error_ set).
    bool Refill() {
        in_pos_ = in_len_ = 0;
        if (remaining_ == 0) return false;
        size_t n = size_t(std::min<uint64_t>(remaining_, in_.size()));
        if (!PreadFull(fd_, in_.data(), n, pos_)) { error_ = true; return false; }
        pos_ += n;
        remaining_ -= n;
        if (scheme_ == Scheme::kZipCrypto) {
            for (size_t i = 0; i < n; i++) in_[i] = zc_.Decrypt(in_[i]);
        } else {
            hmac_.Update(in_.data(), n); // MAC is over the ciphertext
            for (size_t off = 0; off < n; off += 16) {
                for (int i = 0; i < 16; i++) // little-endian block counter, starting at 1
                    if (++counter_[i]) break;
                uint8_t ks[16];
                aes_.EncryptBlock(counter_, ks);
                for (size_t i = 0; i < 16 && off + i < n; i++) in_[off + i] ^= ks[i];
            }
        }
        in_len_ = n;
        return true;
    }

    int fd_ = -1;
    Scheme scheme_ = Scheme::kZipCrypto;
    uint16_t method_ = 8;
    bool check_crc_ = true;
    bool error_ = false;
    uint32_t expected_crc_ = 0;

    uint64_t pos_ = 0, remaining_ = 0, trailer_ofs_ = 0;
    std::vector<uint8_t> in_;
    size_t in_pos_ = 0, in_len_ = 0;

    ZipCryptoKeys zc_;
    Aes aes_;
    HmacSha1 hmac_;
    size_t key_len_ = 0;
    uint8_t counter_[16] = {};

    tinfl_decompressor inflate_ {};
    std::vector<uint8_t> dict_;
    size_t dict_ofs_ = 0;
    bool inflate_done_ = false;
    uint32_t crc_ = 0; // running CRC of the plaintext
};

} // namespace

bool IsEncrypted(uint16_t bit_flag) { return (bit_flag & 1u) != 0; }

std::unique_ptr<EntryReader> EntryReader::Open(int fd, const EntryInfo &info, const std::string &password, OpenResult *result) {
    auto reader = std::make_unique<Reader>();
    OpenResult r = reader->Init(fd, info, password);
    if (result) *result = r;
    if (r != OpenResult::kOk) return nullptr;
    return reader;
}

void Sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    Sha1State s;
    s.Update(data, len);
    s.Final(out);
}

void Pbkdf2HmacSha1(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                    uint32_t iterations, uint8_t *out, size_t out_len) {
    HmacSha1 base;
    base.Init(password, password_len);
    for (uint32_t block = 1; out_len > 0; block++) {
        uint8_t idx[4] = {uint8_t(block >> 24), uint8_t(block >> 16), uint8_t(block >> 8), uint8_t(block)};
        HmacSha1 h = base;
        h.Update(salt, salt_len);
        h.Update(idx, sizeof idx);
        uint8_t u[20], t[20];
        h.Final(u);
        memcpy(t, u, sizeof t);
        for (uint32_t i = 1; i < iterations; i++) {
            h = base;
            h.Update(u, sizeof u);
            h.Final(u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        size_t n = std::min<size_t>(out_len, sizeof t);
        memcpy(out, t, n);
        out += n;
        out_len -= n;
    }
}

void AesEncryptBlock(const uint8_t *key, size_t key_len, const uint8_t in[16], uint8_t out[16]) {
    Aes aes;
    aes.SetKey(key, key_len);
    aes.EncryptBlock(in, out);
}

} // namespace prospero::zipcrypto
