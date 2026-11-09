#pragma once
// Reads a file through the bundled FTP server, which hands a SELF back as a plain ELF.

#include <memory>
#include <string>

#include "http_server.hpp"

namespace prospero {

// Returns null with `err` set when the FTP server is not running or cannot read the file.
std::shared_ptr<HttpBodyStream> OpenDecryptedFile(const std::string &path, std::shared_ptr<void> lease,
                                                  std::string *err);

} // namespace prospero
