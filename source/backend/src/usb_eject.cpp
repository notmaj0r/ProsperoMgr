#include "usb_eject.hpp"

#include <sys/mount.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "file_jobs.hpp"
#include "json_writer.hpp"
#include "log.hpp"

namespace prospero {
namespace {

constexpr size_t kMaxDevices = 8;
constexpr size_t kMountPointSize = 128;
constexpr uint32_t kMethodListDevices = 0x30015;
constexpr uint32_t kMethodMountPoint = 0x30013;
constexpr uint32_t kMethodEject = 0x30025;
constexpr auto kEjectWait = std::chrono::seconds(15);

// Entry points of the system's IPMI library, bound by symbol name. Every result is checked and
// nothing is retried.
struct DataInfo {
    const void *data;
    size_t size;
};
struct BufferInfo {
    void *data;
    size_t capacity;
    size_t written; // filled in by the service
};
static_assert(sizeof(DataInfo) == 16 && sizeof(BufferInfo) == 24, "IPMI argument layout");

extern "C" {
void ipmi_config_init(void *) __asm__("_ZN4IPMI6Client6ConfigC1Ev");
uint64_t ipmi_memory_needed(void *) __asm__("_ZN4IPMI6Client6Config24estimateClientMemorySizeEv");
int ipmi_create(void **, const void *, void *, void *) __asm__("_ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_");
int ipmi_connect(void *, const void *, size_t, int32_t *) __asm__("_ZN4IPMI4impl10ClientImpl7connectEPKvmPi");
int ipmi_call(void *, uint32_t, const DataInfo *, uint32_t, int32_t *, BufferInfo *, uint32_t)
    __asm__("_ZN4IPMI4impl10ClientImpl16invokeSyncMethodEjPKNS_8DataInfoEjPiPNS_10BufferInfoEj");
void ipmi_terminate(void *) __asm__("_ZN4IPMI4impl10ClientImpl19terminateConnectionEv");
void ipmi_destroy(void *) __asm__("_ZN4IPMI4impl10ClientImpl7destroyEv");
}

// Once a client could not be torn down its memory is kept, and no further calls are made.
std::atomic<bool> g_service_unusable{false};
std::mutex g_service_mu;

int ErrnoFromNative(int32_t status) {
    uint32_t value = static_cast<uint32_t>(status);
    if ((value & 0xffff0000u) == 0x80020000u && (value & 0xffffu)) return static_cast<int>(value & 0xffffu);
    return status ? EIO : 0;
}

class ServiceClient {
  public:
    ~ServiceClient() {
        if (!client_) return;
        ipmi_terminate(client_);
        ipmi_destroy(client_);
        // A released client marks its id as all ones; otherwise the service may still use the memory.
        uint32_t id = 0;
        std::memcpy(&id, static_cast<const char *>(client_) + sizeof(void *), sizeof(id));
        if (id != UINT32_MAX) {
            g_service_unusable = true;
            LogEvent("usb: the USB service client was not released; USB removal is disabled until restart");
            return;
        }
        std::free(storage_);
    }

    int Open(int32_t *native) {
        if (g_service_unusable) return EIO;
        alignas(16) unsigned char config[0x200] = {0};
        ipmi_config_init(config);
        std::memcpy(config, "SceAutoMounterIpc", sizeof("SceAutoMounterIpc"));
        uint32_t flag = 1;
        uint64_t request_size = 0x200, response_size = 0xf800;
        std::memcpy(config + 0x1c, &flag, sizeof(flag));
        std::memcpy(config + 0x28, &request_size, sizeof(request_size));
        std::memcpy(config + 0x30, &response_size, sizeof(response_size));
        uint64_t size = ipmi_memory_needed(config);
        if (size < 0x1000 || size > 0x100000) return ENOTSUP;
        storage_ = std::calloc(1, static_cast<size_t>(size));
        if (!storage_) return ENOMEM;
        int rc = ipmi_create(&client_, config, nullptr, storage_);
        if (rc || !client_) {
            *native = rc;
            client_ = nullptr;
            std::free(storage_);
            storage_ = nullptr;
            return rc ? ErrnoFromNative(rc) : EIO;
        }
        int32_t result = 0;
        rc = ipmi_connect(client_, nullptr, 0, &result);
        *native = rc ? rc : result;
        return ErrnoFromNative(*native);
    }

    int Call(uint32_t method, const DataInfo *input, BufferInfo *output, uint32_t output_count, int32_t *native) {
        int32_t result = 0;
        int rc = ipmi_call(client_, method, input, input ? 1 : 0, &result, output, output_count);
        *native = rc ? rc : result;
        return ErrnoFromNative(*native);
    }

  private:
    void *client_ = nullptr;
    void *storage_ = nullptr;
};

bool RemovableMountPath(const std::string &path) {
    if (path == "/mnt/ext0") return true;
    return path.size() == 9 && path.compare(0, 8, "/mnt/usb") == 0 && path[8] >= '0' && path[8] <= '7';
}

// The filesystem currently mounted at `path`, or false when something else (or nothing) is.
bool MountedAt(const std::string &path, struct statfs *out) {
    struct statfs info {};
    if (statfs(path.c_str(), &info) != 0 || path != info.f_mntonname) return false;
    if (out) *out = info;
    return true;
}

bool ListLocked(ServiceClient *service, std::vector<UsbDevice> *out, int32_t *native, int *status) {
    uint32_t ids[kMaxDevices] = {0};
    int32_t count = 0;
    uint64_t capacity = kMaxDevices;
    DataInfo input {&capacity, sizeof(capacity)};
    BufferInfo output[] = {{ids, sizeof(ids), 0}, {&count, sizeof(count), 0}};
    *status = service->Call(kMethodListDevices, &input, output, 2, native);
    if (*status) return false;
    if (count < 0 || count > static_cast<int32_t>(kMaxDevices) || output[0].written != sizeof(ids) ||
        output[1].written != sizeof(count)) {
        *status = EIO;
        return false;
    }
    for (int32_t i = 0; i < count; i++) {
        char mount_point[kMountPointSize] = {0};
        uint32_t args[] = {ids[i], static_cast<uint32_t>(sizeof(mount_point))};
        DataInfo point_input {args, sizeof(args)};
        BufferInfo point_output {mount_point, sizeof(mount_point), 0};
        *status = service->Call(kMethodMountPoint, &point_input, &point_output, 1, native);
        if (*status) return false;
        if (!std::memchr(mount_point, '\0', sizeof(mount_point)) || point_output.written != sizeof(mount_point)) {
            *status = EIO;
            return false;
        }
        out->push_back({ids[i], mount_point});
    }
    return true;
}

} // namespace

bool ListUsbDevices(std::vector<UsbDevice> *out, std::string *err) {
    std::lock_guard<std::mutex> lock(g_service_mu);
    ServiceClient service;
    int32_t native = 0;
    int status = service.Open(&native);
    if (!status) ListLocked(&service, out, &native, &status);
    if (status && err) *err = std::string("USB service error ") + std::to_string(status);
    LogEvent("usb: listed " + std::to_string(out->size()) + " device(s), status " + std::to_string(status));
    return status == 0;
}

bool EjectUsbDrive(const std::string &mount_path, std::string *code, std::string *err) {
    auto fail = [&](const char *c, const std::string &message) {
        if (code) *code = c;
        if (err) *err = message;
        LogEvent("usb: removal of " + mount_path + " refused (" + c + ": " + message + ")");
        return false;
    };
    if (!RemovableMountPath(mount_path)) return fail("not_removable", "this storage cannot be removed");
    struct statfs before {};
    if (!MountedAt(mount_path, &before)) return fail("not_mounted", "the drive is not mounted");

    std::lock_guard<std::mutex> lock(g_service_mu);
    ServiceClient service;
    int32_t native = 0;
    int status = service.Open(&native);
    std::vector<UsbDevice> devices;
    if (!status) ListLocked(&service, &devices, &native, &status);
    if (status) return fail("unavailable", "the console's USB service did not answer");

    const UsbDevice *target = nullptr;
    for (const UsbDevice &device : devices) {
        if (device.mount_point == mount_path) target = &device;
    }
    if (!target) return fail("not_mounted", "the console does not list a USB device at this path");

    // The drive may have been replaced while the service was being queried.
    struct statfs now {};
    if (!MountedAt(mount_path, &now) || std::memcmp(&now.f_fsid, &before.f_fsid, sizeof(now.f_fsid)) != 0 ||
        std::strcmp(now.f_mntfromname, before.f_mntfromname) != 0) {
        return fail("not_mounted", "the drive changed while it was being removed");
    }

    sync();
    LogEvent("usb: removing " + mount_path + " through the USB service");
    uint32_t id = target->id;
    DataInfo eject_input {&id, sizeof(id)};
    status = service.Call(kMethodEject, &eject_input, nullptr, 0, &native); // never repeated, even on an error
    if (status == EBUSY) return fail("busy", "the drive is in use");
    if (status) return fail("failed", "the console refused to remove the drive (status " + std::to_string(status) + ")");

    auto deadline = std::chrono::steady_clock::now() + kEjectWait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!MountedAt(mount_path, nullptr)) {
            LogEvent("usb: " + mount_path + " removed");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return fail("unconfirmed", "removal was not confirmed");
}

void RegisterUsbRoutes(HttpServer *server) {
    // Diagnostic and read-only: what the console's USB service reports.
    server->Route("GET", "/api/storage/usb", [](const HttpRequest &) {
        std::vector<UsbDevice> devices;
        std::string err;
        bool ok = ListUsbDevices(&devices, &err);
        std::vector<std::string> rows;
        for (const UsbDevice &device : devices) {
            rows.push_back(JsonWriter().Int("id", device.id).Str("mount_point", device.mount_point).Done());
        }
        return HttpResponse::Json(JsonWriter().Bool("ok", ok).Str("error", err).Raw("devices", JsonWriter::Array(rows)).Done());
    });

    server->Route("POST", "/api/storage/eject", [](const HttpRequest &req) {
        std::string path = JsonExtractString(req.body, "path");
        std::string err, code;
        // No file operation may run on the drive while it is detached.
        if (!BeginImmediateFileMutation(&err)) return HttpResponse::CodedError("busy", err, 409);
        bool ok = EjectUsbDrive(path, &code, &err);
        EndImmediateFileMutation();
        if (!ok) return HttpResponse::CodedError(code, err, code == "busy" ? 409 : 400);
        return HttpResponse::Json(JsonWriter().Bool("ok", true).Done());
    });
}

} // namespace prospero
