// SDRplay API v3.x stub for CI builds
// Real API: https://www.sdrplay.com/api/
// This stub allows compilation without the full SDK
#ifndef SDRPLAY_API_H
#define SDRPLAY_API_H

#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
typedef void *HANDLE;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Device limits
#define SDRPLAY_MAX_DEVICES 16

// Error codes
typedef enum {
    sdrplay_api_Success = 0,
    sdrplay_api_Fail = 1,
    sdrplay_api_InvalidParam = 2,
    sdrplay_api_OutOfRange = 3,
    sdrplay_api_GainUpdateError = 4,
    sdrplay_api_RfUpdateError = 5,
    sdrplay_api_FsUpdateError = 6,
    sdrplay_api_HwError = 7,
    sdrplay_api_AliasingError = 8,
    sdrplay_api_AlreadyInitialised = 9,
    sdrplay_api_NotInitialised = 10,
    sdrplay_api_NotEnabled = 11,
    sdrplay_api_HwVerError = 12,
    sdrplay_api_OutOfMemError = 13,
    sdrplay_api_ServiceNotResponding = 14,
    sdrplay_api_StartPending = 15,
    sdrplay_api_StopPending = 16,
    sdrplay_api_InvalidMode = 17,
    sdrplay_api_FailedVerification1 = 18,
    sdrplay_api_FailedVerification2 = 19,
    sdrplay_api_FailedVerification3 = 20,
    sdrplay_api_FailedVerification4 = 21,
    sdrplay_api_FailedVerification5 = 22,
    sdrplay_api_FailedVerification6 = 23,
    sdrplay_api_InvalidServiceVersion = 24
} sdrplay_api_ErrT;

typedef enum {
    sdrplay_api_DbgLvl_Disable = 0,
    sdrplay_api_DbgLvl_Verbose = 1,
    sdrplay_api_DbgLvl_Warning = 2,
    sdrplay_api_DbgLvl_Error = 3,
    sdrplay_api_DbgLvl_Message = 4
} sdrplay_api_DbgLvl_t;

typedef enum {
    sdrplay_api_TunerA = 0,
    sdrplay_api_TunerB = 1,
    sdrplay_api_Tuner_Both = 2,
    sdrplay_api_Tuner_A = 0,  // Alias
    sdrplay_api_Tuner_B = 1   // Alias
} sdrplay_api_TunerSelectT;

typedef enum {
    sdrplay_api_LO_Undefined = 0,
    sdrplay_api_LO_Auto = 1,
    sdrplay_api_LO_120MHz = 2,
    sdrplay_api_LO_144MHz = 3,
    sdrplay_api_LO_168MHz = 4
} sdrplay_api_LoModeT;

typedef enum {
    sdrplay_api_IF_Undefined = -1,
    sdrplay_api_IF_Zero = 0,
    sdrplay_api_IF_0_450 = 1,
    sdrplay_api_IF_1_620 = 2,
    sdrplay_api_IF_2_048 = 3
} sdrplay_api_If_kHzT;

typedef enum {
    sdrplay_api_BW_Undefined = 0,
    sdrplay_api_BW_0_200 = 200,
    sdrplay_api_BW_0_300 = 300,
    sdrplay_api_BW_0_600 = 600,
    sdrplay_api_BW_1_536 = 1536,
    sdrplay_api_BW_5_000 = 5000,
    sdrplay_api_BW_6_000 = 6000,
    sdrplay_api_BW_7_000 = 7000,
    sdrplay_api_BW_8_000 = 8000
} sdrplay_api_Bw_MHzT;

typedef enum {
    sdrplay_api_AGC_DISABLE = 0,
    sdrplay_api_AGC_100HZ = 1,
    sdrplay_api_AGC_50HZ = 2,
    sdrplay_api_AGC_5HZ = 3,
    sdrplay_api_AGC_CTRL = 4
} sdrplay_api_AgcControlT;

typedef enum {
    sdrplay_api_EXTENDED_MIN_GR = 0,
    sdrplay_api_NORMAL_MIN_GR = 20
} sdrplay_api_MinGainReductionT;

typedef enum {
    sdrplay_api_Rsp2_AMPORT_1 = 0,
    sdrplay_api_Rsp2_AMPORT_2 = 1
} sdrplay_api_Rsp2_AmPortSelectT;

typedef enum {
    sdrplay_api_Rsp2_ANTENNA_A = 0,
    sdrplay_api_Rsp2_ANTENNA_B = 1
} sdrplay_api_Rsp2_AntennaSelectT;

typedef enum {
    sdrplay_api_Update_None = 0x00000000,
    sdrplay_api_Update_Dev_Fs = 0x00000001,
    sdrplay_api_Update_Dev_Ppm = 0x00000002,
    sdrplay_api_Update_Dev_SyncUpdate = 0x00000004,
    sdrplay_api_Update_Dev_ResetFlags = 0x00000008,
    sdrplay_api_Update_Rsp1a_BiasTControl = 0x00000010,
    sdrplay_api_Update_Rsp1a_RfNotchControl = 0x00000020,
    sdrplay_api_Update_Rsp1a_RfDabNotchControl = 0x00000040,
    sdrplay_api_Update_Rsp2_BiasTControl = 0x00000080,
    sdrplay_api_Update_Rsp2_AmPortSelect = 0x00000100,
    sdrplay_api_Update_Rsp2_AntennaControl = 0x00000200,
    sdrplay_api_Update_Rsp2_RfNotchControl = 0x00000400,
    sdrplay_api_Update_Rsp2_ExtRefControl = 0x00000800,
    sdrplay_api_Update_RspDuo_ExtRefControl = 0x00001000,
    sdrplay_api_Update_Master_Spare_1 = 0x00002000,
    sdrplay_api_Update_Master_Spare_2 = 0x00004000,
    sdrplay_api_Update_Tuner_Gr = 0x00008000,
    sdrplay_api_Update_Tuner_GrLimits = 0x00010000,
    sdrplay_api_Update_Tuner_Frf = 0x00020000,
    sdrplay_api_Update_Tuner_BwType = 0x00040000,
    sdrplay_api_Update_Tuner_IfType = 0x00080000,
    sdrplay_api_Update_Tuner_DcOffset = 0x00100000,
    sdrplay_api_Update_Tuner_LoMode = 0x00200000,
    sdrplay_api_Update_Ctrl_DCoffsetIQimbalance = 0x00400000,
    sdrplay_api_Update_Ctrl_Decimation = 0x00800000,
    sdrplay_api_Update_Ctrl_Agc = 0x01000000,
    sdrplay_api_Update_Ctrl_AdsbMode = 0x02000000,
    sdrplay_api_Update_Ctrl_OverloadMsgAck = 0x04000000,
    sdrplay_api_Update_RspDuo_BiasTControl = 0x08000000,
    sdrplay_api_Update_RspDuo_AmPortSelect = 0x10000000,
    sdrplay_api_Update_RspDuo_Tuner1AmNotchControl = 0x20000000,
    sdrplay_api_Update_RspDuo_RfNotchControl = 0x40000000,
    sdrplay_api_Update_RspDuo_RfDabNotchControl = 0x80000000
} sdrplay_api_ReasonForUpdateT;

typedef enum {
    sdrplay_api_Update_Ext1_None = 0x00000000
} sdrplay_api_ReasonForUpdateExtension1T;

typedef enum {
    sdrplay_api_GainChange = 0,
    sdrplay_api_PowerOverloadChange = 1,
    sdrplay_api_DeviceRemoved = 2,
    sdrplay_api_RspDuoModeChange = 3
} sdrplay_api_EventT;

typedef enum {
    sdrplay_api_Overload_Detected = 0,
    sdrplay_api_Overload_Corrected = 1
} sdrplay_api_PowerOverloadChangeTypeT;

// Nested structs for tuner params
typedef struct {
    double rfHz;
} sdrplay_api_RfFreqT;

typedef struct {
    unsigned char gRdB;
    unsigned char LNAstate;
    unsigned char minGr;
} sdrplay_api_GainT;

// Tuner params
typedef struct {
    sdrplay_api_Bw_MHzT bwType;
    sdrplay_api_If_kHzT ifType;
    sdrplay_api_LoModeT loMode;
    sdrplay_api_RfFreqT rfFreq;
    sdrplay_api_GainT gain;
    sdrplay_api_MinGainReductionT minGr;
    unsigned char gainReductionDb;
} sdrplay_api_TunerParamsT;

// DC offset
typedef struct {
    unsigned char DCenable;
    unsigned char IQenable;
} sdrplay_api_DcOffsetTunerT;

// Decimation
typedef struct {
    unsigned char enable;
    unsigned char decimationFactor;
    unsigned char wideBandSignal;
} sdrplay_api_DecimationT;

// AGC
typedef struct {
    sdrplay_api_AgcControlT enable;
    int setPoint_dBfs;
    unsigned short attack_ms;
    unsigned short decay_ms;
    unsigned short decay_delay_ms;
    unsigned short decay_threshold_dB;
    int syncUpdate;
} sdrplay_api_AgcT;

// Control params
typedef struct {
    sdrplay_api_DcOffsetTunerT dcOffset;
    sdrplay_api_DecimationT decimation;
    sdrplay_api_AgcT agc;
} sdrplay_api_ControlParamsT;

// RSP2 tuner params
typedef struct {
    unsigned char biasTEnable;
    sdrplay_api_Rsp2_AmPortSelectT amPortSel;
    sdrplay_api_Rsp2_AntennaSelectT antennaSel;
    unsigned char rfNotchEnable;
    unsigned char extRefOutputEn;
} sdrplay_api_Rsp2TunerParamsT;

// RX channel params (combines tuner + control + device-specific)
typedef struct {
    sdrplay_api_TunerParamsT tunerParams;
    sdrplay_api_ControlParamsT ctrlParams;
    char rsp1aTunerParams[32];  // Placeholder
    sdrplay_api_Rsp2TunerParamsT rsp2TunerParams;
    char rspDuoTunerParams[32]; // Placeholder
    char rspDxTunerParams[32];  // Placeholder
} sdrplay_api_RxChannelParamsT;

// Device fsFreq
typedef struct {
    double fsHz;
    unsigned char syncUpdate;
    unsigned char reCal;
} sdrplay_api_FsFreqT;

// Device params
typedef struct {
    double ppm;
    sdrplay_api_FsFreqT fsFreq;
    char syncUpdate[16];   // Placeholder
    char resetFlags[16];   // Placeholder
    char mode[8];          // Placeholder
    unsigned int samplesPerPkt;
    char rsp1aParams[32];  // Placeholder
    char rsp2Params[32];   // Placeholder
    char rspDuoParams[32]; // Placeholder
    char rspDxParams[32];  // Placeholder
} sdrplay_api_DevParamsT;

// Device params container
typedef struct {
    sdrplay_api_DevParamsT *devParams;
    sdrplay_api_RxChannelParamsT *rxChannelA;
    sdrplay_api_RxChannelParamsT *rxChannelB;
} sdrplay_api_DeviceParamsT;

// Device structure
typedef struct {
    char SerNo[64];
    unsigned char hwVer;
    sdrplay_api_TunerSelectT tuner;
    char rspDuoMode[8];  // Placeholder
    unsigned char valid;
    double rspDuoSampleFreq;
    HANDLE dev;
} sdrplay_api_DeviceT;

// Callback parameter structs
typedef struct {
    uint32_t gRdB;
    uint32_t lnaGRdB;
    double currGain;
} sdrplay_api_GainCbParamT;

typedef struct {
    sdrplay_api_PowerOverloadChangeTypeT powerOverloadChangeType;
} sdrplay_api_PowerOverloadCbParamT;

typedef struct {
    uint32_t gRdB;
    uint32_t lnaState;
    sdrplay_api_GainCbParamT gainParams;
    sdrplay_api_PowerOverloadCbParamT powerOverloadParams;
} sdrplay_api_EventParamsT;

typedef struct {
    uint32_t fsHz;
    uint8_t syncUpdate;
    uint8_t reCal;
} sdrplay_api_StreamCbParamsT;

// Callback function types
typedef void (*sdrplay_api_StreamCallback_t)(short *xi, short *xq,
    sdrplay_api_StreamCbParamsT *params, unsigned int numSamples,
    unsigned int reset, void *cbContext);

typedef void (*sdrplay_api_EventCallback_t)(sdrplay_api_EventT eventId,
    sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params,
    void *cbContext);

typedef struct {
    sdrplay_api_StreamCallback_t StreamACbFn;
    sdrplay_api_StreamCallback_t StreamBCbFn;
    sdrplay_api_EventCallback_t EventCbFn;
} sdrplay_api_CallbackFnsT;

// Stub function declarations (will not link without real DLL)
sdrplay_api_ErrT sdrplay_api_Open(void);
sdrplay_api_ErrT sdrplay_api_Close(void);
sdrplay_api_ErrT sdrplay_api_ApiVersion(float *apiVer);
sdrplay_api_ErrT sdrplay_api_LockDeviceApi(void);
sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi(void);
sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT *devices,
    unsigned int *numDevs, unsigned int maxDevs);
sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *device);
sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT *device);
const char* sdrplay_api_GetErrorString(sdrplay_api_ErrT err);
sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE dev, sdrplay_api_DbgLvl_t dbgLvl);
sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE dev,
    sdrplay_api_DeviceParamsT **deviceParams);
sdrplay_api_ErrT sdrplay_api_Init(HANDLE dev, sdrplay_api_CallbackFnsT *cbFns,
    void *cbContext);
sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE dev);
sdrplay_api_ErrT sdrplay_api_Update(HANDLE dev, sdrplay_api_TunerSelectT tuner,
    sdrplay_api_ReasonForUpdateT reasonForUpdate,
    sdrplay_api_ReasonForUpdateExtension1T reasonForUpdateExt1);

#ifdef __cplusplus
}
#endif

#endif // SDRPLAY_API_H

