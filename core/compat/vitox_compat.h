// Vitox compat layer: forced include (/FI) for the selected Vita3K core sources on WindowsStore.
// Inactive unless VITOX_UWP is defined, so desktop builds are unchanged.
//
// B4: AddVectoredExceptionHandler / RemoveVectoredExceptionHandler are exported on Xbox
//     (M2 VEH probe, resolved at runtime) but only declared for the desktop partition. The
//     calls are redirected to wrappers that resolve them with GetProcAddress, so no static
//     desktop-only import is created.
// B5: NOT handled here. SetConsoleTitleA/W and SetConsoleOutputCP ARE declared for the app
//     partition (consoleapi2.h), so a macro would break the SDK declarations. The only issue is
//     that WindowsStore defines UNICODE, turning SetConsoleTitle("...") into SetConsoleTitleW.
//     util/src/logging.cpp (the only user) is not in the core link set; when it is selected,
//     compile that file without UNICODE/_UNICODE, as the desktop build does.
//
// This header must not include anything: it is seen before every other header of each TU.
#pragma once

#if defined(VITOX_UWP) && !defined(VITOX_COMPAT_IMPL)

#ifdef __cplusplus
extern "C" {
#endif
// Same ABI as the Win32 functions (first, handler -> cookie / cookie -> success); handler is
// passed as an opaque pointer so this header needs no Windows types.
void *__stdcall vitox_compat_add_veh(unsigned long first, void *handler);
unsigned long __stdcall vitox_compat_remove_veh(void *cookie);
#ifdef __cplusplus
}
#endif

#define AddVectoredExceptionHandler(first, handler) \
    vitox_compat_add_veh((first), reinterpret_cast<void *>(handler))
#define RemoveVectoredExceptionHandler(cookie) vitox_compat_remove_veh(cookie)

#endif
