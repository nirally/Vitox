// Vitox M3.1 - import resolution for a SELECTED subset of NIDs.
// Vita3K's own resolver (modules/module_parent.cpp resolve_import) is a switch over the whole of
// nids/nids.inc and therefore references every export of every module. This file applies the same
// mechanism to an explicit subset, with the NID values taken from the REAL nids.inc at compile
// time: if Vita3K renames/renumbers a selected export, this fails to compile (no silent drift).
// 0.0.10.0 (M5B): the shimmed module_parent.cpp resolve_import() delegates here, and its real
// call_import() reports every call to vitox_note_import_call() (observation only).
// 0.0.16.x (M7B Phase B): the selected set is one X-macro list; + the 24 SceGxm exports of VITOX0003's draw path.
#include "vitox_import_dispatch.h"

#include <module/bridge.h>

#include <cpu/functions.h>      // M7B.6: read_reg / read_sp
#include <emuenv/state.h>       // M7B.6
#include <kernel/state.h>       // M8B.1 trap: guest threads
#include <kernel/thread/thread_state.h>
#include <nids/functions.h>     // M8B.1 trap: import_name
#include <renderer/gxm_types.h> // M7B.6: GxmContextState, SceGxmVertexProgram
#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The selected exports: X(name). Every one is the ImportFn object defined by the real module TU (EXPORT macro).
#define VITOX_SELECTED_IMPORTS(X)                                                                                \
    /* modules/SceDriverUser/SceRtcUser.cpp */                                                                   \
    X(sceRtcIsLeapYear) X(sceRtcGetDaysInMonth)                                                                  \
    /* 0.0.7.0 (M4.1): modules/SceSysmem/SceSysmem.cpp */                                                        \
    X(sceKernelAllocMemBlock) X(sceKernelGetMemBlockBase) X(sceKernelFreeMemBlock) X(sceKernelFindMemBlockByAddr) \
    /* 0.0.10.0 (M5B): modules/SceLibKernel/SceLibKernel.cpp, modules/SceIofilemgr/SceIofilemgr.cpp */           \
    X(sceIoMkdir) X(sceIoOpen) X(sceKernelExitProcess) X(sceIoWrite) X(sceIoClose)                               \
    /* 0.0.14.x (M7A): modules/SceGxm/SceGxm.cpp, SceDisplay/SceDisplay.cpp, SceDriverUser/SceDisplayUser.cpp */ \
    X(sceGxmInitialize) X(sceGxmMapMemory) X(sceGxmTerminate) X(sceGxmTransferFill) X(sceGxmTransferFinish)      \
    X(sceGxmUnmapMemory) X(sceDisplaySetFrameBuf) X(sceDisplayWaitVblankStart)                                   \
    /* 0.0.16.x (M7B Phase B): modules/SceGxm/SceGxm.cpp - VITOX0003's GXM draw path */                          \
    X(sceGxmCreateContext) X(sceGxmCreateRenderTarget) X(sceGxmColorSurfaceInit) X(sceGxmSyncObjectCreate)       \
    X(sceGxmMapVertexUsseMemory) X(sceGxmMapFragmentUsseMemory)                                                  \
    X(sceGxmShaderPatcherCreate) X(sceGxmShaderPatcherRegisterProgram) X(sceGxmShaderPatcherCreateVertexProgram) \
    X(sceGxmShaderPatcherCreateFragmentProgram) X(sceGxmProgramCheck) X(sceGxmProgramFindParameterByName)        \
    X(sceGxmProgramParameterGetResourceIndex)                                                                    \
    X(sceGxmSetVertexStream) X(sceGxmDraw) X(sceGxmSetVertexProgram) X(sceGxmSetFragmentProgram)                 \
    X(sceGxmReserveVertexDefaultUniformBuffer) X(sceGxmSetUniformDataF)                                          \
    X(sceGxmBeginScene) X(sceGxmEndScene) X(sceGxmDisplayQueueAddEntry) X(sceGxmDisplayQueueFinish) X(sceGxmFinish) \
    /* 0.0.16.9 (M7B.7): VITOX0003's explicit depth-stencil surface, modules/SceGxm/SceGxm.cpp */               \
    X(sceGxmDepthStencilSurfaceInit)                                                                             \
    /* M8B.1: runtime-exact VitaGraphs (lpp-vita) set, added one trapped call at a time (TU already in vitox_hle) */ \
    X(sceKernelCreateLwMutex) X(sceKernelCreateMutex) X(sceKernelGetThreadId) X(sceKernelGetTLSAddr)    \
    X(sceKernelUnlockLwMutex2) X(sceKernelLockLwMutex) X(sceCtrlSetSamplingMode)                               \
    /* M8B.2: the SceCtrl calls of VitaGraphs' import table (modules/SceCtrl/SceCtrl.cpp, REAL; VITOX0004 probe) */ \
    X(sceCtrlPeekBufferPositive) X(sceCtrlGetControllerPortInfo)                                               \
    X(sceTouchSetSamplingState) X(sceSysmoduleLoadModule) X(sceAppUtilInit)                    \
    X(sceAppUtilSystemParamGetInt) X(sceCommonDialogSetConfigParam) X(sceShellUtilInitEvents)                  \
    /* M8B.1 Option B (approved): the 27 remaining SceGxm exports VitaGraphs imports (modules/SceGxm/SceGxm.cpp) */  \
    X(sceGxmSetFrontPolygonMode) X(sceGxmTextureSetMagFilter) X(sceGxmShaderPatcherUnregisterProgram)            \
    X(sceGxmDestroyContext) X(sceGxmShaderPatcherDestroy) X(sceGxmTextureGetFormat) X(sceGxmTextureSetPalette)   \
    X(sceGxmShaderPatcherReleaseFragmentProgram) X(sceGxmSetFrontStencilFunc) X(sceGxmTextureGetMagFilter)       \
    X(sceGxmShaderPatcherReleaseVertexProgram) X(sceGxmTextureGetMinFilter) X(sceGxmSetFrontStencilRef)          \
    X(sceGxmSyncObjectDestroy) X(sceGxmUnmapFragmentUsseMemory) X(sceGxmReserveFragmentDefaultUniformBuffer)     \
    X(sceGxmSetRegionClip) X(sceGxmTextureGetHeight) X(sceGxmTextureGetData) X(sceGxmTextureInitLinear)          \
    X(sceGxmTextureSetMinFilter) X(sceGxmPadHeartbeat) X(sceGxmSetFragmentTexture) X(sceGxmTextureGetWidth)      \
    X(sceGxmTextureGetPalette) X(sceGxmDestroyRenderTarget) X(sceGxmUnmapVertexUsseMemory)                     \
    /* M8C.3 (approved): the 9 HLE imports of firmware libpgf (SceLibKernel.cpp / SceIofilemgr.cpp, already linked) */ \
    X(sceClibMemcpy_safe) X(sceClibMemset) X(sceClibStrncmp) X(sceClibStrncat) X(sceClibStrncpy)                 \
    X(__stack_chk_fail) X(sceIoLseek) X(sceIoGetstat) X(sceIoRead)                                               \
    /* M8D: VitaGraphs runtime traps with the staged PGF (REAL exports, already-linked TUs), one trapped call at a time */ \
    X(sceSysmoduleIsLoaded) X(sceIoRemove) X(sceIoGetstatByFd) X(sceRtcGetTime_t)          \
    X(sceKernelAllocMemBlockForVM) X(sceKernelOpenVMDomain) /* upstream UNIMPLEMENTED(): Vita3K memory is always RW */ \
    X(sceKernelSyncVMDomain) X(sceKernelCloseVMDomain) /* upstream UNIMPLEMENTED() (pair of OpenVMDomain) */      \
    X(sceTouchPeek)                                                                                              \
    /* M8F.2A (approved): VitaGraphs' IME dialog set, modules/SceCommonDialog/SceCommonDialog.cpp (already linked) */ \
    X(sceImeDialogInit) X(sceImeDialogGetStatus) X(sceImeDialogGetResult) X(sceImeDialogTerm)                    \
    X(sceCommonDialogUpdate) /* upstream UNIMPLEMENTED() (returns 0) */                                          \
    /* M8F.2C closure (approved): lpp-vita System.exit() -> modules/SceAppUtil (linked), upstream UNIMPLEMENTED() */ \
    X(sceAppUtilShutdown)                                                                                        \
    /* M8F.2C closure, proven by the next exit-path trap: lpp-vita main() unloads NET + HTTP before            \
       sceKernelExitProcess (modules/SceSysmodule, linked; MANUAL [libpgf]: HLE bookkeeping erase only) */      \
    X(sceSysmoduleUnloadModule)                                                                                \
    /* M9A.4 (approved single-NID closure): app-staging trap 1, SceLibc module_start -> modules/SceProcessmgr (REAL) */ \
    X(sceKernelGetProcessParam)                                                                                \
    /* M9A.4 (approved single-NID closure): app-staging trap 2, SceLibc module_start -> modules/SceLibKernel (REAL, linked) */ \
    X(sceKernelCreateLwCond)                                                                                   \
    /* M9A.4 (approved stdio batch): app-staging trap 3, SceLibc module_start stdio init -> modules/SceProcessmgr (REAL, linked) */ \
    X(sceKernelGetStdin) X(sceKernelGetStdout) X(sceKernelGetStderr)                                           \
    /* M9A.4 (approved single-NID closure): app-staging trap 4, SceLibc module_start -> modules/SceLibKernel (REAL, linked; non-2 variant) */ \
    X(sceKernelUnlockLwMutex)                                                                                  \
    /* M9A.4 (approved single-NID closure): app-staging trap 5, end of SceLibc module_start -> modules/SceKernelThreadMgr/SceThreadmgrCoredumpTime.cpp (REAL) */ \
    X(sceKernelExitThread)                                                                                     \
    /* M9A.4 (approved closure): app-staging trap 6, SceLibFios2 module_start -> modules/SceKernelThreadMgr/SceThreadmgr.cpp (REAL ImportFn; upstream STUBBED pid 1, linked) */ \
    X(sceKernelGetProcessId)                                                                                   \
    /* M9A.4 (approved closure): app-staging trap 7, SceLibFios2 module_start -> modules/SceDriverUser/SceFios2User.cpp (REAL upstream overlay list; TU added) */ \
    X(sceFiosOverlayGetList02)                                                                                 \
    /* M9A.4 (approved closure): app-staging trap 8 (GXM batch), main thread -> modules/SceGxm/SceGxm.cpp (15 imported upstream-REAL SceGxm exports; TU already linked; sceGxmTextureValidate UNIMPLEMENTED, excluded) */ \
    X(sceGxmGetNotificationRegion) X(sceGxmProgramGetParameter) X(sceGxmProgramGetParameterCount) X(sceGxmProgramGetDefaultUniformBufferSize) X(sceGxmProgramFindParameterBySemantic) X(sceGxmProgramParameterGetCategory) X(sceGxmProgramParameterGetName) X(sceGxmTextureSetUAddrMode) X(sceGxmTextureSetVAddrMode) X(sceGxmSetViewport) X(sceGxmSetViewportEnable) X(sceGxmSetFrontDepthFunc) X(sceGxmSetFrontDepthWriteEnable) X(sceGxmSetBackDepthFunc) X(sceGxmSetBackDepthWriteEnable)\
    /* M9A.4 (approved closure): app-staging trap 9, main thread -> modules/SceNet/SceNet.cpp (approved A2: REAL upstream sceNetInit only; socket/epoll backends linked, never selected) */ \
    X(sceNetInit)                                                                                              \
    /* M9A.4 (approved closure): app-staging trap 10, main thread -> modules/SceNetCtl/SceNetCtl.cpp (autonomous boot work: init/callbacks/result/term; InetGetState held - reports connected) */ \
    X(sceNetCtlInit) X(sceNetCtlInetRegisterCallback) X(sceNetCtlInetUnregisterCallback) X(sceNetCtlCheckCallback) X(sceNetCtlInetGetResult) X(sceNetCtlTerm)\
    /* M9A.4 (approved closure): app-staging trap 11, main thread sceKernelCreateThread -> kernel thread/sync/time/info + read-only IO + input + rtc + system param, all in already-linked TUs (autonomous boot work; ledger 0.0.24.16-batch.txt) */ \
    X(sceKernelCreateThread) X(sceKernelStartThread) X(sceKernelDelayThread) X(sceKernelWaitThreadEnd) X(sceKernelDeleteThread) X(sceKernelExitDeleteThread) X(sceKernelGetThreadInfo) X(sceKernelGetThreadCurrentPriority) X(sceKernelChangeThreadPriority2) X(sceKernelChangeThreadCpuAffinityMask) X(sceKernelGetThreadCpuAffinityMask2) X(sceKernelRegisterThreadEventHandler) X(sceKernelChangeThreadVfpException) X(sceKernelTryLockLwMutex) X(sceKernelDeleteLwMutex) X(sceKernelWaitLwCond) X(sceKernelSignalLwCond) X(sceKernelSignalLwCondAll) X(sceKernelDeleteLwCond) X(sceKernelGetSystemTimeWide) X(sceKernelGetProcessTime) X(sceKernelGetProcessTimeWide) X(sceKernelLibcClock) X(sceKernelLibcTime) X(sceKernelLibcGettimeofday) X(sceKernelLibcGmtime_r) X(sceKernelLibcLocaltime_r) X(sceKernelLibcMktime) X(sceKernelGetFreeMemorySize) X(sceKernelGetModuleInfoByAddr) X(sceClibPrintf) X(sceClibSnprintf) X(sceIoDopen) X(sceIoDread) X(sceIoDclose) X(sceIoPread) X(sceCtrlGetWirelessControllerInfo) X(sceCtrlPeekBufferPositive2) X(sceTouchGetPanelInfo) X(sceRtcGetCurrentClock) X(sceRtcGetCurrentTick) X(sceRtcGetTickResolution) X(sceRtcGetTime64_t) X(sceRtcSetTime64_t) X(sceAppUtilSystemParamGetString)\
    /* M9A.4 (approved closure): app-staging trap 12, main thread sceSslInit -> SceSsl/SceHttp/SceNpManager library init/term + NP state queries (offline: no TLS, SIGNED_OUT; ledger 0.0.24.17-batch.txt) */ \
    X(sceSslInit) X(sceSslTerm) X(sceHttpInit) X(sceHttpTerm) X(sceNpInit) X(sceNpTerm) X(sceNpCheckCallback) X(sceNpGetServiceState)\
    /* M9A.4 (approved closure): app-staging trap 13, checkCallbackThread sceNetCtlInetGetState -> modules/SceNetCtl (OFFLINE adaptation: DISCONNECTED instead of upstream IPOBTAINED; ledger 0.0.24.18-batch.txt) */ \
    X(sceNetCtlInetGetState)                                                                                   \
    /* M9A.4 (approved closure): app-staging trap 14, SceLibFios2 sceAppMgrIsGameProgram -> modules/SceAppMgr + SceDriverUser/SceAppMgrUser (ledger 0.0.24.19-batch.txt) */ \
    X(sceAppMgrIsGameProgram) X(_sceAppMgrGetAppState)                                                         \
    /* M9A.4 (approved closure): app-staging trap 15, main thread sceGxmTextureValidate -> SceGxm.cpp (upstream UNIMPLEMENTED: returns 0 after the null check; ledger 0.0.24.20-batch.txt) */ \
    X(sceGxmTextureValidate)                                                                                   \
    /* M9A.4 (approved closure): app-staging trap 16, main thread sceKernelLoadStartModule(app0 GameMaker extension) -> SceLibKernel/SceModulemgr (REAL; the PE extension is refused by the fself check -> module_parent returns SCE_ERROR_ERRNO_ENOENT; ledger 0.0.24.21-batch.txt) */ \
    X(sceKernelLoadStartModule) X(sceKernelStopUnloadModule)                                                   \
    /* M9A.4 (approved closure): app-staging trap 17, main thread sceAudioOutOpenPort -> modules/SceAudio + vita3k/audio over the Vitox XAudio2 adapter (UWP port; ledger 0.0.24.22-batch.txt) */ \
    X(sceAudioOutOpenPort) X(sceAudioOutOutput) X(sceAudioOutReleasePort) X(sceAudioOutSetVolume)              \
    /* M9A.4 (approved closure): app-staging trap 18, main thread sceAppMgrAcquireBgmPort -> SceAppMgr.cpp (upstream UNIMPLEMENTED pair, returns 0; ledger 0.0.24.23-batch.txt) */ \
    X(sceAppMgrAcquireBgmPort) X(sceAppMgrReleaseBgmPort)                                                      \
    /* M9A.4 (approved closure): app-staging trap 19, main thread sceNpBasicInit -> NP service library init/term only (upstream UNIMPLEMENTED, return 0; ledger 0.0.24.24-batch.txt) */ \
    X(sceNpBasicInit) X(sceNpBasicTerm) X(sceNpScoreInit) X(sceNpScoreTerm) X(sceNpTusInit) X(sceNpMatching2Init) X(sceNpLookupInit) X(sceNpLookupTerm)\
    /* M9A.4 (approved closure): app-staging trap 20, main thread sceMotionReset -> SceDriverUser/SceMotion + vita3k/motion (no host sensors; ledger 0.0.24.25-batch.txt) */ \
    X(sceMotionReset) X(sceMotionStartSampling) X(sceMotionGetState)                                           \
    /* M9A.4 (approved closure): app-staging trap 21, main thread sceNpTrophyInit -> SceNpTrophy + np/src/trophy (init/context/handle/read-only queries; ledger 0.0.24.26-batch.txt) */ \
    X(sceNpTrophyInit) X(sceNpTrophyCreateContext) X(sceNpTrophyCreateHandle) X(sceNpTrophyDestroyHandle) X(sceNpTrophyGetTrophyUnlockState) X(sceNpTrophyGetTrophyInfo)\
    /* M9A.4 (approved closure): app-staging trap 22, main thread sceNpTrophySetupDialogInit -> SceCommonDialog (trophy setup + message dialog families, REAL; ledger 0.0.24.27-batch.txt) */ \
    X(sceNpTrophySetupDialogInit) X(sceNpTrophySetupDialogGetStatus) X(sceNpTrophySetupDialogGetResult) X(sceNpTrophySetupDialogTerm) X(sceMsgDialogInit) X(sceMsgDialogGetStatus) X(sceMsgDialogGetResult) X(sceMsgDialogClose) X(sceMsgDialogTerm)\
    /* M9A.4 (approved closure): app-staging trap 23, main thread sceAppUtilSaveDataSlotGetParam -> SceAppUtil save-data slot/quota/save (REAL; DataRemove held; ledger 0.0.24.28-batch.txt) */ \
    X(sceAppUtilSaveDataSlotGetParam) X(sceAppUtilSaveDataSlotSetParam) X(sceAppUtilSaveDataGetQuota) X(sceAppUtilSaveDataDataSave)\
    /* M9A.4 (approved closure): app-staging trap 24, main thread sceSaveDataDialogGetStatus -> SceCommonDialog save-data dialog family (REAL, no deletion path; ledger 0.0.24.29-batch.txt) */ \
    X(sceSaveDataDialogContinue) X(sceSaveDataDialogTerm) X(sceSaveDataDialogSubClose) X(sceSaveDataDialogProgressBarSetValue) X(sceSaveDataDialogFinish) X(sceSaveDataDialogGetStatus) X(sceSaveDataDialogGetResult) X(sceSaveDataDialogGetSubStatus) X(sceSaveDataDialogInit)\
    /* M9A.4 (approved closure): app-staging trap 25, UnlockTrophy thread sceNpTrophyUnlockTrophy -> SceNpTrophy (REAL; ledger 0.0.24.30-batch.txt) */ \
    X(sceNpTrophyUnlockTrophy)                                                                                 \
    /* M9A.4 (approved closure): app-staging trap GR-1, GR main thread sceKernelSetGPO -> kernel sync/time/clib/io-seek/ctrl/display/power/debug-LED batch (ledger 0.0.24.38-batch.txt) */ \
    X(sceKernelSetGPO) X(scePowerSetConfigurationMode) X(sceKernelPowerTick) X(sceKernelGetProcessTimeLow) X(sceKernelChangeThreadPriority) X(sceKernelClearEventFlag) X(sceKernelCreateCond) X(sceKernelCreateEventFlag) X(sceKernelCreateSema_16XX) X(sceKernelDelayThreadCB) X(sceKernelDeleteCond) X(sceKernelDeleteEventFlag) X(sceKernelDeleteSema) X(sceKernelLockMutex) X(sceKernelSetEventFlag) X(sceKernelSignalCond) X(sceKernelSignalSema) X(sceKernelUnlockMutex) X(sceKernelWaitCond) X(sceKernelWaitEventFlag) X(sceKernelWaitEventFlagCB) X(sceKernelWaitLwCondCB) X(sceKernelWaitSema) X(sceKernelWaitSemaCB) X(sceKernelWaitThreadEndCB) X(sceClibMemcmp) X(sceClibMemcpy) X(sceClibMemmove) X(sceClibMspaceCreate) X(sceClibMspaceDestroy) X(sceClibMspaceFree) X(sceClibMspaceMalloc) X(sceClibMspaceMemalign) X(sceClibStrncasecmp) X(sceClibStrrchr) X(sceClibVprintf) X(sceClibVsnprintf) X(sceIoLseek32) X(sceCtrlReadBufferPositive) X(sceDisplayWaitSetFrameBuf) X(sceDisplayWaitSetFrameBufMulti) X(sceAudioOutGetAdopt) X(sceMotionSetTiltCorrection) X(sceAppUtilDrmOpen) X(sceAppUtilDrmClose)\
    /* M9A.4 (approved closure): app-staging trap GR-1b, GR imported SceGxm remainder (25 REAL, 2 STUBBED, 3 UNIMPLEMENTED visibility no-ops; ledger 0.0.24.38-batch.txt) */ \
    X(_sceGxmProgramFindParameterBySemantic) X(_sceGxmSetVertexTexture) X(sceGxmColorSurfaceSetGammaMode) X(sceGxmDepthStencilSurfaceSetBackgroundDepth) X(sceGxmDepthStencilSurfaceSetBackgroundStencil) X(sceGxmDepthStencilSurfaceSetForceLoadMode) X(sceGxmDepthStencilSurfaceSetForceStoreMode) X(sceGxmGetRenderTargetMemSize) X(sceGxmNotificationWait) X(sceGxmProgramParameterIsRegFormat) X(sceGxmSetBackPolygonMode) X(sceGxmSetCullMode) X(sceGxmSetFragmentUniformBuffer) X(sceGxmSetFrontDepthBias) X(sceGxmSetFrontFragmentProgramEnable) X(sceGxmSetFrontVisibilityTestEnable) X(sceGxmSetFrontVisibilityTestIndex) X(sceGxmSetFrontVisibilityTestOp) X(sceGxmSetVertexUniformBuffer) X(sceGxmSetVisibilityBuffer) X(sceGxmTextureGetMipmapCountUnsafe) X(sceGxmTextureInitLinearStrided) X(sceGxmTextureInitSwizzled) X(sceGxmTextureInitTiled) X(sceGxmTextureSetData) X(sceGxmTextureSetGammaMode) X(sceGxmTextureSetLodBias) X(sceGxmTextureSetMipFilter) X(sceGxmTextureSetUAddrModeSafe) X(sceGxmTextureSetVAddrModeSafe)\
    /* M9A.4 (approved closure): app-staging trap GR-2, GR main thread sceScreenShotEnable -> ScreenShot/Fiber/NpActivity/NpBasic/NetCtl-adhoc-callbacks/trophy-term/dialog-abort (ledger 0.0.24.39-batch.txt) */ \
    X(sceScreenShotEnable) X(sceScreenShotSetOverlayImage) X(sceScreenShotSetParam) X(_sceFiberInitializeImpl) X(sceFiberFinalize) X(sceFiberReturnToThread) X(sceFiberRun) X(sceFiberSwitch) X(sceNpActivityInit) X(sceNpActivityTerm) X(sceNpBasicCheckCallback) X(sceNpBasicRegisterHandler) X(sceNpBasicUnregisterHandler) X(sceNetCtlAdhocRegisterCallback) X(sceNetCtlAdhocUnregisterCallback) X(sceNpManagerGetContentRatingFlag) X(sceNpTrophyAbortHandle) X(sceNpTrophyDestroyContext) X(sceNpTrophyTerm) X(sceSaveDataDialogAbort)\
    /* M9A.4 (approved closure): app-staging trap GR-3, libfios2 thread _sceFiosKernelOverlayThreadSetDisabled -> SceFios2Kernel (upstream stubs; identity resolve; ledger 0.0.24.40-batch.txt) */ \
    X(_sceFiosKernelOverlayAdd) X(_sceFiosKernelOverlayAddForProcess) X(_sceFiosKernelOverlayDHChstatSync) X(_sceFiosKernelOverlayDHCloseSync) X(_sceFiosKernelOverlayDHOpenSync) X(_sceFiosKernelOverlayDHReadSync) X(_sceFiosKernelOverlayDHStatSync) X(_sceFiosKernelOverlayDHSyncSync) X(_sceFiosKernelOverlayGetInfo) X(_sceFiosKernelOverlayGetInfoForProcess) X(_sceFiosKernelOverlayGetList) X(_sceFiosKernelOverlayGetRecommendedScheduler) X(_sceFiosKernelOverlayModify) X(_sceFiosKernelOverlayModifyForProcess) X(_sceFiosKernelOverlayRemove) X(_sceFiosKernelOverlayRemoveForProcess) X(_sceFiosKernelOverlayResolveSync) X(_sceFiosKernelOverlayResolveWithRangeSync) X(_sceFiosKernelOverlayThreadIsDisabled) X(_sceFiosKernelOverlayThreadSetDisabled)\
    /* M9A.4 (approved closure): app-staging trap GR-5, GR main thread sceAudiodecInitLibrary -> SceAudiodec over the ATRAC9 decoder (no FFmpeg: AAC/MP3 unavailable; ledger 0.0.24.42-batch.txt) */ \
    X(sceAudiodecClearContext) X(sceAudiodecCreateDecoder) X(sceAudiodecCreateDecoderExternal) X(sceAudiodecDecode) X(sceAudiodecDeleteDecoder) X(sceAudiodecDeleteDecoderExternal) X(sceAudiodecGetContextSize) X(sceAudiodecInitLibrary) X(sceAudiodecTermLibrary)\
    /* M9A.4 (approved closure): app-staging trap GR-6, GR main thread sceHttpsLoadCert -> LOCAL SceHttp setup only (certs/template/timeouts/options; no connection; ledger 0.0.24.43-batch.txt) */ \
    X(sceHttpsLoadCert) X(sceHttpsUnloadCert) X(sceHttpCreateTemplate) X(sceHttpDeleteTemplate) X(sceHttpSetConnectTimeOut) X(sceHttpSetRecvTimeOut) X(sceHttpSetSendTimeOut) X(sceHttpSetAutoRedirect) X(sceHttpSetAuthEnabled) X(sceHttpsEnableOption) X(sceHttpsDisableOption) X(sceHttpsGetSslError) X(sceHttpGetLastErrno)\
    /* M9A.4 (approved closure): app-staging trap GR-7, GR main thread sceJpegInitMJpeg -> SceJpeg/SceVideodec/SceCodecEngine over FFmpeg n7.1.1 WindowsStore (ledger 0.0.24.44-batch.txt) */ \
    X(sceAvcdecCreateDecoder) X(sceAvcdecDecode) X(sceAvcdecDecodeFlush) X(sceAvcdecDecodeStop) X(sceAvcdecDeleteDecoder) X(sceAvcdecQueryDecoderMemSize) X(sceCodecEngineAllocMemoryFromUnmapMemBlock) X(sceCodecEngineCloseUnmapMemBlock) X(sceCodecEngineFreeMemoryFromUnmapMemBlock) X(sceCodecEngineOpenUnmapMemBlock) X(sceJpegDecodeMJpeg) X(sceJpegFinishMJpeg) X(sceJpegGetOutputInfo) X(sceJpegInitMJpeg) X(sceVideodecInitLibraryWithUnmapMem) X(sceVideodecQueryMemSize) X(sceVideodecTermLibrary)\
    /* M9A.4 (approved closure): app-staging trap GR-9, GR BackgroundFiber sceMp4OpenFile -> SceMp4 (upstream 14/14 UNIMPLEMENTED; ledger 0.0.24.46-batch.txt) */ \
    X(sceMp4OpenFile) X(sceMp4GetNextUnit) X(sceMp4StartFileStreaming) X(sceMp4GetNextUnit2Ref) X(sceMp4Reset) X(sceMp4GetNextUnit3Ref) X(sceMp4EnableStream) X(sceMp4TimeToPTS) X(sceMp4ReleaseBuffer) X(sceMp4GetStreamInfo) X(sceMp4CloseFile) X(sceMp4JumpPTS) X(sceMp4PTSToTime) X(sceMp4GetNextUnitData)\
    /* M9A.4 (approved closure): app-staging trap GR-mp4-1, libscemp4 (firmware MP4, user-approved): upstream-REAL SceLibKernel sceClibStrnlen */ \
    X(sceClibStrnlen)                                                                                          \
    /* M9A.4 (approved closure): app-staging trap GR-mp4-2, libscemp4 movie player: upstream-REAL SceProcessmgr sceKernelGetProcessTitleId */ \
    X(sceKernelGetProcessTitleId)                                                                              \
    /* M9A.4 (approved closure): app-staging trap GR-mp4-3, libscemp4 movie player: the remaining upstream-REAL SceLibKernel sceClib exports (UNIMPLEMENTED/STUBBED excluded) */ \
    X(sceClibLookCtypeTable) X(sceClibMspaceCalloc) X(sceClibMspaceRealloc) X(sceClibStrchr) X(sceClibStrcmp) X(sceClibStrlcat) X(sceClibStrlcpy) X(sceClibStrstr) X(sceClibStrtoll) X(sceClibTolower) X(sceClibToupper)\
    /* M9A.4 (approved closure): app-staging trap GR-mp4-4, movie end teardown: sceKernelCloseMutex is UNIMPLEMENTED upstream (returns 0, the upstream path); DeleteMutex + TryLockMutex upstream-REAL */ \
    X(sceKernelCloseMutex) X(sceKernelDeleteMutex) X(sceKernelTryLockMutex)                                    \
    /* M9A.4 (approved closure): app-staging trap GR-play-1, gameplay: NID 0x01414F0B imported from SceThreadmgr = upstream-REAL ksceKernelGetThreadCurrentPriority (SceThreadmgrForDriver TU added) */ \
    X(ksceKernelGetThreadCurrentPriority)                                                                      \
    /* M9A.4 (approved closure): app-staging trap DR-1, Danganronpa first trap sceKernelLockLwMutexCB + the eboot's unselected imports: SceNgs/SceAvPlayer/SceIme/SceLiveAreaUtil TUs + ngs engine added; upstream-REAL except sceAppMgrReceiveSystemEvent, sceLiveAreaUpdateFrameSync, sceNgsVoiceGetParamsOutOfRange, __sce_aeabi_idiv0/ldiv0 (STUBBED/UNIMPLEMENTED upstream = the upstream path); ledger scratchpad dr/classified.txt */ \
    X(sceAppUtilSaveDataDataRemove) X(sceImeDialogAbort) X(sceGxmSetFrontPointLineWidth) X(sceGxmTextureInitCube) X(sceGxmTextureGetUAddrMode) X(sceGxmTextureGetVAddrMode) X(sceGxmRenderTargetGetDriverMemBlock) X(sceGxmTextureInitSwizzledArbitrary) X(sceGxmColorSurfaceGetScaleMode) X(sceGxmSetBackPointLineWidth) X(sceGxmShaderPatcherGetProgramFromId) X(sceGxmTextureInitCubeArbitrary) X(sceGxmColorSurfaceGetStrideInPixels) X(sceGxmColorSurfaceGetFormat) X(sceDisplayGetFrameBuf) X(sceDisplayWaitVblankStartMultiCB) X(sceDisplayRegisterVblankStartCallback) X(sceDisplayWaitVblankStartCB) X(sceDisplayGetVcount) X(sceTouchRead) X(sceAvPlayerResume) X(sceAvPlayerIsActive) X(sceAvPlayerPause) X(sceAvPlayerInit) X(sceAvPlayerClose) X(sceAvPlayerGetVideoData) X(sceAvPlayerGetAudioData) X(sceAvPlayerAddSource) X(sceAvPlayerStop) X(sceRtcGetCurrentClockLocalTime) X(sceLiveAreaUpdateFrameSync) X(sceAppMgrReceiveSystemEvent) X(sceImeOpen) X(sceImeUpdate) X(sceImeClose) X(sceNgsRackInit) X(sceNgsVoiceDefGetCompressorBuss) X(sceNgsVoiceKill) X(sceNgsVoiceDefGetAtrac9Voice) X(sceNgsVoiceSetModuleCallback) X(sceNgsVoiceUnlockParams) X(sceNgsRackGetRequiredMemorySize) X(sceNgsSystemRelease) X(sceNgsVoiceGetParamsOutOfRange) X(sceNgsVoiceDefGetDelayBuss) X(sceNgsVoiceResume) X(sceNgsVoiceGetInfo) X(sceNgsSystemUpdate) X(sceNgsSystemGetRequiredMemorySize) X(sceNgsVoiceDefGetMasterBuss) X(sceNgsPatchGetInfo) X(sceNgsVoiceBypassModule) X(sceNgsVoiceDefGetReverbBuss) X(sceNgsVoicePatchSetVolumesMatrix) X(sceNgsVoiceDefGetDistortionBuss) X(sceNgsVoiceLockParams) X(sceNgsVoiceKeyOff) X(sceNgsVoiceGetStateData) X(sceNgsPatchRemoveRouting) X(sceNgsPatchCreateRouting) X(sceNgsVoicePause) X(sceNgsRackRelease) X(sceNgsVoiceDefGetMixerBuss) X(sceNgsVoiceDefGetTemplate1) X(sceNgsSystemInit) X(sceNgsVoiceDefGetEqBuss) X(sceNgsVoicePlay) X(sceNgsVoiceDefGetPitchShiftBuss) X(sceNgsRackGetVoiceHandle) X(sceKernelLockLwMutexCB) X(__sce_aeabi_idiv0) X(sceKernelGetThreadExitStatus) X(__sce_aeabi_ldiv0) X(sceKernelCreateCallback) X(sceKernelCheckCallback)\
    /* M9A.4 (approved closure): app-staging trap DR-2, Danganronpa sceIoIoctl (from LLE libfios2) + the remaining unselected imports of the bundled LLE modules whose TUs are built: upstream-REAL except sceIoIoctl/sceIoChstat/sceIoSync/sceIoSyncByFd/sceKernelCallModuleExit/sceKernelCallAbortHandler/sceKernelUnregisterThreadEventHandler/4 Fios2 overlay calls (UNIMPLEMENTED/STUBBED upstream = the upstream path) and PARTIAL sceIoDevctl/sceFiberGetInfo/_sceFiberAttachContextAndSwitch; ledger scratchpad dr/mods_classified.txt */ \
    X(_sceFiberAttachContextAndSwitch) X(_sceFiberInitializeWithInternalOptionImpl) X(sceFiberGetInfo) X(sceFiosOverlayAddForProcess02) X(sceFiosOverlayGetInfoForProcess02) X(sceFiosOverlayGetRecommendedScheduler02) X(sceFiosOverlayModifyForProcess02) X(sceFiosOverlayRemoveForProcess02) X(sceFiosOverlayResolveWithRangeSync02) X(sceFiosOverlayThreadSetDisabled02) X(sceIoChstat) X(sceIoDevctl) X(sceIoIoctl) X(sceIoPwrite) X(sceIoRename) X(sceIoRmdir) X(sceIoSync) X(sceIoSyncByFd) X(sceKernelCallAbortHandler) X(sceKernelCallModuleExit) X(sceKernelSendSignal) X(sceKernelUnregisterThreadEventHandler) X(sceKernelWaitSignal)\
    /* M9A.4 (approved closure): app-staging trap P4-1, Persona 4 Dancing All Night (pre-run import closure): the eboot + bundled-module unselected imports whose TUs are built - GXM precomputed draw/state, kernel timers/sema, NpManager, Videodec, touch, fiber; upstream-REAL except sceAppUtilStoreBrowse, sceNpActivityPostStatus, 9 SceNpScore calls (UNIMPLEMENTED upstream: no network) and PARTIAL sceGxmColorSurfaceSetClip; sceNetCheckDialog* excluded (M9A.5 deny list); ledger scratchpad p4/all.txt */ \
    X(sceAppUtilStoreBrowse) X(sceAvcdecDecodeAvailableSize) X(sceFiberGetSelf) X(sceGxmColorSurfaceSetClip) X(sceGxmDrawPrecomputed) X(sceGxmFragmentProgramGetProgram) X(sceGxmGetPrecomputedDrawSize) X(sceGxmGetPrecomputedFragmentStateSize) X(sceGxmGetPrecomputedVertexStateSize) X(sceGxmPrecomputedDrawInit) X(sceGxmPrecomputedDrawSetParams) X(sceGxmPrecomputedDrawSetVertexStream) X(sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer) X(sceGxmPrecomputedFragmentStateInit) X(sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer) X(sceGxmPrecomputedFragmentStateSetTexture) X(sceGxmPrecomputedVertexStateGetDefaultUniformBuffer) X(sceGxmPrecomputedVertexStateInit) X(sceGxmPrecomputedVertexStateSetDefaultUniformBuffer) X(sceGxmPrecomputedVertexStateSetTexture) X(sceGxmPrecomputedVertexStateSetUniformBuffer) X(sceGxmSetPrecomputedFragmentState) X(sceGxmSetPrecomputedVertexState) X(sceGxmSetVertexTexture) X(sceGxmTextureGetMipmapCount) X(sceGxmTextureGetType) X(sceGxmVertexProgramGetProgram) X(sceKernelCreateSema) X(sceKernelCreateTimer) X(sceKernelDeleteTimer) X(sceKernelGetSemaInfo) X(sceKernelGetTimerTimeWide) X(sceKernelStartTimer) X(sceKernelStopTimer) X(sceNpActivityPostStatus) X(sceNpManagerGetChatRestrictionFlag) X(sceNpManagerGetNpId) X(sceNpRegisterServiceStateCallback) X(sceNpScoreCreateRequest) X(sceNpScoreCreateTitleCtx) X(sceNpScoreDeleteRequest) X(sceNpScoreDeleteTitleCtx) X(sceNpScoreGetGameDataAsync) X(sceNpScoreGetRankingByRangeAsync) X(sceNpScorePollAsync) X(sceNpScoreRecordGameDataAsync) X(sceNpScoreRecordScoreAsync) X(sceTouchGetSamplingState) X(sceVideodecInitLibrary)\
    /* M9A.4 (approved closure): app-staging trap CFB-1, Catherine Full Body (pre-run import closure): the eboot unselected imports - online-play stubs (NpMatching2/NpTus/NpScore/NpWebApi/RUDP/adhoc matching/VoiceQoS, all UNIMPLEMENTED or STUBBED upstream: no network), libJson (UNIMPLEMENTED upstream), NetCtl adhoc/inet info (local state only), GameUpdate, FriendList2/Invitation dialogs, GXM TransferCopy + ColorSurfaceInitDisabled, Rtc ticks (upstream-REAL); excluded: sceNetCheckDialog* (M9A.5 deny list) and the SceNet host-socket calls (Socket/Bind/Sendto/Recvfrom/sockopt/Shutdown/SocketClose/EpollControl: real traffic); ledger scratchpad cfb/all.txt */ \
    X(_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE) X(_ZN3sce4Json11Initializer9terminateEv) X(_ZN3sce4Json11InitializerC1Ev) X(_ZN3sce4Json11InitializerD1Ev) X(_ZN3sce4Json12MemAllocator11notifyErrorEijPv) X(_ZN3sce4Json12MemAllocatorC2Ev) X(_ZN3sce4Json12MemAllocatorD2Ev) X(_ZN3sce4Json5Array8iteratorD1Ev) X(_ZN3sce4Json5Array8iteratorppEi) X(_ZN3sce4Json5Array9push_backERKNS0_5ValueE) X(_ZN3sce4Json5ArrayC1Ev) X(_ZN3sce4Json5ArrayD1Ev) X(_ZN3sce4Json5ArrayaSERKS1_) X(_ZN3sce4Json5Value3setERKNS0_6ObjectE) X(_ZN3sce4Json5Value5clearEv) X(_ZN3sce4Json5Value9serializeERNS0_6StringE) X(_ZN3sce4Json5ValueC1ERKNS0_5ArrayE) X(_ZN3sce4Json5ValueC1ERKNS0_6StringE) X(_ZN3sce4Json5ValueC1Eb) X(_ZN3sce4Json5ValueC1Ev) X(_ZN3sce4Json5ValueC1Ex) X(_ZN3sce4Json5ValueD1Ev) X(_ZN3sce4Json5ValueaSERKS1_) X(_ZN3sce4Json6ObjectC1Ev) X(_ZN3sce4Json6ObjectD1Ev) X(_ZN3sce4Json6ObjectixERKNS0_6StringE) X(_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcj) X(_ZN3sce4Json6StringC1EPKc) X(_ZN3sce4Json6StringC1Ev) X(_ZN3sce4Json6StringD1Ev) X(_ZNK3sce4Json5Array3endEv) X(_ZNK3sce4Json5Array4sizeEv) X(_ZNK3sce4Json5Array5beginEv) X(_ZNK3sce4Json5Array8iteratordeEv) X(_ZNK3sce4Json5Array8iteratorneES2_) X(_ZNK3sce4Json5Value10getBooleanEv) X(_ZNK3sce4Json5Value11getUIntegerEv) X(_ZNK3sce4Json5Value7getTypeEv) X(_ZNK3sce4Json5Value8getArrayEv) X(_ZNK3sce4Json5Value9getStringEv) X(_ZNK3sce4Json5ValueixEPKc) X(_ZNK3sce4Json5ValueixEj) X(_ZNK3sce4Json6String5c_strEv) X(_ZNK3sce4Json6String6lengthEv) X(sceAppUtilAppEventParseSessionInvitation) X(sceAppUtilReceiveAppEvent) X(sceCommonDialogIsRunning) X(sceGameUpdateInit) X(sceGameUpdateRun) X(sceGameUpdateTerm) X(sceGxmColorSurfaceInitDisabled) X(sceGxmTransferCopy) X(sceInvitationDialogGetResult) X(sceInvitationDialogGetStatus) X(sceInvitationDialogInit) X(sceInvitationDialogTerm) X(sceNetAdhocMatchingCancelTarget) X(sceNetAdhocMatchingCreate) X(sceNetAdhocMatchingDelete) X(sceNetAdhocMatchingGetMembers) X(sceNetAdhocMatchingInit) X(sceNetAdhocMatchingSelectTarget) X(sceNetAdhocMatchingSendData) X(sceNetAdhocMatchingStart) X(sceNetAdhocMatchingStop) X(sceNetAdhocMatchingTerm) X(sceNetCtlAdhocDisconnect) X(sceNetCtlAdhocGetInAddr) X(sceNetCtlAdhocGetPeerList) X(sceNetCtlInetGetInfo) X(sceNetGetStatisticsInfo) X(sceNetHtons) X(sceNetTerm) X(sceNpBasicRecordPlaySessionLog) X(sceNpFriendList2DialogGetResult) X(sceNpFriendList2DialogGetStatus) X(sceNpFriendList2DialogInit) X(sceNpFriendList2DialogTerm) X(sceNpMatching2AbortContextStart) X(sceNpMatching2ContextStart) X(sceNpMatching2ContextStop) X(sceNpMatching2CreateContext) X(sceNpMatching2CreateJoinRoom) X(sceNpMatching2DestroyContext) X(sceNpMatching2GetRoomDataExternalList) X(sceNpMatching2GetRoomMemberDataInternal) X(sceNpMatching2GetServerLocal) X(sceNpMatching2GetUserInfoList) X(sceNpMatching2GetWorldInfoList) X(sceNpMatching2JoinRoom) X(sceNpMatching2KickoutRoomMember) X(sceNpMatching2LeaveRoom) X(sceNpMatching2RegisterContextCallback) X(sceNpMatching2RegisterLobbyEventCallback) X(sceNpMatching2RegisterLobbyMessageCallback) X(sceNpMatching2RegisterRoomEventCallback) X(sceNpMatching2RegisterRoomMessageCallback) X(sceNpMatching2RegisterSignalingCallback) X(sceNpMatching2SearchRoom) X(sceNpMatching2SendRoomMessage) X(sceNpMatching2SetDefaultRequestOptParam) X(sceNpMatching2SetRoomDataExternal) X(sceNpMatching2SetRoomDataInternal) X(sceNpMatching2SetRoomMemberDataInternal) X(sceNpMatching2SignalingGetConnectionInfo) X(sceNpMatching2SignalingGetConnectionStatus) X(sceNpMatching2SignalingGetPingInfo) X(sceNpMatching2Term) X(sceNpScoreAbortRequest) X(sceNpScoreCensorComment) X(sceNpScoreGetBoardInfo) X(sceNpScoreGetFriendsRanking) X(sceNpScoreGetGameData) X(sceNpScoreGetRankingByNpId) X(sceNpScoreGetRankingByNpIdAsync) X(sceNpScoreGetRankingByRange) X(sceNpScoreRecordGameData) X(sceNpScoreRecordScore) X(sceNpScoreSanitizeComment) X(sceNpTusAbortRequest) X(sceNpTusCreateRequest) X(sceNpTusCreateTitleCtx) X(sceNpTusDeleteRequest) X(sceNpTusDeleteTitleCtx) X(sceNpTusGetDataAsync) X(sceNpTusPollAsync) X(sceNpTusSetDataAsync) X(sceNpTusTerm) X(sceNpWebApiCreateRequest) X(sceNpWebApiDeleteRequest) X(sceNpWebApiInitialize) X(sceNpWebApiReadData) X(sceNpWebApiSendRequest2) X(sceNpWebApiSetNpTitleId) X(sceNpWebApiTerminate) X(sceRtcFormatRFC3339LocalTime) X(sceRtcGetCurrentNetworkTick) X(sceRtcGetTick) X(sceRtcSetTick) X(sceRudpActivate) X(sceRudpBind) X(sceRudpCreateContext) X(sceRudpEnableInternalIOThread) X(sceRudpEnd) X(sceRudpGetContextStatus) X(sceRudpGetOption) X(sceRudpGetSizeReadable) X(sceRudpInit) X(sceRudpInitiate) X(sceRudpPollControl) X(sceRudpPollCreate) X(sceRudpPollDestroy) X(sceRudpPollWait) X(sceRudpRead) X(sceRudpSetEventHandler) X(sceRudpSetOption) X(sceRudpTerminate) X(sceRudpWrite) X(sceScreenShotDisable) X(sceVoiceQoSConnect) X(sceVoiceQoSCreateLocalEndpoint) X(sceVoiceQoSCreateRemoteEndpoint) X(sceVoiceQoSDeleteLocalEndpoint) X(sceVoiceQoSDeleteRemoteEndpoint) X(sceVoiceQoSDisconnect) X(sceVoiceQoSGetConnectionAttribute) X(sceVoiceQoSGetLocalEndpointAttribute) X(sceVoiceQoSReadPacket) X(sceVoiceQoSSetConnectionAttribute) X(sceVoiceQoSSetLocalEndpointAttribute) X(sceVoiceQoSWritePacket) \
    /* M9A.4 (approved closure): app-staging trap P3D-1, Persona 3 Dancing in Moonlight (pre-run import closure): the eboot unselected imports whose TUs are built - 5 SceNpTus calls (STUBBED upstream: no network) and sceRtcConvertUtcToLocalTime (upstream-REAL); excluded: sceNetCheckDialog* (M9A.5 deny list) and 5 debug/perf hooks of the bundled libult/libfios2 (SceUlobjDbg/SceUlobjMgr/ScePerf/SceDbg: TUs not built, never called by the other titles); ledger scratchpad p3/all.txt */ \
    X(sceNpTusAddAndGetVariable) X(sceNpTusAddAndGetVariableVUser) X(sceNpTusGetMultiSlotVariable) X(sceNpTusSetMultiSlotVariable) X(sceNpTusTryAndSetVariableVUser) X(sceRtcConvertUtcToLocalTime) \
    /* 0.0.29.3 (user-approved 2026-11-04, Uncharted bring-up): app-staging trap UGA-1, the eboot is a loader that calls sceAppMgrLoadExec -> SceDriverUser/SceAppMgrUser + SceAppMgr _sceAppMgrLoadExec (upstream-REAL: request_process_exit with a LoadExec relaunch, which the launcher performs) */ \
    X(sceAppMgrLoadExec) \
    /* 0.0.29.3 (user-approved Uncharted bring-up): app-staging trap UGA-2 sceGxmProgramParameterGetArraySize + the pre-run import closure of uncharted.self (the eboot's unselected imports whose TUs are built): GXM/NGS/JPEG/AppUtil/motion/kernel/LiveArea/NpAuth, upstream-REAL or UNIMPLEMENTED/STUBBED upstream (= the upstream path; NpAuth: no network); excluded: SceNet host sockets + resolver and SceHttp requests (real traffic), sceNetCheckDialog* (M9A.5 deny list), SceCamera (host camera access), 23 NIDs with no Vita3K export; LLE-bound SceLibc/SceFios2/SceUlt/SceLibft2 imports resolve to the loaded modules; ledger scratchpad uga/unselected.csv */ \
    X(sceAppUtilSaveDataSlotCreate) X(sceAppUtilSaveDataSlotDelete) X(sceMotionSetDeadband) X(sceMotionStopSampling) X(sceGxmPrecomputedFragmentStateSetAllTextures) X(sceGxmPrecomputedFragmentStateSetAllUniformBuffers) X(sceGxmPrecomputedFragmentStateSetUniformBuffer) X(sceGxmPrecomputedVertexStateSetAllUniformBuffers) X(sceGxmProgramIsDepthReplaceUsed) X(sceGxmProgramIsDiscardUsed) X(sceGxmProgramParameterGetArraySize) X(sceGxmProgramParameterGetComponentCount) X(sceGxmSetBackFragmentProgramEnable) X(sceGxmSetBackVisibilityTestEnable) X(sceGxmTextureGetGammaMode) X(sceGxmTextureGetLodBias) X(sceGxmTextureGetMipFilter) X(sceGxmTextureGetUAddrModeSafe) X(sceJpegDecodeMJpegYCbCr) X(sceJpegInitMJpegWithParam) X(sceJpegEncoderCsc) X(sceJpegEncoderEncode) X(sceJpegEncoderEnd) X(sceJpegEncoderGetContextSize) X(sceJpegEncoderInitWithParam) X(sceJpegEncoderSetCompressionRatio) X(sceJpegEncoderSetValidRegion) X(sceKernelOpenSema) X(sceKernelCloseSema) X(sceDbgAssertionHandler) X(sceLiveAreaGetFrameRevision) X(sceLiveAreaGetStatus) X(sceLiveAreaReplaceAllSync) X(sceNetErrnoLoc) X(sceNetInetPton) X(sceNetNtohs) X(sceNgsAT9GetSectionDetails) X(sceNgsVoiceDefGetCompressorSideChainBuss) X(sceNgsVoiceDefGetScreamAtrac9Voice) X(sceNgsVoiceDefGetScreamVoice) X(sceNgsVoiceInit) X(sceNgsVoiceSetFinishedCallback) X(sceNgsVoiceSetParamsBlock) X(sceNgsSystemSetFlags) X(sceNpAuthCreateStartRequest) X(sceNpAuthGetTicket) X(sceNpAuthGetTicketParam) X(sceNpAuthInit) X(sceNpAuthTerm) X(sceKernelOpenMemBlock)

#define X(name) extern const ImportFn import_##name;
VITOX_SELECTED_IMPORTS(X)
#undef X
// LIBRARY_INIT(SceSysmem): the same object Vita3K's init_libraries() calls (library_init_list.inc)
extern const LibraryInitFn import_library_init_SceSysmem;
// M9A.6: LIBRARY_INIT(SceFiber) creates FiberState in kernel.obj_store (sceFiber* dereference it; null -> host crash in 0.0.24.40)
extern const LibraryInitFn import_library_init_SceFiber;
// M9A.6: LIBRARY_INIT(SceAudiodec) creates AudiodecState
extern const LibraryInitFn import_library_init_SceAudiodec;

namespace {

// NID of an export name, straight from Vita3K's nids.inc (0 if absent).
constexpr uint32_t nid_of(std::string_view name) {
#define VAR_NID(n, nid)
#define NID(n, nid)      \
    if (name == #n)      \
        return nid;
#include <nids/nids.inc>
#undef NID
#undef VAR_NID
    return 0;
}

#define X(name)                                  \
    constexpr uint32_t k_##name = nid_of(#name); \
    static_assert(k_##name != 0, "selected export " #name " missing from Vita3K nids.inc");
VITOX_SELECTED_IMPORTS(X)
#undef X

// M9A.5 deny list: exports that must NEVER be selected. sceNetCheckDialogInit(SCE_NETCHECK_DIALOG_MODE_ADHOC_CONN) sets
// emuenv.netctl.adhocCondVarReady, which releases SceNetCtl's adhoc host thread into sockets + UDP broadcast through direct
// CALL_EXPORTs that bypass this dispatcher (no network I/O is authorized). The current app-staging title imports it; it stays fail-closed.
constexpr uint32_t k_vitox_denied[] = { 0xA38A4A0D };
static_assert(nid_of("sceNetCheckDialogInit") == 0xA38A4A0D, "deny-list NID differs from Vita3K nids.inc");
constexpr bool vitox_denied(uint32_t nid) {
    for (const uint32_t d : k_vitox_denied)
        if (d == nid)
            return true;
    return false;
}
#define X(name) static_assert(!vitox_denied(k_##name), "M9A.5 deny list: " #name " must not be selected (adhoc network activation)");
VITOX_SELECTED_IMPORTS(X)
#undef X

// the import tables of the homebrews (vita-elf-create -v) must name exactly these values
static_assert(k_sceGxmInitialize == 0xB0F1E4EC && k_sceGxmMapMemory == 0xC61E34FC && k_sceGxmTerminate == 0xB627DE66
        && k_sceGxmTransferFill == 0x69DDFF5E && k_sceGxmTransferFinish == 0x45229C39 && k_sceGxmUnmapMemory == 0x828C68E8
        && k_sceDisplaySetFrameBuf == 0x7A410B64 && k_sceDisplayWaitVblankStart == 0x5795E898,
    "M7A selected NID differs from the VITOX0002 import table");
static_assert(k_sceGxmCreateContext == 0xE84CE5B4 && k_sceGxmCreateRenderTarget == 0x207AF96B && k_sceGxmColorSurfaceInit == 0xED0F6E25
        && k_sceGxmSyncObjectCreate == 0x6A6013E1 && k_sceGxmMapVertexUsseMemory == 0xFA437510 && k_sceGxmMapFragmentUsseMemory == 0x008402C6
        && k_sceGxmShaderPatcherCreate == 0x05032658 && k_sceGxmShaderPatcherRegisterProgram == 0x2B528462
        && k_sceGxmShaderPatcherCreateVertexProgram == 0xB7BBA6D5 && k_sceGxmShaderPatcherCreateFragmentProgram == 0x4ED2E49D
        && k_sceGxmProgramCheck == 0xED8B6C69 && k_sceGxmProgramFindParameterByName == 0x277794C4
        && k_sceGxmProgramParameterGetResourceIndex == 0x5C79D59A && k_sceGxmSetVertexStream == 0x895DF2E9 && k_sceGxmDraw == 0xBC059AFC
        && k_sceGxmSetVertexProgram == 0x31FF8ABD && k_sceGxmSetFragmentProgram == 0xAD2F48D9
        && k_sceGxmReserveVertexDefaultUniformBuffer == 0x97118913 && k_sceGxmSetUniformDataF == 0x65DD0C84 && k_sceGxmBeginScene == 0x8734FF4E
        && k_sceGxmEndScene == 0xFE300E2F && k_sceGxmDisplayQueueAddEntry == 0xEC5C26B5 && k_sceGxmDisplayQueueFinish == 0xB98C5B0D
        && k_sceGxmFinish == 0x0733D8AE && k_sceGxmDepthStencilSurfaceInit == 0xCA9D41D1,
    "M7B selected NID differs from the VITOX0003 import table");
static_assert(k_sceKernelGetProcessParam == 0x2BE3E066, "M9A.4 selected NID differs from the app-staging trap 1 NID");
static_assert(k_sceKernelCreateLwCond == 0x48C7EAE6, "M9A.4 selected NID differs from the app-staging trap 2 NID");
static_assert(k_sceKernelGetStdin == 0xC1727F59 && k_sceKernelGetStdout == 0xE5AA625C && k_sceKernelGetStderr == 0xFA5E3ADA,
    "M9A.4 selected stdio NID differs from the canonical nids.inc value");
static_assert(k_sceKernelUnlockLwMutex == 0x91FA6614, "M9A.4 selected NID differs from the app-staging trap 4 NID");
static_assert(k_sceKernelExitThread == 0x0C8A38E1, "M9A.4 selected NID differs from the app-staging trap 5 NID");
static_assert(k_sceKernelGetProcessId == 0x9DCB4B7A, "M9A.4 selected NID differs from the app-staging trap 6 NID");
static_assert(k_sceFiosOverlayGetList02 == 0x1DD808D1, "M9A.4 selected NID differs from the app-staging trap 7 NID");
static_assert(k_sceGxmGetNotificationRegion == 0x8BDE825A && k_sceGxmProgramGetParameter == 0x06FF9151 && k_sceGxmProgramGetParameterCount == 0xD5D5FCCD && k_sceGxmProgramGetDefaultUniformBufferSize == 0x8FA3F9C3 && k_sceGxmProgramFindParameterBySemantic == 0x633CAA54 && k_sceGxmProgramParameterGetCategory == 0x1997DC17 && k_sceGxmProgramParameterGetName == 0x6AF88A5D && k_sceGxmTextureSetUAddrMode == 0x4281763E && k_sceGxmTextureSetVAddrMode == 0x126CDAA3 && k_sceGxmSetViewport == 0x3EB3380B && k_sceGxmSetViewportEnable == 0x814F61EB && k_sceGxmSetFrontDepthFunc == 0x14BD831F && k_sceGxmSetFrontDepthWriteEnable == 0xF32CBF34 && k_sceGxmSetBackDepthFunc == 0xB042A4D2 && k_sceGxmSetBackDepthWriteEnable == 0xC18B706B, "M9A.4 selected NID differs from the app-staging trap 8 (GXM batch) NID");
static_assert(k_sceNetInit == 0xEB03E265, "M9A.4 selected NID differs from the app-staging trap 9 NID");
static_assert(k_sceNetCtlInit == 0x495CA1DB && k_sceNetCtlInetRegisterCallback == 0xEAEE6185 && k_sceNetCtlInetUnregisterCallback == 0xD0C3BF3F && k_sceNetCtlCheckCallback == 0xDFFC3ED4 && k_sceNetCtlInetGetResult == 0x6B20EC02 && k_sceNetCtlTerm == 0xCD188648, "M9A.4 selected NID differs from the app-staging trap 10 NID");
static_assert(k_sceKernelCreateThread == 0xC5C11EE7 && k_sceKernelStartThread == 0xF08DE149 && k_sceKernelDelayThread == 0x4B675D05 && k_sceKernelWaitThreadEnd == 0xDDB395A9 && k_sceKernelDeleteThread == 0x1BBDE3D9 && k_sceKernelExitDeleteThread == 0x1D17DECF && k_sceKernelGetThreadInfo == 0x8D9C5461 && k_sceKernelGetThreadCurrentPriority == 0xC53A9848 && k_sceKernelChangeThreadPriority2 == 0xC5FFCA61 && k_sceKernelChangeThreadCpuAffinityMask == 0x15129174 && k_sceKernelGetThreadCpuAffinityMask2 == 0x8C57AC2A && k_sceKernelRegisterThreadEventHandler == 0x6D8C0F13 && k_sceKernelChangeThreadVfpException == 0xCC18FBAE && k_sceKernelTryLockLwMutex == 0xA6A2C915 && k_sceKernelDeleteLwMutex == 0x244E76D2 && k_sceKernelWaitLwCond == 0xE1878282 && k_sceKernelSignalLwCond == 0x3AC63B9A && k_sceKernelSignalLwCondAll == 0xE5241A0C && k_sceKernelDeleteLwCond == 0x721F6CB3 && k_sceKernelGetSystemTimeWide == 0xF4EE4FA9 && k_sceKernelGetProcessTime == 0x4C4672BF && k_sceKernelGetProcessTimeWide == 0xB110C123 && k_sceKernelLibcClock == 0x9E45DA09 && k_sceKernelLibcTime == 0x0039BE45 && k_sceKernelLibcGettimeofday == 0x4B879059 && k_sceKernelLibcGmtime_r == 0xBCA437CD && k_sceKernelLibcLocaltime_r == 0x94F041ED && k_sceKernelLibcMktime == 0x890BDC39 && k_sceKernelGetFreeMemorySize == 0x87CC580B && k_sceKernelGetModuleInfoByAddr == 0xD11A5103 && k_sceClibPrintf == 0xFA26BC62 && k_sceClibSnprintf == 0x8CBA03D5 && k_sceIoDopen == 0xA9283DD0 && k_sceIoDread == 0x9C8B6624 && k_sceIoDclose == 0x422A221A && k_sceIoPread == 0x52315AD7 && k_sceCtrlGetWirelessControllerInfo == 0xB8844141 && k_sceCtrlPeekBufferPositive2 == 0x15F81E8C && k_sceTouchGetPanelInfo == 0x10A2CA25 && k_sceRtcGetCurrentClock == 0x70FDE8F1 && k_sceRtcGetCurrentTick == 0x23F79274 && k_sceRtcGetTickResolution == 0x811313B3 && k_sceRtcGetTime64_t == 0xC995DE02 && k_sceRtcSetTime64_t == 0xA6C36B6A && k_sceAppUtilSystemParamGetString == 0x6E6AA267, "M9A.4 selected NID differs from the app-staging trap 11 NID");
static_assert(k_sceSslInit == 0x3C733316 && k_sceSslTerm == 0x03CE6E3A && k_sceHttpInit == 0x214926D9 && k_sceHttpTerm == 0xC9076666 && k_sceNpInit == 0x04D9F484 && k_sceNpTerm == 0x19E40AE1 && k_sceNpCheckCallback == 0x3B0AE9A9 && k_sceNpGetServiceState == 0x54060DF6, "M9A.4 selected NID differs from the app-staging trap 12 NID");
static_assert(k_sceNetCtlInetGetState == 0x6D26AC68, "M9A.4 selected NID differs from the app-staging trap 13 NID");
static_assert(k_sceAppMgrLoadExec == 0xE6774ABC, "0.0.29.3 selected NID differs from the app-staging trap UGA-1 NID");
static_assert(k_sceAppMgrIsGameProgram == 0xFFF8F7F0 && k__sceAppMgrGetAppState == 0x5E86319A, "M9A.4 selected NID differs from the app-staging trap 14 NID");
static_assert(k_sceGxmTextureValidate == 0x5331BED3, "M9A.4 selected NID differs from the app-staging trap 15 NID");
static_assert(k_sceKernelLoadStartModule == 0x2DCC4AFA && k_sceKernelStopUnloadModule == 0x2415F8A4, "M9A.4 selected NID differs from the app-staging trap 16 NID");
static_assert(k_sceAudioOutOpenPort == 0x5BC341E4 && k_sceAudioOutOutput == 0x02DB3F5F && k_sceAudioOutReleasePort == 0x69E2E6B5 && k_sceAudioOutSetVolume == 0x64167F11, "M9A.4 selected NID differs from the app-staging trap 17 NID");
static_assert(k_sceAppMgrAcquireBgmPort == 0xAFCEAB96 && k_sceAppMgrReleaseBgmPort == 0xF3717E37, "M9A.4 selected NID differs from the app-staging trap 18 NID");
static_assert(k_sceNpBasicInit == 0xEFB91A99 && k_sceNpBasicTerm == 0x389BCB3B && k_sceNpScoreInit == 0x0433069F && k_sceNpScoreTerm == 0x2050F98F && k_sceNpTusInit == 0xB2141F8D && k_sceNpMatching2Init == 0xEBB1FE74 && k_sceNpLookupInit == 0x9246A673 && k_sceNpLookupTerm == 0x0158B61B, "M9A.4 selected NID differs from the app-staging trap 19 NID");
static_assert(k_sceMotionReset == 0x0FD2CDA2 && k_sceMotionStartSampling == 0x28034AC9 && k_sceMotionGetState == 0xBDB32767, "M9A.4 selected NID differs from the app-staging trap 20 NID");
static_assert(k_sceNpTrophyInit == 0x34516838 && k_sceNpTrophyCreateContext == 0xC49FD33F && k_sceNpTrophyCreateHandle == 0x4EBC6977 && k_sceNpTrophyDestroyHandle == 0xFF142071 && k_sceNpTrophyGetTrophyUnlockState == 0xC8D2A4DE && k_sceNpTrophyGetTrophyInfo == 0xA4ADDD91, "M9A.4 selected NID differs from the app-staging trap 21 NID");
static_assert(k_sceNpTrophySetupDialogInit == 0x9E2C02C9 && k_sceNpTrophySetupDialogGetStatus == 0xC3A59547 && k_sceNpTrophySetupDialogGetResult == 0xE37069D5 && k_sceNpTrophySetupDialogTerm == 0xA81082DD && k_sceMsgDialogInit == 0x755FF270 && k_sceMsgDialogGetStatus == 0x4107019E && k_sceMsgDialogGetResult == 0xBB3BFC89 && k_sceMsgDialogClose == 0xC296D396 && k_sceMsgDialogTerm == 0x81ACF695, "M9A.4 selected NID differs from the app-staging trap 22 NID");
static_assert(k_sceAppUtilSaveDataSlotGetParam == 0x93F0D89F && k_sceAppUtilSaveDataSlotSetParam == 0x98630136 && k_sceAppUtilSaveDataGetQuota == 0xC560E716 && k_sceAppUtilSaveDataDataSave == 0x607647BA, "M9A.4 selected NID differs from the app-staging trap 23 NID");
static_assert(k_sceSaveDataDialogContinue == 0x19192C8B && k_sceSaveDataDialogTerm == 0x2192A10A && k_sceSaveDataDialogSubClose == 0x415D6068 && k_sceSaveDataDialogProgressBarSetValue == 0x5C322D1E && k_sceSaveDataDialogFinish == 0x6C49924B && k_sceSaveDataDialogGetStatus == 0x6E258046 && k_sceSaveDataDialogGetResult == 0xB2FF576E && k_sceSaveDataDialogGetSubStatus == 0xBA0542CA && k_sceSaveDataDialogInit == 0xBF5248FA, "M9A.4 selected NID differs from the app-staging trap 24 NID");
static_assert(k_sceNpTrophyUnlockTrophy == 0xB397AA24, "M9A.4 selected NID differs from the app-staging trap 25 NID");
static_assert(k_sceKernelSetGPO == 0x78E702D3 && k_scePowerSetConfigurationMode == 0x3CE187B6 && k_sceKernelPowerTick == 0x2252890C && k_sceKernelGetProcessTimeLow == 0xE9F973B1 && k_sceKernelChangeThreadPriority == 0xBD0139F2 && k_sceKernelClearEventFlag == 0x4CB87CA7 && k_sceKernelCreateCond == 0x50572FDA && k_sceKernelCreateEventFlag == 0x8516D040 && k_sceKernelCreateSema_16XX == 0x297AA2AE && k_sceKernelDelayThreadCB == 0x9C0180E1 && k_sceKernelDeleteCond == 0x879E6EBD && k_sceKernelDeleteEventFlag == 0x5840162C && k_sceKernelDeleteSema == 0xDB32948A && k_sceKernelLockMutex == 0x1D8D7945 && k_sceKernelSetEventFlag == 0xEC94DFF7 && k_sceKernelSignalCond == 0x6ED2E2DC && k_sceKernelSignalSema == 0xE6B761D1 && k_sceKernelUnlockMutex == 0x1A372EC8 && k_sceKernelWaitCond == 0xC88D44AD && k_sceKernelWaitEventFlag == 0x83C0E2AF && k_sceKernelWaitEventFlagCB == 0xE737B1DF && k_sceKernelWaitLwCondCB == 0x8FA54B07 && k_sceKernelWaitSema == 0x0C7B834B && k_sceKernelWaitSemaCB == 0x174692B4 && k_sceKernelWaitThreadEndCB == 0xC54941ED && k_sceClibMemcmp == 0x9CC2BFDF && k_sceClibMemcpy == 0x14E9DBD7 && k_sceClibMemmove == 0x736753C8 && k_sceClibMspaceCreate == 0x3B9E301A && k_sceClibMspaceDestroy == 0xAE1A21EC && k_sceClibMspaceFree == 0x9C56B4D1 && k_sceClibMspaceMalloc == 0x86EF7680 && k_sceClibMspaceMemalign == 0x3C847D57 && k_sceClibStrncasecmp == 0xB54C0BE4 && k_sceClibStrrchr == 0x6E728AAE && k_sceClibVprintf == 0x5EA3B6CE && k_sceClibVsnprintf == 0xFA6BE467 && k_sceIoLseek32 == 0x49252B9B && k_sceCtrlReadBufferPositive == 0x67E7AB83 && k_sceDisplayWaitSetFrameBuf == 0x9423560C && k_sceDisplayWaitSetFrameBufMulti == 0x7D9864A8 && k_sceAudioOutGetAdopt == 0x12FB1767 && k_sceMotionSetTiltCorrection == 0xAF09FCDB && k_sceAppUtilDrmOpen == 0x2DB7BE3B && k_sceAppUtilDrmClose == 0x6A140498, "M9A.4 selected NID differs from the app-staging trap GR-1 NID");
static_assert(k__sceGxmProgramFindParameterBySemantic == 0x7FFFDD7A && k__sceGxmSetVertexTexture == 0x16C9D339 && k_sceGxmColorSurfaceSetGammaMode == 0xF5C89643 && k_sceGxmDepthStencilSurfaceSetBackgroundDepth == 0x32F280F0 && k_sceGxmDepthStencilSurfaceSetBackgroundStencil == 0xF5D3F3E8 && k_sceGxmDepthStencilSurfaceSetForceLoadMode == 0x0C44ACD7 && k_sceGxmDepthStencilSurfaceSetForceStoreMode == 0x12AAA7AF && k_sceGxmGetRenderTargetMemSize == 0xB291C959 && k_sceGxmNotificationWait == 0x9F448E79 && k_sceGxmProgramParameterIsRegFormat == 0x871E5009 && k_sceGxmSetBackPolygonMode == 0xF66EC6FE && k_sceGxmSetCullMode == 0xE1CA72AE && k_sceGxmSetFragmentUniformBuffer == 0xEA0FC310 && k_sceGxmSetFrontDepthBias == 0xAAA97F81 && k_sceGxmSetFrontFragmentProgramEnable == 0x575958A8 && k_sceGxmSetFrontVisibilityTestEnable == 0x30459117 && k_sceGxmSetFrontVisibilityTestIndex == 0x12625C34 && k_sceGxmSetFrontVisibilityTestOp == 0xD0E3CD9A && k_sceGxmSetVertexUniformBuffer == 0xC68015E4 && k_sceGxmSetVisibilityBuffer == 0x7767EC49 && k_sceGxmTextureGetMipmapCountUnsafe == 0x4CC42929 && k_sceGxmTextureInitLinearStrided == 0x6679BEF0 && k_sceGxmTextureInitSwizzled == 0xD572D547 && k_sceGxmTextureInitTiled == 0xE6F0DB27 && k_sceGxmTextureSetData == 0x855814C4 && k_sceGxmTextureSetGammaMode == 0xA6D9F4DA && k_sceGxmTextureSetLodBias == 0xB65EE6F7 && k_sceGxmTextureSetMipFilter == 0x1CA9FE0B && k_sceGxmTextureSetUAddrModeSafe == 0x8699ECF4 && k_sceGxmTextureSetVAddrModeSafe == 0xFA22F6CC, "M9A.4 selected NID differs from the app-staging trap GR-1b NID");
static_assert(k_sceScreenShotEnable == 0x76E674D1 && k_sceScreenShotSetOverlayImage == 0x7061665B && k_sceScreenShotSetParam == 0x05DB59C7 && k__sceFiberInitializeImpl == 0xF24A298C && k_sceFiberFinalize == 0xE160F844 && k_sceFiberReturnToThread == 0x3B42921F && k_sceFiberRun == 0x7DF23243 && k_sceFiberSwitch == 0xE4283144 && k_sceNpActivityInit == 0xE0FFEE97 && k_sceNpActivityTerm == 0x9EA4901F && k_sceNpBasicCheckCallback == 0x20146AEC && k_sceNpBasicRegisterHandler == 0x26E6E048 && k_sceNpBasicUnregisterHandler == 0x050AE072 && k_sceNetCtlAdhocRegisterCallback == 0xFFA9D594 && k_sceNetCtlAdhocUnregisterCallback == 0xA4471E10 && k_sceNpManagerGetContentRatingFlag == 0xAF0073B2 && k_sceNpTrophyAbortHandle == 0xD55C6F4C && k_sceNpTrophyDestroyContext == 0x56F5CBA5 && k_sceNpTrophyTerm == 0xBFE0F28F && k_sceSaveDataDialogAbort == 0x013E7F74, "M9A.4 selected NID differs from the app-staging trap GR-2 NID");
static_assert(k__sceFiosKernelOverlayAdd == 0x6DBCF0B2 && k__sceFiosKernelOverlayAddForProcess == 0x2A381357 && k__sceFiosKernelOverlayDHChstatSync == 0xF6A3E335 && k__sceFiosKernelOverlayDHCloseSync == 0x021B4AF7 && k__sceFiosKernelOverlayDHOpenSync == 0x5D6A1CCE && k__sceFiosKernelOverlayDHReadSync == 0x2F06ADC6 && k__sceFiosKernelOverlayDHStatSync == 0x759EBEE6 && k__sceFiosKernelOverlayDHSyncSync == 0x2A9724C9 && k__sceFiosKernelOverlayGetInfo == 0xF44F3505 && k__sceFiosKernelOverlayGetInfoForProcess == 0xBC6B3CC5 && k__sceFiosKernelOverlayGetList == 0x9379E2D5 && k__sceFiosKernelOverlayGetRecommendedScheduler == 0xB02E0B26 && k__sceFiosKernelOverlayModify == 0x6D6CDE05 && k__sceFiosKernelOverlayModifyForProcess == 0x6DF2FC05 && k__sceFiosKernelOverlayRemove == 0xB4927173 && k__sceFiosKernelOverlayRemoveForProcess == 0xF8277E07 && k__sceFiosKernelOverlayResolveSync == 0xE9AE60FB && k__sceFiosKernelOverlayResolveWithRangeSync == 0x8CCA471A && k__sceFiosKernelOverlayThreadIsDisabled == 0x629F4FE4 && k__sceFiosKernelOverlayThreadSetDisabled == 0x3E9172EA, "M9A.4 selected NID differs from the app-staging trap GR-3 NID");
static_assert(k_sceAudiodecClearContext == 0xF72F9B64 && k_sceAudiodecCreateDecoder == 0x4DFD3AAA && k_sceAudiodecCreateDecoderExternal == 0x56085DFB && k_sceAudiodecDecode == 0xCCDABA04 && k_sceAudiodecDeleteDecoder == 0xE7A24E16 && k_sceAudiodecDeleteDecoderExternal == 0xE4EA05BB && k_sceAudiodecGetContextSize == 0xDB712ABC && k_sceAudiodecInitLibrary == 0x445C2CEF && k_sceAudiodecTermLibrary == 0x45719B9D, "M9A.4 selected NID differs from the app-staging trap GR-5 NID");
static_assert(k_sceHttpsLoadCert == 0xAE8D7C33 && k_sceHttpsUnloadCert == 0x8577833F && k_sceHttpCreateTemplate == 0x62241DAB && k_sceHttpDeleteTemplate == 0xEC85ECFB && k_sceHttpSetConnectTimeOut == 0x237CA86E && k_sceHttpSetRecvTimeOut == 0x94BF196E && k_sceHttpSetSendTimeOut == 0x8AE3F008 && k_sceHttpSetAutoRedirect == 0x34891C3F && k_sceHttpSetAuthEnabled == 0x11EC42D0 && k_sceHttpsEnableOption == 0x9FBE2869 && k_sceHttpsDisableOption == 0xC6D60403 && k_sceHttpsGetSslError == 0x2B79BDE0 && k_sceHttpGetLastErrno == 0x9A68C3DE, "M9A.4 selected NID differs from the app-staging trap GR-6 NID");
static_assert(k_sceAvcdecCreateDecoder == 0xE82BB69B && k_sceAvcdecDecode == 0xD6190A06 && k_sceAvcdecDecodeFlush == 0x25F31020 && k_sceAvcdecDecodeStop == 0x9648D853 && k_sceAvcdecDeleteDecoder == 0x8A0E359E && k_sceAvcdecQueryDecoderMemSize == 0x97E95EDB && k_sceCodecEngineAllocMemoryFromUnmapMemBlock == 0x1A3A53E5 && k_sceCodecEngineCloseUnmapMemBlock == 0x95EA3B3E && k_sceCodecEngineFreeMemoryFromUnmapMemBlock == 0x6A31831D && k_sceCodecEngineOpenUnmapMemBlock == 0xF0B4C892 && k_sceJpegDecodeMJpeg == 0x6215B095 && k_sceJpegFinishMJpeg == 0x62842598 && k_sceJpegGetOutputInfo == 0x353BA9B0 && k_sceJpegInitMJpeg == 0xB030773B && k_sceVideodecInitLibraryWithUnmapMem == 0x230A78FF && k_sceVideodecQueryMemSize == 0x6731BB2D && k_sceVideodecTermLibrary == 0x3A5F4924, "M9A.4 selected NID differs from the app-staging trap GR-7 NID");
static_assert(k_sceMp4OpenFile == 0x05474AF0 && k_sceMp4GetNextUnit == 0x20A01112 && k_sceMp4StartFileStreaming == 0x30E49E4D && k_sceMp4GetNextUnit2Ref == 0x32A15788 && k_sceMp4Reset == 0x40351E1A && k_sceMp4GetNextUnit3Ref == 0x40C64DC0 && k_sceMp4EnableStream == 0x609E57AD && k_sceMp4TimeToPTS == 0x6B69F900 && k_sceMp4ReleaseBuffer == 0x7B4832FE && k_sceMp4GetStreamInfo == 0x8BE0E3D3 && k_sceMp4CloseFile == 0x920623C8 && k_sceMp4JumpPTS == 0x96676BA0 && k_sceMp4PTSToTime == 0xA7C09746 && k_sceMp4GetNextUnitData == 0xD092A066, "M9A.4 selected NID differs from the app-staging trap GR-9 NID");
static_assert(k_sceClibStrnlen == 0xAC595E68, "M9A.4 selected NID differs from the app-staging trap GR-mp4-1 NID");
static_assert(k_sceKernelGetProcessTitleId == 0x03A48771, "M9A.4 selected NID differs from the app-staging trap GR-mp4-2 NID");
static_assert(k_sceClibLookCtypeTable == 0xD8EBBB7E && k_sceClibMspaceCalloc == 0x678374AD && k_sceClibMspaceRealloc == 0x774891D6 && k_sceClibStrchr == 0x614076B7 && k_sceClibStrcmp == 0xA2FB4D9D && k_sceClibStrlcat == 0x70CBC2D5 && k_sceClibStrlcpy == 0x2CDFCD1C && k_sceClibStrstr == 0xE265498B && k_sceClibStrtoll == 0x2E581B88 && k_sceClibTolower == 0x4C5471BC && k_sceClibToupper == 0xB0335388, "M9A.4 selected NID differs from the app-staging trap GR-mp4-3 NID");
static_assert(k_sceKernelCloseMutex == 0x03E23AF6 && k_sceKernelDeleteMutex == 0xCB78710D && k_sceKernelTryLockMutex == 0x72FC1F54, "M9A.4 selected NID differs from the app-staging trap GR-mp4-4 NID");
static_assert(k_ksceKernelGetThreadCurrentPriority == 0x01414F0B, "M9A.4 selected NID differs from the app-staging trap GR-play-1 NID");
static_assert(k_sceAppUtilSaveDataDataRemove == 0xD1C6AB8E && k_sceImeDialogAbort == 0x594A220E && k_sceGxmSetFrontPointLineWidth == 0x06752183 && k_sceGxmTextureInitCube == 0x11DC8DC9 && k_sceGxmTextureGetUAddrMode == 0x2AE22788 && k_sceGxmTextureGetVAddrMode == 0x46136CA9 && k_sceGxmRenderTargetGetDriverMemBlock == 0x49553737 && k_sceGxmTextureInitSwizzledArbitrary == 0x5DBFBA2C && k_sceGxmColorSurfaceGetScaleMode == 0x6E3FA74D && k_sceGxmSetBackPointLineWidth == 0x8DCB0EDB && k_sceGxmShaderPatcherGetProgramFromId == 0xA949A803 && k_sceGxmTextureInitCubeArbitrary == 0xE3DF5E3B && k_sceGxmColorSurfaceGetStrideInPixels == 0xF33D9980 && k_sceGxmColorSurfaceGetFormat == 0xF3C1C6C6 && k_sceDisplayGetFrameBuf == 0x42AE6BBC && k_sceDisplayWaitVblankStartMultiCB == 0x05F27764 && k_sceDisplayRegisterVblankStartCallback == 0x6BDF4C4D && k_sceDisplayWaitVblankStartCB == 0x78B41B92 && k_sceDisplayGetVcount == 0xB6FDE0BA && k_sceTouchRead == 0x169A1D58 && k_sceAvPlayerResume == 0x11FF162D && k_sceAvPlayerIsActive == 0x2E4FF35F && k_sceAvPlayerPause == 0x34FD744B && k_sceAvPlayerInit == 0x4C847ADF && k_sceAvPlayerClose == 0x7AA29B2D && k_sceAvPlayerGetVideoData == 0x8BC3221B && k_sceAvPlayerGetAudioData == 0xA10FC252 && k_sceAvPlayerAddSource == 0xBC83C8FD && k_sceAvPlayerStop == 0xBD35E360 && k_sceRtcGetCurrentClockLocalTime == 0x0572EDDC && k_sceLiveAreaUpdateFrameSync == 0xCFCEDE95 && k_sceAppMgrReceiveSystemEvent == 0x10B5765F && k_sceImeOpen == 0x0E050613 && k_sceImeUpdate == 0x71D6898A && k_sceImeClose == 0x889A8421 && k_sceNgsRackInit == 0x0A92E4EC && k_sceNgsVoiceDefGetCompressorBuss == 0x0E0ACB68 && k_sceNgsVoiceKill == 0x0E291AAD && k_sceNgsVoiceDefGetAtrac9Voice == 0x14EF65A0 && k_sceNgsVoiceSetModuleCallback == 0x24E909A8 && k_sceNgsVoiceUnlockParams == 0x3D46D8A7 && k_sceNgsRackGetRequiredMemorySize == 0x477318C0 && k_sceNgsSystemRelease == 0x4A25BEBC && k_sceNgsVoiceGetParamsOutOfRange == 0x4CBE08F3 && k_sceNgsVoiceDefGetDelayBuss == 0x4D705E3E && k_sceNgsVoiceResume == 0x54CFB981 && k_sceNgsVoiceGetInfo == 0x5551410D && k_sceNgsSystemUpdate == 0x684F080C && k_sceNgsSystemGetRequiredMemorySize == 0x6CE8B36F && k_sceNgsVoiceDefGetMasterBuss == 0x79A121D1 && k_sceNgsPatchGetInfo == 0x98703DBC && k_sceNgsVoiceBypassModule == 0x9AB87E71 && k_sceNgsVoiceDefGetReverbBuss == 0x9DCF50F5 && k_sceNgsVoicePatchSetVolumesMatrix == 0xA0F5402D && k_sceNgsVoiceDefGetDistortionBuss == 0xAAD90DEB && k_sceNgsVoiceLockParams == 0xAB6BEF8F && k_sceNgsVoiceKeyOff == 0xBB13373D && k_sceNgsVoiceGetStateData == 0xC9B8C0B4 && k_sceNgsPatchRemoveRouting == 0xD0C9AE5A && k_sceNgsPatchCreateRouting == 0xD668B49C && k_sceNgsVoicePause == 0xD7786E99 && k_sceNgsRackRelease == 0xDD5CA10B && k_sceNgsVoiceDefGetMixerBuss == 0xE0AC8776 && k_sceNgsVoiceDefGetTemplate1 == 0xE9B572B7 && k_sceNgsSystemInit == 0xED14CF4A && k_sceNgsVoiceDefGetEqBuss == 0xF964120E && k_sceNgsVoicePlay == 0xFA0A0F34 && k_sceNgsVoiceDefGetPitchShiftBuss == 0xFBE515D4 && k_sceNgsRackGetVoiceHandle == 0xFE1A98E9 && k_sceKernelLockLwMutexCB == 0x3148C6B6 && k___sce_aeabi_idiv0 == 0x4373B548 && k_sceKernelGetThreadExitStatus == 0xD5DC26C4 && k___sce_aeabi_ldiv0 == 0xFB235848 && k_sceKernelCreateCallback == 0xB19CF7E9 && k_sceKernelCheckCallback == 0xE53E41F6, "M9A.4 selected NID differs from the app-staging trap DR-1 NID");
static_assert(k__sceFiberAttachContextAndSwitch == 0xE00B9AFE && k__sceFiberInitializeWithInternalOptionImpl == 0xC6A3F9BB && k_sceFiberGetInfo == 0x189599B4 && k_sceFiosOverlayAddForProcess02 == 0x6C4BE9CD && k_sceFiosOverlayGetInfoForProcess02 == 0xAB7B4213 && k_sceFiosOverlayGetRecommendedScheduler02 == 0xF5C1F928 && k_sceFiosOverlayModifyForProcess02 == 0xCA388053 && k_sceFiosOverlayRemoveForProcess02 == 0xE5D1B6F5 && k_sceFiosOverlayResolveWithRangeSync02 == 0x61C4AAC4 && k_sceFiosOverlayThreadSetDisabled02 == 0x70321220 && k_sceIoChstat == 0x29482F7F && k_sceIoDevctl == 0x04B30CB2 && k_sceIoIoctl == 0x54ABACFA && k_sceIoPwrite == 0x8FFFF5A8 && k_sceIoRename == 0xF737E369 && k_sceIoRmdir == 0xE9F91EC8 && k_sceIoSync == 0x98ACED6D && k_sceIoSyncByFd == 0x16512F59 && k_sceKernelCallAbortHandler == 0xEB6E50BB && k_sceKernelCallModuleExit == 0x15E2A45D && k_sceKernelSendSignal == 0xD4C367B2 && k_sceKernelUnregisterThreadEventHandler == 0x2C8ED6F0 && k_sceKernelWaitSignal == 0xADCA94E5, "M9A.4 selected NID differs from the app-staging trap DR-2 NID");
static_assert(k_sceAppUtilStoreBrowse == 0x85FA94EE && k_sceAvcdecDecodeAvailableSize == 0x441673E3 && k_sceFiberGetSelf == 0x414D8CA5 && k_sceGxmColorSurfaceSetClip == 0x86456F7B && k_sceGxmDrawPrecomputed == 0xED3F78B8 && k_sceGxmFragmentProgramGetProgram == 0xE0E3B3F8 && k_sceGxmGetPrecomputedDrawSize == 0x41BBD792 && k_sceGxmGetPrecomputedFragmentStateSize == 0x85DE8506 && k_sceGxmGetPrecomputedVertexStateSize == 0x9D83CA3B && k_sceGxmPrecomputedDrawInit == 0xA197F096 && k_sceGxmPrecomputedDrawSetParams == 0x884D0D08 && k_sceGxmPrecomputedDrawSetVertexStream == 0x6C936214 && k_sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer == 0xCECB584A && k_sceGxmPrecomputedFragmentStateInit == 0xE297D7AF && k_sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer == 0x91236858 && k_sceGxmPrecomputedFragmentStateSetTexture == 0x29118BF1 && k_sceGxmPrecomputedVertexStateGetDefaultUniformBuffer == 0xBE5A68EF && k_sceGxmPrecomputedVertexStateInit == 0xBE937F8D && k_sceGxmPrecomputedVertexStateSetDefaultUniformBuffer == 0x34BF64E3 && k_sceGxmPrecomputedVertexStateSetTexture == 0x6A29EB06 && k_sceGxmPrecomputedVertexStateSetUniformBuffer == 0xDBF97ED6 && k_sceGxmSetPrecomputedFragmentState == 0xF8952750 && k_sceGxmSetPrecomputedVertexState == 0xB7626A93 && k_sceGxmSetVertexTexture == 0x9EB4380F && k_sceGxmTextureGetMipmapCount == 0xF7B7B1E4 && k_sceGxmTextureGetType == 0xF65D4917 && k_sceGxmVertexProgramGetProgram == 0xBC52320E && k_sceKernelCreateSema == 0x1BD67366 && k_sceKernelCreateTimer == 0x2255B2A5 && k_sceKernelDeleteTimer == 0xAB1E42C4 && k_sceKernelGetSemaInfo == 0x595D3FA6 && k_sceKernelGetTimerTimeWide == 0x3EFD3165 && k_sceKernelStartTimer == 0x48091E0C && k_sceKernelStopTimer == 0x869E9F20 && k_sceNpActivityPostStatus == 0xBC7FDC77 && k_sceNpManagerGetChatRestrictionFlag == 0x60C575B1 && k_sceNpManagerGetNpId == 0x3C94B4B4 && k_sceNpRegisterServiceStateCallback == 0x44239C35 && k_sceNpScoreCreateRequest == 0xD30D1993 && k_sceNpScoreCreateTitleCtx == 0x5685F225 && k_sceNpScoreDeleteRequest == 0xFFF24BB1 && k_sceNpScoreDeleteTitleCtx == 0xF52EA88A && k_sceNpScoreGetGameDataAsync == 0xCE416993 && k_sceNpScoreGetRankingByRangeAsync == 0xC45E3FCD && k_sceNpScorePollAsync == 0x9F2A7AC9 && k_sceNpScoreRecordGameDataAsync == 0x40573917 && k_sceNpScoreRecordScoreAsync == 0x24B09634 && k_sceTouchGetSamplingState == 0x26531526 && k_sceVideodecInitLibrary == 0xF1AF65A3, "M9A.4 selected NID differs from the app-staging trap P4-1 NID");
static_assert(k__ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE == 0xA3EA44E9 && k__ZN3sce4Json11Initializer9terminateEv == 0x30AD8C6A && k__ZN3sce4Json11InitializerC1Ev == 0x5405EE04 && k__ZN3sce4Json11InitializerD1Ev == 0xA19AA8B4 && k__ZN3sce4Json12MemAllocator11notifyErrorEijPv == 0x38C51D0C && k__ZN3sce4Json12MemAllocatorC2Ev == 0x5EC4F6CB && k__ZN3sce4Json12MemAllocatorD2Ev == 0x47A56DF5 && k__ZN3sce4Json5Array8iteratorD1Ev == 0x866C0131 && k__ZN3sce4Json5Array8iteratorppEi == 0x883F0120 && k__ZN3sce4Json5Array9push_backERKNS0_5ValueE == 0x21AE1E00 && k__ZN3sce4Json5ArrayC1Ev == 0x8090B2D1 && k__ZN3sce4Json5ArrayD1Ev == 0xAA0C1A25 && k__ZN3sce4Json5ArrayaSERKS1_ == 0xBFE9F993 && k__ZN3sce4Json5Value3setERKNS0_6ObjectE == 0x8725FD85 && k__ZN3sce4Json5Value5clearEv == 0xC9813AC7 && k__ZN3sce4Json5Value9serializeERNS0_6StringE == 0xCFC4D75F && k__ZN3sce4Json5ValueC1ERKNS0_5ArrayE == 0x0E917571 && k__ZN3sce4Json5ValueC1ERKNS0_6StringE == 0xB7582884 && k__ZN3sce4Json5ValueC1Eb == 0x7A6FE666 && k__ZN3sce4Json5ValueC1Ev == 0x7A9DECC0 && k__ZN3sce4Json5ValueC1Ex == 0x1B23D989 && k__ZN3sce4Json5ValueD1Ev == 0xE07648EC && k__ZN3sce4Json5ValueaSERKS1_ == 0x705ED2BB && k__ZN3sce4Json6ObjectC1Ev == 0xB669382C && k__ZN3sce4Json6ObjectD1Ev == 0x3CF6AF04 && k__ZN3sce4Json6ObjectixERKNS0_6StringE == 0x8F2E1720 && k__ZN3sce4Json6Parser5parseERNS0_5ValueEPKcj == 0x59B1A5B8 && k__ZN3sce4Json6StringC1EPKc == 0x70703FC7 && k__ZN3sce4Json6StringC1Ev == 0x692125B6 && k__ZN3sce4Json6StringD1Ev == 0xB70F12A8 && k__ZNK3sce4Json5Array3endEv == 0x0F533297 && k__ZNK3sce4Json5Array4sizeEv == 0x9DC07B95 && k__ZNK3sce4Json5Array5beginEv == 0x82074D93 && k__ZNK3sce4Json5Array8iteratordeEv == 0x81EF83DC && k__ZNK3sce4Json5Array8iteratorneES2_ == 0x5467A38D && k__ZNK3sce4Json5Value10getBooleanEv == 0x7DDFCAE2 && k__ZNK3sce4Json5Value11getUIntegerEv == 0x35EAAEDA && k__ZNK3sce4Json5Value7getTypeEv == 0xA95EF0EC && k__ZNK3sce4Json5Value8getArrayEv == 0x000926DB && k__ZNK3sce4Json5Value9getStringEv == 0xBC3391BE && k__ZNK3sce4Json5ValueixEPKc == 0xD5B3E509 && k__ZNK3sce4Json5ValueixEj == 0x3CE5DEC7 && k__ZNK3sce4Json6String5c_strEv == 0x327D74CE && k__ZNK3sce4Json6String6lengthEv == 0x35F974C1 && k_sceAppUtilAppEventParseSessionInvitation == 0xA58E6FCE && k_sceAppUtilReceiveAppEvent == 0xEE0DBED9 && k_sceCommonDialogIsRunning == 0x043A353E && k_sceGameUpdateInit == 0x2F401247 && k_sceGameUpdateRun == 0x3C616238 && k_sceGameUpdateTerm == 0x48D879BA && k_sceGxmColorSurfaceInitDisabled == 0x613639FA && k_sceGxmTransferCopy == 0x62312BF8 && k_sceInvitationDialogGetResult == 0xEA14FB1E && k_sceInvitationDialogGetStatus == 0x5B735F60 && k_sceInvitationDialogInit == 0x8723A5F2 && k_sceInvitationDialogTerm == 0x4136382C && k_sceNetAdhocMatchingCancelTarget == 0x04FF010C && k_sceNetAdhocMatchingCreate == 0x7BAD7EA2 && k_sceNetAdhocMatchingDelete == 0x7BCDD889 && k_sceNetAdhocMatchingGetMembers == 0x85B23CEB && k_sceNetAdhocMatchingInit == 0x8E0E0EAE && k_sceNetAdhocMatchingSelectTarget == 0x9A6B1D0F && k_sceNetAdhocMatchingSendData == 0x83C0E435 && k_sceNetAdhocMatchingStart == 0xF13E17BE && k_sceNetAdhocMatchingStop == 0x63ABB632 && k_sceNetAdhocMatchingTerm == 0x6E195CD1 && k_sceNetCtlAdhocDisconnect == 0xED43B79A && k_sceNetCtlAdhocGetInAddr == 0x7118C99D && k_sceNetCtlAdhocGetPeerList == 0x77586C59 && k_sceNetCtlInetGetInfo == 0xB26D07F3 && k_sceNetGetStatisticsInfo == 0xA86F8FE5 && k_sceNetHtons == 0x9FA3207B && k_sceNetTerm == 0xEA3CC286 && k_sceNpBasicRecordPlaySessionLog == 0x3B0A7F47 && k_sceNpFriendList2DialogGetResult == 0xC0CF1ED4 && k_sceNpFriendList2DialogGetStatus == 0xAC735AF9 && k_sceNpFriendList2DialogInit == 0xBBCDF66D && k_sceNpFriendList2DialogTerm == 0x05B18EA3 && k_sceNpMatching2AbortContextStart == 0xF2847E3B && k_sceNpMatching2ContextStart == 0xBB2E7559 && k_sceNpMatching2ContextStop == 0x506454DE && k_sceNpMatching2CreateContext == 0xADF578E1 && k_sceNpMatching2CreateJoinRoom == 0x818A9499 && k_sceNpMatching2DestroyContext == 0x368AA759 && k_sceNpMatching2GetRoomDataExternalList == 0x5BE8B86F && k_sceNpMatching2GetRoomMemberDataInternal == 0x28C643B7 && k_sceNpMatching2GetServerLocal == 0x65C0FEED && k_sceNpMatching2GetUserInfoList == 0x78521326 && k_sceNpMatching2GetWorldInfoList == 0xC086B560 && k_sceNpMatching2JoinRoom == 0x33F7D5AE && k_sceNpMatching2KickoutRoomMember == 0xA8021394 && k_sceNpMatching2LeaveRoom == 0xC8B0C9EE && k_sceNpMatching2RegisterContextCallback == 0xF9E35566 && k_sceNpMatching2RegisterLobbyEventCallback == 0x3C507D4A && k_sceNpMatching2RegisterLobbyMessageCallback == 0x4E3193D2 && k_sceNpMatching2RegisterRoomEventCallback == 0xF486991B && k_sceNpMatching2RegisterRoomMessageCallback == 0xFA51949B && k_sceNpMatching2RegisterSignalingCallback == 0x1C60BC5B && k_sceNpMatching2SearchRoom == 0xD48BAF13 && k_sceNpMatching2SendRoomMessage == 0x7B908D99 && k_sceNpMatching2SetDefaultRequestOptParam == 0xF3A43C50 && k_sceNpMatching2SetRoomDataExternal == 0x8F88AC7E && k_sceNpMatching2SetRoomDataInternal == 0x94784A8C && k_sceNpMatching2SetRoomMemberDataInternal == 0xD3096C55 && k_sceNpMatching2SignalingGetConnectionInfo == 0x20598618 && k_sceNpMatching2SignalingGetConnectionStatus == 0x4E4C55BD && k_sceNpMatching2SignalingGetPingInfo == 0x881F7428 && k_sceNpMatching2Term == 0x0124641C && k_sceNpScoreAbortRequest == 0xA7E93CE1 && k_sceNpScoreCensorComment == 0xA0C94D46 && k_sceNpScoreGetBoardInfo == 0x00F90E7B && k_sceNpScoreGetFriendsRanking == 0x4F795659 && k_sceNpScoreGetGameData == 0xDFAD64D3 && k_sceNpScoreGetRankingByNpId == 0xBAE55B34 && k_sceNpScoreGetRankingByNpIdAsync == 0x45CD1D00 && k_sceNpScoreGetRankingByRange == 0x427D3412 && k_sceNpScoreRecordGameData == 0xC2862B67 && k_sceNpScoreRecordScore == 0x320C0277 && k_sceNpScoreSanitizeComment == 0x6FD2041A && k_sceNpTusAbortRequest == 0x2C5F1162 && k_sceNpTusCreateRequest == 0x99DC7420 && k_sceNpTusCreateTitleCtx == 0xF1D9529E && k_sceNpTusDeleteRequest == 0xB46B5D7F && k_sceNpTusDeleteTitleCtx == 0xD53D3692 && k_sceNpTusGetDataAsync == 0x883AC44A && k_sceNpTusPollAsync == 0xC003C588 && k_sceNpTusSetDataAsync == 0x9FABA1E5 && k_sceNpTusTerm == 0x7EDC33B3 && k_sceNpWebApiCreateRequest == 0x6FC7357E && k_sceNpWebApiDeleteRequest == 0x57B2826E && k_sceNpWebApiInitialize == 0x8519EB34 && k_sceNpWebApiReadData == 0xBA75BF6E && k_sceNpWebApiSendRequest2 == 0x1E37862A && k_sceNpWebApiSetNpTitleId == 0x9AE6BC46 && k_sceNpWebApiTerminate == 0x001254C2 && k_sceRtcFormatRFC3339LocalTime == 0x742250A9 && k_sceRtcGetCurrentNetworkTick == 0xCDDD25FE && k_sceRtcGetTick == 0xF2B238E2 && k_sceRtcSetTick == 0xCD89F464 && k_sceRudpActivate == 0xF1151D91 && k_sceRudpBind == 0x8B766108 && k_sceRudpCreateContext == 0x772C07E8 && k_sceRudpEnableInternalIOThread == 0x3A495E69 && k_sceRudpEnd == 0x3EA0961E && k_sceRudpGetContextStatus == 0xD620B557 && k_sceRudpGetOption == 0xE68005E3 && k_sceRudpGetSizeReadable == 0x6B74CA22 && k_sceRudpInit == 0x22C9AA29 && k_sceRudpInitiate == 0x46B635BD && k_sceRudpPollControl == 0x18668BD8 && k_sceRudpPollCreate == 0x943D96EC && k_sceRudpPollDestroy == 0xC9ACBDF0 && k_sceRudpPollWait == 0x55918C7C && k_sceRudpRead == 0x30A9438E && k_sceRudpSetEventHandler == 0x396A24D6 && k_sceRudpSetOption == 0xBD87A5CA && k_sceRudpTerminate == 0x552B2D3E && k_sceRudpWrite == 0x69C9DE15 && k_sceScreenShotDisable == 0x50AE9FF9 && k_sceVoiceQoSConnect == 0xE0C5CEEE && k_sceVoiceQoSCreateLocalEndpoint == 0xAAB54BE4 && k_sceVoiceQoSCreateRemoteEndpoint == 0xBAB98727 && k_sceVoiceQoSDeleteLocalEndpoint == 0x68FABF6F && k_sceVoiceQoSDeleteRemoteEndpoint == 0xC2F2C771 && k_sceVoiceQoSDisconnect == 0x3C7A08B0 && k_sceVoiceQoSGetConnectionAttribute == 0xE81B8D44 && k_sceVoiceQoSGetLocalEndpointAttribute == 0xC981AB3B && k_sceVoiceQoSReadPacket == 0x2D613549 && k_sceVoiceQoSSetConnectionAttribute == 0xE757806F && k_sceVoiceQoSSetLocalEndpointAttribute == 0x540CEBA5 && k_sceVoiceQoSWritePacket == 0x2FE1F28F, "M9A.4 selected NID differs from the app-staging trap CFB-1 NID");

#define X(name) { k_##name, #name, &import_##name },
const VitoxSelectedImport kTable[] = { VITOX_SELECTED_IMPORTS(X) };
#undef X

std::mutex g_stats_mutex;
std::map<uint32_t, std::pair<unsigned, bool>> g_stats; // nid -> (calls, resolved)

} // namespace

const VitoxSelectedImport *vitox_resolve_selected_import(uint32_t nid) {
    for (const auto &e : kTable) // the selected set is small (< 64)
        if (e.nid == nid)
            return &e;
    return nullptr;
}

void vitox_init_selected_libraries(EmuEnvState &emuenv) {
    // Vita3K's init_libraries() runs every LIBRARY_INIT in library_init_list.inc; the selected
    // modules need only SceSysmem's (creates SysmemState in kernel.obj_store). SceGxm/SceDisplay have none.
    import_library_init_SceSysmem(emuenv);
    // M9A.6: SceFiber (FiberState), SceAudiodec (AudiodecState); the other upstream LIBRARY_INITs are SceSharedFb, taihen - not linked
    import_library_init_SceFiber(emuenv);
    import_library_init_SceAudiodec(emuenv);
}

static std::map<int, uint32_t> g_last_nid; // M8D: per guest thread, the most recent import NID (shutdown diagnostics)

void vitox_note_import_call(uint32_t nid, int thread_id, bool resolved) {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    auto &s = g_stats[nid];
    ++s.first;
    s.second = resolved;
    g_last_nid[thread_id] = nid;
}

uint32_t vitox_last_import_nid(int thread_id) {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    const auto it = g_last_nid.find(thread_id);
    return it == g_last_nid.end() ? 0 : it->second;
}

// ---- M7B.6 DIAGNOSTIC (read-only): the guest arguments / data / GXM context state behind VITOX0003's draws ----
void vitox_observe_import_args(EmuEnvState &emuenv, CPUState &cpu, uint32_t nid) {
    static std::atomic<uint64_t> n_draw{ 0 }, n_uni{ 0 }, n_vs{ 0 };
    const auto sample = [](uint64_t n) { return n <= 3 || n == 60 || n == 600 || n == 2000; };
    MemState &mem = emuenv.mem;
    const auto arg5 = [&]() { return *Ptr<uint32_t>(read_sp(cpu)).get(mem); }; // 5th AAPCS argument: first stack word
    if (nid == k_sceGxmDepthStencilSurfaceInit) {
        // M7B.7: the explicit depth-stencil surface VITOX0003 creates (arguments as passed, before the real call)
        const uint32_t sp = read_sp(cpu);
        LOG_INFO("Vitox [M7B.7] guest sceGxmDepthStencilSurfaceInit: surface 0x{:08X} format 0x{:08X} type 0x{:08X} strideInSamples {} depthData 0x{:08X} "
                 "stencilData 0x{:08X}",
            read_reg(cpu, 0), read_reg(cpu, 1), read_reg(cpu, 2), read_reg(cpu, 3), *Ptr<uint32_t>(sp).get(mem), *Ptr<uint32_t>(sp + 4).get(mem));
        return;
    }
#ifndef VITOX_DIAG_M7B
    return; // M8A: the per-call argument logging below is a VITOX_DIAG_M7B diagnostic
#endif
    if (nid == k_sceGxmSetVertexStream) {
        const uint64_t n = ++n_vs;
        if (sample(n))
            LOG_INFO("Vitox [M7B.6] guest sceGxmSetVertexStream #{}: ctx 0x{:08X} stream {} data 0x{:08X}", n, read_reg(cpu, 0), read_reg(cpu, 1), read_reg(cpu, 2));
    } else if (nid == k_sceGxmSetUniformDataF) {
        const uint64_t n = ++n_uni;
        if (!sample(n))
            return;
        const uint32_t ub = read_reg(cpu, 0), param = read_reg(cpu, 1), off = read_reg(cpu, 2), count = read_reg(cpu, 3), src = arg5();
        std::string v;
        const float *f = src ? Ptr<const float>(src).get(mem) : nullptr;
        for (uint32_t i = 0; f && i < count && i < 16; i++)
            v += fmt::format("{} ", f[i]);
        LOG_INFO("Vitox [M7B.6] guest sceGxmSetUniformDataF #{}: uniformBuffer 0x{:08X} parameter 0x{:08X} componentOffset {} count {} source 0x{:08X} values [ {}]", n, ub,
            param, off, count, src, v);
    } else if (nid == k_sceGxmDraw) {
        const uint64_t n = ++n_draw;
        if (!sample(n))
            return;
        const uint32_t ctx = read_reg(cpu, 0), prim = read_reg(cpu, 1), itype = read_reg(cpu, 2), idata = read_reg(cpu, 3), icount = arg5();
        std::vector<uint32_t> idx;
        for (uint32_t i = 0; idata && i < icount && i < 64; i++)
            idx.push_back(itype == SCE_GXM_INDEX_FORMAT_U32 ? Ptr<const uint32_t>(idata).get(mem)[i] : Ptr<const uint16_t>(idata).get(mem)[i]);
        std::string is;
        for (uint32_t i : idx)
            is += fmt::format("{} ", i);
        LOG_INFO("Vitox [M7B.6] guest sceGxmDraw #{}: ctx 0x{:08X} primitive {} indexType 0x{:08X} indexData 0x{:08X} indexCount {} indices [ {}]", n, ctx, prim, itype, idata,
            icount, is);
        // SceGxmContext (modules/SceGxm/SceGxm.cpp) starts with its GxmContextState: read it in place, never written
        const GxmContextState &st = *reinterpret_cast<const GxmContextState *>(Ptr<const uint8_t>(ctx).get(mem));
        LOG_INFO("Vitox [M7B.6]   ctx viewport enable {} offset ({} {} {}) scale ({} {} {}) | region clip mode {} min ({},{}) max ({},{}) | cull {} two_sided {} | polygon "
                 "front {} back {}",
            static_cast<int>(st.viewport.enable), st.viewport.offset.x, st.viewport.offset.y, st.viewport.offset.z, st.viewport.scale.x, st.viewport.scale.y,
            st.viewport.scale.z, static_cast<int>(st.region_clip_mode), st.region_clip_min.x, st.region_clip_min.y, st.region_clip_max.x, st.region_clip_max.y,
            static_cast<int>(st.cull_mode), static_cast<int>(st.two_sided), static_cast<int>(st.front_polygon_mode), static_cast<int>(st.back_polygon_mode));
        LOG_INFO("Vitox [M7B.6]   ctx depth func front {} back {} | depth write front {} back {} (GXM: 0=NEVER 1=LESS 2=EQUAL 3=LESS_EQUAL 4=GREATER 7=ALWAYS)",
            static_cast<int>(st.front_depth_func), static_cast<int>(st.back_depth_func), static_cast<int>(st.front_depth_write_enable),
            static_cast<int>(st.back_depth_write_enable));
        const GxmStencilState *ss[2] = { &st.front_stencil, &st.back_stencil };
        for (int i = 0; i < 2; i++)
            LOG_INFO("Vitox [M7B.6]   ctx stencil {}: func {} fail {} depth_fail {} depth_pass {} compare 0x{:02X} write 0x{:02X} ref {}", i ? "back" : "front",
                static_cast<int>(ss[i]->func), static_cast<int>(ss[i]->stencil_fail), static_cast<int>(ss[i]->depth_fail), static_cast<int>(ss[i]->depth_pass),
                ss[i]->compare_mask, ss[i]->write_mask, ss[i]->ref);
        const SceGxmColorSurface &cs = st.color_surface;
        LOG_INFO("Vitox [M7B.6]   ctx colour surface: disabled {} {}x{} stride {} px data 0x{:08X} colorFormat 0x{:08X} type {}", static_cast<int>(cs.disabled), cs.width,
            cs.height, cs.strideInPixels, cs.data.address(), static_cast<uint32_t>(cs.colorFormat), static_cast<int>(cs.surfaceType));
        const SceGxmDepthStencilSurface &dss = st.depth_stencil_surface;
        LOG_INFO("Vitox [M7B.6]   ctx depth-stencil surface: disabled {} depth_data 0x{:08X} stencil_data 0x{:08X} background_depth {} force_load {} force_store {} "
                 "stencil {}",
            dss.disabled(), dss.depth_data.address(), dss.stencil_data.address(), dss.background_depth, static_cast<int>(dss.force_load),
            static_cast<int>(dss.force_store), static_cast<int>(dss.stencil));
        const SceGxmVertexProgram *vp = st.vertex_program ? st.vertex_program.get(mem) : nullptr;
        uint32_t stride = 0;
        if (vp) {
            for (size_t s = 0; s < vp->streams.size(); s++)
                LOG_INFO("Vitox [M7B.6]   vertex program stream {}: stride {} indexSource {} | data 0x{:08X}", s, vp->streams[s].stride, vp->streams[s].indexSource,
                    s < st.stream_data.size() ? st.stream_data[s].address() : 0);
            for (const SceGxmVertexAttribute &a : vp->attributes)
                LOG_INFO("Vitox [M7B.6]   vertex attribute: stream {} offset {} format {} components {} regIndex {}", a.streamIndex, a.offset, static_cast<int>(a.format),
                    a.componentCount, a.regIndex);
            stride = vp->streams.empty() ? 0 : vp->streams[0].stride;
        }
        const Address vb = st.stream_data[0].address();
        const std::set<uint32_t> seen(idx.begin(), idx.end());
        for (uint32_t i : seen) {
            if (!vb || !stride || stride > 64)
                break;
            const float *f = Ptr<const float>(vb + i * stride).get(mem);
            std::string fs;
            for (uint32_t k = 0; k < stride / 4; k++)
                fs += fmt::format("{} ", f[k]);
            LOG_INFO("Vitox [M7B.6]   vertex {} @0x{:08X}: floats [ {}]", i, vb + i * stride, fs);
        }
    }
}

unsigned vitox_import_stats(VitoxImportStat *out, unsigned max) {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    unsigned n = 0;
    for (const auto &[nid, s] : g_stats) {
        if (n == max)
            break;
        const VitoxSelectedImport *imp = vitox_resolve_selected_import(nid);
        out[n++] = { nid, imp ? imp->name : nullptr, imp ? *imp->fn : nullptr, s.first, s.second };
    }
    return n;
}

void vitox_import_stats_reset() {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    g_stats.clear();
}

// ---- M8B.1 DIAGNOSTIC TRAP (runtime-exact import discovery; never used for acceptance runs) ----
// A guest call to a NID outside the selected set stops EVERY guest thread at once (the same exit_delete(false) loop
// as KernelState::process_exit, which is what sceKernelExitDeleteThread does for one thread) and returns nothing to the
// guest: r0 is not written, upstream's "return 0" is not reached, and no guest instruction after the call executes.
namespace {
std::atomic<bool> g_trap_enabled{ false };
std::mutex g_trap_mutex;
std::vector<VitoxTrapRecord> g_traps;
std::atomic<void (*)(const char *)> g_checkpoint{ nullptr };
} // namespace

void vitox_import_checkpoint_hook(void (*checkpoint)(const char *line)) { g_checkpoint = checkpoint; }

void vitox_import_trap_enable(bool on) {
    std::lock_guard<std::mutex> lock(g_trap_mutex);
    g_trap_enabled = on;
    g_traps.clear();
}

// 0.0.29.4 (user-approved Uncharted bring-up, closure UGA-3): NIDs that NO Vita3K module exports (Vita3K classifies them
// "no export of this name"), imported by uncharted.self (PCSA00029). For these the upstream behaviour is call_import's
// not-found path - r0 = 0 and "Import function for NID ... not found" - so they take that path instead of the trap. Exact
// list from the title's import inventory (scratchpad uga/unselected.csv); nothing is implemented or faked for them.
static const uint32_t k_upstream_not_found_nids[] = {
    0xCB91CEBC, 0xF6AFBB09, 0x023EAA62, 0x04DCBE87, 0x1625D348, 0xEFE299C9, 0xA870587F, 0x1AA394BA, 0xE9176B09, 0xE608000B, 0x1AE3EC1C, 0xA412E9CA,
    0x49A97D5F, 0x9FB9277B, 0xBF2BBE1F, 0x9B3F2DCE, 0xD2871F1C, 0x69EE6FB3, 0x76C4807C, 0x88540CEC, 0x94E1305D, 0xCA742B97, 0x8095FDEF,
};

bool vitox_import_trap(EmuEnvState &emuenv, CPUState &cpu, uint32_t nid, int thread_id) {
    if (!g_trap_enabled)
        return false;
    if (std::find(std::begin(k_upstream_not_found_nids), std::end(k_upstream_not_found_nids), nid) != std::end(k_upstream_not_found_nids)) {
        static std::mutex seen_m;
        static std::set<uint32_t> seen;
        std::lock_guard<std::mutex> lock(seen_m);
        if (seen.insert(nid).second)
            LOG_WARN("Vitox [UGA-3] NID 0x{:08X} has no Vita3K export: upstream's not-found path (r0 = 0), as Vita3K", nid);
        return false;
    }
    VitoxTrapRecord rec{};
    rec.nid = nid;
    rec.pc = read_pc(cpu);
    rec.lr = read_lr(cpu);
    rec.thread_id = thread_id;
    const auto b = emuenv.kernel.func_binding_infos.find(nid);
    rec.library_nid = b != emuenv.kernel.func_binding_infos.end() ? b->second.library_nid : 0;
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (thread)
        snprintf(rec.thread_name, sizeof(rec.thread_name), "%s", thread->name.c_str());
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_trap_mutex);
        g_traps.push_back(rec);
        first = g_traps.size() == 1;
    }
    LOG_ERROR("Vitox [M8B.1] TRAP: guest called NID 0x{:08X} {} (library_nid 0x{:08X}) - not in the selected dispatcher | thread {} '{}' pc 0x{:08X} lr 0x{:08X} "
              "| args r0 0x{:08X} r1 0x{:08X} r2 0x{:08X} r3 0x{:08X} | all guest threads stopped, nothing returned to the guest",
        nid, import_name(nid), rec.library_nid, thread_id, rec.thread_name, rec.pc, rec.lr, read_reg(cpu, 0), read_reg(cpu, 1), read_reg(cpu, 2),
        read_reg(cpu, 3));
    if (vitox_denied(nid))
        LOG_ERROR("Vitox [M9A.5] TRAP is on a DENY-LISTED export ({}): offline build - adhoc network activation is never selected", import_name(nid));
    if (const auto checkpoint = g_checkpoint.load(); first && checkpoint) {
        const char *name = import_name(nid);
        char line[256];
        snprintf(line, sizeof(line), "m9a4 cp 15 FIRST TRAP: nid=0x%08X %s library_nid=0x%08X | thread %d '%s' pc=0x%08X lr=0x%08X", nid, name ? name : "?",
            rec.library_nid, thread_id, rec.thread_name, rec.pc, rec.lr);
        checkpoint(line);
    }
    std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    for (auto &[_, t] : emuenv.kernel.threads)
        t->exit_delete(false);
    return true;
}

unsigned vitox_import_traps(VitoxTrapRecord *out, unsigned max) {
    std::lock_guard<std::mutex> lock(g_trap_mutex);
    unsigned n = 0;
    for (const auto &t : g_traps) {
        if (n == max)
            break;
        out[n++] = t;
    }
    return n;
}
