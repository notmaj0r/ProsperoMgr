#include "system_info.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include <ps5/kernel.h>
#include <sys/sysctl.h>

#include "avatar.hpp"

namespace prospero {
namespace {

std::string LocalIpAddress() {
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return "Unavailable";
    std::string result = "Unavailable";
    for (ifaddrs *it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET ||
            std::strncmp(it->ifa_name, "lo", 2) == 0) continue;
        char text[INET_ADDRSTRLEN] {};
        auto *address = reinterpret_cast<sockaddr_in *>(it->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) {
            result = text;
            break;
        }
    }
    freeifaddrs(list);
    return result;
}

std::string VersionString(uint32_t encoded) {
    // PS5 version components are represented as hexadecimal digits.
    char text[24] {};
    std::snprintf(text, sizeof(text), "%x.%02x", (encoded >> 8) & 0xff, encoded & 0xff);
    return text;
}

} // namespace

SystemInfo ReadSystemInfo() {
    SystemInfo info;
    info.platform = "PlayStation 5";
    info.model = "PS5 (Prospero)";
    uint32_t firmware = kernel_get_fw_version() >> 16;
    info.firmware = VersionString(firmware);

    uint32_t sdk = 0;
    size_t sdk_size = sizeof(sdk);
    if (sysctlbyname("kern.sdk_version", &sdk, &sdk_size, nullptr, 0) == 0) {
        info.sdk_version = VersionString(sdk >> 16);
    }

    char hostname[128] {};
    if (gethostname(hostname, sizeof(hostname) - 1) == 0) info.hostname = hostname;
    info.ip_address = LocalIpAddress();
    // Leave this empty rather than a placeholder: callers treat an empty string as
    // "no user" instead of as a name to show.
    AvatarUser user = GetAvatarUser();
    if (user.ok) {
        char id[16] {};
        std::snprintf(id, sizeof(id), "0x%08x", user.user_id);
        info.user_id = id;
        if (!user.user_name.empty() && user.user_name != "?") info.user_name = user.user_name;
    }
    info.storage = ListStorageVolumes();

    // Values stay 0 if the kernel hides these counters.
    // Read once: the kernel logs an error on every hw.physmem read.
    static const uint64_t physmem = []() {
        uint64_t value = 0;
        size_t len = sizeof(value);
        return sysctlbyname("hw.physmem", &value, &len, nullptr, 0) == 0 ? value : 0;
    }();
    uint32_t free_pages = 0, page_size = 0;
    size_t free_size = sizeof(free_pages), page_size_len = sizeof(page_size);
    if (physmem &&
        sysctlbyname("vm.stats.vm.v_free_count", &free_pages, &free_size, nullptr, 0) == 0 &&
        sysctlbyname("hw.pagesize", &page_size, &page_size_len, nullptr, 0) == 0) {
        info.ram_total_bytes = physmem;
        info.ram_free_bytes = static_cast<uint64_t>(free_pages) * page_size;
    }
    return info;
}

} // namespace prospero
