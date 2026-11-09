#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "file_manager.hpp"

namespace prospero {

struct SystemInfo {
    std::string platform;
    std::string firmware;
    std::string sdk_version;
    std::string model;
    std::string hostname;
    std::string ip_address;
    std::string user_name;
    std::string user_id;
    std::vector<StorageVolume> storage;
    uint64_t ram_total_bytes = 0; // 0 when the kernel doesn't report it
    uint64_t ram_free_bytes = 0;
};

SystemInfo ReadSystemInfo();

} // namespace prospero
