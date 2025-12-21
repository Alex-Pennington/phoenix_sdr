// SDRplay API stub implementations for CI builds
// These are no-op functions that allow linking without the real DLL

#include "sdrplay_api.h"
#include <string.h>

// Global no-op implementations
sdrplay_api_ErrT sdrplay_api_Open(void) {
    return sdrplay_api_ServiceNotResponding;
}

sdrplay_api_ErrT sdrplay_api_Close(void) {
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_ApiVersion(float *apiVer) {
    if (apiVer) *apiVer = 3.0f;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_LockDeviceApi(void) {
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi(void) {
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT *devices,
    unsigned int *numDevs, unsigned int maxDevs) {
    (void)devices; (void)maxDevs;
    if (numDevs) *numDevs = 0;
    return sdrplay_api_ServiceNotResponding;
}

sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *device) {
    (void)device;
    return sdrplay_api_Fail;
}

sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT *device) {
    (void)device;
    return sdrplay_api_Success;
}

const char* sdrplay_api_GetErrorString(sdrplay_api_ErrT err) {
    switch (err) {
        case sdrplay_api_Success: return "Success";
        case sdrplay_api_Fail: return "Fail";
        case sdrplay_api_ServiceNotResponding: return "Service not responding (stub)";
        default: return "Unknown error";
    }
}

sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE dev, sdrplay_api_DbgLvl_t dbgLvl) {
    (void)dev; (void)dbgLvl;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE dev,
    sdrplay_api_DeviceParamsT **deviceParams) {
    (void)dev; (void)deviceParams;
    return sdrplay_api_Fail;
}

sdrplay_api_ErrT sdrplay_api_Init(HANDLE dev, sdrplay_api_CallbackFnsT *cbFns,
    void *cbContext) {
    (void)dev; (void)cbFns; (void)cbContext;
    return sdrplay_api_Fail;
}

sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE dev) {
    (void)dev;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_Update(HANDLE dev, sdrplay_api_TunerSelectT tuner,
    sdrplay_api_ReasonForUpdateT reasonForUpdate,
    sdrplay_api_ReasonForUpdateExtension1T reasonForUpdateExt1) {
    (void)dev; (void)tuner; (void)reasonForUpdate; (void)reasonForUpdateExt1;
    return sdrplay_api_Success;
}
