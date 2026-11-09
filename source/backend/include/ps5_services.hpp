#pragma once
// Extern declarations for the PS5 first-party functions we link against (libSceUserService
// / libSceSystemService, see root Makefile LDFLAGS_PS5) — no SDK headers ship these.

#include <cstdint>

extern "C" {
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(uint32_t *user_id);
int sceUserServiceGetUserName(int32_t user_id, char *name, size_t max_size);
int sceUserServiceSetUserName(int32_t user_id, const char *name);

int sceSystemServiceLaunchApp(const char *title_id, char **argv, void *ctx);
int sceSystemServiceLaunchWebBrowser(const char *uri);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceGetAppTitleId(int app_id, char *title_id /* char[16] */);
int sceSystemServiceParamGetInt(int32_t param_id, int32_t *value);

int sceAppInstUtilInitialize(void);

// Power actions, all exported by libSceSystemService.
int sceSystemServiceRequestReboot(void);
int sceSystemStateMgrEnterStandby(int trigger);
int sceShellCoreUtilRequestShutdown(int type);

// RegMgr — read/write triplet for offline account activation. No SDK
// header ships these.
int sceRegMgrGetInt(int entity, int *val);
int sceRegMgrSetInt(int entity, int val);
int sceRegMgrGetStr(int entity, char *val, size_t max_size);
int sceRegMgrSetStr(int entity, const char *val, size_t size);
int sceRegMgrGetBin(int entity, void *val, size_t max_size);
int sceRegMgrSetBin(int entity, const void *val, size_t size);

// libkernel_sys — read-only sensor getters.
int sceKernelGetCpuTemperature(int *out_celsius);
int sceKernelGetSocSensorTemperature(int channel, int *out_celsius);
int sceKernelGetCurrentFanDuty(uint16_t *out_duty, uint64_t *out_chassis_info);
}

namespace prospero {

// Matches the ABI sceSystemServiceLaunchApp expects — the fields after
// check_flag are ours (argv passthrough), the rest is the SCE-defined layout.
struct AppLaunchCtx {
    uint32_t structsize;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
    char **argv;
};

} // namespace prospero
