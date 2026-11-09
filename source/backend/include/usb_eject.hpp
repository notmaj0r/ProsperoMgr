#pragma once
// Safe removal of USB drives through the console's own USB service, the way its UI ejects a drive.

#include <cstdint>
#include <string>
#include <vector>

#include "http_server.hpp"

namespace prospero {

struct UsbDevice {
    uint32_t id = 0;
    std::string mount_point;
};

// Read-only: asks the service which USB devices it manages and where they are mounted.
bool ListUsbDevices(std::vector<UsbDevice> *out, std::string *err);

// `code` is one of: not_removable, not_mounted, unavailable, busy, failed, unconfirmed.
bool EjectUsbDrive(const std::string &mount_path, std::string *code, std::string *err);

void RegisterUsbRoutes(HttpServer *server);

} // namespace prospero
