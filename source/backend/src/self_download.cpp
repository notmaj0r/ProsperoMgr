#include "self_download.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include "config.hpp"

namespace prospero {
namespace {

// Decrypting a large executable happens before the first byte comes back.
constexpr int kReplyTimeoutSec = 180;

int ConnectLoopback(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval timeout {kReplyTimeoutSec, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool SendCommand(int fd, const std::string &command) {
    std::string line = command + "\r\n";
    size_t sent = 0;
    while (sent < line.size()) {
        ssize_t n = send(fd, line.data() + sent, line.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// A reply ends at the first line that is a three-digit code followed by a space.
int ReadReply(int fd, std::string *text = nullptr) {
    std::string all;
    for (;;) {
        std::string line;
        char c;
        for (;;) {
            ssize_t n = recv(fd, &c, 1, 0);
            if (n <= 0) return -1;
            if (c == '\n') break;
            if (c != '\r') line += c;
        }
        all += line + "\n";
        if (line.size() >= 4 && line[3] == ' ' && std::isdigit(static_cast<unsigned char>(line[0])) &&
            std::isdigit(static_cast<unsigned char>(line[1])) && std::isdigit(static_cast<unsigned char>(line[2]))) {
            if (text) *text = all;
            return std::atoi(line.substr(0, 3).c_str());
        }
    }
}

class FtpStream final : public HttpBodyStream {
  public:
    FtpStream(int control, int data, std::shared_ptr<void> lease)
        : control_(control), data_(data), lease_(std::move(lease)) {}
    ~FtpStream() override {
        close(data_);
        SendCommand(control_, "QUIT");
        close(control_);
    }

    ssize_t Read(uint64_t, char *buffer, size_t max_bytes) override {
        ssize_t got;
        do {
            got = recv(data_, buffer, max_bytes, 0);
        } while (got < 0 && errno == EINTR);
        if (got > 0) return got;
        if (got < 0) return MHD_CONTENT_READER_END_WITH_ERROR;
        // The server confirms a complete transfer; anything else was cut short.
        return ReadReply(control_) == 226 ? MHD_CONTENT_READER_END_OF_STREAM : MHD_CONTENT_READER_END_WITH_ERROR;
    }

  private:
    int control_;
    int data_;
    std::shared_ptr<void> lease_;
};

} // namespace

std::shared_ptr<HttpBodyStream> OpenDecryptedFile(const std::string &path, std::shared_ptr<void> lease,
                                                  std::string *err) {
    auto fail = [&](const std::string &message, int control, int data = -1) {
        if (err) *err = message;
        if (data >= 0) close(data);
        if (control >= 0) close(control);
        return std::shared_ptr<HttpBodyStream>();
    };
    if (path.empty() || path[0] != '/' || path.find_first_of("\r\n") != std::string::npos) {
        return fail("invalid path", -1);
    }

    int control = ConnectLoopback(Config::kFtpsrvPort);
    if (control < 0) return fail("ftpsrv is not running — launch it from Payloads first", -1);

    std::string text;
    if (ReadReply(control) != 220 || !SendCommand(control, "USER anonymous")) return fail("ftpsrv did not answer", control);
    int code = ReadReply(control);
    if (code == 331) {
        if (!SendCommand(control, "PASS prospero")) return fail("ftpsrv did not answer", control);
        code = ReadReply(control);
    }
    if (code != 230) return fail("ftpsrv refused the login", control);

    if (!SendCommand(control, "TYPE I") || ReadReply(control) != 200) return fail("ftpsrv did not answer", control);
    if (!SendCommand(control, "PASV") || ReadReply(control, &text) != 227) return fail("ftpsrv did not answer", control);
    int h[4], port_hi = 0, port_lo = 0;
    size_t open = text.find('(');
    if (open == std::string::npos ||
        std::sscanf(text.c_str() + open, "(%d,%d,%d,%d,%d,%d)", &h[0], &h[1], &h[2], &h[3], &port_hi, &port_lo) != 6) {
        return fail("ftpsrv sent an unreadable reply", control);
    }
    int data = ConnectLoopback(port_hi * 256 + port_lo);
    if (data < 0) return fail("could not open the transfer connection", control);

    if (!SendCommand(control, "RETR " + path)) return fail("ftpsrv did not answer", control, data);
    code = ReadReply(control, &text);
    if (code != 150 && code != 125) return fail("ftpsrv could not read this file", control, data);
    return std::make_shared<FtpStream>(control, data, std::move(lease));
}

} // namespace prospero
