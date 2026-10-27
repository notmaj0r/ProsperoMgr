#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Password-protected ZIP entries. miniz can't decrypt, so an encrypted entry is
// read raw from the archive file, decrypted, then inflated here. Handles the
// traditional PKWARE scheme ("ZipCrypto") and WinZip AES-128/192/256 (AE-1/AE-2).
namespace prospero::zipcrypto {

// General-purpose flag bit 0.
bool IsEncrypted(uint16_t bit_flag);

struct EntryInfo {
    uint64_t local_header_ofs = 0;
    uint64_t comp_size = 0; // as stored: includes the 12-byte ZipCrypto header or AES salt/verifier/MAC
    uint32_t crc = 0; // not "crc32": miniz.h #defines that name
    uint16_t bit_flag = 0;
    uint16_t method = 0; // 0 store, 8 deflate, 99 = AES wrapper
};

enum class OpenResult {
    kOk,
    kBadPassword,  // empty or wrong password
    kUnsupported,  // strong encryption, or a compression method other than store/deflate
    kCorrupt,      // truncated or malformed headers
};

enum class Chunk { kData, kEnd, kError };

enum class VerifyResult { kOk, kBadCrc, kBadAuth, kIoError };

class EntryReader {
public:
    virtual ~EntryReader() = default;

    // `fd` must stay open for the reader's lifetime. Reads use pread, so the
    // descriptor's own file position is never touched.
    static std::unique_ptr<EntryReader> Open(int fd, const EntryInfo &info, const std::string &password,
                                             OpenResult *result);

    // Next run of plaintext. The pointer stays valid until the following call.
    virtual Chunk Next(const uint8_t **data, size_t *len) = 0;

    // Call after Next() returns kEnd: checks the CRC-32 (ZipCrypto, AE-1) and the
    // HMAC-SHA1 authentication code (AES).
    virtual VerifyResult Verify() = 0;

    // ZipCrypto's password check is a single byte, so ~1 in 128 wrong passwords
    // slip past Open(). A CRC failure on such an entry most likely means a wrong
    // password, not a damaged archive.
    virtual bool WeakPasswordCheck() const = 0;
};

// Exposed for testing against known vectors.
void Sha1(const uint8_t *data, size_t len, uint8_t out[20]);
void Pbkdf2HmacSha1(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                    uint32_t iterations, uint8_t *out, size_t out_len);
void AesEncryptBlock(const uint8_t *key, size_t key_len, const uint8_t in[16], uint8_t out[16]);

} // namespace prospero::zipcrypto
