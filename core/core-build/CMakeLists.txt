# Vitox M3 (inventory) - out-of-tree WindowsStore BUILD-ONLY of selected Vita3K core targets.
# Uses Vita3K's own per-module CMakeLists (util, mem, http, rtc, nids, cpu, kernel) read-only from
# C:\Vitox\Vita3K; nothing is written into that tree. Static libraries only: nothing is linked into
# the Vitox app. Third-party dependencies that are not provided are deliberately left as empty
# placeholder targets so the first real blocker of each target shows up as a compile error.
cmake_minimum_required(VERSION 3.22)
# CMAKE_SYSTEM_NAME=WindowsStore makes CMake treat this as cross-compiling, which leaves
# CMAKE_SYSTEM_PROCESSOR empty; capstone's CPackConfig.txt:25 dereferences it unquoted.
if (NOT CMAKE_SYSTEM_PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
endif()
project(vitox_m3_core CXX C)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    message(FATAL_ERROR "Configure with -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.26100.0")
endif()

set(VITA3K "C:/Vitox/Vita3K" CACHE PATH "Vita3K source (read-only)")
set(VITOX_M2_DYNARMIC_BUILD "C:/Vitox/Vitox-M2/dynarmic-build" CACHE PATH "Approved W^X-shimmed Dynarmic wrapper")

# ---- same global settings as Vita3K's root CMakeLists.txt (Windows branch)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_compile_definitions(_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _CRT_SECURE_NO_WARNINGS NOMINMAX)
file(WRITE "${CMAKE_BINARY_DIR}/vita3k/config/gitver.h"
    "#define APP_VER_HI 0\n#define APP_VER_MID 2\n#define APP_VER_LO 1\n#define APP_NUMBER 0\n")
include_directories("${CMAKE_BINARY_DIR}/vita3k/config")
# M8A: the M7B.5/.6 per-scene/per-frame GPU-state diagnostics (read-only logging) are compiled in only with this option.
# Lightweight health counters (draws, pipelines, PSO failures, presents, unknown imports) are always on.
option(VITOX_DIAG_M7B "M7B.5/.6 render/display-path diagnostic logging" OFF)
if (VITOX_DIAG_M7B)
    add_compile_definitions(VITOX_DIAG_M7B)
endif()
# M8E: periodic per-frame / per-draw sample lines of the external presenter (high volume in long app sessions).
option(VITOX_DIAG_VERBOSE "periodic draw/present sample logging" OFF)
if (VITOX_DIAG_VERBOSE)
    add_compile_definitions(VITOX_DIAG_VERBOSE)
endif()

# ---- Layer 0: in-tree externals, configured the way Vita3K's external/CMakeLists.txt does
set(CMAKE_FOLDER externals)
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
set(CAPSTONE_ARM_SUPPORT ON CACHE BOOL "" FORCE)
add_subdirectory("${VITA3K}/external/capstone" ext/capstone EXCLUDE_FROM_ALL)
set(FMT_INSTALL ON)  # as Vita3K: dynarmic's install(EXPORT) requires fmt in an export set
add_subdirectory("${VITA3K}/external/fmt" ext/fmt EXCLUDE_FROM_ALL)
set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "")
set(SPDLOG_NO_THREAD_ID ON CACHE BOOL "")
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "")
add_subdirectory("${VITA3K}/external/spdlog" ext/spdlog EXCLUDE_FROM_ALL)
add_library(miniz STATIC "${VITA3K}/external/miniz/miniz.c" "${VITA3K}/external/miniz/miniz.h")
target_include_directories(miniz PUBLIC "${VITA3K}/external/miniz")
add_library(vita-toolchain INTERFACE)
target_include_directories(vita-toolchain INTERFACE "${VITA3K}/external/vita-toolchain/src")
add_library(winsock INTERFACE)
target_link_libraries(winsock INTERFACE WS2_32 IPHLPAPI)  # WSOCK32 is not in the WindowsStore SDK set

# Dynarmic + mcl via the approved M2 wrapper (W^X ordering shim). fmt::fmt already exists, so
# Dynarmic's bundled fmt is skipped exactly as in Vita3K's own build.
add_subdirectory("${VITOX_M2_DYNARMIC_BUILD}" ext/m2-dynarmic)

# Boost: header-only use here. Vita3K links Boost::filesystem (a compiled library); building it
# for WindowsStore is a later link-stage item, not needed for static-library compilation.
add_library(vitox_boost_headers INTERFACE)
target_include_directories(vitox_boost_headers INTERFACE "${VITA3K}/external/boost")
target_compile_definitions(vitox_boost_headers INTERFACE BOOST_ALL_NO_LIB)
set(Boost_LIBRARIES vitox_boost_headers)

# SDL3: headers only (from external/sdl/include); SDL3 itself is NOT built here.
add_library(vitox_sdl3_headers INTERFACE)
target_include_directories(vitox_sdl3_headers INTERFACE "${VITA3K}/external/sdl/include")
add_library(SDL3::SDL3 ALIAS vitox_sdl3_headers)

# Not provided (Vita3K downloads/fetches them; out of scope for this inventory): empty placeholders.
add_library(ssl INTERFACE)
add_library(crypto INTERFACE)
add_library(libcurl INTERFACE)
add_library(googletest INTERFACE)  # only referenced by mem-tests, which is never built here

# ---- Layer 1..3: Vita3K targets, using Vita3K's own CMakeLists
set(CMAKE_FOLDER vita3k)
foreach (m nids util mem http rtc cpu kernel)
    add_subdirectory("${VITA3K}/vita3k/${m}" vita3k/${m})
endforeach()
# util/CMakeLists.txt: "vc_runtime_checker.cpp is directly added from the main CMakeList" - not needed for a static lib.

# ---- M3: first core LINK probe -------------------------------------------------------------
# A single static library built from an explicit subset of REAL Vita3K sources plus the Vitox
# compat layer and SDL core shim. Vita3K's own targets above stay inventory-only.
set(VITOX_M3 "${CMAKE_CURRENT_LIST_DIR}/..")
set(VITOX_CORE_VITA3K_SOURCES
    vita3k/mem/src/mem.cpp
    vita3k/mem/src/allocator.cpp
    vita3k/cpu/src/cpu.cpp
    vita3k/cpu/src/disasm.cpp
    vita3k/cpu/src/dynarmic_cpu.cpp
    vita3k/kernel/src/kernel.cpp
    vita3k/kernel/src/thread.cpp
    vita3k/kernel/src/sync_primitives.cpp
    vita3k/kernel/src/callback.cpp
    vita3k/kernel/src/debugger.cpp
    vita3k/rtc/src/rtc.cpp
    # M3.1: loader + NID table + one real module TU
    vita3k/kernel/src/load_self.cpp
    vita3k/kernel/src/relocation.cpp
    vita3k/nids/src/nids.cpp
    vita3k/modules/SceDriverUser/SceRtcUser.cpp
    vita3k/modules/SceRtc/SceRtc.cpp
)
set(VITOX_CORE_EXCLUDED
    "util/* (only arm.cpp + string_utils.cpp needed so far)"
    "util/src/hash.cpp (OpenSSL)" "util/src/net_utils.cpp (curl)" "util/src/fs_utils.cpp (SDL IOStream)"
    "util/src/logging.cpp (logging::init not used; spdlog default logger)"
    "http/*" "updater/*"
    "modules/module_parent.cpp (resolve_import switch references every export of every module)"
    "emuenv/src/emuenv.cpp (EmuEnvState ctor: all subsystem states incl. SDL3 input types)"
    "all other modules/* TUs"
)
list(APPEND VITOX_CORE_VITA3K_SOURCES
    vita3k/util/src/arm.cpp                  # encode_arm_inst (import stubs / export branches)
    vita3k/module/src/write_return_value.cpp # module bridge return path
    # 0.0.7.0 (M4.1): stateful HLE export family SceSysmem (memory blocks, SysmemState in obj_store)
    vita3k/modules/SceSysmem/SceSysmem.cpp
    vita3k/modules/SceSysmem/SceSysmemForDriver.cpp # ksceKernel{Alloc,Free}MemBlock/GetMemBlockBase (CALL_EXPORT targets)
    vita3k/packages/src/sfo.cpp              # sfo::get_data_by_key (sceKernelGetFreeMemorySize ATTRIBUTE2)
    vita3k/util/src/string_utils.cpp         # string_utils::stoi_def (same export)
)

# ---- Vitox shim: load_self.cpp ELF-dump block -> std::filesystem ---------------------------
# The only Boost.Filesystem (compiled-library) and fs_utils (SDL_IOStream TU) uses on the selected
# path are in load_self's `kernel.debugger.dump_elfs` debug block. A patched copy replaces exactly
# that block with std::filesystem + std::ofstream (same file name, same bytes). Configure fails if
# the upstream text drifts. Vita3K's tree is not modified.
function(vitox_substitute src_rel dst_name)
    set(_src "${VITA3K}/${src_rel}")
    set(_dst "${CMAKE_BINARY_DIR}/vitox-patched/${dst_name}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
    file(READ "${_src}" _t)
    string(REPLACE "\r\n" "\n" _t "${_t}")
    math(EXPR _last "${ARGC} - 1")
    if (ARGC GREATER 2) # 0.0.24.170: a call may end up with no pairs (plain copy)
    foreach (i RANGE 2 ${_last} 2)
        math(EXPR j "${i} + 1")
        set(_old "${ARGV${i}}")
        set(_new "${ARGV${j}}")
        string(FIND "${_t}" "${_old}" _pos)
        if (_pos EQUAL -1)
            message(FATAL_ERROR "Vitox shim ${dst_name}: expected upstream text not found in ${src_rel}:\n${_old}")
        endif()
        string(REPLACE "${_old}" "${_new}" _t "${_t}")
    endforeach()
    endif()
    file(WRITE "${_dst}.tmp" "${_t}")
    file(COPY_FILE "${_dst}.tmp" "${_dst}" ONLY_IF_DIFFERENT)
    message(STATUS "Vitox shim: ${src_rel} -> ${_dst}")
endfunction()
vitox_substitute(vita3k/kernel/src/load_self.cpp load_self.cpp
[=[#include <fstream>]=]
[=[#include <filesystem> // Vitox shim
#include <fstream>]=]
[=[        fs::create_directories(dump_path);
        const auto start = dump_segments[0].p_vaddr;
        const auto end = dump_segments[last_index].p_vaddr + dump_segments[last_index].p_filesz;
        const auto elf_name = fs::path(self_path).filename().stem().string();
        const auto filename = dump_path / fmt::format("{}-{}_{}.elf", log_hex_full(start), log_hex_full(end), elf_name);
        fs_utils::dump_data(filename, dump_elf.data(), dump_elf.size());]=]
[=[        // Vitox shim: std::filesystem instead of compiled Boost.Filesystem + fs_utils::dump_data (SDL_IOStream)
        const std::filesystem::path vitox_dump_path(dump_path.native());
        std::filesystem::create_directories(vitox_dump_path);
        const auto start = dump_segments[0].p_vaddr;
        const auto end = dump_segments[last_index].p_vaddr + dump_segments[last_index].p_filesz;
        const auto elf_name = std::filesystem::path(reinterpret_cast<const char8_t *>(self_path.c_str())).filename().stem().string();
        const auto filename = vitox_dump_path / fmt::format("{}-{}_{}.elf", log_hex_full(start), log_hex_full(end), elf_name);
        std::ofstream(filename, std::ios::binary).write(reinterpret_cast<const char *>(dump_elf.data()), static_cast<std::streamsize>(dump_elf.size()));]=])
# ---- 0.1.1.10 COMPAT (start-of-game buzzer): upstream fills every new guest thread stack with 0xCC. Games that queue an
# audio buffer from their stack before writing it (seen: Killzone Mercenary, every other 256-frame buffer = 0xCCCC for the
# first ~0.3 s) then play a loud -0.4 DC square wave. Zero-filled stacks make such a buffer silent.
vitox_substitute(vita3k/kernel/src/thread.cpp thread.cpp
[=[    memset(stack.get_ptr<void>().get(mem), 0xcc, stack_size);]=]
[=[    memset(stack.get_ptr<void>().get(mem), 0, stack_size); // Vitox 0.1.1.10: upstream 0xcc (audible as a buzz)]=])
list(REMOVE_ITEM VITOX_CORE_VITA3K_SOURCES vita3k/kernel/src/load_self.cpp vita3k/kernel/src/thread.cpp)
list(TRANSFORM VITOX_CORE_VITA3K_SOURCES PREPEND "${VITA3K}/" OUTPUT_VARIABLE _vitox_core_abs)
list(APPEND _vitox_core_abs "${CMAKE_BINARY_DIR}/vitox-patched/load_self.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/thread.cpp")
list(APPEND VITOX_CORE_VITA3K_SOURCES "vita3k/kernel/src/load_self.cpp(shimmed:dump->std::filesystem)" "vita3k/kernel/src/thread.cpp(shimmed:stack fill 0)")
string(REPLACE ";" " " _vitox_src_str "${VITOX_CORE_VITA3K_SOURCES}")
string(REPLACE ";" "; " _vitox_exc_str "${VITOX_CORE_EXCLUDED}")
file(WRITE "${CMAKE_BINARY_DIR}/vitox-core/vitox_core_sources.h.tmp"
    "// Generated by Vitox-M3 core-build. Do not edit.\n#pragma once\n"
    "#define VITOX_CORE_SOURCES \"${_vitox_src_str}\"\n#define VITOX_CORE_EXCLUDED \"${_vitox_exc_str}\"\n")
file(COPY_FILE "${CMAKE_BINARY_DIR}/vitox-core/vitox_core_sources.h.tmp" "${CMAKE_BINARY_DIR}/vitox-core/vitox_core_sources.h" ONLY_IF_DIFFERENT)

add_library(vitox_core STATIC
    ${_vitox_core_abs}
    "${VITOX_M3}/compat/vitox_compat.cpp"
    "${VITOX_M3}/sdl-shim/vitox_sdl_shim.cpp"
    "${VITOX_M3}/core-probe/vitox_core_probe.cpp")
# 0.0.10.0: core-probe/vitox_import_dispatch.cpp moved to vitox_hle (it now references SceLibKernel /
# SceIofilemgr exports); the app links both libraries.
set_target_properties(vitox_core PROPERTIES FOLDER vitox)
# SDL core shim FIRST and the real SDL include dir never added: any SDL header or symbol
# outside the shim is a compile error.
target_include_directories(vitox_core BEFORE PRIVATE "${VITOX_M3}/sdl-shim")
target_include_directories(vitox_core PRIVATE
    "${VITOX_M3}/compat" "${VITOX_M3}/core-probe" "${CMAKE_BINARY_DIR}/vitox-core"
    "${VITA3K}/vita3k/mem/include" "${VITA3K}/vita3k/cpu/include" "${VITA3K}/vita3k/kernel/include"
    "${VITA3K}/vita3k/util/include" "${VITA3K}/vita3k/rtc/include" "${VITA3K}/vita3k/nids/include"
    "${VITA3K}/vita3k/emuenv/include"
    "${VITA3K}/vita3k/module/include" "${VITA3K}/vita3k/config/include" "${VITA3K}/vita3k/input/include"
    "${VITA3K}/vita3k/modules/include" "${VITA3K}/vita3k/packages/include" "${VITA3K}/vita3k/io/include"
    "${VITA3K}/external/vita-toolchain/src")
target_compile_definitions(vitox_core PRIVATE VITOX_UWP)
target_compile_options(vitox_core PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/FI${VITOX_M3}/compat/vitox_compat.h>")
set_source_files_properties("${VITOX_M3}/compat/vitox_compat.cpp" PROPERTIES COMPILE_DEFINITIONS VITOX_COMPAT_IMPL)
# WindowsStore projects default to /sdl, which turns C4146 into an error. SceSysmemForDriver.cpp uses
# `size & -size` (lowest set bit of an unsigned SceSize: well-defined). Vita3K's desktop build has no
# /sdl, so this TU gets the same flags as upstream.
set_source_files_properties("${VITA3K}/vita3k/modules/SceSysmem/SceSysmemForDriver.cpp" PROPERTIES COMPILE_OPTIONS "/sdl-")
target_link_libraries(vitox_core PUBLIC dynarmic merry::mcl capstone fmt::fmt spdlog::spdlog vitox_boost_headers miniz)
message(STATUS "Vitox M3 core link: ${_vitox_src_str}")

# ---- M3.1 inventory (compile-only, never linked): emuenv / config / io / module dispatch / loader.
# Every Vita3K include root plus in-tree third-party headers; the SDL core shim is searched first and
# the REAL SDL headers after it, so a TU that needs more SDL than the shim hits SDL's UWP #error.
file(GLOB _vitox_v3k_inc LIST_DIRECTORIES true "${VITA3K}/vita3k/*/include")
add_library(vitox_inv_l2 STATIC EXCLUDE_FROM_ALL
    "${VITA3K}/vita3k/emuenv/src/emuenv.cpp"
    "${VITA3K}/vita3k/config/src/config.cpp"
    "${VITA3K}/vita3k/config/src/settings.cpp"
    "${VITA3K}/vita3k/io/src/device.cpp"
    "${VITA3K}/vita3k/io/src/filesystem.cpp"
    "${VITA3K}/vita3k/io/src/io.cpp"
    "${VITA3K}/vita3k/io/src/state_functions.cpp"
    "${VITA3K}/vita3k/modules/module_parent.cpp"
    "${VITA3K}/vita3k/modules/SceDriverUser/SceRtcUser.cpp"
    "${VITA3K}/vita3k/modules/SceRtc/SceRtc.cpp"
    "${VITA3K}/vita3k/kernel/src/load_self.cpp"
    "${VITA3K}/vita3k/kernel/src/relocation.cpp"
    "${VITA3K}/vita3k/nids/src/nids.cpp")
set_target_properties(vitox_inv_l2 PROPERTIES FOLDER vitox)
target_include_directories(vitox_inv_l2 BEFORE PRIVATE "${VITOX_M3}/sdl-shim")
target_include_directories(vitox_inv_l2 PRIVATE ${_vitox_v3k_inc} "${VITA3K}/vita3k" "${VITOX_M3}/compat"
    "${VITA3K}/external/sdl/include" "${VITA3K}/external/dirent/include" "${VITA3K}/external/yaml-cpp/include"
    "${VITA3K}/external/pugixml/src" "${VITA3K}/external/cli11" "${VITA3K}/external/miniz"
    "${VITA3K}/external/vita-toolchain/src" "${VITA3K}/external/xxHash" "${VITA3K}/external/stb"
    "${VITA3K}/external/VulkanMemoryAllocator-Hpp/Vulkan-Headers/include" "${VITA3K}/external/VulkanMemoryAllocator-Hpp/include"
    "${VITA3K}/external/VulkanMemoryAllocator-Hpp/VulkanMemoryAllocator/include")
target_compile_definitions(vitox_inv_l2 PRIVATE VITOX_UWP)
target_compile_options(vitox_inv_l2 PRIVATE "/FI${VITOX_M3}/compat/vitox_compat.h")
target_link_libraries(vitox_inv_l2 PRIVATE fmt::fmt spdlog::spdlog vitox_boost_headers capstone dynarmic merry::mcl)

# ---- M4 inventory: EmuEnvState construction closure (compile + LINK, never packaged) ----------
# One generated TU per EmuEnvState-owned member does exactly what EmuEnvState does with it
# (construct + destroy, or destroy an empty unique_ptr). Each is linked as its own WindowsStore DLL
# against vitox_core, so its unresolved externals are that member's construction closure.
# SDL: the Vitox type-only shim is the ONLY SDL include path (real SDL3 is never added).
file(GLOB _vitox_ext_inc LIST_DIRECTORIES true "${VITA3K}/external/*/include")
list(FILTER _vitox_ext_inc EXCLUDE REGEX "/sdl/")
set(VITOX_M4_INCLUDES ${_vitox_v3k_inc} "${VITA3K}/vita3k" ${_vitox_ext_inc}
    "${VITA3K}/external/cli11" "${VITA3K}/external/pugixml/src" "${VITA3K}/external/miniz" "${VITA3K}/external/stb"
    "${VITA3K}/external/xxHash" "${VITA3K}/external/vita-toolchain/src" "${VITA3K}/external/concurrentqueue"
    "${VITA3K}/external/ddspp" "${VITA3K}/external/printf" "${VITA3K}/external/LibAtrac9/C/src"
    "${VITA3K}/external/VulkanMemoryAllocator-Hpp/Vulkan-Headers/include"
    "${VITA3K}/external/VulkanMemoryAllocator-Hpp/VulkanMemoryAllocator/include")
set(VITOX_M4_MEMBERS
    "sfo|packages/sfo.h|make:sfo::SfoAppInfo"
    "config|config/state.h|make:Config"
    "mem|mem/state.h|make:MemState"
    "ctrl|ctrl/state.h|make:CtrlState"
    "touch|touch/state.h|make:TouchState"
    "kernel|kernel/state.h|make:KernelState"
    "app|app/state.h|make:AppState"
    "audio|audio/state.h|make:AudioState"
    "gxm|gxm/state.h|make:GxmState"
    "io|io/state.h|make:IOState"
    "motion|motion/state.h|make:MotionState"
    "net|net/state.h|make:NetState"
    "netctl|net/state.h|make:NetCtlState"
    "ngs|ngs/state.h|make:ngs::State"
    "np|np/state.h|make:NpState"
    "display|display/state.h|make:DisplayState"
    "dialog|dialog/state.h|make:DialogState"
    "ime|ime/state.h|make:Ime"
    "license|packages/license.h|make:License"
    "regmgr|regmgr/state.h|make:RegMgrState"
    "sfofile|packages/sfo.h|make:SfoFile"
    "gdb|gdbstub/state.h|make:GDBState"
    "http|http/state.h|make:HTTPState"
    "camera|camera/state.h|make:CameraState"
    "compat|compat/state.h|make:CompatState"
    "renderer|renderer/state.h|null:renderer::State"
    "vkinfo|renderer/functions.h|null:renderer::VulkanDeviceInfo"
    "overlay|overlay/display_manager.h|null:overlay::display_manager"
    # narrow variant: gdbstub/state.h includes desktop-only <winsock.h>; measure app-partition <winsock2.h>
    "gdb_ws2|gdbstub/state.h|make:GDBState|winsock2.h"
)
set(_m4_all "")
foreach (entry ${VITOX_M4_MEMBERS})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 key)
    list(GET parts 1 header)
    list(GET parts 2 how)
    string(FIND "${how}" ":" _colon)
    string(SUBSTRING "${how}" 0 ${_colon} mode)
    math(EXPR _tstart "${_colon} + 1")
    string(SUBSTRING "${how}" ${_tstart} -1 type) # keeps namespace '::' intact
    set(pre "")
    list(LENGTH parts _np)
    if (_np GREATER 3)
        list(GET parts 3 _pre_h)
        set(pre "#include <${_pre_h}> // narrow variant\n")
    endif()
    # Construction and destruction live in separate exported functions so the optimizer cannot prove
    # the members null and drop destructor references (EmuEnvState's real dtor keeps them all).
    if (mode STREQUAL "make")
        set(body "extern \"C\" __declspec(dllexport) void *vitox_m4_new_${key}() { return new ${type}(); }\nextern \"C\" __declspec(dllexport) void vitox_m4_delete_${key}(void *p) { delete static_cast<${type} *>(p); }\n")
    else()
        set(body "// as EmuEnvState: owned through unique_ptr, empty by default, destroyed\nextern \"C\" __declspec(dllexport) void vitox_m4_delete_${key}(void *p) { std::unique_ptr<${type}> u(static_cast<${type} *>(p)); }\n")
    endif()
    set(tu "${CMAKE_BINARY_DIR}/vitox-m4/inv_${key}.cpp")
    file(WRITE "${tu}.tmp" "// Generated (Vitox M4 inventory): construct/destroy one EmuEnvState member.\n${pre}#include <emuenv/state.h> // emuenv.cpp includes this first\n#include <${header}>\n#include <memory>\n${body}")
    file(COPY_FILE "${tu}.tmp" "${tu}" ONLY_IF_DIFFERENT)
    add_library(vitox_m4_${key} SHARED EXCLUDE_FROM_ALL "${tu}")
    set_target_properties(vitox_m4_${key} PROPERTIES FOLDER vitox-m4)
    target_include_directories(vitox_m4_${key} BEFORE PRIVATE "${VITOX_M3}/sdl-shim")
    target_include_directories(vitox_m4_${key} PRIVATE ${VITOX_M4_INCLUDES})
    target_compile_definitions(vitox_m4_${key} PRIVATE VITOX_UWP)
    target_compile_options(vitox_m4_${key} PRIVATE "/FI${VITOX_M3}/compat/vitox_compat.h")
    target_link_libraries(vitox_m4_${key} PRIVATE vitox_core)
    list(APPEND _m4_all vitox_m4_${key})
endforeach()
set(VITOX_M4_TARGETS ${_m4_all} CACHE INTERNAL "")

# gdbstub/state.h includes desktop-only <winsock.h> (SOCKET/WSADATA undefined on WindowsStore).
# Guarded header substitution: same header with the app-partition <winsock2.h>, searched first.
vitox_substitute(vita3k/gdbstub/include/gdbstub/state.h include/gdbstub/state.h
[=[#include <winsock.h>]=]
[=[#include <winsock2.h> // Vitox shim: app-partition WinSock (was <winsock.h>, desktop-only)]=])

# EmuEnvState itself: real emuenv.cpp + a TU that constructs/destroys it in separate functions.
file(WRITE "${CMAKE_BINARY_DIR}/vitox-m4/inv_emuenv.cpp.tmp"
"// Generated (Vitox M4 inventory): construct/destroy a real EmuEnvState.\n#include <emuenv/state.h>\n\
extern \"C\" __declspec(dllexport) void *vitox_m4_new_emuenv() { return new EmuEnvState(); }\n\
extern \"C\" __declspec(dllexport) void vitox_m4_delete_emuenv(void *p) { delete static_cast<EmuEnvState *>(p); }\n")
file(COPY_FILE "${CMAKE_BINARY_DIR}/vitox-m4/inv_emuenv.cpp.tmp" "${CMAKE_BINARY_DIR}/vitox-m4/inv_emuenv.cpp" ONLY_IF_DIFFERENT)
set(VITOX_M4_EMUENV_EXTRA "" CACHE STRING "Extra Vita3K sources (relative) linked into the emuenv probe")
set(_m4_extra "")
foreach (s ${VITOX_M4_EMUENV_EXTRA})
    list(APPEND _m4_extra "${VITA3K}/${s}")
endforeach()
add_library(vitox_m4_emuenv SHARED EXCLUDE_FROM_ALL "${CMAKE_BINARY_DIR}/vitox-m4/inv_emuenv.cpp" "${VITA3K}/vita3k/emuenv/src/emuenv.cpp" ${_m4_extra})
set_target_properties(vitox_m4_emuenv PROPERTIES FOLDER vitox-m4)
target_include_directories(vitox_m4_emuenv BEFORE PRIVATE "${VITOX_M3}/sdl-shim" "${CMAKE_BINARY_DIR}/vitox-patched/include")
target_include_directories(vitox_m4_emuenv PRIVATE ${VITOX_M4_INCLUDES})
target_compile_definitions(vitox_m4_emuenv PRIVATE VITOX_UWP)
target_compile_options(vitox_m4_emuenv PRIVATE "/FI${VITOX_M3}/compat/vitox_compat.h")
target_link_libraries(vitox_m4_emuenv PRIVATE vitox_core)

# ---- 0.0.6.0: vitox_emuenv - a legitimately constructible EmuEnvState (M4 A-list) -------------
# Real emuenv.cpp + the light closure TUs + two approved inert items (camera, SDL_CloseSensor).
# Drift guard for the inert camera substitute: upstream camera.h / camera.cpp are pinned by hash.
set(_vitox_cam_pins
    "vita3k/camera/include/camera/camera.h|b875d86960a8d2557b3dcc5fcd1d1657f1334dfb88677e0b79352e90157f41e6"
    "vita3k/camera/src/camera.cpp|b296af635b6e456b229fcb95686324b396940f4959d4d1301586d149c4fdfd25")
foreach (pin ${_vitox_cam_pins})
    string(REPLACE "|" ";" pp "${pin}")
    list(GET pp 0 f)
    list(GET pp 1 want)
    file(SHA256 "${VITA3K}/${f}" got)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VITA3K}/${f}")
    if (NOT got STREQUAL want)
        message(FATAL_ERROR "Vitox inert camera: upstream ${f} changed (sha256 ${got} != ${want}); re-check compat/vitox_camera_inert.cpp")
    endif()
endforeach()
add_library(vitox_emuenv STATIC
    "${VITA3K}/vita3k/emuenv/src/emuenv.cpp"
    "${VITA3K}/vita3k/motion/src/motion_input.cpp"
    "${VITA3K}/vita3k/overlay/src/display_manager.cpp"
    "${VITA3K}/vita3k/overlay/src/input.cpp"
    "${VITA3K}/vita3k/overlay/src/overlay.cpp"
    "${VITOX_M3}/compat/vitox_camera_inert.cpp"
    "${VITOX_M3}/sdl-shim/vitox_sdl_sensor.cpp")
set_target_properties(vitox_emuenv PROPERTIES FOLDER vitox)
target_include_directories(vitox_emuenv BEFORE PRIVATE "${VITOX_M3}/sdl-shim" "${CMAKE_BINARY_DIR}/vitox-patched/include")
target_include_directories(vitox_emuenv PRIVATE ${VITOX_M4_INCLUDES} "${VITOX_M3}/compat")
target_compile_definitions(vitox_emuenv PRIVATE VITOX_UWP)
target_compile_options(vitox_emuenv PRIVATE "/FI${VITOX_M3}/compat/vitox_compat.h")
target_link_libraries(vitox_emuenv PUBLIC vitox_core)
message(STATUS "Vitox emuenv: emuenv.cpp motion_input.cpp overlay/{display_manager,input,overlay}.cpp + inert camera + SDL_CloseSensor shim")

# ---- M4.1 inventory: IO init + programmatic Config (compile + LINK probes, never packaged) ------
# Boost.Filesystem compiled from the in-tree sources with the definitions of its own CMakeLists
# (Vita3K builds it with b2 inside external/boost, which would write into the read-only tree).
set(_bfs "${VITA3K}/external/boost/libs/filesystem")
set(_bfs_src codecvt_error_category exception operations directory path path_traits portability unique_path utf8_codecvt_facet windows_file_codecvt)
list(TRANSFORM _bfs_src PREPEND "${_bfs}/src/")
list(TRANSFORM _bfs_src APPEND ".cpp")
# 0.0.8.0: the substitutes are instrumented functions (compat/vitox_bfs_compat.cpp, in this library)
add_library(vitox_boost_filesystem STATIC ${_bfs_src} "${VITOX_M3}/compat/vitox_bfs_compat.cpp")
set_target_properties(vitox_boost_filesystem PROPERTIES FOLDER vitox-m41)
target_include_directories(vitox_boost_filesystem PRIVATE "${_bfs}/src")
target_compile_definitions(vitox_boost_filesystem
    PUBLIC BOOST_FILESYSTEM_NO_LIB BOOST_FILESYSTEM_STATIC_LINK=1
    PRIVATE BOOST_FILESYSTEM_SOURCE _SCL_SECURE_NO_WARNINGS _SCL_SECURE_NO_DEPRECATE BOOST_USE_WINDOWS_H _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN
        BOOST_FILESYSTEM_HAS_BCRYPT VITOX_UWP) # HAS_BCRYPT: what Boost's own CMake check sets when BCrypt is available
# App-partition substitutes for the four remaining desktop-only APIs (see the header).
target_compile_options(vitox_boost_filesystem PRIVATE "/FI${VITOX_M3}/compat/vitox_bfs_compat.h")
target_link_libraries(vitox_boost_filesystem PUBLIC vitox_boost_headers bcrypt)

# yaml-cpp / pugixml: in-tree, configured as in Vita3K's external/CMakeLists.txt
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${VITA3K}/external/yaml-cpp" ext/yaml-cpp EXCLUDE_FROM_ALL)
add_subdirectory("${VITA3K}/external/pugixml" ext/pugixml EXCLUDE_FROM_ALL)

# config.cpp without its CLI11 command line: CLI11's Windows path needs CommandLineToArgvW
# (shellapi.h, desktop partition). Guarded substitution: drop the CLI11 include, compile out
# init_config(), and expose the file-static parse() as config::vitox_parse_config().
vitox_substitute(vita3k/config/src/config.cpp config.cpp
[=[#include <CLI11.hpp>
]=]
[=[// Vitox shim: <CLI11.hpp> removed (CommandLineToArgvW is desktop-partition only)
]=]
[=[ExitCode init_config(Config &cfg, int argc, char **argv, const Root &root_paths, bool portable) {]=]
[=[#if 0 // Vitox shim: CLI11 command-line front end compiled out
ExitCode init_config(Config &cfg, int argc, char **argv, const Root &root_paths, bool portable) {]=]
[=[    return Success;
}

} // namespace config]=]
[=[    return Success;
}
#endif // Vitox shim

// Vitox shim: programmatic entry to the YAML load path init_config() used (no command line)
ExitCode vitox_parse_config(Config &cfg, const fs::path &load_path, const fs::path &root_vita_fs_path) {
    return parse(cfg, load_path, root_vita_fs_path);
}

} // namespace config]=])

# util/src/fs_utils.cpp: SDL is used only by read_data() (SDL_IOFromFile/GetIOSize/ReadIO/CloseIO,
# plain whole-file reads). Guarded substitution: same semantics with std::ifstream (empty file or
# short read -> false, data cleared), no SDL. Everything else in the TU is unchanged.
vitox_substitute(vita3k/util/src/fs_utils.cpp fs_utils.cpp
[=[#include <SDL3/SDL_iostream.h>]=]
[=[#include <fstream> // Vitox shim: was <SDL3/SDL_iostream.h>]=]
[=[    SDL_IOStream *file = SDL_IOFromFile(fs_utils::path_to_utf8(path).c_str(), "rb");
    if (!file) {
        return false;
    }

    // Get the size of the file
    const Sint64 size = SDL_GetIOSize(file);
    if (size <= 0) {
        SDL_CloseIO(file);
        return false;
    }

    // Resize the vector to fit the file content
    data.resize(size);

    // Read the content of the file
    if (SDL_ReadIO(file, data.data(), size) != size) {
        SDL_CloseIO(file);
        data.clear();
        return false;
    }

    SDL_CloseIO(file);
    return true;]=]
[=[    // Vitox shim: std::ifstream instead of SDL_IOStream (same results)
    std::ifstream file(path.native(), std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }

    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return false;
    }

    data.resize(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(data.data()), size)) {
        data.clear();
        return false;
    }
    return true;]=]
# 0.0.9.0 (M5A, U3): copy_directory_contents' fs::relative (canonicalises: ACCESS_DENIED on Xbox) ->
# containment-checked lexical relative path; any path not strictly inside src_path aborts the copy.
[=[#include <util/string_utils.h>]=]
[=[#include <util/string_utils.h>

#include <util/log.h>
#include <vitox_lexical_relative.h> // Vitox shim (U3)]=]
[=[            const auto relative_path = fs::relative(src.path(), src_path);]=]
[=[            // Vitox shim (U3): lexical, containment-checked; fs::relative canonicalises (fails on Xbox)
            fs::path relative_path;
            std::string vitox_why;
            if (!vitox::lexical_relative(src.path(), src_path, relative_path, &vitox_why)) {
                LOG_ERROR("copy_directory_contents: refusing {} (base {}): {}", fs_utils::path_to_utf8(src.path()), fs_utils::path_to_utf8(src_path), vitox_why);
                return false;
            }]=])

# packages/src/sce_utils.cpp (content install): the same U3 substitution, PREPARED only. The TU is
# not linked (package install is out of scope and it needs OpenSSL); target vitox_m5a_sce_utils
# compiles the patched copy to show where it stands.
vitox_substitute(vita3k/packages/src/sce_utils.cpp sce_utils.cpp
[=[#include <util/string_utils.h>]=]
[=[#include <util/string_utils.h>

#include <vitox_lexical_relative.h> // Vitox shim (U3)]=]
[=[            const auto output_file_path = output_path / fs::relative(entry.path(), input_path);]=]
[=[            // Vitox shim (U3): lexical, containment-checked; fs::relative canonicalises (fails on Xbox)
            fs::path vitox_rel;
            std::string vitox_why;
            if (!vitox::lexical_relative(entry.path(), input_path, vitox_rel, &vitox_why)) {
                LOG_ERROR("Refusing {} (base {}): {}", fs_utils::path_to_utf8(entry.path()), fs_utils::path_to_utf8(input_path), vitox_why);
                continue;
            }
            const auto output_file_path = output_path / vitox_rel;]=]
[=[            const auto out_rel = fs::relative(output_file_path, cache_path);]=]
[=[            // Vitox shim (U3): log-only path; fall back to the full path if it is not under cache_path
            fs::path out_rel;
            if (!vitox::lexical_relative(output_file_path, cache_path, out_rel))
                out_rel = output_file_path;]=])
add_library(vitox_m5a_sce_utils STATIC EXCLUDE_FROM_ALL "${CMAKE_BINARY_DIR}/vitox-patched/sce_utils.cpp")
set_target_properties(vitox_m5a_sce_utils PROPERTIES FOLDER vitox-m5a)
target_include_directories(vitox_m5a_sce_utils BEFORE PRIVATE "${VITOX_M3}/sdl-shim" "${CMAKE_BINARY_DIR}/vitox-patched/include")
target_include_directories(vitox_m5a_sce_utils PRIVATE ${VITOX_M4_INCLUDES} "${VITOX_M3}/compat" "${VITA3K}/vita3k/packages/src")
target_compile_definitions(vitox_m5a_sce_utils PRIVATE VITOX_UWP)
target_compile_options(vitox_m5a_sce_utils PRIVATE "/FI${VITOX_M3}/compat/vitox_compat.h")

set(VITOX_M41_IO_SOURCES vita3k/io/src/device.cpp vita3k/io/src/filesystem.cpp vita3k/io/src/io.cpp vita3k/io/src/state_functions.cpp)
list(TRANSFORM VITOX_M41_IO_SOURCES PREPEND "${VITA3K}/" OUTPUT_VARIABLE _m41_io_abs)
# Each probe is linked twice: without and with Boost.Filesystem, so the closure is measured in layers.
foreach (variant nobfs bfs)
    add_library(vitox_m41_io_${variant} SHARED EXCLUDE_FROM_ALL "${VITOX_M3}/inv-io-config/inv_io.cpp" ${_m41_io_abs})
    add_library(vitox_m41_config_${variant} SHARED EXCLUDE_FROM_ALL "${VITOX_M3}/inv-io-config/inv_config.cpp"
        "${CMAKE_BINARY_DIR}/vitox-patched/config.cpp" "${VITA3K}/vita3k/config/src/settings.cpp")
    if (variant STREQUAL "bfs") # second layer: + fs_utils (shimmed) + input key names
        target_sources(vitox_m41_io_${variant} PRIVATE "${CMAKE_BINARY_DIR}/vitox-patched/fs_utils.cpp" "${VITOX_M3}/compat/vitox_lexical_relative.cpp")
        target_sources(vitox_m41_config_${variant} PRIVATE "${CMAKE_BINARY_DIR}/vitox-patched/fs_utils.cpp" "${VITOX_M3}/compat/vitox_lexical_relative.cpp"
            "${VITA3K}/vita3k/input/src/physical_key.cpp")
    endif()
    foreach (t vitox_m41_io_${variant} vitox_m41_config_${variant})
        set_target_properties(${t} PROPERTIES FOLDER vitox-m41)
        target_include_directories(${t} BEFORE PRIVATE "${VITOX_M3}/sdl-shim" "${CMAKE_BINARY_DIR}/vitox-patched/include")
        target_include_directories(${t} PRIVATE ${VITOX_M4_INCLUDES} "${VITA3K}/external/yaml-cpp/include" "${VITOX_M3}/compat")
        target_compile_definitions(${t} PRIVATE VITOX_UWP)
        target_compile_options(${t} PRIVATE "/FI${VITOX_M3}/compat/vitox_compat.h")
        target_link_libraries(${t} PRIVATE vitox_core)
        if (variant STREQUAL "bfs")
            target_link_libraries(${t} PRIVATE vitox_boost_filesystem)
        endif()
    endforeach()
    target_link_libraries(vitox_m41_config_${variant} PRIVATE yaml-cpp::yaml-cpp pugixml::pugixml)
endforeach()

# ---- 0.0.8.0 (M4.2): vitox_hostio - real IO + CLI-free config, linked into the app -------------
# The M4.1 inventory closure, now packaged: real io/*.cpp, settings.cpp, physical_key.cpp; guarded
# shims of config.cpp (CLI11 compiled out) and fs_utils.cpp (read_data without SDL); Boost.Filesystem
# built out-of-tree with the instrumented WindowsStore compat layer; yaml-cpp + pugixml.
# M8C.3 (approved): device.cpp construct_emulated_path gets a VITOX_UWP branch - only while a separate firmware root is
# set (compat/vitox_firmware_root.h; staged-firmware probes), os0/vs0/sa0/pd0 resolve under it. Unset = upstream.
vitox_substitute(vita3k/io/src/device.cpp m8c_device.cpp
[=[#include <io/device.h>
]=]
[=[#include <io/device.h>
#ifdef VITOX_UWP
#include <vitox_firmware_root.h> // Vitox M8C.3
#endif
]=]
[=[    return fs_utils::construct_file_name(base_path, get_device_string(dev, false), path, ext);
}]=]
[=[#ifdef VITOX_UWP
    // Vitox M8C.3: firmware devices from the separate (staged) firmware root, only while one is set
    if (const std::filesystem::path *fw = vitox_firmware_root();
        fw && (dev == VitaIoDevice::os0 || dev == VitaIoDevice::vs0 || dev == VitaIoDevice::sa0 || dev == VitaIoDevice::pd0))
        return fs_utils::construct_file_name(fs::path(fw->native()), get_device_string(dev, false), path, ext);
#endif
    return fs_utils::construct_file_name(base_path, get_device_string(dev, false), path, ext);
}]=])
set(VITOX_HOSTIO_VITA3K_SOURCES
    vita3k/io/src/filesystem.cpp vita3k/io/src/io.cpp vita3k/io/src/state_functions.cpp
    vita3k/config/src/settings.cpp vita3k/input/src/physical_key.cpp
    vita3k/util/src/logging.cpp) # 0.0.9.0 (M5A): real logging::init
list(TRANSFORM VITOX_HOSTIO_VITA3K_SOURCES PREPEND "${VITA3K}/" OUTPUT_VARIABLE _hostio_abs)
add_library(vitox_hostio STATIC ${_hostio_abs}
    "${CMAKE_BINARY_DIR}/vitox-patched/m8c_device.cpp" "${VITOX_M3}/compat/vitox_firmware_root.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/config.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/fs_utils.cpp"
    "${VITOX_M3}/compat/vitox_lexical_relative.cpp"
    "${VITOX_M3}/core-probe/vitox_io_probe.cpp")
set_target_properties(vitox_hostio PROPERTIES FOLDER vitox)
# M3 B5 (approved): logging.cpp calls SetConsoleTitle("...") with a narrow literal; WindowsStore
# projects define UNICODE/_UNICODE, so this one TU is compiled without them, as on desktop.
set_source_files_properties("${VITA3K}/vita3k/util/src/logging.cpp" PROPERTIES COMPILE_OPTIONS "/UUNICODE;/U_UNICODE")
target_include_directories(vitox_hostio BEFORE PRIVATE "${VITOX_M3}/sdl-shim" "${CMAKE_BINARY_DIR}/vitox-patched/include")
target_include_directories(vitox_hostio PRIVATE ${VITOX_M4_INCLUDES} "${VITA3K}/external/yaml-cpp/include" "${VITOX_M3}/compat" "${VITOX_M3}/core-probe")
target_compile_definitions(vitox_hostio PRIVATE VITOX_UWP)
target_compile_options(vitox_hostio PRIVATE "/FI${VITOX_M3}/compat/vitox_compat.h")
target_link_libraries(vitox_hostio PUBLIC vitox_emuenv vitox_boost_filesystem yaml-cpp::yaml-cpp pugixml::pugixml)
message(STATUS "Vitox hostio: ${VITOX_HOSTIO_VITA3K_SOURCES} + shimmed config.cpp fs_utils.cpp + Boost.Filesystem(compat) + yaml-cpp + pugixml")

# ---- M5A inventory: init_libraries() / module_parent closure (compile + LINK probes, never packaged)
# Source list = Vita3K's own Windows SOURCE_LIST of vita3k/modules/CMakeLists.txt (215 TUs).
file(STRINGS "${VITOX_M3}/inv-modules/modules_win_sources.txt" _m5a_mod_rel)
list(TRANSFORM _m5a_mod_rel PREPEND "${VITA3K}/vita3k/modules/" OUTPUT_VARIABLE _m5a_mod_abs)
set(VITOX_M5A_MOD_INCLUDES ${VITOX_M4_INCLUDES} "${VITA3K}/vita3k/modules/taiHEN" "${VITA3K}/external/substitute"
    "${VITA3K}/external/dlmalloc" "${VITA3K}/external/xxHash" "${VITA3K}/external/LibAtrac9/C/src" "${VITA3K}/external/ffmpeg/include")
function(vitox_m5a_flags t)
    set_target_properties(${t} PROPERTIES FOLDER vitox-m5a)
    target_include_directories(${t} BEFORE PRIVATE "${VITOX_M3}/sdl-shim" "${CMAKE_BINARY_DIR}/vitox-patched/include")
    target_include_directories(${t} PRIVATE ${VITOX_M5A_MOD_INCLUDES})
    target_compile_definitions(${t} PRIVATE VITOX_UWP ONLY_MSPACES=1 USE_LOCK=0) # + dlmalloc's PUBLIC definitions (external/CMakeLists.txt)
    # /sdl- : Vita3K's desktop flags (WindowsStore defaults to /sdl, which turns C4146 & co. into errors)
    target_compile_options(${t} PRIVATE "/FI${VITOX_M3}/compat/vitox_compat.h" "/sdl-" "/bigobj")
    target_link_libraries(${t} PRIVATE vitox_hostio) # usage requirements: Boost headers, fmt (/utf-8), spdlog, ...
endfunction()
# 1) every module TU, compile-only: first WindowsStore blocker per TU (out\m5a\modules_compile.log)
add_library(vitox_m5a_modules_compile STATIC EXCLUDE_FROM_ALL ${_m5a_mod_abs})
vitox_m5a_flags(vitox_m5a_modules_compile)
# 2) stock-semantics init_libraries(): the same body as module_parent.cpp (LIBRARY() over
#    library_init_list.inc) + the TUs that define those LIBRARY_INITs. SceSysmem is in vitox_core.
file(WRITE "${CMAKE_BINARY_DIR}/vitox-m5a/inv_init_libraries.cpp.tmp"
"// Generated (Vitox M5A inventory): Vita3K's init_libraries() body, outside module_parent.cpp.\n\
#include <emuenv/state.h>\n#include <module/bridge.h>\n\
#define LIBRARY(name) extern const LibraryInitFn import_library_init_##name;\n#include <modules/library_init_list.inc>\n#undef LIBRARY\n\
extern \"C\" __declspec(dllexport) void vitox_m5a_init_libraries(EmuEnvState &emuenv) {\n\
#define LIBRARY(name) import_library_init_##name(emuenv);\n#include <modules/library_init_list.inc>\n#undef LIBRARY\n}\n")
file(COPY_FILE "${CMAKE_BINARY_DIR}/vitox-m5a/inv_init_libraries.cpp.tmp" "${CMAKE_BINARY_DIR}/vitox-m5a/inv_init_libraries.cpp" ONLY_IF_DIFFERENT)
add_library(vitox_m5a_initlib SHARED EXCLUDE_FROM_ALL "${CMAKE_BINARY_DIR}/vitox-m5a/inv_init_libraries.cpp"
    "${VITA3K}/vita3k/modules/SceAppMgr/SceSharedFb.cpp" "${VITA3K}/vita3k/modules/SceAudiodec/SceAudiodecUser.cpp"
    "${VITA3K}/vita3k/modules/SceFiber/SceFiber.cpp" "${VITA3K}/vita3k/modules/taiHEN/taiHEN.cpp")
vitox_m5a_flags(vitox_m5a_initlib)
target_link_libraries(vitox_m5a_initlib PRIVATE vitox_hostio)
# 3) module_parent.cpp itself (resolve_import / load_module / init_libraries) + every module TU,
#    minus those already in vitox_core. Set VITOX_M5A_PARENT_EXCLUDE to drop TUs that fail to compile.
set(VITOX_M5A_PARENT_EXCLUDE "" CACHE STRING "module TUs (relative to vita3k/modules) left out of the module_parent link probe")
set(_m5a_parent ${_m5a_mod_rel})
list(REMOVE_ITEM _m5a_parent SceDriverUser/SceRtcUser.cpp SceRtc/SceRtc.cpp SceSysmem/SceSysmem.cpp SceSysmem/SceSysmemForDriver.cpp ${VITOX_M5A_PARENT_EXCLUDE})
list(TRANSFORM _m5a_parent PREPEND "${VITA3K}/vita3k/modules/")
add_library(vitox_m5a_module_parent SHARED EXCLUDE_FROM_ALL ${_m5a_parent})
vitox_m5a_flags(vitox_m5a_module_parent)
target_link_libraries(vitox_m5a_module_parent PRIVATE vitox_hostio)

# ---- M5B inventory: closure of the homebrew's class-B import TUs (link probe, never packaged) ----
add_library(vitox_m5b_b_tus SHARED EXCLUDE_FROM_ALL
    "${VITA3K}/vita3k/modules/SceLibKernel/SceLibKernel.cpp" "${VITA3K}/vita3k/modules/SceIofilemgr/SceIofilemgr.cpp")
vitox_m5a_flags(vitox_m5b_b_tus)
# layer 2: + the CALL_EXPORT targets (SceThreadmgr, SceModulemgr) + dlmalloc (external, C)
add_library(vitox_dlmalloc STATIC EXCLUDE_FROM_ALL "${VITA3K}/external/dlmalloc/dlmalloc.cc")
target_include_directories(vitox_dlmalloc PUBLIC "${VITA3K}/external/dlmalloc")
target_compile_definitions(vitox_dlmalloc PUBLIC ONLY_MSPACES=1 USE_LOCK=0)
set_target_properties(vitox_dlmalloc PROPERTIES FOLDER vitox-m5b)
add_library(vitox_m5b_b_tus2 SHARED EXCLUDE_FROM_ALL
    "${VITA3K}/vita3k/modules/SceLibKernel/SceLibKernel.cpp" "${VITA3K}/vita3k/modules/SceIofilemgr/SceIofilemgr.cpp"
    "${VITA3K}/vita3k/modules/SceKernelThreadMgr/SceThreadmgr.cpp" "${VITA3K}/vita3k/modules/SceKernelModulemgr/SceModulemgr.cpp")
vitox_m5a_flags(vitox_m5b_b_tus2)
target_link_libraries(vitox_m5b_b_tus2 PRIVATE vitox_dlmalloc)

# ---- 0.0.10.0 (M5B): vitox_hle - REAL module_parent.cpp (load_module / call_import / start_module /
# unload_module / init_exported_vars) with ONLY its NID tables narrowed to the selected subset ----
# Guarded substitution, nothing else in the TU changes:
#   - resolve_import(): Vita3K's switch over all of nids.inc -> vitox_resolve_selected_import()
#     (every selected NID is the real ImportFn; unselected -> nullptr -> Vita3K's own unknown-NID path)
#   - the extern declarations of every import_* / import_library_init_* object are dropped
#   - init_exported_vars(): variable-export table -> empty (no variable exports selected)
#   - init_libraries(): -> vitox_init_selected_libraries() (the real LIBRARY_INITs needed)
#   - call_import(): + one observation call (vitox_note_import_call) before dispatch
#   - load_module(): decrypt_fself (sce_utils.cpp, OpenSSL) -> its unencrypted-only path (compat/vitox_fself)
vitox_substitute(vita3k/modules/module_parent.cpp module_parent.cpp
[=[#include <unordered_set>
]=]
[=[#include <unordered_set>

#include <vitox_fself.h>           // Vitox shim
#include <vitox_import_dispatch.h> // Vitox shim
]=]
[=[#define LIBRARY(name) extern const LibraryInitFn import_library_init_##name;
#include <modules/library_init_list.inc>
#undef LIBRARY

#define VAR_NID(name, nid) extern const ImportVarFactory import_##name;
#define NID(name, nid) extern const ImportFn import_##name;
#include <nids/nids.inc>
#undef NID
#undef VAR_NID
]=]
[=[// Vitox shim: no extern declarations of every module's import_* objects (selected subset only)
]=]
[=[static const ImportFn *resolve_import(uint32_t nid) {
    switch (nid) {
#define VAR_NID(name, nid)
#define NID(name, nid) \
    case nid:          \
        return &import_##name;
#include <nids/nids.inc>
#undef NID
#undef VAR_NID
    default:
        return nullptr;
    }
}]=]
[=[static const ImportFn *resolve_import(uint32_t nid) {
    // Vitox shim: selected-subset dispatcher (core-probe/vitox_import_dispatch.cpp), real ImportFns
    const VitoxSelectedImport *selected = vitox_resolve_selected_import(nid);
    return selected ? selected->fn : nullptr;
}]=]
[=[    const auto var_exports = std::to_array<VarExport>({
#define NID(name, nid)
#define VAR_NID(name, nid) \
    {                      \
        nid,               \
        import_##name      \
    },
#include <nids/nids.inc>
#undef VAR_NID
#undef NID
    });]=]
[=[    // Vitox shim: the selected variable exports only. M8C.3 (approved): Vita3K's REAL VAR_EXPORT(__stack_chk_guard)
    // (modules/SceLibKernel/SceLibKernel.cpp: a guest-allocated word, value chosen by Vita3K), needed by firmware libpgf.
    extern const ImportVarFactory import___stack_chk_guard;
    const std::vector<VarExport> var_exports{ { 0x93B8AA67 /* VAR_NID(__stack_chk_guard) in nids.inc */, import___stack_chk_guard } };]=]
[=[    const ImportFn *fn = resolve_import(nid);
    if (fn) {]=]
[=[    const ImportFn *fn = resolve_import(nid);
    vitox_note_import_call(nid, thread_id, fn != nullptr); // Vitox shim: observation only
    vitox_observe_import_args(emuenv, cpu, nid); // Vitox M7B.6: read-only argument logging (diagnostic)
    if (fn) {]=]
# M8B.1 diagnostic trap (only when the launcher enables it): an unselected NID stops the guest, upstream's return-0
# below is not reached
[=[    } else {
        const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
        // make the function return 0
        write_reg(*thread->cpu, 0, 0);
]=]
[=[    } else if (vitox_import_trap(emuenv, cpu, nid, thread_id)) {
        // Vitox M8B.1 diagnostic trap: every guest thread is stopped and nothing is returned to the guest
    } else {
        const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
        // make the function return 0
        write_reg(*thread->cpu, 0, 0);
]=]
[=[    module_buffer = decrypt_fself(module_buffer, emuenv.license.rif[emuenv.io.title_id].key);]=]
[=[    // Vitox shim: the unencrypted-only path by default (M5B); M8C.2 (staged LLE precheck only) switches to Vita3K's REAL
    // decrypt_fself (sce_utils.cpp over the VITOX_UWP BCrypt EVP shim, vitox_m8c_firmware) - no fallback when enabled
    module_buffer = vitox_fself_real_decrypt() ? decrypt_fself(module_buffer, emuenv.license.rif[emuenv.io.title_id].key)
                                               : vitox_decrypt_fself_unencrypted_only(module_buffer);]=]
[=[#define LIBRARY(name) import_library_init_##name(emuenv);
#include <modules/library_init_list.inc>
#undef LIBRARY]=]
[=[    vitox_init_selected_libraries(emuenv); // Vitox shim: the selected modules' real LIBRARY_INITs]=])
set_target_properties(vitox_dlmalloc PROPERTIES EXCLUDE_FROM_ALL OFF)
file(GLOB _vitox_libatrac9 "${VITA3K}/external/LibAtrac9/C/src/*.c") # M9A.6: ATRAC9 decoder (plain C)
add_library(vitox_hle STATIC
    "${CMAKE_BINARY_DIR}/vitox-patched/module_parent.cpp"
    "${VITA3K}/vita3k/module/src/load_module.cpp"
    "${VITA3K}/vita3k/modules/SceLibKernel/SceLibKernel.cpp"
    "${VITA3K}/vita3k/modules/SceIofilemgr/SceIofilemgr.cpp"
    "${VITA3K}/vita3k/modules/SceKernelThreadMgr/SceThreadmgr.cpp"
    "${VITA3K}/vita3k/modules/SceKernelModulemgr/SceModulemgr.cpp"
    "${VITOX_M3}/compat/vitox_fself.cpp"
    "${VITOX_M3}/compat/vitox_sha256.cpp"
    "${VITOX_M3}/core-probe/vitox_import_dispatch.cpp"
    "${VITOX_M3}/core-probe/vitox_homebrew_probe.cpp"
    # M8B.2 (approved): REAL SceCtrl HLE + REAL vita3k/ctrl, fed by the WGI-backed 18-function SDL3 gamepad shim
    "${VITA3K}/vita3k/modules/SceCtrl/SceCtrl.cpp"
    "${VITA3K}/vita3k/ctrl/src/ctrl.cpp"
    "${VITOX_M3}/sdl-shim/vitox_sdl_gamepad.cpp"
    # M8B.1 (VitaGraphs runtime trap 8): REAL SceTouch HLE + REAL vita3k/touch (no host touch: peek returns no reports)
    "${VITA3K}/vita3k/modules/SceTouch/SceTouch.cpp"
    "${VITA3K}/vita3k/touch/src/touch.cpp"
    # M8B.1 (trap 10): REAL SceSysmodule (VitaGraphs loads SCE_SYSMODULE_NET = HLE path: marked loaded, no LLE/firmware)
    "${VITA3K}/vita3k/modules/SceSysmodule/SceSysmodule.cpp"
    # M8B.1 (trap 11): REAL SceAppUtil (sceAppUtilInit is an upstream UNIMPLEMENTED() export: logs once, returns 0)
    "${CMAKE_BINARY_DIR}/vitox-patched/m8b_SceAppUtil.cpp"
    # M8B.1 (trap 13): REAL SceCommonDialog (sceCommonDialogSetConfigParam is an upstream UNIMPLEMENTED() export)
    "${CMAKE_BINARY_DIR}/vitox-patched/vx_SceCommonDialog.cpp" # 0.1.1.4: patched copy (msg dialog re-init after FINISHED), see vitox_substitute below
    # M8F.2A (approved): REAL frontend-neutral IME helpers (ime_commit_text, submit/dismiss_current_ime, finish/cancel)
    "${VITA3K}/vita3k/ime/src/ime.cpp"
    "${VITA3K}/vita3k/ime/src/active_ime.cpp"
    # M8B.1 (trap 14): REAL SceShellUtil (sceShellUtilInitEvents is an upstream UNIMPLEMENTED() export)
    "${VITA3K}/vita3k/modules/SceShellSvc/SceShellUtil.cpp"
    # M9A.4 (approved single-NID closure, app-staging trap 1 in SceLibc module_start): REAL SceProcessmgr
    # (sceKernelGetProcessParam returns the registered KernelState::process_param)
    "${VITA3K}/vita3k/modules/SceProcessmgr/SceProcessmgr.cpp"
    # M9A.4 (approved single-NID closure, app-staging trap 5 at the end of SceLibc module_start): REAL sceKernelExitThread
    # (upstream thread->exit(status); this TU defines only that export)
    "${VITA3K}/vita3k/modules/SceKernelThreadMgr/SceThreadmgrCoredumpTime.cpp"
    # 0.0.24.72 (M9A.6, Gravity Rush gameplay trap): REAL SceThreadmgrForDriver - the eboot imports NID 0x01414F0B from
    # SceThreadmgr, which upstream exports as ksceKernelGetThreadCurrentPriority (CALL_EXPORT to the selected user one)
    "${VITA3K}/vita3k/modules/SceKernelThreadMgr/SceThreadmgrForDriver.cpp"
    # M9A.4 (approved single-NID closure, app-staging trap 7 in SceLibFios2 module_start): REAL SceFios2User
    # (only sceFiosOverlayGetList02 is selected; the other overlay exports stay unselected)
    "${VITA3K}/vita3k/modules/SceDriverUser/SceFios2User.cpp"
    # M9A.4J (approved A2, app-staging trap 9 on the main thread): REAL SceNet - only sceNetInit is selected.
    # The TU's other exports reference the socket/epoll backends, linked here but never exposed to the guest
    # (unselected -> fail-closed). Address helpers: compat/vitox_net_addr.cpp (net_utils.cpp stays excluded: curl).
    # M9A.4K: SceNet.cpp is the shimmed copy below (sceNetGetMacAddress: GetAdaptersInfo -> GetAdaptersAddresses).
    "${CMAKE_BINARY_DIR}/vitox-patched/m9a4_SceNet.cpp"
    "${VITA3K}/vita3k/net/src/net.cpp"
    "${VITA3K}/vita3k/net/src/epoll.cpp"
    "${VITA3K}/vita3k/net/src/posixsocket.cpp"
    "${VITA3K}/vita3k/net/src/p2psocket.cpp"
    # M9A.4M: upstream net_utils.cpp (patched copy below) replaces 0.0.24.14's compat/vitox_net_addr.cpp - SceHttp.cpp
    # needs its HTTP string helpers too; curl and GetAdaptersInfo are the only two substitutions.
    "${CMAKE_BINARY_DIR}/vitox-patched/m9a4_net_utils.cpp"
    # M9A.4M (autonomous boot work, app-staging trap 12 on the main thread): REAL SceSsl + SceHttp + vita3k/http, built
    # against compat/openssl-offline (no TLS implementation: SSL_CTX_new/SSL_new/SSL_connect fail, see its ssl.h).
    "${VITA3K}/vita3k/modules/SceSsl/SceSsl.cpp"
    "${VITA3K}/vita3k/modules/SceHttp/SceHttp.cpp"
    "${VITA3K}/vita3k/http/src/http.cpp"
    "${VITOX_M3}/compat/vitox_ssl_offline.cpp"
    # M9A.4K (autonomous boot work, app-staging trap 10 on the main thread): REAL SceNetCtl (init/callbacks/term).
    # sceNetCtlInit starts upstream's adhoc host thread; it only leaves its wait when sceNetCheckDialogInit(ADHOC_CONN)
    # sets adhocCondVarReady - imported by this title but never selected (M9A.5 compile-time deny list in
    # vitox_import_dispatch.cpp) - and ~NetCtlState joins it at cleanup.
    # M9A.4N: patched copy - offline sceNetCtlInetGetState (see the vitox_substitute below)
    "${CMAKE_BINARY_DIR}/vitox-patched/m9a4_SceNetCtl.cpp"
    # SceNetCtl's adhoc thread calls sceNpManagerGetNpId (CALL_EXPORT): REAL SceNpManager + np/src/init.cpp (NpState)
    "${VITA3K}/vita3k/modules/SceNpManager/SceNpManager.cpp"
    "${VITA3K}/vita3k/np/src/init.cpp"
    # M9A.4O (autonomous boot work, app-staging trap 14 from SceLibFios2 code on the main thread): REAL SceAppMgr +
    # SceAppMgrUser (sceAppMgrIsGameProgram returns 0; _sceAppMgrGetAppState -> __sceAppMgrGetAppState zeroes the state)
    "${VITA3K}/vita3k/modules/SceAppMgr/SceAppMgr.cpp"
    "${VITA3K}/vita3k/modules/SceDriverUser/SceAppMgrUser.cpp"
    # 0.0.29.3 (user-approved Uncharted bring-up, closure UGA-2): REAL SceJpegEncUser (codec), SceDbg (assertion
    # handler), SceNpCommon (NpAuth: UNIMPLEMENTED/STUBBED upstream, no network)
    "${VITA3K}/vita3k/modules/SceJpegEnc/SceJpegEncUser.cpp"
    "${VITA3K}/vita3k/modules/SceLibDbg/SceDbg.cpp"
    "${VITA3K}/vita3k/modules/SceNpCommon/SceNpCommon.cpp"
    # M9A.4R (autonomous boot work, app-staging trap 17 sceAudioOutOpenPort on the main thread): REAL SceAudio + REAL
    # vita3k/audio AudioState (patched copy below) over the Vitox XAudio2 adapter (compat/vitox_xaudio2_audio.cpp, UWP
    # platform port: upstream's SDL3 / cubeb backends are not part of this build). SceAudioIn is not linked.
    "${VITA3K}/vita3k/modules/SceAudio/SceAudio.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/m9a4_audio.cpp"
    "${VITOX_M3}/compat/vitox_xaudio2_audio.cpp"
    # M9A.4T (autonomous boot work, app-staging trap 19 sceNpBasicInit): REAL upstream NP service TUs (stub-only files:
    # every export UNIMPLEMENTED). Only the library init/term exports are selected (offline: they initialise locally).
    "${VITA3K}/vita3k/modules/SceNpBasic/SceNpBasic.cpp"
    "${VITA3K}/vita3k/modules/SceNpScore/SceNpScore.cpp"
    "${VITA3K}/vita3k/modules/SceNpTus/SceNpTus.cpp"
    "${VITA3K}/vita3k/modules/SceNpMatching2/SceNpMatching2.cpp"
    "${VITA3K}/vita3k/modules/SceNpUtility/SceNpUtility.cpp"
    # M9A.4U (autonomous boot work, app-staging trap 20 sceMotionReset): REAL SceMotion + REAL vita3k/motion/motion.cpp;
    # host sensor enumeration = sdl-shim/vitox_sdl_sensor_enum.cpp (no built-in sensors on Xbox -> no device motion support)
    "${VITA3K}/vita3k/modules/SceDriverUser/SceMotion.cpp"
    "${VITA3K}/vita3k/motion/src/motion.cpp"
    "${VITOX_M3}/sdl-shim/vitox_sdl_sensor_enum.cpp"
    # M9A.4V (autonomous boot work, app-staging trap 21 sceNpTrophyInit): REAL SceNpTrophy + REAL np/src/trophy
    # (context/collection/TRP parser; no decryption: the TRP's files are copied as-is to ux0:user/00/trophy/conf, the
    # progress file lives in ux0:user/00/trophy/data - new files only, as on desktop Vita3K)
    "${VITA3K}/vita3k/modules/SceNpTrophy/SceNpTrophy.cpp"
    "${VITA3K}/vita3k/np/src/trophy/context.cpp"
    "${VITA3K}/vita3k/np/src/trophy/collection.cpp"
    "${VITA3K}/vita3k/np/src/trophy/trp_parser.cpp"
    # M9A.6 (Gravity Rush, app-staging trap 1 sceKernelSetGPO): REAL upstream SceDebugLed (debug LED: UNIMPLEMENTED, returns 0)
    # and ScePower (power configuration / battery queries; host battery = sdl-shim/vitox_sdl_power.cpp, "no battery")
    "${VITA3K}/vita3k/modules/SceSysmem/SceDebugLed.cpp"
    "${VITA3K}/vita3k/modules/ScePower/ScePower.cpp"
    "${VITOX_M3}/sdl-shim/vitox_sdl_power.cpp"
    # M9A.6 (GR trap 2 sceScreenShotEnable): REAL upstream SceScreenShot (UNIMPLEMENTED exports), SceFiber, SceNpActivity
    "${VITA3K}/vita3k/modules/SceScreenShot/SceScreenShot.cpp"
    "${VITA3K}/vita3k/modules/SceFiber/SceFiber.cpp"
    "${VITA3K}/vita3k/modules/SceNpActivity/SceNpActivity.cpp"
    # M9A.6 (GR trap 3, libfios2 'SceFiosCallback' thread): REAL upstream SceFios2Kernel (overlay kernel API: UNIMPLEMENTED,
    # ResolveWithRangeSync = identity path copy - no overlays)
    "${VITA3K}/vita3k/modules/SceFios2Kernel/SceFios2Kernel.cpp"
    # M9A.6 (GR trap 5 sceAudiodecInitLibrary / trap 7 sceJpegInitMJpeg): REAL SceAudiodec, SceJpeg, SceVideodec and the REAL
    # vita3k/codec decoders (ATRAC9 over external/LibAtrac9; AAC, MP3, MJPEG, H264 over FFmpeg n7.1.1 built for WindowsStore:
    # toolchain/ffmpeg-n7.1.1-uwp, decoders-only, same versions as Vita3K's external/ffmpeg headers; see the app link list)
    "${VITA3K}/vita3k/modules/SceAudiodec/SceAudiodecUser.cpp"
    "${VITA3K}/vita3k/modules/SceJpeg/SceJpegUser.cpp"
    "${VITA3K}/vita3k/modules/SceVideodec/SceVideodecUser.cpp"
    "${VITA3K}/vita3k/modules/SceCodecEngine/SceCodecEngineUser.cpp"
    # M9A.6 (GR trap 9 sceMp4OpenFile, 'BackgroundFiber::Precedence'): REAL upstream SceMp4 - all 14 exports UNIMPLEMENTED (return 0)
    "${VITA3K}/vita3k/modules/SceMp4/SceMp4.cpp"
    "${VITA3K}/vita3k/codec/src/decoder.cpp"
    "${VITA3K}/vita3k/codec/src/atrac9.cpp"
    "${VITA3K}/vita3k/codec/src/aac.cpp"
    "${VITA3K}/vita3k/codec/src/mp3.cpp"
    "${VITA3K}/vita3k/codec/src/mjpeg.cpp"
    "${VITA3K}/vita3k/codec/src/h264.cpp"
    # M9A.7 (Danganronpa, first trap + import batch): REAL upstream SceNgs (with the vita3k/ngs engine, which
    # upstream app_init starts with ngs::init), SceAvPlayer (over codec/player.cpp + FFmpeg), SceIme and SceLiveAreaUtil
    "${VITA3K}/vita3k/modules/SceNgsUser/SceNgs.cpp"
    "${VITA3K}/vita3k/modules/SceAvPlayer/SceAvPlayer.cpp"
    "${VITA3K}/vita3k/modules/SceIme/SceIme.cpp"
    "${VITA3K}/vita3k/modules/SceLiveArea/SceLiveAreaUtil.cpp"
    # M9A.9 (Catherine Full Body, pre-run import batch CFB-1): upstream stub TUs (UNIMPLEMENTED / STUBBED only, no host
    # sockets): online play (RUDP, adhoc matching, voice QoS, NP WebApi + invitation dialog), game update, libJson
    "${VITA3K}/vita3k/modules/SceRudp/SceLibRudp.cpp"
    # 0.1.1.0 (BOOT-4: Killzone Mercenary / WipEout 2048 / Sly Cooper / Project DIVA X pre-run import closure): upstream
    # stub TUs (every export UNIMPLEMENTED upstream: no host access) - coredump handlers, NP party, photo export, shutter
    # sound, system gesture recognizers, and the libult debug/perf hooks (SceUlobjMgr / SceUlobjDbg / ScePerf)
    "${VITA3K}/vita3k/modules/SceCoredump/SceCoredump.cpp"
    "${VITA3K}/vita3k/modules/SceCoredump/SceCoredumpNounlink.cpp"
    "${VITA3K}/vita3k/modules/SceNpParty/SceNpPartyGameUtil.cpp"
    "${VITA3K}/vita3k/modules/ScePhotoExport/ScePhotoExport.cpp"
    "${VITA3K}/vita3k/modules/SceShutterSound/SceShutterSound.cpp"
    "${VITA3K}/vita3k/modules/SceSystemGesture/SceSystemGesture.cpp"
    "${VITA3K}/vita3k/modules/SceUlobjMgr/SceUlobjMgr.cpp"
    "${VITA3K}/vita3k/modules/SceUlobjDbg/SceUlobjDbg.cpp"
    "${VITA3K}/vita3k/modules/ScePerf/ScePerf.cpp"
    # 0.1.1.1 (BOOT-4, Project DIVA X): upstream SceCamera module over the inert camera backend (compat/vitox_camera_inert.cpp,
    # hash-pinned above): every camera open fails NOT_OPEN, no SDL camera, no host device
    "${VITA3K}/vita3k/modules/SceCamera/SceCamera.cpp"
    "${VITA3K}/vita3k/modules/SceNetAdhocMatching/SceNetAdhocMatching.cpp"
    "${VITA3K}/vita3k/modules/SceVoiceQoS/SceVoiceQoS.cpp"
    "${VITA3K}/vita3k/modules/SceNpWebApi/SceNpWebApi.cpp"
    "${VITA3K}/vita3k/modules/SceCommonDialog/SceNpWebApiCommonDialog.cpp"
    "${VITA3K}/vita3k/modules/SceGameUpdate/SceGameUpdate.cpp"
    "${VITA3K}/vita3k/modules/SceLibJson/SceLibJson.cpp"
    "${VITA3K}/vita3k/codec/src/player.cpp"
    "${VITA3K}/vita3k/codec/src/pcm.cpp"
    "${VITA3K}/vita3k/ngs/src/modules/atrac9.cpp" "${VITA3K}/vita3k/ngs/src/modules/compressor.cpp" "${VITA3K}/vita3k/ngs/src/modules/delay.cpp"
    "${VITA3K}/vita3k/ngs/src/modules/distortion.cpp" "${VITA3K}/vita3k/ngs/src/modules/envelope.cpp" "${VITA3K}/vita3k/ngs/src/modules/equalizer.cpp"
    "${VITA3K}/vita3k/ngs/src/modules/filter.cpp" "${VITA3K}/vita3k/ngs/src/modules/generator.cpp" "${VITA3K}/vita3k/ngs/src/modules/mixer.cpp"
    "${VITA3K}/vita3k/ngs/src/modules/output.cpp" "${VITA3K}/vita3k/ngs/src/modules/pauser.cpp" "${VITA3K}/vita3k/ngs/src/modules/pitchshift.cpp"
    "${VITA3K}/vita3k/ngs/src/modules/player.cpp" "${VITA3K}/vita3k/ngs/src/modules/reverb.cpp" "${VITA3K}/vita3k/ngs/src/definitions.cpp"
    "${VITA3K}/vita3k/ngs/src/ngs.cpp" "${VITA3K}/vita3k/ngs/src/rate_resampler.cpp" "${VITA3K}/vita3k/ngs/src/route.cpp"
    "${VITA3K}/vita3k/ngs/src/scheduler.cpp"
    ${_vitox_libatrac9})
# M9A.5 audio.cpp: open_port returns nullptr without a backend / on a failed host stream (no null dereference).
# M9A.4R audio.cpp: the backend switch gains "XAudio2" (the only backend here; SDL/Cubeb names log an error as an
# unknown adapter does upstream), and deinit's SDL audio-in stream release is compiled out (SceAudioIn is not linked,
# so in_port is never running).
vitox_substitute(vita3k/audio/src/audio.cpp m9a4_audio.cpp
[=[#include <audio/impl/cubeb_audio.h>
#include <audio/impl/sdl_audio.h>
]=]
[=[#include <vitox_xaudio2_audio.h> // Vitox shim (M9A.4R): was cubeb_audio.h / sdl_audio.h
]=]
[=[        SDL_DestroyAudioStream(static_cast<SDL_AudioStream *>(in_port.id));
]=]
[=[        // Vitox shim (M9A.4R): no SDL audio-in stream in this build (SceAudioIn not linked; never running)
]=]
[=[    if (adapter_name == "SDL") {
        adapter = std::make_unique<SDLAudioAdapter>(*this);
    } else if (adapter_name == "Cubeb") {
        adapter = std::make_unique<CubebAudioAdapter>(*this);
    } else {]=]
[=[    if (adapter_name == "XAudio2") { // Vitox shim (M9A.4R): the UWP backend (SDL / Cubeb are not built)
        adapter = std::make_unique<VitoxXAudio2Adapter>(*this);
    } else {]=]
[=[    AudioOutPortPtr port = adapter->open_port(nb_channels, freq, nb_sample);
    set_volume(*port, port->volume);
    return port;]=]
[=[    // Vitox shim (M9A.5): no backend (init failed) or a failed host stream -> nullptr; sceAudioOutOpenPort / SetConfig
    // already turn that into SCE_AUDIO_OUT_ERROR_NOT_OPENED (upstream dereferenced both)
    if (!adapter)
        return nullptr;
    AudioOutPortPtr port = adapter->open_port(nb_channels, freq, nb_sample);
    if (port)
        set_volume(*port, port->volume);
    return port;]=])
# M9A.4N explicit OFFLINE adaptation (documented semantic change): upstream sceNetCtlInetGetState unconditionally reports
# SCE_NET_CTL_STATE_IPOBTAINED (connected). This build has no network capability and no TLS, so the honest state is
# DISCONNECTED - the same state sceNetCtlCheckCallback already delivers to registered callbacks (DISCONNECTED event).
vitox_substitute(vita3k/modules/SceNetCtl/SceNetCtl.cpp m9a4_SceNetCtl.cpp
[=[    *state = SCE_NET_CTL_STATE_IPOBTAINED;
    return STUBBED("state = SCE_NETCTL_STATE_CONNECTED");]=]
[=[    // Vitox shim (M9A.4N): offline build - report DISCONNECTED instead of upstream's unconditional IPOBTAINED
    *state = SCE_NET_CTL_STATE_DISCONNECTED;
    LOG_INFO_ONCE("Vitox offline: sceNetCtlInetGetState -> SCE_NET_CTL_STATE_DISCONNECTED");
    return 0;]=])
set_property(SOURCE "${CMAKE_BINARY_DIR}/vitox-patched/m9a4_SceNetCtl.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES "${VITA3K}/vita3k/modules/SceNetCtl")
# M9A.6: FFmpeg n7.1.1 WindowsStore build (C:/Vitox/toolchain/ffmpeg-n7.1.1-uwp; build script toolchain/src/build-ffmpeg-uwp.sh)
set(VITOX_FFMPEG_UWP "C:/Vitox/toolchain/ffmpeg-n7.1.1-uwp" CACHE PATH "FFmpeg n7.1.1 static, WindowsStore x64, decoders only")
target_include_directories(vitox_hle PRIVATE "${VITA3K}/vita3k/codec/include" "${VITA3K}/external/LibAtrac9/C/src" "${VITOX_FFMPEG_UWP}/include"
    "${VITA3K}/vita3k/ngs/include") # M9A.7: ngs engine
# SceNet.cpp (patched copy) is built without UNICODE/_UNICODE as on desktop: its InetNtop/InetPton calls are the char
# forms (same treatment as logging.cpp). No behaviour change.
set_source_files_properties("${CMAKE_BINARY_DIR}/vitox-patched/m9a4_SceNet.cpp"
    PROPERTIES COMPILE_OPTIONS "/UUNICODE;/U_UNICODE")
set_property(SOURCE "${CMAKE_BINARY_DIR}/vitox-patched/m9a4_SceNet.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES "${VITA3K}/vita3k/modules/SceNet")
# M9A.4K UWP port (replaces the 0.0.24.14 declaration-only GetAdaptersInfo shim): GetAdaptersInfo is desktop-partition
# only on WindowsStore. Same semantics: find the adapter owning the selected IPv4 address, copy its 6-byte MAC, else EINVAL.
# (Upstream's fixed 16-entry IP_ADAPTER_INFO array fails with EINVAL when the buffer is too small; the port retries.)
vitox_substitute(vita3k/modules/SceNet/SceNet.cpp m9a4_SceNet.cpp
[=[    IP_ADAPTER_INFO AdapterInfo[16];
    DWORD dwBufLen = sizeof(AdapterInfo);
    if (GetAdaptersInfo(AdapterInfo, &dwBufLen) != ERROR_SUCCESS)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    for (auto adapter = AdapterInfo; adapter != nullptr; adapter = adapter->Next) {
        for (auto ip = &adapter->IpAddressList; ip != nullptr; ip = ip->Next) {
            if ((selected_addr.addr == ip->IpAddress.String) && (adapter->AddressLength == sizeof(addr->data))) {
                memcpy(addr->data, adapter->Address, sizeof(addr->data));
                return 0;
            }
        }
    }]=]
[=[    // Vitox shim (M9A.4K): was GetAdaptersInfo (desktop partition only on WindowsStore)
    ULONG dwBufLen = 15 * 1024;
    std::vector<uint8_t> vitox_buf;
    ULONG vitox_ret = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && vitox_ret == ERROR_BUFFER_OVERFLOW; ++attempt) {
        vitox_buf.resize(dwBufLen);
        vitox_ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(vitox_buf.data()), &dwBufLen);
    }
    if (vitox_ret != ERROR_SUCCESS)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    for (auto adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(vitox_buf.data()); adapter != nullptr; adapter = adapter->Next) {
        for (auto ip = adapter->FirstUnicastAddress; ip != nullptr; ip = ip->Next) {
            if (!ip->Address.lpSockaddr || ip->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            char ip_str[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(ip->Address.lpSockaddr)->sin_addr, ip_str, sizeof(ip_str));
            if ((selected_addr.addr == ip_str) && (adapter->PhysicalAddressLength == sizeof(addr->data))) {
                memcpy(addr->data, adapter->PhysicalAddress, sizeof(addr->data));
                return 0;
            }
        }
    }]=])
# M9A.4M: upstream net_utils.cpp with two substitutions (configure fails if the upstream text drifts):
#  1. curl is not part of this build: get_web_response returns {} - upstream's own result when curl_easy_init fails.
#     Its only caller is the desktop Qt updater (gui-qt/src/update_manager.cpp), which is not built.
#  2. get_all_assigned_addrs' _WIN32 branch: GetAdaptersInfo (desktop partition only on WindowsStore) -> GetAdaptersAddresses,
#     same output - one entry per IPv4 unicast address in adapter order, "0.0.0.0" and the loopback pseudo-interface skipped
#     (GetAdaptersInfo never lists it), dotted mask from the on-link prefix length, same LOG_CRITICAL + localhost fallback.
vitox_substitute(vita3k/util/src/net_utils.cpp m9a4_net_utils.cpp
[=[#include <curl/curl.h>
]=]
[=[// Vitox shim (M9A.4M): no curl in this build (see get_web_response)
#include <ws2tcpip.h>
]=]
[=[std::string get_web_response(const std::string &url) {
    auto curl = curl_easy_init();
    if (!curl)
        return {};
]=]
[=[std::string get_web_response(const std::string &url) {
    // Vitox shim (M9A.4M): no HTTP client in this build - upstream's result when curl_easy_init fails
    LOG_ERROR("Vitox: get_web_response({}) unavailable (no curl in this build)", url);
    return {};
#if 0 // upstream curl body
    auto curl = curl_easy_init();
    if (!curl)
        return {};
]=]
[=[    if (response_code / 100 == 2)
        return response_string;

    return {};
}]=]
[=[    if (response_code / 100 == 2)
        return response_string;

    return {};
#endif
}]=]
[=[    PIP_ADAPTER_INFO pAdapterInfo;
    DWORD dwRetVal = 0;
    UINT i;
    ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
    pAdapterInfo = (IP_ADAPTER_INFO *)malloc(sizeof(IP_ADAPTER_INFO));
    if (pAdapterInfo == NULL) {
        LOG_CRITICAL("Error allocating memory needed to call GetAdaptersinfo");
        return ret_addrs();
    }
    // Make an initial call to GetAdaptersInfo to get the necessary size into the ulOutBufLen variable
    if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO *)malloc(ulOutBufLen);
        if (pAdapterInfo == NULL) {
            LOG_CRITICAL("Error allocating memory needed to call GetAdaptersinfo");
            return ret_addrs();
        }
    }
    if ((dwRetVal = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen)) == NO_ERROR) {
        PIP_ADAPTER_INFO pAdapter = pAdapterInfo;
        const std::string noAddress = "0.0.0.0";
        while (pAdapter) {
            IP_ADDR_STRING *pIPAddr = &pAdapter->IpAddressList;
            while (pIPAddr) {
                if (noAddress.compare(pIPAddr->IpAddress.String) != 0)
                    out_addrs.push_back({ pAdapter->Description, pIPAddr->IpAddress.String, pIPAddr->IpMask.String });
                pIPAddr = pIPAddr->Next;
            }
            pAdapter = pAdapter->Next;
        }
    } else {
        LOG_CRITICAL("GetAdaptersInfo failed with error: {}", dwRetVal);
    }]=]
[=[    // Vitox shim (M9A.4M): was GetAdaptersInfo (desktop partition only on WindowsStore)
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ulOutBufLen = 15 * 1024; // Microsoft's recommended starting size
    PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
    ULONG dwRetVal = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && dwRetVal == ERROR_BUFFER_OVERFLOW; ++attempt) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES *)malloc(ulOutBufLen);
        if (pAddresses == NULL) {
            LOG_CRITICAL("Error allocating memory needed to call GetAdaptersAddresses");
            return ret_addrs();
        }
        dwRetVal = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &ulOutBufLen);
    }
    if (dwRetVal == NO_ERROR) {
        const std::string noAddress = "0.0.0.0";
        for (PIP_ADAPTER_ADDRESSES pAdapter = pAddresses; pAdapter; pAdapter = pAdapter->Next) {
            if (pAdapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS pUni = pAdapter->FirstUnicastAddress; pUni; pUni = pUni->Next) {
                const SOCKADDR *sa = pUni->Address.lpSockaddr;
                if (!sa || sa->sa_family != AF_INET)
                    continue;
                char addressBuffer[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(sa)->sin_addr, addressBuffer, sizeof(addressBuffer));
                if (noAddress.compare(addressBuffer) == 0)
                    continue;
                const ULONG prefix = pUni->OnLinkPrefixLength;
                in_addr mask{};
                mask.s_addr = htonl(prefix == 0 ? 0u : (prefix >= 32 ? 0xFFFFFFFFu : 0xFFFFFFFFu << (32 - prefix)));
                char maskBuffer[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &mask, maskBuffer, sizeof(maskBuffer));
                std::string name;
                if (pAdapter->Description) {
                    const int n = WideCharToMultiByte(CP_UTF8, 0, pAdapter->Description, -1, nullptr, 0, nullptr, nullptr);
                    if (n > 1) {
                        name.resize(n - 1);
                        WideCharToMultiByte(CP_UTF8, 0, pAdapter->Description, -1, name.data(), n, nullptr, nullptr);
                    }
                }
                out_addrs.push_back({ name, addressBuffer, maskBuffer });
            }
        }
    } else {
        LOG_CRITICAL("GetAdaptersAddresses failed with error: {}", dwRetVal);
    }
    free(pAddresses);]=])
# SceSsl / SceHttp / http.cpp: <openssl/ssl.h> + <openssl/err.h> resolve to the offline stand-in (compat/openssl-offline)
set_property(SOURCE "${VITA3K}/vita3k/modules/SceSsl/SceSsl.cpp" "${VITA3K}/vita3k/modules/SceHttp/SceHttp.cpp"
    "${VITA3K}/vita3k/http/src/http.cpp" "${VITOX_M3}/compat/vitox_ssl_offline.cpp"
    APPEND PROPERTY INCLUDE_DIRECTORIES "${VITOX_M3}/compat/openssl-offline")
set_source_files_properties("${CMAKE_BINARY_DIR}/vitox-patched/m9a4_net_utils.cpp" "${VITA3K}/vita3k/modules/SceHttp/SceHttp.cpp"
    PROPERTIES COMPILE_OPTIONS "/UUNICODE;/U_UNICODE")
# SceAppUtil.cpp: <winsock.h> is desktop-partition only on WindowsStore; <winsock2.h> declares the same gethostname
# (ws2_32) in the app partition. Include swap only - the user-name -> hostname -> "Vita3K" fallback is unchanged.
vitox_substitute(vita3k/modules/SceAppUtil/SceAppUtil.cpp m8b_SceAppUtil.cpp
[=[#ifdef _WIN32
#include <winsock.h>
#else]=]
[=[#ifdef _WIN32
#include <winsock2.h> // Vitox shim (M8B.1): was <winsock.h> (desktop partition only on WindowsStore)
#else]=])
vitox_m5a_flags(vitox_hle)
set_target_properties(vitox_hle PROPERTIES FOLDER vitox)
target_include_directories(vitox_hle PRIVATE "${VITOX_M3}/compat" "${VITOX_M3}/core-probe" "${VITA3K}/vita3k/modules")
target_include_directories(vitox_hle PRIVATE "${VITA3K}/vita3k/ctrl/include" "${VITA3K}/vita3k/touch/include")
set_property(SOURCE "${CMAKE_BINARY_DIR}/vitox-patched/m8b_SceAppUtil.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES "${VITA3K}/vita3k/modules/SceAppUtil")
target_link_libraries(vitox_hle PUBLIC vitox_hostio vitox_dlmalloc)
message(STATUS "Vitox hle: module_parent.cpp(shim: selected NID tables) load_module.cpp SceLibKernel SceIofilemgr SceThreadmgr SceModulemgr + dlmalloc")

# ---- M6B part B (BUILD-ONLY): Vita3K Vulkan backend with the direct dzn ICD entry point ---------
# Guarded substitution of renderer/src/vulkan/renderer.cpp: both DispatchLoaderDynamic init sites get a
# VITOX_UWP branch next to the existing Android/adreno one, taking vk_icdGetInstanceProcAddr from the
# packaged vulkan_dzn.dll (compat/vitox_vk_entry.cpp) and logging its source. Vita3K still creates and
# owns its VkInstance/VkDevice. VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0 removes vulkan-hpp's
# vulkan-1.dll loader entirely (no global Vulkan loader dependency).
vitox_substitute(vita3k/renderer/src/vulkan/renderer.cpp vk_renderer.cpp
[=[#include <renderer/functions.h>
]=]
[=[#include <renderer/functions.h>
#ifdef VITOX_UWP
#include <vitox_vk_entry.h> // Vitox shim (M6B)
#endif
]=]
[=[#else
        VULKAN_HPP_DEFAULT_DISPATCHER.init();
#endif]=]
[=[#elif defined(VITOX_UWP)
        // Vitox shim (M6B): no Vulkan loader on Xbox - direct Mesa Dozen ICD entry point
        PFN_vkGetInstanceProcAddr vk_get_instance_proc_addr = vitox_vk_get_instance_proc_addr();
        LOG_INFO("Vitox: Vulkan entry points from {}", vitox_vk_proc_addr_source());
        if (!vk_get_instance_proc_addr)
            return false;

        VULKAN_HPP_DEFAULT_DISPATCHER.init(vk_get_instance_proc_addr);
#else
        VULKAN_HPP_DEFAULT_DISPATCHER.init();
#endif]=]
[=[#else
        dispatch.init();
#endif]=]
[=[#elif defined(VITOX_UWP)
        // Vitox shim (M6B): direct Mesa Dozen ICD entry point (see vulkan::create)
        PFN_vkGetInstanceProcAddr vk_get_instance_proc_addr = vitox_vk_get_instance_proc_addr();
        if (!vk_get_instance_proc_addr)
            return info;

        dispatch.init(vk_get_instance_proc_addr);
#else
        dispatch.init();
#endif]=]
# ---- M6C external presentation (only when a presenter is registered; compat/vitox_external_present.h) ----
[=[#include <vitox_vk_entry.h> // Vitox shim (M6B)
#endif
]=]
[=[#include <vitox_vk_entry.h> // Vitox shim (M6B)
#include <vitox_external_present_vk.h> // Vitox M6C: external presentation
#endif
]=]
[=[        } else {
            instance_extensions.push_back(vk::KHRSurfaceExtensionName);]=]
[=[#ifdef VITOX_UWP
        } else if (vitox_external_presenter()) {
            // Vitox M6C: external presentation - no VkSurfaceKHR, so no surface instance extensions
            LOG_INFO("Vitox: external presentation active - no surface instance extensions");
#endif
        } else {
            instance_extensions.push_back(vk::KHRSurfaceExtensionName);]=]
[=[            && vk_state.physical_device.getSurfaceSupportKHR(i, vk_state.screen_renderer.surface)) {]=]
[=[#ifdef VITOX_UWP
            // Vitox M6C: with external presentation the host D3D12 queue presents; any graphics+transfer family works
            && (vitox_external_presenter() || vk_state.physical_device.getSurfaceSupportKHR(i, vk_state.screen_renderer.surface))) {
#else
            && vk_state.physical_device.getSurfaceSupportKHR(i, vk_state.screen_renderer.surface)) {
#endif]=]
[=[        if (!physical_device.getSurfaceSupportKHR(
                general_family_index, screen_renderer.surface)) {]=]
[=[#ifdef VITOX_UWP
        if (!vitox_external_presenter() && !physical_device.getSurfaceSupportKHR(general_family_index, screen_renderer.surface)) { // Vitox M6C
#else
        if (!physical_device.getSurfaceSupportKHR(
                general_family_index, screen_renderer.surface)) {
#endif]=]
[=[        device_info.get().setQueueCreateInfos(queue_infos);
        device_info.get().setPEnabledExtensionNames(device_extensions);]=]
[=[#ifdef VITOX_UWP
        // Vitox M6C: external-memory/semaphore + timeline extensions for the host bridge (before the pointer is taken)
        if (vitox_external_presenter() && !vitox_xp_add_device_extensions(physical_device, device_extensions))
            return false;
#endif
        device_info.get().setQueueCreateInfos(queue_infos);
        device_info.get().setPEnabledExtensionNames(device_extensions);]=]
[=[        try {
            device = physical_device.createDevice(device_info.get());]=]
[=[#ifdef VITOX_UWP
        // Vitox M6C: timeline-semaphore feature, chained after every StructureChain unlink above
        if (vitox_external_presenter())
            vitox_xp_chain_device_features(instance, physical_device, device_info.get());
#endif
        try {
            device = physical_device.createDevice(device_info.get());]=]
[=[        vk::ImageView surface_handle = surface_cache.sourcing_color_surface_for_presentation(
            frame.base, frame.pitch, viewport);
]=]
[=[        vk::ImageView surface_handle = surface_cache.sourcing_color_surface_for_presentation(
            frame.base, frame.pitch, viewport);
#ifdef VITOX_DIAG_M7B
        {
            // Vitox M7B.5 DIAGNOSTIC (read-only): checkpoint B - which source the screen renderer is fed
            static uint64_t vitox_n = 0, vitox_hits = 0, vitox_misses = 0;
            ++vitox_n;
            surface_handle ? ++vitox_hits : ++vitox_misses;
            if (vitox_n <= 3 || vitox_n == 60 || vitox_n == 600 || vitox_n == 2000 || vitox_n % 300 == 0) {
                const uint32_t *gm = static_cast<const uint32_t *>(frame.base.get(mem));
                const uint32_t gw = static_cast<uint32_t>(frame.image_size.x), gh = static_cast<uint32_t>(frame.image_size.y);
                LOG_INFO("Vitox [M7B.5] B display #{}: fb 0x{:08X} pitch {} {}x{} -> surface cache {} (view {}) | viewport off {},{} {}x{} tex {}x{} | "
                         "{} | guest memory centre 0x{:08X} corner(4,4) 0x{:08X} | totals hit {} miss {}",
                    vitox_n, frame.base.address(), frame.pitch, gw, gh, surface_handle ? "HIT" : "MISS",
                    static_cast<void *>(static_cast<VkImageView>(surface_handle)), viewport.offset_x, viewport.offset_y, viewport.width, viewport.height,
                    viewport.texture_width, viewport.texture_height,
                    surface_handle ? "GPU colour surface used directly" : "fallback: guest-memory upload into vita_surface",
                    gm ? gm[(gh / 2) * frame.pitch + gw / 2] : 0u, gm ? gm[4 * frame.pitch + 4] : 0u, vitox_hits, vitox_misses);
            }
        }
#endif
]=])
# screen_renderer.cpp (M6C): with a registered presenter, ScreenRenderer keeps its swapchain_images/views/
# framebuffers/command buffers/fences semantics but gets N exportable images (no VkSurfaceKHR, no VkSwapchainKHR,
# no acquireNextImageKHR); submit signals the exported timeline semaphore and the host presents.
vitox_substitute(vita3k/renderer/src/vulkan/screen_renderer.cpp m6c_screen_renderer.cpp
[=[#include "vkutil/vkutil.h"
]=]
[=[#include "vkutil/vkutil.h"
#ifdef VITOX_UWP
#include <vitox_external_present_vk.h> // Vitox M6C: external presentation
#endif
]=]
[=[bool ScreenRenderer::create() {
]=]
[=[bool ScreenRenderer::create() {
#ifdef VITOX_UWP
    if (vitox_external_presenter()) {
        LOG_INFO("Vitox: external presentation - no VkSurfaceKHR");
        return true;
    }
#endif
]=]
[=[    const auto surface_formats = state.physical_device.getSurfaceFormatsKHR(surface);]=]
[=[#ifdef VITOX_UWP
    const auto surface_formats = vitox_external_presenter()
        ? std::vector<vk::SurfaceFormatKHR>{ { vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear } } // Vitox M6C: host back buffer format
        : state.physical_device.getSurfaceFormatsKHR(surface);
#else
    const auto surface_formats = state.physical_device.getSurfaceFormatsKHR(surface);
#endif]=]
[=[    const auto present_modes = state.physical_device.getSurfacePresentModesKHR(surface);]=]
[=[#ifdef VITOX_UWP
    const auto present_modes = vitox_external_presenter()
        ? std::vector<vk::PresentModeKHR>{ vk::PresentModeKHR::eFifo } // Vitox M6C: host Present(1, 0) = vsync
        : state.physical_device.getSurfacePresentModesKHR(surface);
#else
    const auto present_modes = state.physical_device.getSurfacePresentModesKHR(surface);
#endif]=]
[=[    surface_capabilities = state.physical_device.getSurfaceCapabilitiesKHR(surface);
]=]
[=[#ifdef VITOX_UWP
    surface_capabilities = vitox_external_presenter() ? vitox_xp_capabilities() : state.physical_device.getSurfaceCapabilitiesKHR(surface);
#else
    surface_capabilities = state.physical_device.getSurfaceCapabilitiesKHR(surface);
#endif
]=]
[=[    // Create Swapchain
    {]=]
[=[#ifdef VITOX_UWP
    if (!vitox_external_presenter()) // Vitox M6C: no VkSwapchainKHR with external presentation
#endif
    // Create Swapchain
    {]=]
[=[    swapchain_images = state.device.getSwapchainImagesKHR(swapchain);]=]
[=[#ifdef VITOX_UWP
    swapchain_images = vitox_external_presenter() ? vitox_xp_create_images(*this) : state.device.getSwapchainImagesKHR(swapchain);
#else
    swapchain_images = state.device.getSwapchainImagesKHR(swapchain);
#endif]=]
[=[    if (swapchain) {
        state.device.destroySwapchainKHR(swapchain);
        swapchain = nullptr;
    }
}]=]
[=[    if (swapchain) {
        state.device.destroySwapchainKHR(swapchain);
        swapchain = nullptr;
    }
#ifdef VITOX_UWP
    if (vitox_external_presenter())
        vitox_xp_destroy_images(*this);
#endif
}]=]
[=[    state.device.destroy(swapchain);
    swapchain = nullptr;
]=]
[=[    state.device.destroy(swapchain);
    swapchain = nullptr;
#ifdef VITOX_UWP
    if (vitox_external_presenter())
        vitox_xp_destroy_images(*this);
#endif
]=]
[=[void ScreenRenderer::destroy_surface() {
]=]
[=[void ScreenRenderer::destroy_surface() {
#ifdef VITOX_UWP
    if (vitox_external_presenter()) {
        surface = nullptr;
        return;
    }
#endif
]=]
[=[    if (swapchain)
        acquire_result = state.device.acquireNextImageKHR(swapchain,]=]
[=[#ifdef VITOX_UWP
    if (vitox_external_presenter()) // Vitox M6C: round-robin over the exported images, waits for the host copy
        acquire_result = vitox_xp_acquire(*this) ? vk::Result::eSuccess : vk::Result::eNotReady;
    else
#endif
    if (swapchain)
        acquire_result = state.device.acquireNextImageKHR(swapchain,]=]
[=[    // first submit the command buffer
    current_cmd_buffer.endRenderPass();
    current_cmd_buffer.end();
]=]
[=[    // first submit the command buffer
    current_cmd_buffer.endRenderPass();
    current_cmd_buffer.end();
#ifdef VITOX_UWP
    if (vitox_external_presenter()) { // Vitox M6C: signal the exported timeline semaphore, host waits + presents
        vitox_xp_submit_present(*this);
        swapchain_image_idx = ~0;
        current_cmd_buffer = nullptr;
        return;
    }
#endif
]=]
[=[    if (!need_rebuild && !need_surface_recreate && swapchain && surface_matches_window_size())]=]
[=[#ifdef VITOX_UWP
    if (!need_rebuild && !need_surface_recreate && (swapchain || (vitox_external_presenter() && !swapchain_images.empty())) && surface_matches_window_size())
#else
    if (!need_rebuild && !need_surface_recreate && swapchain && surface_matches_window_size())
#endif]=]
[=[    create_swapchain();
    return static_cast<bool>(swapchain);]=]
[=[    create_swapchain();
#ifdef VITOX_UWP
    if (vitox_external_presenter())
        return !swapchain_images.empty();
#endif
    return static_cast<bool>(swapchain);]=])
# pipeline_cache.cpp: the only SDL use in the Vulkan backend is SDL_GetNumLogicalCPUCores() (worker count
# for async pipeline compilation) -> std::thread::hardware_concurrency(), the same value on Windows.
vitox_substitute(vita3k/renderer/src/vulkan/pipeline_cache.cpp pipeline_cache.cpp
# STUTTER (0.0.24.171, user-chosen option 3): upstream compiles pipelines already listed in the saved pipeline cache
# synchronously on the render thread ("creation time almost instantaneous"). On Xbox the D3D12 driver still compiles each PSO
# to GPU code, so every known pipeline stalled a frame at first use per session. Known pipelines now take the async path too
# (draws using a pipeline still compiling are skipped for a frame or two); upstream's consider_for_async exemptions remain.
[=[    const bool compile_pipeline_async = !already_in_cache && consider_for_async && use_async_compilation;
]=]
[=[#ifdef VITOX_UWP
    const bool compile_pipeline_async = consider_for_async && use_async_compilation; // Vitox 0.0.24.171: also when already_in_cache
#else
    const bool compile_pipeline_async = !already_in_cache && consider_for_async && use_async_compilation;
#endif
]=]
# M9A.10 FIX (0.0.24.169): precompile_shader() loads "vk<CURRENT_VERSION>-<hash>.spv" from the disk cache and uses it directly;
# the compile path (load_spirv_shader, Vitox tag "vk13vx...") only runs when that file is missing. Gravity Rush's cache predates
# the Vitox tag, so every Vitox shader-translator change since 0.0.24.118 never reached the GPU. Use the same tagged name here.
[=[    const std::string shader_file_name = fmt::format("vk{}-{}.spv", shader::CURRENT_VERSION, hex_string(shader_hash));
]=]
[=[#ifdef VITOX_UWP
    const std::string shader_file_name = fmt::format("vk{}vx164-{}.spv", shader::CURRENT_VERSION, hex_string(shader_hash)); // Vitox M9A.10 (0.0.24.169)
#else
    const std::string shader_file_name = fmt::format("vk{}-{}.spv", shader::CURRENT_VERSION, hex_string(shader_hash));
#endif
]=]
[=[#include <SDL3/SDL_cpuinfo.h>]=]
[=[#include <thread> // Vitox shim (M6B): was <SDL3/SDL_cpuinfo.h>]=]
[=[    const int nb_logical_threads = SDL_GetNumLogicalCPUCores();]=]
[=[    const int nb_logical_threads = static_cast<int>(std::thread::hardware_concurrency()); // Vitox shim: was SDL_GetNumLogicalCPUCores()]=]
# ---- M7B.6 DIAGNOSTIC (read-only, VITOX_UWP): render-pass attachments, full pipeline create-info, converted shaders ----
[=[#include <thread> // Vitox shim (M6B): was <SDL3/SDL_cpuinfo.h>]=]
[=[#include <thread> // Vitox shim (M6B): was <SDL3/SDL_cpuinfo.h>
#ifdef VITOX_UWP
#include <vitox_m7b6_diag.h> // Vitox M7B.6 diagnostic (logging only)
#endif]=]
[=[    if (maskupdate)
        LOG_WARN_ONCE("Mask not implemented in the vulkan renderer!");
]=]
[=[    if (maskupdate)
        LOG_WARN_ONCE("Mask not implemented in the vulkan renderer!");
#ifdef VITOX_UWP
    {
        // Vitox M7B.6 DIAGNOSTIC: the SPIR-V this GXP converts to, recomputed with the same call load_spirv_shader makes
        // (no dumper: nothing is cached or written), logged as GLSL. The module actually used is still created below.
        static unsigned vitox_n = 0;
        if (vitox_m7b6_diag_enabled() && vitox_n++ < 4) {
            const shader::GeneratedShader vitox_gen = shader::convert_gxp(*program, hex_string(hash), state.features, shader::Target::SpirVVulkan, hints, maskupdate);
            vitox_m7b6_log_shader(is_vertex, hex_string(hash), vitox_gen.spirv);
        }
    }
#endif
]=]
[=[    render_passes_map[format] = state.device.createRenderPass(pass_info);
]=]
[=[    render_passes_map[format] = state.device.createRenderPass(pass_info);
#ifdef VITOX_UWP
    vitox_m7b6_log_render_pass(render_passes_map[format], format, force_load, force_store, is_color_transient, no_color, state.deep_stencil_use,
        color_attachment, ds_attachment); // Vitox M7B.6 diagnostic
#endif
]=]
[=[    const auto result = state.device.createGraphicsPipeline(pipeline_cache, pipeline_info);
]=]
[=[    const auto result = state.device.createGraphicsPipeline(pipeline_cache, pipeline_info);
#ifdef VITOX_UWP
    // Vitox M7B.6 DIAGNOSTIC: the exact create-info this pipeline got, plus the GXM inputs that chose it.
    // 0.0.24.51: formatted only when the diagnostic is enabled, and only from the record prefix (record_pipeline_len):
    // the async CompileRequest copies just that prefix, so reading the dynamic state after vertex_streams (stencil
    // values, depth bias) ran past the end of the heap request (compiler_thread access violation, 0.0.24.46).
    if (vitox_m7b6_diag_enabled())
        vitox_m7b6_log_pipeline(result.value, result.result, pipeline_info,
            fmt::format("gxm: fragment_disabled {} (front mode {} has_no_effect {}) OUTPUT_UNDEFINED {} use_shader_interlock {} | frag program_flags 0x{:X} "
                        "frag_color_used {} native_color {} discard {} depth_replace {} | frag output type {} components {} | coherent_fb_fetch {} direct_fragcolor {} "
                        "| primitive {} | two_sided {} | depth write mode {} func {}",
                is_fragment_disabled, static_cast<int>(record.front_side_fragment_program_mode), gxm_fragment_shader->has_no_effect(), frag_has_no_output,
                use_shader_interlock, gxm_fragment_shader->program_flags, gxm_fragment_shader->is_frag_color_used(), gxm_fragment_shader->is_native_color(),
                gxm_fragment_shader->is_discard_used(), gxm_fragment_shader->is_depth_replace_used(), static_cast<int>(gxm_fragment_shader->get_fragment_output_type()),
                gxm_fragment_shader->get_fragment_output_component_count(), support_coherent_framebuffer_fetch, state.features.direct_fragcolor, static_cast<int>(type),
                static_cast<int>(record.two_sided), static_cast<int>(record.front_depth_write_mode), static_cast<int>(record.front_depth_func)));
#endif
]=]
[=[#include <vitox_m7b6_diag.h> // Vitox M7B.6 diagnostic (logging only)]=]
[=[#include <vitox_m7b6_diag.h> // Vitox M7B.6 diagnostic (logging only)
#include <vitox_vertex_realign.h> // Vitox M9A.8: 4-byte aligned vertex streams]=]
# M9A.8 (0.0.24.94): the pipeline side of the aligned stream layout (vitox_vertex_realign.h; scene.cpp repacks the data)
[=[    uint32_t used_streams = 0;
]=]
[=[    uint32_t used_streams = 0;
#ifdef VITOX_UWP
    // Vitox M9A.8: 4-byte aligned stream layouts (memory mapping off: scene.cpp repacks each per-draw stream copy)
    static thread_local std::vector<uint16_t> vitox_new_offsets;
    vitox_new_offsets.assign(vertex_program.attributes.size(), 0);
    VitoxStreamLayout vitox_layouts[SCE_GXM_MAX_VERTEX_STREAMS];
    if (!state.features.enable_memory_mapping)
        for (uint32_t s = 0; s < vertex_program.streams.size() && s < SCE_GXM_MAX_VERTEX_STREAMS; s++)
            vitox_layouts[s] = vitox_realign_stream(vertex_program, s,
                [&](const SceGxmVertexAttribute &a) { return vkvert->attribute_infos.contains(a.regIndex); }, vitox_new_offsets.data());
#endif
]=]
[=[                .offset = attribute.offset + i * array_element_size });]=]
[=[#ifdef VITOX_UWP
                .offset = (vitox_layouts[attribute.streamIndex].repack ? vitox_new_offsets[&attribute - vertex_program.attributes.data()] : attribute.offset) + i * array_element_size });
#else
                .offset = attribute.offset + i * array_element_size });
#endif]=]
[=[#else
        const uint32_t stride = stream.stride;
#endif]=]
[=[#else
#ifdef VITOX_UWP
        const uint32_t stride = vitox_layouts[stream_index].repack ? vitox_layouts[stream_index].stride : stream.stride; // Vitox M9A.8
#else
        const uint32_t stride = stream.stride;
#endif
#endif]=]
# M9A.10 (0.0.24.118): the Vita3K shader cache key is "vk<CURRENT_VERSION>-<program hash>"; the gxp_parser uniform-buffer
# change alters the generated code for the same program, so the Vitox cache gets its own version tag
# (0.0.24.170: the diagnostic pairs of an M9A.10 block were removed here; the pairs below are older fixes)
[=[    const std::string shader_version = fmt::format("vk{}", shader::CURRENT_VERSION);]=]
[=[#ifdef VITOX_UWP
    const std::string shader_version = fmt::format("vk{}vx164", shader::CURRENT_VERSION); // Vitox M9A.10: bump with Vita3K-side shader changes (0.0.24.164: exact uniform copy)
#else
    const std::string shader_version = fmt::format("vk{}", shader::CURRENT_VERSION);
#endif]=]

# STUTTER option 1 (0.0.24.173, user-chosen): pipeline pre-warm at boot (compat/vitox_pipeline_prewarm.h). Every pipeline created
# is recorded as a recipe (its create-info inputs + render pass, under Vita3K's pipeline key), the patched dzn reports each D3D12
# variant a pipeline needs at draw time, and both are saved next to the SPIR-V cache. read_pipeline_cache() (now called at boot by
# the launcher) rebuilds all recipes with cached shaders, with their variants, on worker threads, before the game's first draw.
[=[#include <vitox_vertex_realign.h> // Vitox M9A.8: 4-byte aligned vertex streams]=]
[=[#include <vitox_vertex_realign.h> // Vitox M9A.8: 4-byte aligned vertex streams
#include <vitox_pipeline_prewarm.h> // Vitox 0.0.24.173: pipeline pre-warm]=]
[=[    const GxmRecordState *get_record() {
]=]
[=[#ifdef VITOX_UWP
    uint64_t vitox_key = 0; // Vitox 0.0.24.173: pipeline pre-warm recipe key + render pass
    vitox_prewarm::RenderPassParams vitox_rp{};
#endif
    const GxmRecordState *get_record() {
]=]
[=[        vk::Pipeline pipeline = compile_pipeline(request->type, request->render_pass, *request->vertex_program_gxm, *request->fragment_program_gxm, *request->get_record(), request->hints, mem);
]=]
[=[#ifdef VITOX_UWP
        vitox_prewarm::tl_context = { request->vitox_key, request->vitox_rp }; // Vitox 0.0.24.173
#endif
        vk::Pipeline pipeline = compile_pipeline(request->type, request->render_pass, *request->vertex_program_gxm, *request->fragment_program_gxm, *request->get_record(), request->hints, mem);
]=]
[=[        LOG_CRITICAL("Failed to create pipeline.");
        return nullptr;
    }

    return result.value;
]=]
[=[        LOG_CRITICAL("Failed to create pipeline.");
        return nullptr;
    }

#ifdef VITOX_UWP
    vitox_prewarm::record(&next_pipeline_cache_save, static_cast<VkPipeline>(result.value), *reinterpret_cast<const VkGraphicsPipelineCreateInfo *>(&pipeline_info),
        vertex_program.hash.data(), fragment_program.hash.data(), vertex_program.texture_count, fragment_program.texture_count); // Vitox 0.0.24.173
#endif
    return result.value;
]=]
[=[    const vk::RenderPass render_pass = use_shader_interlock ? context.current_shader_interlock_pass : context.current_render_pass;
]=]
[=[    const vk::RenderPass render_pass = use_shader_interlock ? context.current_shader_interlock_pass : context.current_render_pass;
#ifdef VITOX_UWP
    // Vitox 0.0.24.173: the parameters of this render pass, for the pre-warm recipe (only reached when the pipeline is not built yet)
    vitox_prewarm::RenderPassParams vitox_rp{};
    {
        const auto vitox_find = [&](const std::map<vk::Format, vk::RenderPass> &m, uint32_t transient, uint32_t load, uint32_t store, uint32_t no_color) {
            for (const auto &[fmt, pass] : m)
                if (pass == render_pass) {
                    vitox_rp = { static_cast<uint32_t>(fmt), load, store, transient, no_color, 1 };
                    return true;
                }
            return false;
        };
        bool vitox_found = vitox_find(shader_interlock_pass, 0, 0, 0, 1);
        for (uint32_t i = 0; i < 2 && !vitox_found; i++)
            for (uint32_t j = 0; j < 2 && !vitox_found; j++)
                for (uint32_t k = 0; k < 2 && !vitox_found; k++)
                    vitox_found = vitox_find(render_passes[i][j][k], i, j, k, 0);
    }
#endif
]=]
[=[        memcpy(request->record_data, &record, record_pipeline_len);
]=]
[=[        memcpy(request->record_data, &record, record_pipeline_len);
#ifdef VITOX_UWP
        request->vitox_key = key; // Vitox 0.0.24.173
        request->vitox_rp = vitox_rp;
#endif
]=]
[=[        vk::Pipeline result = compile_pipeline(type, render_pass, vertex_program_gxm, fragment_program_gxm, record, context.shader_hints, mem);
]=]
[=[#ifdef VITOX_UWP
        vitox_prewarm::tl_context = { key, vitox_rp }; // Vitox 0.0.24.173
#endif
        vk::Pipeline result = compile_pipeline(type, render_pass, vertex_program_gxm, fragment_program_gxm, record, context.shader_hints, mem);
]=]
[=[    renderer::save_shaders_cache_hashs(state, shader_cache_copy);
]=]
[=[    renderer::save_shaders_cache_hashs(state, shader_cache_copy);
#ifdef VITOX_UWP
    vitox_prewarm::save(state.shaders_path.wstring()); // Vitox 0.0.24.173
#endif
]=]
[=[void PipelineCache::read_pipeline_cache() {
]=]
[=[void PipelineCache::read_pipeline_cache() {
#ifdef VITOX_UWP
    // Vitox 0.0.24.173: pipeline pre-warm, run when this function returns (after the saved pipeline cache data, if any, is loaded)
    struct VitoxAtExit {
        std::function<void()> f;
        ~VitoxAtExit() { f(); }
    } vitox_at_exit{ [this] {
        vitox_prewarm::run(&next_pipeline_cache_save, state.device, pipeline_cache, state.shaders_path.wstring(), shader::GAMMA_CORRECTION_SPECIALIZATION_ID,
            [this](const uint8_t *h) {
                Sha256Hash hash;
                memcpy(hash.data(), h, hash.size());
                return static_cast<VkShaderModule>(precompile_shader(hash));
            },
            [this](const vitox_prewarm::RenderPassParams &p) {
                return static_cast<VkRenderPass>(retrieve_render_pass(static_cast<vk::Format>(p.format), p.force_load, p.force_store, p.is_color_transient, p.no_color));
            },
            [this](uint32_t v, uint32_t f) { return static_cast<VkPipelineLayout>(pipeline_layouts[v][f]); },
            [this](uint64_t key, VkPipeline p) {
                vk::Pipeline &slot = pipelines[key];
                if (slot)
                    state.device.destroy(vk::Pipeline(p)); // already built (not expected at boot)
                else
                    slot = vk::Pipeline(p);
            });
    } };
#endif
]=]
)
# ---- 0.0.24.51 (M9A.6) DIAGNOSTIC (logging only): a failed texture image creation throws vk::SystemError out of
# VKTextureCache, which ended the process with SIGABRT and no record (Gravity Rush, 0.0.24.50). Log the create-info, the
# result and what the device reports for that format, then rethrow: the behaviour stays upstream's.
# 0.0.24.53 (M9A.6) COMPAT: dzn (Vulkan on D3D12) has no A2R10G10B10 (D3D12 has only R10G10B10A2 = Vulkan A2B10G10R10),
# so GXM U2U10U10U10 textures (0.0.24.51: ErrorFormatNotSupported) and colour surfaces (0.0.24.52: init_image throw) failed
# to create (Gravity Rush). When the device cannot sample AND render A2R10G10B10, vitox_a2rgb10_fallback makes both format
# translations return A2B10G10R10: the same logical RGBA, so shaders, attachments, swizzles and surface/texture matching
# are unchanged. Only the packed bit order differs, which matters where guest bytes meet the image: texture upload
# converts it (texture/cache.cpp below). Surface-to-guest-memory sync needs memory mapping, which is disabled here.
# Devices with A2R10G10B10 keep upstream's path exactly. Upstream never produces A2B10G10R10 itself.
vitox_substitute(vita3k/renderer/src/vulkan/texture.cpp m9a6_vk_texture.cpp
# (0.0.24.170: the diagnostic pairs of an M9A.10 block were removed here; the pairs below are older fixes)
[=[    support_a2rgb10 = static_cast<bool>(a2rgb10_support.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage);
]=]
[=[    support_a2rgb10 = static_cast<bool>(a2rgb10_support.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage);
    {
        // Vitox M9A.6 compat (see core-build/CMakeLists.txt)
        const vk::FormatFeatureFlags vitox_need = vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eColorAttachment;
        const vk::FormatProperties vitox_b10 = state.physical_device.getFormatProperties(vk::Format::eA2B10G10R10UnormPack32);
        const bool vitox_r10_ok = (a2rgb10_support.optimalTilingFeatures & vitox_need) == vitox_need;
        const bool vitox_b10_ok = (vitox_b10.optimalTilingFeatures & vitox_need) == vitox_need;
        vitox_a2rgb10_fallback = !vitox_r10_ok && vitox_b10_ok;
        LOG_INFO("Vitox [M9A.6] A2R10G10B10 sampled+attachment {} | A2B10G10R10 {} -> U2U10U10U10 uses {}", vitox_r10_ok, vitox_b10_ok,
            vitox_a2rgb10_fallback ? "A2B10G10R10 (bit order converted on upload)" : "A2R10G10B10 (upstream)");
    }
]=]
[=[    std::tie(image.image, image.allocation) = state.allocator.createImage(image_info, vkutil::vma_auto_alloc);
]=]
[=[    try {
        std::tie(image.image, image.allocation) = state.allocator.createImage(image_info, vkutil::vma_auto_alloc);
    } catch (const vk::SystemError &e) {
        // Vitox M9A.6 diagnostic: what failed, then the upstream exception continues unchanged
        std::string vitox_props;
        try {
            const vk::ImageFormatProperties p = state.physical_device.getImageFormatProperties(image_info.format, image_info.imageType,
                image_info.tiling, image_info.usage, image_info.flags);
            vitox_props = fmt::format("format query: supported maxExtent {}x{} maxMips {} maxLayers {} maxBytes {}", p.maxExtent.width,
                p.maxExtent.height, p.maxMipLevels, p.maxArrayLayers, p.maxResourceSize);
        } catch (const vk::SystemError &q) {
            vitox_props = fmt::format("format query: {} (code {})", q.what(), q.code().value());
        }
        LOG_CRITICAL("Vitox [M9A.6] texture createImage failed: {} (code {}) | format {} ({}) extent {}x{} mips {} layers {} flags 0x{:X} usage 0x{:X} | {}",
            e.what(), e.code().value(), vk::to_string(image_info.format), static_cast<int>(image_info.format), image_info.extent.width, image_info.extent.height,
            image_info.mipLevels, image_info.arrayLayers, static_cast<uint32_t>(image_info.flags), static_cast<uint32_t>(image_info.usage), vitox_props);
        throw;
    }
]=]
[=[#include <vkutil/vkutil.h>
]=]
[=[#include <vkutil/vkutil.h>

extern bool vitox_a2rgb10_fallback; // Vitox M9A.6 compat, defined in m9a6_gxm_to_vulkan.cpp
#include <mutex> // Vitox 0.1.1.10
#include <unordered_map> // Vitox 0.1.1.10
]=]
# ---- 0.1.1.10 COMPAT (Killzone Mercenary campaign abort): textures in formats Dozen cannot sample (R8/R8G8 sRGB,
# R4G4B4A4, and A1R5G5B5/R5G6B5 if absent) are created as RGBA8 and expanded on upload.
[=[    if (gxm_texture.gamma_mode)
        vk_format = linear_to_srgb(vk_format);
]=]
[=[    if (gxm_texture.gamma_mode)
        vk_format = linear_to_srgb(vk_format);
    {
        // Vitox 0.1.1.10 compat (Killzone Mercenary campaign): Dozen on the Xbox GPU cannot sample some small formats that
        // D3D12 has no equivalent for (R8 sRGB, R8G8 sRGB, R4G4B4A4; createImage -> ErrorOutOfDeviceMemory, the render thread
        // aborted). Such textures are created as RGBA8 (sRGB when the original was) and expanded on upload (upload_texture_impl
        // below), keeping the same logical channels, so the view swizzle and the shaders are unchanged. Supported formats keep
        // upstream's path exactly.
        const vk::Format vitox_orig = vk_format;
        vk::Format vitox_sub = vk::Format::eUndefined;
        switch (vk_format) {
        case vk::Format::eR8Srgb:
        case vk::Format::eR8G8Srgb:
            vitox_sub = vk::Format::eR8G8B8A8Srgb;
            break;
        case vk::Format::eR4G4B4A4UnormPack16:
        case vk::Format::eA1R5G5B5UnormPack16:
        case vk::Format::eR5G6B5UnormPack16:
            vitox_sub = vk::Format::eR8G8B8A8Unorm; // upstream has no sRGB variant of these either
            break;
        default:
            break;
        }
        if (vitox_sub != vk::Format::eUndefined && !vitox_can_sample(state.physical_device, vk_format)) {
            vk_format = vitox_sub;
            static uint32_t vitox_logged_mask = 0;
            const uint32_t bit = 1u << (static_cast<uint32_t>(vitox_orig) & 31);
            if (!(vitox_logged_mask & bit)) {
                vitox_logged_mask |= bit;
                LOG_INFO("Vitox [0.1.1.10] texture format {} not sampleable on this device -> {} (expanded on upload)",
                    vk::to_string(vitox_orig), vk::to_string(vk_format));
            }
        }
    }
]=]
[=[void VKTextureCache::upload_texture_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
]=]
[=[// Vitox 0.1.1.10 compat: expand a small-format texel row block to RGBA8 (the image was substituted in configure_texture).
// Vulkan packings: R4G4B4A4 R=15..12 G=11..8 B=7..4 A=3..0; A1R5G5B5 A=15 R=14..10 G=9..5 B=4..0; R5G6B5 R=15..11 G=10..5 B=4..0.
// Missing channels read as G=B=0, A=1, as the original single/dual-channel image would.
static const void *vitox_expand_to_rgba8(SceGxmTextureBaseFormat base_format, const void *pixels, uint32_t count, std::vector<uint8_t> &data) {
    data.resize(static_cast<size_t>(count) * 4);
    uint8_t *dst = data.data();
    const uint8_t *s8 = static_cast<const uint8_t *>(pixels);
    const uint16_t *s16 = static_cast<const uint16_t *>(pixels);
    for (uint32_t i = 0; i < count; i++, dst += 4) {
        switch (base_format) {
        case SCE_GXM_TEXTURE_BASE_FORMAT_U8:
            dst[0] = s8[i], dst[1] = 0, dst[2] = 0, dst[3] = 255;
            break;
        case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8:
            dst[0] = s8[2 * i], dst[1] = s8[2 * i + 1], dst[2] = 0, dst[3] = 255;
            break;
        case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4: {
            const uint16_t v = s16[i];
            dst[0] = static_cast<uint8_t>(((v >> 12) & 0xF) * 17), dst[1] = static_cast<uint8_t>(((v >> 8) & 0xF) * 17);
            dst[2] = static_cast<uint8_t>(((v >> 4) & 0xF) * 17), dst[3] = static_cast<uint8_t>((v & 0xF) * 17);
            break;
        }
        case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5: {
            const uint16_t v = s16[i];
            const auto x5 = [](uint32_t c) { return static_cast<uint8_t>((c << 3) | (c >> 2)); };
            dst[0] = x5((v >> 10) & 31), dst[1] = x5((v >> 5) & 31), dst[2] = x5(v & 31), dst[3] = (v & 0x8000) ? 255 : 0;
            break;
        }
        case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5: {
            const uint16_t v = s16[i];
            const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            dst[0] = static_cast<uint8_t>((r << 3) | (r >> 2)), dst[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
            dst[2] = static_cast<uint8_t>((b << 3) | (b >> 2)), dst[3] = 255;
            break;
        }
        default:
            dst[0] = dst[1] = dst[2] = 0, dst[3] = 255;
            break;
        }
    }
    return data.data();
}

void VKTextureCache::upload_texture_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
]=]
[=[        base_format = (base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8) ? SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8 : SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8;
    }
]=]
[=[        base_format = (base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8) ? SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8 : SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8;
    }
    // Vitox 0.1.1.10 compat: the image was created as RGBA8 in place of a format the device cannot sample (configure_texture)
    if ((image.format == vk::Format::eR8G8B8A8Unorm || image.format == vk::Format::eR8G8B8A8Srgb)
        && (base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U8 || base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U8U8
            || base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4 || base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5
            || base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5)) {
        text_data = vitox_expand_to_rgba8(base_format, pixels, pixels_per_stride * height, temp_data);
        base_format = SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8;
    }
]=]
[=[static vk::Format linear_to_srgb(const vk::Format format) {
]=]
[=[// Vitox 0.1.1.10 compat: can the device sample this format with optimal tiling? (cached per format)
static bool vitox_can_sample(vk::PhysicalDevice pd, vk::Format f) {
    static std::mutex m;
    static std::unordered_map<int, bool> cache;
    std::lock_guard<std::mutex> lock(m);
    const auto it = cache.find(static_cast<int>(f));
    if (it != cache.end())
        return it->second;
    const bool ok = static_cast<bool>(pd.getFormatProperties(f).optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage);
    cache.emplace(static_cast<int>(f), ok);
    return ok;
}

static vk::Format linear_to_srgb(const vk::Format format) {
]=]
)
vitox_substitute(vita3k/renderer/src/vulkan/gxm_to_vulkan.cpp m9a6_gxm_to_vulkan.cpp
[=[    case SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4:
        return vk::Format::eR4G4B4A4UnormPack16;
]=]
[=[    case SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4:
        // Vitox 0.1.1.3 (Killzone Mercenary): Dozen on the Xbox GPU cannot render to R4G4B4A4 (vkCreateImage with
        // COLOR_ATTACHMENT -> ErrorOutOfDeviceMemory), so 4444 colour surfaces are stored as RGBA8 (as upstream does for
        // U8U8U8); their CPU write-back is skipped (format_support_surface_sync, m6c_surface_cache.cpp)
        return vk::Format::eR8G8B8A8Unorm;
]=]
[=[#include <util/log.h>
]=]
[=[#include <util/log.h>

// Vitox M9A.6 compat (see core-build/CMakeLists.txt): set by VKTextureCache::init when the device lacks A2R10G10B10
bool vitox_a2rgb10_fallback = false;
]=]
[=[        return vk::Format::eA2R10G10B10UnormPack32;
]=]
[=[        return vitox_a2rgb10_fallback ? vk::Format::eA2B10G10R10UnormPack32 : vk::Format::eA2R10G10B10UnormPack32; // Vitox M9A.6 compat
]=]
[=[#include "../texture/astc_formats.inc"]=]
"#include \"${VITA3K}/vita3k/renderer/src/texture/astc_formats.inc\" // Vitox: patched copy lives elsewhere")
vitox_substitute(vita3k/renderer/src/texture/cache.cpp m9a6_texture_cache.cpp
[=[#include <util/log.h>
]=]
[=[#include <util/log.h>

extern bool vitox_a2rgb10_fallback; // Vitox M9A.6 compat, defined in m9a6_gxm_to_vulkan.cpp
]=]
[=[        case SCE_GXM_TEXTURE_BASE_FORMAT_U2F10F10F10:
            // don't change what openGL is doing]=]
[=[        case SCE_GXM_TEXTURE_BASE_FORMAT_U2U10U10U10:
            // Vitox M9A.6 compat: the image is A2B10G10R10, so exchange the bit fields 0-9 and 20-29 of each texel
            if (!is_vulkan || !vitox_a2rgb10_fallback)
                break;
            texture_data_decompressed.resize(pixels_per_stride * memory_height * 4);
            {
                const uint32_t *vitox_src = static_cast<const uint32_t *>(pixels);
                uint32_t *vitox_dst = reinterpret_cast<uint32_t *>(texture_data_decompressed.data());
                for (size_t i = 0; i < static_cast<size_t>(pixels_per_stride) * memory_height; i++) {
                    const uint32_t t = vitox_src[i];
                    vitox_dst[i] = (t & 0xC00FFC00u) | ((t & 0x3FFu) << 20) | ((t >> 20) & 0x3FFu);
                }
            }
            pixels = texture_data_decompressed.data();
            break;
        case SCE_GXM_TEXTURE_BASE_FORMAT_U2F10F10F10:
            // don't change what openGL is doing]=])
# ---- M7B.6 DIAGNOSTIC (read-only, VITOX_UWP): scene setup (context.cpp) and the guest draw inputs (vulkan/scene.cpp) ----
vitox_substitute(vita3k/renderer/src/vulkan/context.cpp m7b6_context.cpp
[=[#include <util/log.h>
]=]
[=[#include <util/log.h>
#ifdef VITOX_UWP
#include <vitox_m7b6_diag.h> // Vitox M7B.6 diagnostic (logging only)
#endif
]=]
[=[    context.current_color_base_image = framebuffer.base_image;
]=]
[=[    context.current_color_base_image = framebuffer.base_image;
#ifdef VITOX_UWP
    {
        // Vitox M7B.6 DIAGNOSTIC: what this guest scene renders into (read-only)
        const uint64_t vitox_scene = vitox_m7b6_begin_scene();
        if (vitox_m7b6_sampled()) {
            const SceGxmDepthStencilSurface &ds = context.record.depth_stencil_surface;
            LOG_INFO("Vitox [M7B.6] scene {} set_context: rt {}x{} macroblock_sync {} | colour data 0x{:08X} gxm format 0x{:X} vk format {} (null colour {}) | "
                     "record depth-stencil: depth_data 0x{:08X} stencil_data 0x{:08X} force_load {} force_store {} background_depth {} stencil {} mask {} -> {} | "
                     "pass force_load {} force_store {} | renderPass {} framebuffer {} colour view {} ds view {} colour image {}",
                vitox_scene, rt->width, rt->height, rt->has_macroblock_sync, context.record.color_surface.data.address(),
                static_cast<uint32_t>(context.record.color_surface.colorFormat), static_cast<int>(vk_format), color_surface_fin == nullptr,
                ds.depth_data.address(), ds.stencil_data.address(), static_cast<int>(ds.force_load), static_cast<int>(ds.force_store), ds.background_depth,
                static_cast<int>(ds.stencil), static_cast<int>(ds.mask), ds_surface_fin ? "memory-backed ds" : "transient ds (not backed by guest memory)",
                force_load, force_store, static_cast<void *>(static_cast<VkRenderPass>(context.current_render_pass)),
                static_cast<void *>(static_cast<VkFramebuffer>(context.current_framebuffer)), static_cast<void *>(static_cast<VkImageView>(context.current_color_view)),
                static_cast<void *>(static_cast<VkImageView>(context.current_ds_view)),
                context.current_color_base_image ? static_cast<void *>(static_cast<VkImage>(context.current_color_base_image->image)) : nullptr);
        }
    }
#endif
]=]
[=[    render_cmd.beginRenderPass(curr_renderpass_info, vk::SubpassContents::eInline);
]=]
[=[    render_cmd.beginRenderPass(curr_renderpass_info, vk::SubpassContents::eInline);
#ifdef VITOX_UWP
    if (vitox_m7b6_sampled()) // Vitox M7B.6 DIAGNOSTIC: begin info as recorded
        LOG_INFO("Vitox [M7B.6] scene {} beginRenderPass: renderPass {} framebuffer {} renderArea {},{} {}x{} | clearValueCount {} | clear[1] depth {} stencil {} "
                 "(clear[0] unused: colour loadOp LOAD)",
            vitox_m7b6_scene(), static_cast<void *>(static_cast<VkRenderPass>(curr_renderpass_info.renderPass)),
            static_cast<void *>(static_cast<VkFramebuffer>(curr_renderpass_info.framebuffer)), curr_renderpass_info.renderArea.offset.x,
            curr_renderpass_info.renderArea.offset.y, curr_renderpass_info.renderArea.extent.width, curr_renderpass_info.renderArea.extent.height,
            curr_renderpass_info.clearValueCount, curr_clear_values[1].depthStencil.depth, curr_clear_values[1].depthStencil.stencil);
#endif
]=]
# M9A.10 EXPERIMENT (0.0.24.133): with memory mapping off, Vulkan never writes a rendered surface back to guest memory
# (upstream's surface sync needs mapping), so CPU readbacks of GPU output see stale memory. After each submitted scene,
# its colour surface (all formats except U8U8U8U8) is copied back to guest memory (waitIdle; U2U10U10U10 re-swapped).
[=[void VKContext::stop_recording(const SceGxmNotification &notif1, const SceGxmNotification &notif2, bool submit) {]=]
[=[#ifdef VITOX_UWP
} // namespace renderer::vulkan
extern bool vitox_a2rgb10_fallback; // Vitox M9A.6 compat, defined (global) in m9a6_gxm_to_vulkan.cpp
namespace renderer::vulkan {
static void vitox_writeback_surface(VKState &state, MemState &mem, SceGxmColorSurface &color) {
    if (!color.data)
        return;
    const SceGxmColorBaseFormat base = gxm::get_base_format(color.colorFormat);
    if (base == SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8)
        return;
    const uint32_t bpp = gxm::bits_per_pixel(base) / 8;
    if (bpp == 0 || bpp > 8)
        return;
    const SurfaceRetrieveResult res = state.surface_cache.retrieve_color_surface_for_framebuffer(mem, &color);
    vkutil::Image *img = res.base_image;
    if (!img || !img->image || img->width == 0 || img->height == 0 || img->width > 4096 || img->height > 4096)
        return;
    const uint32_t w = std::min<uint32_t>(img->width, color.width), h = std::min<uint32_t>(img->height, color.height);
    const uint32_t guest_stride = color.strideInPixels * bpp;
    vkutil::Buffer buf(static_cast<vk::DeviceSize>(w) * h * bpp);
    buf.init_buffer(vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc);
    vk::CommandBuffer cmd = vkutil::create_single_time_command(state.device, state.general_command_pool);
    const vk::BufferImageCopy region{ .bufferOffset = 0, .bufferRowLength = w, .bufferImageHeight = h, .imageSubresource = vkutil::color_subresource_layer,
        .imageOffset = { 0, 0, 0 }, .imageExtent = { w, h, 1 } };
    cmd.copyImageToBuffer(img->image, vk::ImageLayout::eGeneral, buf.buffer, region);
    vkutil::end_single_time_command(state.device, state.general_queue, state.general_command_pool, cmd); // waitIdle (after the scene)
    uint8_t *dst = static_cast<uint8_t *>(color.data.get(mem));
    const uint8_t *src = static_cast<const uint8_t *>(buf.mapped_data);
    const bool swap10 = base == SCE_GXM_COLOR_BASE_FORMAT_U2U10U10U10 && vitox_a2rgb10_fallback;
    for (uint32_t y = 0; y < h; y++) {
        if (swap10) {
            const uint32_t *s = reinterpret_cast<const uint32_t *>(src + static_cast<size_t>(y) * w * 4);
            uint32_t *d = reinterpret_cast<uint32_t *>(dst + static_cast<size_t>(y) * guest_stride);
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t t = s[x];
                d[x] = (t & 0xC00FFC00u) | ((t & 0x3FFu) << 20) | ((t >> 20) & 0x3FFu);
            }
        } else {
            memcpy(dst + static_cast<size_t>(y) * guest_stride, src + static_cast<size_t>(y) * w * bpp, static_cast<size_t>(w) * bpp);
        }
    }
    static std::map<uint32_t, bool> logged;
    if (logged.size() < 64 && logged.emplace(color.data.address(), true).second) {
        fprintf(stderr, "[vitox-wb] surface 0x%08x fmt 0x%08x %ux%u bpp %u stride %u (image %ux%u vk %d)\n", color.data.address(),
            static_cast<uint32_t>(base), w, h, bpp, guest_stride, img->width, img->height, static_cast<int>(img->format));
        fflush(stderr);
    }
}
#endif
void VKContext::stop_recording(const SceGxmNotification &notif1, const SceGxmNotification &notif2, bool submit) {]=]
[=[    state.general_queue.submit(submit_info, fence);
    cmdbuffers_to_submit.clear();
    state.frame().rendered_fences.push_back(fence);
]=]
[=[    state.general_queue.submit(submit_info, fence);
    cmdbuffers_to_submit.clear();
    state.frame().rendered_fences.push_back(fence);
#ifdef VITOX_UWP
    if (false && !state.features.enable_memory_mapping) // 0.0.24.133 write-back: light values unchanged (disproven); off since 0.0.24.134
        vitox_writeback_surface(state, mem, record.color_surface); // Vitox M9A.10 experiment
#endif
]=])
vitox_substitute(vita3k/renderer/src/vulkan/scene.cpp m7b6_vk_scene.cpp
[=[#include <util/log.h>
]=]
[=[#include <util/log.h>
#ifdef VITOX_UWP
#include <vitox_m7b6_diag.h> // Vitox M7B.6 diagnostic (logging only)
#endif
]=]
[=[    const uint32_t data_size_upload = std::min<uint32_t>(size, program->uniform_buffer_sizes.at(block_num) * 4);
]=]
[=[    const uint32_t data_size_upload = std::min<uint32_t>(size, program->uniform_buffer_sizes.at(block_num) * 4);
#ifdef VITOX_UWP
    if (vitox_m7b6_sampled()) {
        // Vitox M7B.6 DIAGNOSTIC: uniform data as uploaded (read-only)
        const float *f = reinterpret_cast<const float *>(data.get(mem));
        std::string vals;
        for (uint32_t i = 0; f && i < std::min<uint32_t>(data_size_upload / 4, 16); i++)
            vals += fmt::format("{}{}", i ? " " : "", f[i]);
        LOG_INFO("Vitox [M7B.6] scene {} uniform: {} block {} guest 0x{:08X} size {} upload {} bytes at offset {} | floats [{}]", vitox_m7b6_scene(),
            vertex_shader ? "vertex" : "fragment", block_num, data.address(), size, data_size_upload, offset * 4, vals);
    }
#endif
]=]
[=[    // can happen with asynchronous pipeline compilation
    if (context.current_pipeline == nullptr)
        return;
]=]
[=[    // can happen with asynchronous pipeline compilation
#ifdef VITOX_UWP
    {
        static uint64_t vitox_skipped = 0; // Vitox M7B.6 DIAGNOSTIC: draws dropped because no pipeline is ready yet
        static bool vitox_first_drawn = false; // M7B.7: the scene of the first draw that reaches vkCmdDrawIndexed
        if (context.current_pipeline == nullptr)
            LOG_INFO("Vitox [M7B.6] scene {} draw SKIPPED: no pipeline yet (asynchronous compilation, count {} <= 6 whole-quad exemption {}) - skipped total {}",
                vitox_m7b6_scene(), count, instance_count == 1 && count <= 6, ++vitox_skipped);
        else if (!vitox_first_drawn) {
            vitox_first_drawn = true;
            LOG_INFO("Vitox [M7B.7] first drawn scene {}: pipeline {} ready (async compile done), draws skipped before it {}", vitox_m7b6_scene(),
                static_cast<void *>(static_cast<VkPipeline>(context.current_pipeline)), vitox_skipped);
        }
    }
#endif
    if (context.current_pipeline == nullptr)
        return;
]=]
[=[    vert_ublock.screen_height = context.render_target->height / context.state.res_multiplier;
]=]
[=[    vert_ublock.screen_height = context.render_target->height / context.state.res_multiplier;
#ifdef VITOX_UWP
    if (vitox_m7b6_take_first_draw()) {
        // Vitox M7B.6 DIAGNOSTIC: everything the first draw of a sampled scene consumes (read-only; streams are not yet bound)
        const GxmRecordState &rec = context.record;
        const SceGxmVertexProgram &vp = *rec.vertex_program.get(mem);
        LOG_INFO("Vitox [M7B.6] scene {} first draw: pipeline {} primitive {} index format {} count {} instances {} indices guest 0x{:08X} | "
                 "viewport x {} y {} w {} h {} minDepth {} maxDepth {} | scissor {},{} {}x{} | region clip mode {} min {},{} max {},{} | "
                 "vert ublock viewport_flip [{} {} {} {}] viewport_flag {} z_offset {} z_scale {} screen {}x{} (viewport_flat {}) | "
                 "stencil front ref {} cmp 0x{:X} wr 0x{:X} | depth bias unit {} slope {} | line width {} | cull {} | mapping {}",
            vitox_m7b6_scene(), static_cast<void *>(static_cast<VkPipeline>(context.current_pipeline)), static_cast<int>(type), static_cast<int>(format), count,
            instance_count, indices.address(), context.viewport.x, context.viewport.y, context.viewport.width, context.viewport.height, context.viewport.minDepth,
            context.viewport.maxDepth, context.scissor.offset.x, context.scissor.offset.y, context.scissor.extent.width, context.scissor.extent.height,
            static_cast<int>(rec.region_clip_mode), rec.region_clip_min.x, rec.region_clip_min.y, rec.region_clip_max.x, rec.region_clip_max.y,
            vert_ublock.viewport_flip[0], vert_ublock.viewport_flip[1], vert_ublock.viewport_flip[2], vert_ublock.viewport_flip[3], vert_ublock.viewport_flag,
            vert_ublock.z_offset, vert_ublock.z_scale, vert_ublock.screen_width, vert_ublock.screen_height, rec.viewport_flat,
            rec.front_stencil_state_values.ref, rec.front_stencil_state_values.compare_mask, rec.front_stencil_state_values.write_mask, rec.depth_bias_unit,
            rec.depth_bias_slope, rec.line_width, static_cast<int>(rec.cull_mode), static_cast<int>(context.state.mapping_method));
        std::string idx;
        uint32_t max_idx = 0;
        for (size_t i = 0; indices_ptr && i < std::min<size_t>(count, 32); i++) {
            const uint32_t v = (format == SCE_GXM_INDEX_FORMAT_U16) ? static_cast<const uint16_t *>(indices_ptr)[i] : static_cast<const uint32_t *>(indices_ptr)[i];
            max_idx = std::max(max_idx, v);
            idx += fmt::format("{}{}", i ? " " : "", v);
        }
        LOG_INFO("Vitox [M7B.6] scene {} indices [{}] max {}", vitox_m7b6_scene(), idx, max_idx);
        for (const SceGxmVertexAttribute &a : vp.attributes)
            LOG_INFO("Vitox [M7B.6] scene {} attribute: stream {} offset {} format {} components {} regIndex {} (used by the shader {})", vitox_m7b6_scene(),
                a.streamIndex, a.offset, static_cast<int>(a.format), static_cast<int>(a.componentCount), a.regIndex,
                vp.renderer_data->attribute_infos.contains(a.regIndex));
        for (size_t s = 0; s < vp.streams.size() && s < 2; s++) {
            const uint32_t stride = vp.streams[s].stride;
            const uint8_t *base = rec.vertex_streams[s].data ? rec.vertex_streams[s].data.get(mem) : nullptr;
            LOG_INFO("Vitox [M7B.6] scene {} stream {}: guest 0x{:08X} size {} stride {} indexSource {}", vitox_m7b6_scene(), s,
                rec.vertex_streams[s].data.address(), rec.vertex_streams[s].size, stride, vp.streams[s].indexSource);
            if (!base || stride == 0 || stride % 4 != 0)
                continue;
            for (uint32_t v = 0; v <= max_idx && v < 16; v++) {
                const float *f = reinterpret_cast<const float *>(base + v * stride);
                std::string vals;
                for (uint32_t k = 0; k < std::min<uint32_t>(stride / 4, 8); k++)
                    vals += fmt::format("{}{}", k ? " " : "", f[k]);
                LOG_INFO("Vitox [M7B.6] scene {} stream {} vertex {}: [{}]", vitox_m7b6_scene(), s, v, vals);
            }
        }
    }
#endif
]=]
[=[#include <vitox_m7b6_diag.h> // Vitox M7B.6 diagnostic (logging only)]=]
[=[#include <vitox_m7b6_diag.h> // Vitox M7B.6 diagnostic (logging only)
#include <vitox_vertex_realign.h> // Vitox M9A.8: 4-byte aligned vertex streams]=]
# M9A.8 (0.0.24.94, Persona 4 DAN character models): the Xbox driver rejects unaligned vertex input offsets, so a stream
# with an unaligned attribute offset or stride is repacked on its per-draw copy (memory mapping is off); see vitox_vertex_realign.h
[=[#endif
                context.vertex_stream_ring_buffer.allocate(context.prerender_cmd, stream_size, stream);
                context.vertex_stream_offsets[i] = context.vertex_stream_ring_buffer.data_offset;
]=]
[=[#endif
#ifdef VITOX_UWP
                // Vitox M9A.8: repack a stream with an unaligned attribute offset/stride (vitox_vertex_realign.h); the
                // pipeline's vertex input (get_vertex_input_state) uses the same layout
                uint8_t *vitox_repacked = nullptr;
                {
                    static thread_local std::vector<uint16_t> vitox_new_offsets;
                    vitox_new_offsets.assign(vertex_program.attributes.size(), 0);
                    const VitoxStreamLayout vitox_layout = vitox_realign_stream(vertex_program, i,
                        [&](const SceGxmVertexAttribute &a) { return vkvert->attribute_infos.contains(a.regIndex); }, vitox_new_offsets.data());
                    if (vitox_layout.repack) {
                        uint32_t vitox_size = 0;
                        vitox_repacked = vitox_repack_stream(stream, stream_size, vertex_program.streams[i].stride, vitox_layout, vitox_size);
                        stream = vitox_repacked;
                        stream_size = vitox_size;
                    }
                }
#endif
                context.vertex_stream_ring_buffer.allocate(context.prerender_cmd, stream_size, stream);
                context.vertex_stream_offsets[i] = context.vertex_stream_ring_buffer.data_offset;
#ifdef VITOX_UWP
                delete[] vitox_repacked;
#endif
]=]
# M9A.10 FIX (0.0.24.115): with memory mapping off every draw copied each vertex stream from vertex 0 up to its max index.
# Gravity Rush's city meshes share big vertex buffers, so ~1-3 GB/s went through the 64 MiB vertex ring, which then wrapped
# every 41-130 ms (as little as ~1 frame; up to MAX_FRAMES_RENDERING=3 frames are in flight, and a wrap has no GPU sync):
# vertex data still queued for the GPU was overwritten = flickering stretched geometry. Now only [min index, max index] is
# copied and drawIndexed gets vertexOffset = -min index (exact; instanced and stride-0 streams are copied whole).
[=[#include <vitox_vertex_realign.h> // Vitox M9A.8: 4-byte aligned vertex streams]=]
[=[#include <vitox_vertex_realign.h> // Vitox M9A.8: 4-byte aligned vertex streams
#ifdef VITOX_UWP
static thread_local uint32_t vitox_base_vertex = 0; // Vitox M9A.10: lowest index of the draw being recorded (memory mapping off)
#endif]=]
[=[        context.index_stream_ring_buffer.allocate(context.prerender_cmd, index_buffer_size, indices_ptr);
]=]
[=[#ifdef VITOX_UWP
        {
            // Vitox M9A.10: the lowest index used; bind_vertex_streams copies from there.
            // 0.0.24.161: the indices themselves are rebased (minus that lowest index) and drawIndexed gets vertexOffset 0,
            // instead of a negative vertexOffset (dzn emulates base vertex; GR floor/walls index into shared level buffers).
            uint32_t vitox_min = UINT32_MAX;
            if (format == SCE_GXM_INDEX_FORMAT_U16) {
                const uint16_t *d = static_cast<const uint16_t *>(indices_ptr);
                for (uint32_t k = 0; k < count; k++)
                    vitox_min = std::min<uint32_t>(vitox_min, d[k]);
            } else {
                const uint32_t *d = static_cast<const uint32_t *>(indices_ptr);
                for (uint32_t k = 0; k < count; k++)
                    vitox_min = std::min<uint32_t>(vitox_min, d[k]);
            }
            vitox_base_vertex = (count && vitox_min <= 0x7fffffffu) ? vitox_min : 0;
            if (vitox_base_vertex) {
                static thread_local std::vector<uint8_t> vitox_rebased;
                vitox_rebased.resize(index_buffer_size);
                if (format == SCE_GXM_INDEX_FORMAT_U16) {
                    const uint16_t *d = static_cast<const uint16_t *>(indices_ptr);
                    uint16_t *o = reinterpret_cast<uint16_t *>(vitox_rebased.data());
                    for (uint32_t k = 0; k < count; k++)
                        o[k] = static_cast<uint16_t>(d[k] - vitox_base_vertex);
                } else {
                    const uint32_t *d = static_cast<const uint32_t *>(indices_ptr);
                    uint32_t *o = reinterpret_cast<uint32_t *>(vitox_rebased.data());
                    for (uint32_t k = 0; k < count; k++)
                        o[k] = d[k] - vitox_base_vertex;
                }
                indices_ptr = vitox_rebased.data();
            }
        }
#endif
        context.index_stream_ring_buffer.allocate(context.prerender_cmd, index_buffer_size, indices_ptr);
]=]
[=[#ifdef VITOX_UWP
                // Vitox M9A.8: repack a stream with an unaligned attribute offset/stride]=]
[=[#ifdef VITOX_UWP
                // Vitox M9A.10: copy only from the draw's lowest index (0.0.24.161: the index copy is rebased by the same amount)
                {
                    const SceGxmVertexStream &vitox_bs = vertex_program.streams[i];
                    const uint64_t vitox_skip = (uint64_t)vitox_base_vertex * vitox_bs.stride;
                    if (vitox_skip && !gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(vitox_bs.indexSource)) && vitox_skip < stream_size) {
                        stream += vitox_skip;
                        stream_size -= static_cast<uint32_t>(vitox_skip);
                    }
                }
#endif
#ifdef VITOX_UWP
                // Vitox M9A.8: repack a stream with an unaligned attribute offset/stride]=]
)
# M6B.1 (BUILD-ONLY): Vulkan-only generic renderer. The generic TUs below dispatch on renderer.current_backend;
# each `case Backend::OpenGL:` block (always directly followed by `case Backend::Vulkan:` in the same switch) and
# each <renderer/gl/*.h> include is wrapped in #ifndef VITOX_UWP. Nothing is stubbed: with GL compiled out,
# renderer::init(Backend::OpenGL) reaches its existing default branch (LOG_ERROR + return false) and the other
# switches reach their default (REPORT_MISSING) or no-op. Configure fails if the case/include counts drift or
# if a wrapped block contains anything that looks like another switch label.
function(vitox_strip_gl_dispatch src_rel dst_name expected_cases expected_includes)
    set(_src "${VITA3K}/${src_rel}")
    set(_dst "${CMAKE_BINARY_DIR}/vitox-patched/${dst_name}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
    file(READ "${_src}" _t)
    string(REPLACE "\r\n" "\n" _t "${_t}")
    string(REGEX MATCHALL "#include <renderer/gl/[a-z_]+\\.h>" _incs "${_t}")
    list(LENGTH _incs _ninc)
    if (NOT _ninc EQUAL expected_includes)
        message(FATAL_ERROR "Vitox vk-only ${dst_name}: expected ${expected_includes} <renderer/gl/*.h> includes in ${src_rel}, found ${_ninc}")
    endif()
    string(REGEX REPLACE "(#include <renderer/gl/[a-z_]+\\.h>)" "#ifndef VITOX_UWP // Vitox vk-only\n\\1\n#endif" _t "${_t}")
    # renderer/gl/state.h was these TUs' (transitive) source of the full Config definition
    string(FIND "${_t}" "#ifndef VITOX_UWP // Vitox vk-only\n" _fi)
    string(SUBSTRING "${_t}" 0 ${_fi} _pre)
    string(SUBSTRING "${_t}" ${_fi} -1 _post)
    set(_t "${_pre}#ifdef VITOX_UWP // Vitox vk-only: was reached via renderer/gl/state.h\n#include <config/state.h>\n#endif\n${_post}")
    string(LENGTH "case Backend::OpenGL:" _gl_len)
    set(_out "")
    set(_n 0)
    while (TRUE)
        string(FIND "${_t}" "case Backend::OpenGL:" _p)
        if (_p EQUAL -1)
            break()
        endif()
        string(SUBSTRING "${_t}" 0 ${_p} _head)
        string(FIND "${_head}" "\n" _ls REVERSE) # start of the GL case line
        math(EXPR _ls "${_ls} + 1")
        string(SUBSTRING "${_t}" 0 ${_ls} _before)
        string(SUBSTRING "${_t}" ${_ls} -1 _rest)
        string(FIND "${_rest}" "case Backend::Vulkan:" _v)
        if (_v EQUAL -1)
            message(FATAL_ERROR "Vitox vk-only ${dst_name}: GL case #${_n} in ${src_rel} is not followed by case Backend::Vulkan:")
        endif()
        string(SUBSTRING "${_rest}" 0 ${_v} _vhead)
        string(FIND "${_vhead}" "\n" _ve REVERSE) # end of the GL block = start of the Vulkan case line
        math(EXPR _ve "${_ve} + 1")
        string(SUBSTRING "${_rest}" 0 ${_ve} _block)
        string(SUBSTRING "${_rest}" ${_ve} -1 _t)
        string(FIND "${_block}" "case Backend::OpenGL:" _gp)
        math(EXPR _gp "${_gp} + ${_gl_len}")
        string(SUBSTRING "${_block}" ${_gp} -1 _body)
        if (_body MATCHES "switch[ \t]*\\(|[ \t]case[ \t]|default:")
            message(FATAL_ERROR "Vitox vk-only ${dst_name}: GL case #${_n} in ${src_rel} contains another switch label:\n${_block}")
        endif()
        string(APPEND _out "${_before}#ifndef VITOX_UWP // Vitox vk-only: no OpenGL on Xbox UWP\n${_block}#endif\n")
        math(EXPR _n "${_n} + 1")
    endwhile()
    string(APPEND _out "${_t}")
    if (NOT _n EQUAL expected_cases)
        message(FATAL_ERROR "Vitox vk-only ${dst_name}: expected ${expected_cases} OpenGL cases in ${src_rel}, found ${_n}")
    endif()
    file(WRITE "${_dst}.tmp" "${_out}")
    file(COPY_FILE "${_dst}.tmp" "${_dst}" ONLY_IF_DIFFERENT)
    message(STATUS "Vitox vk-only: ${src_rel} -> ${_dst} (${_n} GL cases, ${_ninc} GL includes compiled out)")
endfunction()
vitox_strip_gl_dispatch(vita3k/renderer/src/creation.cpp vkonly_creation.cpp 6 2)
vitox_strip_gl_dispatch(vita3k/renderer/src/renderer.cpp vkonly_renderer.cpp 1 2)
vitox_strip_gl_dispatch(vita3k/renderer/src/scene.cpp vkonly_scene.cpp 3 2)
vitox_strip_gl_dispatch(vita3k/renderer/src/state_set.cpp vkonly_state_set.cpp 16 3)
vitox_strip_gl_dispatch(vita3k/renderer/src/sync.cpp vkonly_sync.cpp 1 2)
# 0.0.24.70 (M9A.6): swscale restored. Since 0.0.24.44 the static WindowsStore FFmpeg n7.1.1 (toolchain/ffmpeg-n7.1.1-uwp)
# includes libswscale, so the three sites below run upstream code again; the compiled-out branches stay behind
# VITOX_SWSCALE_DISABLED (not defined). Found by Gravity Rush: its H.264 movie frames are YUV420 textures (black otherwise).
# ---- M6C swscale (approved): FFmpeg swscale is not linked on WindowsStore yet. ONLY its three call sites are
# compiled out under VITOX_UWP; each disabled path logs, counts (compat/vitox_swscale_disabled.h) and skips via
# the function's existing skip/error path; the upstream code stays in the #else branch for FFmpeg restoration:
#   1 texture/yuv.cpp            yuv420_texture_to_rgb       (+ get_sws_context, ~TextureCache free)
#   2 vulkan/surface_cache.cpp   perform_post_surface_sync   RGB24/BGR24 readback (+ ~ColorSurfaceCacheInfo free)
#   3 transfer.cpp               cmd_handle_transfer_downscale ffmpeg SWS_AREA branch (the nearest-filter fallback stays)
set(_m6c_sws_inc [=[#include <libswscale/swscale.h>
}
]=])
set(_m6c_sws_inc_new [=[#include <libswscale/swscale.h>
}
#ifdef VITOX_UWP
#include <util/log.h>
#include <vitox_swscale_disabled.h> // Vitox M6C: swscale call sites compiled out (see core-build/CMakeLists.txt)
#endif
]=])
vitox_substitute(vita3k/renderer/src/texture/yuv.cpp m6c_yuv.cpp
"${_m6c_sws_inc}" "${_m6c_sws_inc_new}"
[=[static SwsContext *get_sws_context(]=]
[=[#ifndef VITOX_SWSCALE_DISABLED // Vitox M6C: swscale compiled out
static SwsContext *get_sws_context(]=]
[=[        cache.sws_context = context;
    }
    return context;
}
]=]
[=[        cache.sws_context = context;
    }
    return context;
}
#endif
]=]
[=[    SwsContext *context = get_sws_context(cache, width, height, is_p3);
    assert(context);
]=]
[=[#ifdef VITOX_SWSCALE_DISABLED
    // Vitox M6C: swscale compiled out - the YUV texture is NOT converted (dst left untouched).
    vitox_swscale_disabled_hit(VITOX_SWS_YUV420_TO_RGB);
    LOG_ERROR("Vitox: YUV420 {}x{} (p3={}) texture conversion skipped: FFmpeg swscale is not available on WindowsStore", width, height, is_p3);
#else
    SwsContext *context = get_sws_context(cache, width, height, is_p3);
    assert(context);
]=]
[=[    assert(error == height);
}
]=]
[=[    assert(error == height);
#endif
}
]=]
[=[        sws_freeContext(context);
        yuv_conversion_cache.sws_context = nullptr;]=]
[=[#ifdef VITOX_SWSCALE_DISABLED
        vitox_swscale_disabled_hit(VITOX_SWS_UNEXPECTED_CONTEXT); // never created when compiled out
#else
        sws_freeContext(context);
#endif
        yuv_conversion_cache.sws_context = nullptr;]=])
# 0.1.1.4 (Sly Cooper: Thieves in Time black screen after its "signed out of PSN" message): the game calls
# sceMsgDialogInit every frame for its next dialog while the previous one is FINISHED but not terminated (its one early
# sceMsgDialogTerm came while that dialog was still RUNNING). Upstream answers BUSY forever. A finished, unterminated
# message dialog is now terminated by the next sceMsgDialogInit (its result is no longer readable after a new Init anyway).
vitox_substitute(vita3k/modules/SceCommonDialog/SceCommonDialog.cpp vx_SceCommonDialog.cpp
[=[EXPORT(int, sceMsgDialogInit, const Ptr<SceMsgDialogParam> param) {
    TRACY_FUNC(sceMsgDialogInit, param);
    if (emuenv.common_dialog.type != NO_DIALOG)
        return RET_ERROR(SCE_COMMON_DIALOG_ERROR_BUSY);
]=]
[=[EXPORT(int, sceMsgDialogInit, const Ptr<SceMsgDialogParam> param) {
    TRACY_FUNC(sceMsgDialogInit, param);
    if (emuenv.common_dialog.type == MESSAGE_DIALOG && emuenv.common_dialog.status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        std::lock_guard<std::recursive_mutex> lock(emuenv.common_dialog.mutex);
        LOG_INFO("Vitox msg dialog: previous message dialog finished (result {}, button {}) but not terminated - terminated by the new sceMsgDialogInit",
            static_cast<int>(emuenv.common_dialog.result), static_cast<int>(emuenv.common_dialog.msg.status));
        emuenv.common_dialog.status = SCE_COMMON_DIALOG_STATUS_NONE;
        emuenv.common_dialog.type = NO_DIALOG;
    }
    if (emuenv.common_dialog.type != NO_DIALOG)
        return RET_ERROR(SCE_COMMON_DIALOG_ERROR_BUSY);
    if (const SceMsgDialogParam *vp = param.get(emuenv.mem))
        LOG_INFO("Vitox msg dialog: Init mode {} thread {}", static_cast<int>(vp->mode), thread_id);
]=])
vitox_substitute(vita3k/renderer/src/vulkan/surface_cache.cpp m6c_surface_cache.cpp
"${_m6c_sws_inc}" "${_m6c_sws_inc_new}"
[=[    return format != SCE_GXM_COLOR_BASE_FORMAT_U2F10F10F10;
}]=]
[=[    return format != SCE_GXM_COLOR_BASE_FORMAT_U2F10F10F10
        && format != SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4; // Vitox 0.1.1.3: stored as RGBA8 (gxm_to_vulkan), no 16-bit write-back
}]=]
[=[    image.init_image(surface_usages, vkutil::default_comp_mapping, image_create_flags, image_info_pNext);
]=]
[=[    // Vitox 0.1.1.2 (Killzone Mercenary render-thread abort in init_image): a failed colour-surface creation threw out of
    // the render thread. Log the shape and the Vulkan result, then retry with only the usages a surface needs.
    try {
        image.init_image(surface_usages, vkutil::default_comp_mapping, image_create_flags, image_info_pNext);
    } catch (const std::exception &vitox_e) {
        static uint32_t vitox_fail_logs = 0;
        if (vitox_fail_logs++ < 32)
            LOG_ERROR("Vitox surface: colour image {}x{} (guest {}x{} stride {} x{}) {} usage {} flags {} FAILED: {} | cached colour surfaces {}",
                width, height, original_width, original_height, color->strideInPixels, state.res_multiplier, vk::to_string(vk_format),
                vk::to_string(surface_usages), vk::to_string(image_create_flags), vitox_e.what(), color_address_lookup.size());
        const vk::ImageUsageFlags vitox_min = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
        image.init_image(vitox_min, vkutil::default_comp_mapping, image_create_flags, image_info_pNext);
        if (vitox_fail_logs <= 32)
            LOG_ERROR("Vitox surface: retry with usage {} OK", vk::to_string(vitox_min));
    }
]=]
[=[ColorSurfaceCacheInfo::~ColorSurfaceCacheInfo() {
    sws_freeContext(sws_context);
}]=]
[=[ColorSurfaceCacheInfo::~ColorSurfaceCacheInfo() {
#ifdef VITOX_SWSCALE_DISABLED
    if (sws_context)
        vitox_swscale_disabled_hit(VITOX_SWS_UNEXPECTED_CONTEXT); // never created when compiled out
#else
    sws_freeContext(sws_context);
#endif
}]=]
[=[        if (!surface->sws_context) {
            const AVPixelFormat dst_fmt = is_swizzle_identity ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_BGR24;]=]
[=[#ifdef VITOX_SWSCALE_DISABLED
        // Vitox M6C: swscale compiled out - the 24-bit surface is NOT written back to guest memory.
        (void)is_swizzle_identity;
        vitox_swscale_disabled_hit(VITOX_SWS_SURFACE_SYNC_RGB24);
        LOG_ERROR("Vitox: {}x{} 24-bit colour surface sync skipped: FFmpeg swscale is not available on WindowsStore", surface->original_width, surface->original_height);
        return;
#else
        if (!surface->sws_context) {
            const AVPixelFormat dst_fmt = is_swizzle_identity ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_BGR24;]=]
[=[        sws_scale(surface->sws_context, reinterpret_cast<const uint8_t *const *>(&surface->copy_buffer->mapped_data), &src_stride, 0, surface->original_height, &pixels, &dst_stride);
        return;]=]
[=[        sws_scale(surface->sws_context, reinterpret_cast<const uint8_t *const *>(&surface->copy_buffer->mapped_data), &src_stride, 0, surface->original_height, &pixels, &dst_stride);
        return;
#endif]=]
[=[vk::ImageView VKSurfaceCache::sourcing_color_surface_for_presentation(Ptr<const void> address, uint32_t pitch, Viewport &viewport) {
]=]
[=[#ifdef VITOX_DIAG_M7B
// Vitox M7B.5 DIAGNOSTIC (read-only, VITOX_UWP): checkpoint A - every cached colour surface, its key/size/format and
// three pixels read back with vkCmdCopyImageToBuffer (same method and General-layout assumption as dump_frame below).
// Called at sample presentation lookups only; nothing here changes the cache, the surfaces or the lookup result.
static bool vitox_m7b5_sample(uint64_t n) { return n <= 3 || n == 60 || n == 600 || n == 2000; }
static void vitox_m7b5_dump_surfaces(VKState &state, const std::map<Address, ColorSurfaceCacheInfo *> &lookup, Address addr, uint32_t pitch,
    const Viewport &vp, uint64_t n) {
    LOG_INFO("Vitox [M7B.5] A lookup #{}: requested fb 0x{:08X} pitch {} viewport {}x{} | res_multiplier {} | colour surfaces cached {}", n, addr, pitch,
        vp.width, vp.height, state.res_multiplier, lookup.size());
    // the same decision sourcing_color_surface_for_presentation takes, with its reason
    const char *predicted = "HIT";
    auto ite = lookup.upper_bound(addr);
    if (ite == lookup.begin())
        predicted = "MISS: no cached colour surface at or below the address";
    else {
        --ite;
        const ColorSurfaceCacheInfo &info = *ite->second;
        if (info.data.address() + info.total_bytes <= addr)
            predicted = "MISS: closest surface below does not overlap the address";
        else if (info.stride_bytes != pitch * 4)
            predicted = "MISS: surface stride_bytes != pitch*4";
        else if (((addr - ite->first) % (pitch * 4)) != 0)
            predicted = "MISS: address not on a surface line boundary";
    }
    LOG_INFO("Vitox [M7B.5] A lookup #{}: predicted presentation lookup -> {}", n, predicted);
    unsigned k = 0;
    for (const auto &[key, info] : lookup) {
        if (k++ >= 6)
            break;
        const uint32_t w = info->width, h = info->height;
        LOG_INFO("Vitox [M7B.5] A surface[{}]: key 0x{:08X} data 0x{:08X} total_bytes {} | {}x{} (original {}x{}) stride_bytes {} | gxm format 0x{:X} vk format {} "
                 "| VkImage {} layout {} | tiling {} | last_frame_rendered {} | need_surface_sync {}",
            k - 1, key, info->data.address(), info->total_bytes, w, h, info->original_width, info->original_height, info->stride_bytes,
            static_cast<uint32_t>(info->format), static_cast<int>(info->texture.format), static_cast<void *>(static_cast<VkImage>(info->texture.image)),
            static_cast<int>(info->texture.layout), static_cast<int>(info->tiling), info->last_frame_rendered,
            info->need_surface_sync ? (*info->need_surface_sync ? 1 : 0) : -1);
        if (!info->texture.image || w < 8 || h < 8)
            continue;
        const int32_t px[3][2] = { { static_cast<int32_t>(w / 2), static_cast<int32_t>(h / 2) }, { 4, 4 }, { static_cast<int32_t>(w / 2), static_cast<int32_t>(h / 4) } };
        vkutil::Buffer temp_buff(3 * 4);
        temp_buff.init_buffer(vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc);
        vk::CommandBuffer cmd_buffer = vkutil::create_single_time_command(state.device, state.general_command_pool);
        vk::BufferImageCopy regions[3];
        for (int i = 0; i < 3; i++)
            regions[i] = vk::BufferImageCopy{ .bufferOffset = static_cast<vk::DeviceSize>(i * 4), .bufferRowLength = 0, .bufferImageHeight = 0,
                .imageSubresource = vkutil::color_subresource_layer, .imageOffset = { px[i][0], px[i][1], 0 }, .imageExtent = { 1, 1, 1 } };
        cmd_buffer.copyImageToBuffer(info->texture.image, vk::ImageLayout::eGeneral, temp_buff.buffer, 3, regions);
        vkutil::end_single_time_command(state.device, state.general_queue, state.general_command_pool, cmd_buffer); // waitIdle, as dump_frame
        uint32_t v[3];
        memcpy(v, temp_buff.mapped_data, sizeof(v));
        LOG_INFO("Vitox [M7B.5] A surface[{}] readback: centre({},{})=0x{:08X} corner({},{})=0x{:08X} upper({},{})=0x{:08X}", k - 1, px[0][0], px[0][1], v[0],
            px[1][0], px[1][1], v[1], px[2][0], px[2][1], v[2]);
    }
}
static uint64_t vitox_m7b5_lookups = 0;
#endif
vk::ImageView VKSurfaceCache::sourcing_color_surface_for_presentation(Ptr<const void> address, uint32_t pitch, Viewport &viewport) {
#ifdef VITOX_DIAG_M7B
    if (vitox_m7b5_sample(++vitox_m7b5_lookups))
        vitox_m7b5_dump_surfaces(state, color_address_lookup, address.address(), pitch, viewport, vitox_m7b5_lookups);
#endif
]=]
# M9A.10 (0.0.24.134): stencil-as-texture through an R8_UNORM copy (see the comment in the inserted code)
[=[    const bool is_stencil = can_be_stencil;

    const uint32_t delta_samples = (address - surface_address) / bytes_per_sample;
    uint32_t delta_col_samples = delta_samples % stride_samples;
    uint32_t delta_row_samples = delta_samples / stride_samples;
]=]
[=[    const bool is_stencil = can_be_stencil;

    const uint32_t delta_samples = (address - surface_address) / bytes_per_sample;
    uint32_t delta_col_samples = delta_samples % stride_samples;
    uint32_t delta_row_samples = delta_samples / stride_samples;
#ifdef VITOX_UWP
    if (is_stencil) { // 0.0.24.134 R8 stencil copy (off .134-.183); 0.0.24.184 ON (kept in the 0.0.24.196 character fix build)
        // Vitox M9A.10 (0.0.24.134): a GXM U8 texture over a stencil surface means stencil/255. The upstream stencil view is
        // an integer (S8) view read by a float sampler: undefined on D3D12 (dzn: X32_TYPELESS_G8X24_UINT). Gravity Rush's
        // tone-map pass branches on it (u_txMainStencil). Copy the stencil plane into an R8_UNORM image once per scene instead.
        struct VitoxStencilR8 {
            std::unique_ptr<vkutil::Image> img;
            std::unique_ptr<vkutil::Buffer> buf;
            uint32_t w = 0, h = 0;
            uint64_t scene = ~0ull;
        };
        static std::map<const DepthStencilSurfaceCacheInfo *, VitoxStencilR8> vitox_r8;
        VitoxStencilR8 &r8 = vitox_r8[found_info];
        const uint32_t sw = std::min<uint32_t>(width, cached_info.texture.width - delta_col_samples * state.res_multiplier);
        const uint32_t sh = std::min<uint32_t>(height, cached_info.texture.height - delta_row_samples * state.res_multiplier);
        if (!r8.img || r8.w != sw || r8.h != sh) {
            r8.img = std::make_unique<vkutil::Image>(sw, sh, vk::Format::eR8Unorm);
            r8.img->init_image(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);
            r8.buf = std::make_unique<vkutil::Buffer>(static_cast<vk::DeviceSize>(sw) * sh);
            r8.buf->init_buffer(vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc);
            r8.w = sw;
            r8.h = sh;
            r8.scene = ~0ull;
            static unsigned vitox_r8_logged = 0;
            if (vitox_r8_logged++ < 16) {
                fprintf(stderr, "[vitox-stencil] R8 copy of stencil surface 0x%08x for texture 0x%08x %ux%u\n", surface_address, address, sw, sh);
                fflush(stderr);
            }
        }
        const uint64_t vitox_scene_ts = reinterpret_cast<VKContext *>(state.context)->scene_timestamp;
        if (r8.scene != vitox_scene_ts) {
            r8.scene = vitox_scene_ts;
            vk::CommandBuffer cmd = reinterpret_cast<VKContext *>(state.context)->prerender_cmd;
            const vkutil::ImageLayout vitox_old_layout = cached_info.texture.layout;
            cached_info.texture.transition_to(cmd, vkutil::ImageLayout::TransferSrc, vkutil::ds_subresource_range);
            vk::ImageSubresourceLayers st = vkutil::color_subresource_layer;
            st.aspectMask = vk::ImageAspectFlagBits::eStencil;
            const vk::BufferImageCopy to_buf{ .bufferOffset = 0, .bufferRowLength = sw, .bufferImageHeight = sh, .imageSubresource = st,
                .imageOffset = { static_cast<int32_t>(delta_col_samples * state.res_multiplier), static_cast<int32_t>(delta_row_samples * state.res_multiplier), 0 },
                .imageExtent = { sw, sh, 1 } };
            cmd.copyImageToBuffer(cached_info.texture.image, vk::ImageLayout::eTransferSrcOptimal, r8.buf->buffer, to_buf);
            if (vitox_old_layout != vkutil::ImageLayout::Undefined)
                cached_info.texture.transition_to(cmd, vitox_old_layout, vkutil::ds_subresource_range);
            const vk::MemoryBarrier mb{ .srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead };
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, mb, {}, {});
            r8.img->transition_to_discard(cmd, vkutil::ImageLayout::TransferDst);
            const vk::BufferImageCopy to_img{ .bufferOffset = 0, .bufferRowLength = sw, .bufferImageHeight = sh, .imageSubresource = vkutil::color_subresource_layer,
                .imageOffset = { 0, 0, 0 }, .imageExtent = { sw, sh, 1 } };
            cmd.copyBufferToImage(r8.buf->buffer, r8.img->image, vk::ImageLayout::eTransferDstOptimal, to_img);
            r8.img->transition_to(cmd, vkutil::ImageLayout::SampledImage);
        }
        if (state.features.use_texture_viewport) {
            texture_viewport->offset = { 0.0f, 0.0f };
            texture_viewport->ratio = { 1.0f, 1.0f };
        }
        return TextureLookupResult{ r8.img->view, vkutil::ImageLayout::SampledImage, vk::Format::eR8Unorm };
    }
#endif
]=])
vitox_substitute(vita3k/renderer/src/transfer.cpp m6c_transfer.cpp
"${_m6c_sws_inc}" "${_m6c_sws_inc_new}"
[=[            // use ffmpeg with the avg filter
            SwsContext *ctx = sws_getContext(src->width, src->height, pixel_fmt, dst->width, dst->height, pixel_fmt, SWS_AREA, nullptr, nullptr, nullptr);
            if (ctx == nullptr) {
                LOG_ERROR("Failed to get ffmpeg context for format 0x{:0X}", fmt::underlying(src->format));
            } else {
                sws_scale(ctx, &src_ptr, &src->stride, 0, src->height, &dst_ptr, &dst->stride);
                sws_freeContext(ctx);
            }
]=]
[=[#ifdef VITOX_SWSCALE_DISABLED
            // Vitox M6C: swscale compiled out - same outcome as upstream's "no ffmpeg context" path: logged, NOT scaled.
            vitox_swscale_disabled_hit(VITOX_SWS_TRANSFER_DOWNSCALE);
            LOG_ERROR("Vitox: transfer downscale for format 0x{:0X} skipped: FFmpeg swscale is not available on WindowsStore", fmt::underlying(src->format));
#else
            // use ffmpeg with the avg filter
            SwsContext *ctx = sws_getContext(src->width, src->height, pixel_fmt, dst->width, dst->height, pixel_fmt, SWS_AREA, nullptr, nullptr, nullptr);
            if (ctx == nullptr) {
                LOG_ERROR("Failed to get ffmpeg context for format 0x{:0X}", fmt::underlying(src->format));
            } else {
                // Vitox 0.1.1.11 compat (Killzone Mercenary crash, access violation in ff_sws_slice_worker): swscale's SIMD
                // paths read (and may write) past the end of the last row, and a guest image can end exactly at an unmapped
                // page. Scale between padded host copies; only the image rows themselves touch guest memory.
                const size_t vitox_bpp = gxm::get_bits_per_pixel(src->format) / 8;
                const size_t vitox_ss = static_cast<size_t>(src->stride), vitox_ds = static_cast<size_t>(dst->stride);
                const size_t vitox_srow = std::min<size_t>(vitox_ss, static_cast<size_t>(src->width) * vitox_bpp);
                const size_t vitox_drow = std::min<size_t>(vitox_ds, static_cast<size_t>(dst->width) * vitox_bpp);
                std::vector<uint8_t> vitox_sbuf(vitox_ss * src->height + 256), vitox_dbuf(vitox_ds * dst->height + 256);
                for (size_t y = 0; y < src->height; y++)
                    memcpy(vitox_sbuf.data() + y * vitox_ss, src_ptr + y * vitox_ss, vitox_srow);
                uint8_t *vitox_sp = vitox_sbuf.data();
                uint8_t *vitox_dp = vitox_dbuf.data();
                int vitox_sstride = src->stride, vitox_dstride = dst->stride;
                sws_scale(ctx, &vitox_sp, &vitox_sstride, 0, src->height, &vitox_dp, &vitox_dstride);
                for (size_t y = 0; y < dst->height; y++)
                    memcpy(dst_ptr + y * vitox_ds, vitox_dbuf.data() + y * vitox_ds, vitox_drow);
                sws_freeContext(ctx);
            }
#endif
]=])
# M9A.10 EXPERIMENT (0.0.24.114): the per-draw stream/uniform ring buffers (memory mapping off) wrap to offset 0 with no
# GPU synchronisation; Gravity Rush's huge vertex shaders reserve their whole uniform storage per draw. The rings go back
# to upstream's commented sizes (uniform 256 MiB, vertex 128 MiB); scene.cpp logs each wrap ([vitox-ring]).
vitox_substitute(vita3k/renderer/src/vulkan/creation.cpp m9a10_vk_creation.cpp
[=[#include <xxh3.h>]=]
[=[#include <xxh3.h>
#include <algorithm> // Vitox M9A.10 (0.0.24.162)
#include <cstdio>]=]
# M9A.10 FIX ATTEMPT (0.0.24.162): .161 showed env draws reading garbage/unwritten uniform data (negative colours late in the
# run) => dynamic storage-buffer offsets not honoured exactly on dzn. Uniform rings are aligned to 256 bytes (D3D12's strictest
# buffer-view placement) instead of the reported minimum; the reported limits are logged once.
[=[    vertex_uniform_stream_ring_buffer.alignment = storage_alignment;
    fragment_uniform_stream_ring_buffer.alignment = storage_alignment;
    vertex_info_uniform_buffer.alignment = uniform_alignment;
    fragment_info_uniform_buffer.alignment = uniform_alignment;
]=]
[=[    vertex_uniform_stream_ring_buffer.alignment = std::max<uint32_t>(storage_alignment, 256u); // Vitox M9A.10 (0.0.24.162)
    fragment_uniform_stream_ring_buffer.alignment = std::max<uint32_t>(storage_alignment, 256u);
    vertex_info_uniform_buffer.alignment = std::max<uint32_t>(uniform_alignment, 256u);
    fragment_info_uniform_buffer.alignment = std::max<uint32_t>(uniform_alignment, 256u);
    fprintf(stderr, "[vitox-align] device minStorageBufferOffsetAlignment=%u minUniformBufferOffsetAlignment=%u -> rings use 256%c",
        storage_alignment, uniform_alignment, 10);
]=]
[=[    , vertex_stream_ring_buffer(vk::BufferUsageFlagBits::eVertexBuffer, MiB(/*128*/ 64))]=]
[=[    , vertex_stream_ring_buffer(vk::BufferUsageFlagBits::eVertexBuffer, MiB(128)) // Vitox M9A.10: was 64]=]
[=[    , vertex_uniform_stream_ring_buffer(vk::BufferUsageFlagBits::eStorageBuffer, MiB(/*256*/ 64))
    , fragment_uniform_stream_ring_buffer(vk::BufferUsageFlagBits::eStorageBuffer, MiB(/*256*/ 64))]=]
[=[    , vertex_uniform_stream_ring_buffer(vk::BufferUsageFlagBits::eStorageBuffer, MiB(256)) // Vitox M9A.10: was 64
    , fragment_uniform_stream_ring_buffer(vk::BufferUsageFlagBits::eStorageBuffer, MiB(256)) // Vitox M9A.10: was 64]=])
set(VITOX_M6B_RENDERER_SOURCES
    vulkan/allocator.cpp # vulkan/creation.cpp: patched m9a10_vk_creation.cpp; vulkan/gxm_to_vulkan.cpp: patched m9a6_gxm_to_vulkan.cpp
    vulkan/overlay_renderer.cpp vulkan/screen_filters.cpp
    vulkan/sync_state.cpp # vulkan/texture.cpp: patched copy m9a6_vk_texture.cpp (0.0.24.51 diagnostic)
    texture/format.cpp # texture/cache.cpp: patched m9a6_texture_cache.cpp
    texture/palette.cpp texture/pvrt-dec.cpp texture/replacement.cpp
    batch.cpp shaders.cpp)
list(TRANSFORM VITOX_M6B_RENDERER_SOURCES PREPEND "${VITA3K}/vita3k/renderer/src/" OUTPUT_VARIABLE _m6b_rend_abs)
add_library(vitox_m6b_renderer STATIC EXCLUDE_FROM_ALL ${_m6b_rend_abs}
    "${CMAKE_BINARY_DIR}/vitox-patched/vkonly_creation.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/vkonly_renderer.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/vkonly_scene.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/vkonly_state_set.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/vkonly_sync.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/m6c_yuv.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/m6c_surface_cache.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/m6c_transfer.cpp" "${VITOX_M3}/compat/vitox_swscale_disabled.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/m6c_screen_renderer.cpp" "${VITOX_M3}/compat/vitox_external_present_vk.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/vk_renderer.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/pipeline_cache.cpp"
    "${VITA3K}/vita3k/vkutil/src/objects.cpp" "${VITA3K}/vita3k/vkutil/src/vkutil.cpp"
    "${VITOX_M3}/compat/vitox_vk_entry.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/m9a6_vk_texture.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/m9a6_gxm_to_vulkan.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/m9a6_texture_cache.cpp"
    # M7B.6 DIAGNOSTIC (logging only): vulkan/context.cpp and vulkan/scene.cpp via read-only substitutions
    "${CMAKE_BINARY_DIR}/vitox-patched/m7b6_context.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/m7b6_vk_scene.cpp"
    "${VITOX_M3}/compat/vitox_m7b6_diag.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/m9a10_vk_creation.cpp")
vitox_m5a_flags(vitox_m6b_renderer)
target_link_libraries(vitox_m6b_renderer PRIVATE spirv-cross-glsl) # M7B.6: converted shaders logged as GLSL
target_include_directories(vitox_m6b_renderer PRIVATE "${VITOX_M3}/compat" "${VITA3K}/external/VulkanMemoryAllocator-Hpp/include"
    "${VITA3K}/external/ddspp" "${VITA3K}/external/concurrentqueue")
target_compile_definitions(vitox_m6b_renderer PRIVATE VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0)
# GL backend (vita3k/renderer/src/gl/*) is not built: OpenGL is not available on Xbox UWP.
# Link-closure probe (never packaged): what renderer::init(..., Backend::Vulkan, ...) still needs.
file(WRITE "${CMAKE_BINARY_DIR}/vitox-m6b/inv_renderer_init.cpp.tmp"
"// Generated (Vitox M6B inventory): link closure of renderer::init with the Vulkan backend.\n\
#include <renderer/functions.h>\n#include <renderer/state.h>\n#include <config/state.h>\n#include <util/fs.h>\n\
extern \"C\" __declspec(dllexport) bool vitox_m6b_renderer_init(renderer::FrameHost &frame, Config &cfg, const Root &roots) {\n\
    std::unique_ptr<renderer::State> state;\n    return renderer::init(frame, state, renderer::Backend::Vulkan, cfg, roots);\n}\n")
file(COPY_FILE "${CMAKE_BINARY_DIR}/vitox-m6b/inv_renderer_init.cpp.tmp" "${CMAKE_BINARY_DIR}/vitox-m6b/inv_renderer_init.cpp" ONLY_IF_DIFFERENT)
add_library(vitox_m6b_renderer_init SHARED EXCLUDE_FROM_ALL "${CMAKE_BINARY_DIR}/vitox-m6b/inv_renderer_init.cpp")
vitox_m5a_flags(vitox_m6b_renderer_init)
target_link_libraries(vitox_m6b_renderer_init PRIVATE vitox_m6b_renderer vitox_hle)
# Narrower: Vita3K's own Vulkan backend create() only (instance/device creation path, no GL switch).
file(WRITE "${CMAKE_BINARY_DIR}/vitox-m6b/inv_vk_create.cpp.tmp"
"// Generated (Vitox M6B inventory): link closure of renderer::vulkan::create.\n\
#include <renderer/vulkan/functions.h>\n#include <renderer/state.h>\n#include <config/state.h>\n\
extern \"C\" __declspec(dllexport) bool vitox_m6b_vk_create(std::unique_ptr<renderer::State> &state, const Config &cfg) {\n\
    return renderer::vulkan::create(state, cfg);\n}\n")
file(COPY_FILE "${CMAKE_BINARY_DIR}/vitox-m6b/inv_vk_create.cpp.tmp" "${CMAKE_BINARY_DIR}/vitox-m6b/inv_vk_create.cpp" ONLY_IF_DIFFERENT)
add_library(vitox_m6b_vk_create SHARED EXCLUDE_FROM_ALL "${CMAKE_BINARY_DIR}/vitox-m6b/inv_vk_create.cpp")
vitox_m5a_flags(vitox_m6b_vk_create)
target_link_libraries(vitox_m6b_vk_create PRIVATE vitox_m6b_renderer vitox_hle)

# ---- M6B.2 (BUILD-ONLY): renderer closure reduction with existing Vita3K code, no stubs -------------
# Opt-in per category (so each one's effect on the closure is measurable); the inventory links the probes
# with every enabled category.
option(VITOX_M6B2_GXM "link vita3k/gxm/src (texture/colour/attribute/transfer format helpers)" ON)
option(VITOX_M6B2_VERSION "generate vita3k/config/src/version.cpp.in exactly as vita3k/CMakeLists.txt does" ON)
option(VITOX_M6B2_HASH "util/src/hash.cpp with SHA-256 via BCrypt instead of OpenSSL EVP (guarded substitution)" ON)
option(VITOX_M6B2_OVERLAY "link vita3k/overlay/src minus trophy_notification.cpp (common_dialog.cpp uses the approved SDL_GetTicks shim, M6C)" ON)
set(_m6b2_srcs "")
if (VITOX_M6B2_GXM)
    foreach (s attributes color gxp stream textures transfer)
        list(APPEND _m6b2_srcs "${VITA3K}/vita3k/gxm/src/${s}.cpp")
    endforeach()
endif()
if (VITOX_M6B2_VERSION)
    # same variables and values as vita3k/CMakeLists.txt with no git checkout information (its own fallback)
    file(STRINGS "${VITA3K}/vita3k/CMakeLists.txt" _m6b2_ver REGEX "^set\\(VITA3K_(ORG_NAME|APP_NAME|APP_VER_HI|APP_VER_MID|APP_VER_LO) ")
    foreach (l ${_m6b2_ver})
        string(REGEX REPLACE "^set\\((VITA3K_[A-Z_]+) \"?([^\")]*)\"?\\)$" "\\1;\\2" _kv "${l}")
        list(GET _kv 0 _k)
        list(GET _kv 1 _v)
        set(${_k} "${_v}")
    endforeach()
    if (NOT VITA3K_APP_NAME OR NOT VITA3K_ORG_NAME OR "${VITA3K_APP_VER_LO}" STREQUAL "")
        message(FATAL_ERROR "Vitox M6B.2: could not read VITA3K_* version variables from vita3k/CMakeLists.txt")
    endif()
    set(VITA3K_APP_VERSION "v${VITA3K_APP_VER_HI}.${VITA3K_APP_VER_MID}.${VITA3K_APP_VER_LO}")
    set(VITA3K_GIT_REV "Development version")
    set(GIT_COUNT 0)
    set(GIT_HASH "")
    set(VITA3K_OFFICIAL_UPDATE_BUILD 0)
    configure_file("${VITA3K}/vita3k/config/src/version.cpp.in" "${CMAKE_BINARY_DIR}/vitox-m6b2/version.cpp")
    list(APPEND _m6b2_srcs "${CMAKE_BINARY_DIR}/vitox-m6b2/version.cpp")
endif()
if (VITOX_M6B2_HASH)
    # OpenSSL is not available for WindowsStore; BCrypt SHA-256 gives the same 32 bytes. hex_buf is untouched.
    vitox_substitute(vita3k/util/src/hash.cpp hash.cpp
[=[#include <openssl/evp.h>]=]
[=[#include <windows.h> // Vitox shim (M6B.2): BCrypt SHA-256 instead of OpenSSL EVP
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")]=]
[=[    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, size);
    unsigned int len;
    EVP_DigestFinal(ctx, hash.data(), &len);
    EVP_MD_CTX_free(ctx);]=]
[=[    // Vitox shim (M6B.2): was OpenSSL EVP_sha256 (Init/Update/Final). A BCrypt failure is fatal, not a fake hash.
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    const bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
        && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0
        && BCryptHashData(h, static_cast<PUCHAR>(const_cast<void *>(data)), static_cast<ULONG>(size), 0) == 0
        && BCryptFinishHash(h, hash.data(), static_cast<ULONG>(hash.size()), 0) == 0;
    if (h)
        BCryptDestroyHash(h);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok)
        std::abort();]=])
    list(APPEND _m6b2_srcs "${CMAKE_BINARY_DIR}/vitox-patched/hash.cpp")
endif()
if (VITOX_M6B2_OVERLAY)
    foreach (s animation common_dialog compiled_resource controls element font layouts pause_overlay perf_overlay shader_compile_notice
        shader_precompile_progress)
        list(APPEND _m6b2_srcs "${VITA3K}/vita3k/overlay/src/${s}.cpp")
    endforeach()
    list(APPEND _m6b2_srcs "${VITOX_M3}/sdl-shim/vitox_sdl_timer.cpp") # M6C: SDL_GetTicks only
    # 0.0.32.0 (user request): the trophy-unlocked banner, upstream's overlay; moved from the top-right corner to the
    # top-left (user choice: the Vitox statistics overlay owns the top-right), growing out of its top-left corner
    vitox_substitute(vita3k/overlay/src/trophy_notification.cpp trophy_notification.cpp
[=[    const int16_t frame_x = static_cast<int16_t>(virtual_width - k_frame_w - k_margin);]=]
[=[    const int16_t frame_x = static_cast<int16_t>(k_margin); // Vitox: top-left (the stats overlay is top-right)]=]
[=[    m_scale_anim.pivot_x = static_cast<float>(frame_x + k_frame_w);]=]
[=[    m_scale_anim.pivot_x = static_cast<float>(frame_x); // Vitox: grows out of the top-left corner]=])
    list(APPEND _m6b2_srcs "${CMAKE_BINARY_DIR}/vitox-patched/trophy_notification.cpp")
endif()
option(VITOX_M6B2_LANG "vita3k/lang (lang::get, used by the overlays) with its catalog generated by Vita3K's own script" ON)
option(VITOX_M6B2_SYSINFO "vita3k/util/src/sysinfo.cpp (util::get_process_memory_usage for perf_overlay)" ON)
if (VITOX_M6B2_LANG)
    # identical custom command to vita3k/lang/CMakeLists.txt; the interpreter is the Vitox toolchain venv
    set(VITOX_PYTHON "C:/Vitox/toolchain/py/Scripts/python.exe" CACHE FILEPATH "Python used for Vita3K's build-time generators")
    set(_m6b2_lang_gen "${CMAKE_BINARY_DIR}/vitox-m6b2/lang")
    file(GLOB _m6b2_lang_json CONFIGURE_DEPENDS "${VITA3K}/i18n/lang/overlay_*.json")
    add_custom_command(OUTPUT "${_m6b2_lang_gen}/generated_catalog.h" "${_m6b2_lang_gen}/generated_catalog.cpp"
        COMMAND "${VITOX_PYTHON}" -I "${VITA3K}/tools/i18n/generate_lang_catalog.py" --input-dir "${VITA3K}/i18n/lang"
            --strings-def "${VITA3K}/vita3k/lang/include/lang/strings.def"
            --output-header "${_m6b2_lang_gen}/generated_catalog.h" --output-source "${_m6b2_lang_gen}/generated_catalog.cpp"
        DEPENDS "${VITA3K}/tools/i18n/generate_lang_catalog.py" "${VITA3K}/vita3k/lang/include/lang/strings.def" ${_m6b2_lang_json}
        VERBATIM)
    list(APPEND _m6b2_srcs "${VITA3K}/vita3k/lang/src/lang.cpp" "${_m6b2_lang_gen}/generated_catalog.h" "${_m6b2_lang_gen}/generated_catalog.cpp")
endif()
if (VITOX_M6B2_SYSINFO)
    list(APPEND _m6b2_srcs "${VITA3K}/vita3k/util/src/sysinfo.cpp" "${VITA3K}/vita3k/util/src/instrset_detect.cpp")
endif()
add_library(vitox_m6b2_renderer_deps STATIC EXCLUDE_FROM_ALL ${_m6b2_srcs})
if (VITOX_M6B2_LANG)
    target_include_directories(vitox_m6b2_renderer_deps PRIVATE "${_m6b2_lang_gen}")
endif()
vitox_m5a_flags(vitox_m6b2_renderer_deps)
target_include_directories(vitox_m6b2_renderer_deps PRIVATE "${VITA3K}/external/stb")
option(VITOX_M6B2_SHADER "vita3k/shader (GXP -> SPIR-V recompiler) with vendored glslang SPIRV + SPIRV-Cross, Vita3K's own options" ON)
if (VITOX_M6B2_SHADER)
    # same option set as Vita3K's external/CMakeLists.txt
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${VITA3K}/external/glslang" ext/glslang EXCLUDE_FROM_ALL)
    foreach (o CLI ENABLE_TESTS ENABLE_HLSL ENABLE_MSL ENABLE_CPP ENABLE_REFLECT ENABLE_C_API ENABLE_UTIL)
        set(SPIRV_CROSS_${o} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory("${VITA3K}/external/SPIRV-Cross" ext/SPIRV-Cross EXCLUDE_FROM_ALL)
    # M9A.10 EXPERIMENT (0.0.24.118): memory mapping is always off on Xbox, so a shader can only read the bytes copied
    # per uniform buffer, and that copy is sized from the declared parameters (gxp_parser). Gravity Rush draws its
    # environment dark: every non-default uniform buffer now gets upstream's "unknown size" treatment
    # (SCE_GXM_MAX_UB_IN_FLOAT_UNIT; SceGxm bounds the copy by the next buffer / the mapped region).
    vitox_substitute(vita3k/shader/src/gxp_parser.cpp m9a10_gxp_parser.cpp
[=[        if ((((investigated_ub & (1 << index)) == 0) && seems_symbols_stripped) || (buffer.size == MAXIMUM_GXP_ARRAY_SIZE)) {]=]
[=[#ifdef VITOX_UWP
        if (false || (((investigated_ub & (1 << index)) == 0) && seems_symbols_stripped) || (buffer.size == MAXIMUM_GXP_ARRAY_SIZE)) { // Vitox M9A.10: 0.0.24.118 forced every non-default UB to MAX: flat grey (reverted 0.0.24.119); 0.0.24.123 vertex programs only: also broken (uniform ring wraps were safe, ~1.6 s); REVERTED in 0.0.24.124
#else
        if ((((investigated_ub & (1 << index)) == 0) && seems_symbols_stripped) || (buffer.size == MAXIMUM_GXP_ARRAY_SIZE)) {
#endif]=])
    # M9A.10 FIX (0.0.24.164): with memory mapping off, each uniform block is copied into the SA register bank as
    # ceil(reg_block_size / 4) whole vec4s, writing up to 3 floats past the block's end. Blocks are copied in container
    # order, not register order, so a block can clobber a neighbour copied earlier: Gravity Rush's environment shader
    # (point-light block, 6 floats at SA 18) overwrote u_f3ColorScale (SA 24) with never-uploaded padding => floor/walls
    # black, later stale ring garbage (colour drift). Copy whole vec4s only for complete groups, then the remaining
    # floats one component at a time.
    vitox_substitute(vita3k/shader/src/spirv_recompiler.cpp m9a10_spirv_recompiler.cpp
[=[                const uint32_t reg_block_size_in_f32v = std::min<uint32_t>(buffer.reg_block_size + 3, REG_SA_COUNT) / 4;
                const auto spv_buffer = utils::create_access_chain(b, spv::StorageClassStorageBuffer, spv_params.buffer_container,
                    { b.makeIntConstant(spv_params.buffers.at(host_idx).index_in_container) });
                copy_uniform_block_to_register(b, spv_params.uniforms, spv_buffer, ite_copy, buffer.reg_start_offset, reg_block_size_in_f32v);
]=]
[=[#ifdef VITOX_UWP
                // Vitox M9A.10 (0.0.24.164): exact copy, never past the block's end (upstream copied ceil(size / 4) vec4s)
                const uint32_t vitox_size = std::min<uint32_t>(buffer.reg_block_size, REG_SA_COUNT - buffer.reg_start_offset);
                const uint32_t vitox_full = vitox_size / 4;
                const auto spv_buffer = utils::create_access_chain(b, spv::StorageClassStorageBuffer, spv_params.buffer_container,
                    { b.makeIntConstant(spv_params.buffers.at(host_idx).index_in_container) });
                if (vitox_full > 0)
                    copy_uniform_block_to_register(b, spv_params.uniforms, spv_buffer, ite_copy, buffer.reg_start_offset, vitox_full);
                for (uint32_t j = vitox_full * 4; j < vitox_size; j++) {
                    const uint32_t reg = buffer.reg_start_offset + j;
                    const spv::Id src = utils::create_access_chain(b, spv::StorageClassStorageBuffer, spv_buffer,
                        { b.makeIntConstant(static_cast<int>(j / 4)), b.makeIntConstant(static_cast<int>(j % 4)) });
                    const spv::Id dst = utils::create_access_chain(b, spv::StorageClassPrivate, spv_params.uniforms,
                        { b.makeIntConstant(static_cast<int>(reg / 4)), b.makeIntConstant(static_cast<int>(reg % 4)) });
                    b.createStore(b.createLoad(src, spv::NoPrecision), dst);
                }
#else
                const uint32_t reg_block_size_in_f32v = std::min<uint32_t>(buffer.reg_block_size + 3, REG_SA_COUNT) / 4;
                const auto spv_buffer = utils::create_access_chain(b, spv::StorageClassStorageBuffer, spv_params.buffer_container,
                    { b.makeIntConstant(spv_params.buffers.at(host_idx).index_in_container) });
                copy_uniform_block_to_register(b, spv_params.uniforms, spv_buffer, ite_copy, buffer.reg_start_offset, reg_block_size_in_f32v);
#endif
]=])
    set(_m6b2_shader_rel translator/alu translator/ialu translator/branch_cond translator/data translator/special translator/illegal
        translator/texture translator/utils usse_disasm usse_program_analyzer usse_decode_helpers usse_translator_entry
        usse_utilities) # gxp_parser: patched m9a10_gxp_parser.cpp; spirv_recompiler: patched m9a10_spirv_recompiler.cpp (0.0.24.164)
    list(TRANSFORM _m6b2_shader_rel PREPEND "${VITA3K}/vita3k/shader/src/" OUTPUT_VARIABLE _m6b2_shader)
    list(TRANSFORM _m6b2_shader APPEND ".cpp")
    add_library(vitox_m6b2_shader STATIC EXCLUDE_FROM_ALL ${_m6b2_shader} "${CMAKE_BINARY_DIR}/vitox-patched/m9a10_gxp_parser.cpp"
        "${CMAKE_BINARY_DIR}/vitox-patched/m9a10_spirv_recompiler.cpp")
    vitox_m5a_flags(vitox_m6b2_shader)
    target_include_directories(vitox_m6b2_shader PRIVATE "${VITA3K}/vita3k/shader/include" "${VITA3K}/vita3k/features/include")
    target_link_libraries(vitox_m6b2_shader PRIVATE SPIRV spirv-cross-glsl)
endif()
foreach (t vitox_m6b_renderer_init vitox_m6b_vk_create)
    target_link_libraries(${t} PRIVATE vitox_m6b2_renderer_deps)
    if (VITOX_M6B2_SHADER)
        target_link_libraries(${t} PRIVATE vitox_m6b2_shader)
    endif()
endforeach()
message(STATUS "Vitox M6B.2 renderer deps: gxm=${VITOX_M6B2_GXM} version=${VITOX_M6B2_VERSION} hash=${VITOX_M6B2_HASH} overlay=${VITOX_M6B2_OVERLAY} lang=${VITOX_M6B2_LANG} sysinfo=${VITOX_M6B2_SYSINFO} shader=${VITOX_M6B2_SHADER}")

# ---- M6B part E (BUILD-ONLY / link probe): ngs::init closure with Vita3K's prebuilt FFmpeg ------
# Vita3K's ffmpeg-core e744193 ffmpeg-windows-x64.zip (sha256 d8e53a3f...ec7c), static libs built with
# the STATIC CRT (/DEFAULTLIB:libcmt). The probe links them into a /MD app-CRT DLL with
# /NODEFAULTLIB:libcmt.lib to see what still resolves, and which Win32 imports end up in the image.
set(VITOX_FFMPEG_PREBUILT "C:/Vitox/toolchain/ffmpeg-e744193" CACHE PATH "Vita3K ffmpeg-core prebuilt (inventory only)")
set(_m6b_ngs ${VITA3K}/vita3k/ngs/src/modules/atrac9.cpp ${VITA3K}/vita3k/ngs/src/modules/compressor.cpp ${VITA3K}/vita3k/ngs/src/modules/delay.cpp
    ${VITA3K}/vita3k/ngs/src/modules/distortion.cpp ${VITA3K}/vita3k/ngs/src/modules/envelope.cpp ${VITA3K}/vita3k/ngs/src/modules/equalizer.cpp
    ${VITA3K}/vita3k/ngs/src/modules/filter.cpp ${VITA3K}/vita3k/ngs/src/modules/generator.cpp ${VITA3K}/vita3k/ngs/src/modules/mixer.cpp
    ${VITA3K}/vita3k/ngs/src/modules/output.cpp ${VITA3K}/vita3k/ngs/src/modules/pauser.cpp ${VITA3K}/vita3k/ngs/src/modules/pitchshift.cpp
    ${VITA3K}/vita3k/ngs/src/modules/player.cpp ${VITA3K}/vita3k/ngs/src/modules/reverb.cpp ${VITA3K}/vita3k/ngs/src/definitions.cpp
    ${VITA3K}/vita3k/ngs/src/ngs.cpp ${VITA3K}/vita3k/ngs/src/rate_resampler.cpp ${VITA3K}/vita3k/ngs/src/route.cpp ${VITA3K}/vita3k/ngs/src/scheduler.cpp)
set(_m6b_codec ${VITA3K}/vita3k/codec/src/atrac9.cpp ${VITA3K}/vita3k/codec/src/decoder.cpp ${VITA3K}/vita3k/codec/src/aac.cpp
    ${VITA3K}/vita3k/codec/src/h264.cpp ${VITA3K}/vita3k/codec/src/mjpeg.cpp ${VITA3K}/vita3k/codec/src/mp3.cpp ${VITA3K}/vita3k/codec/src/pcm.cpp
    ${VITA3K}/vita3k/codec/src/player.cpp)
file(GLOB _m6b_atrac9 "${VITA3K}/external/LibAtrac9/C/src/*.c")
file(WRITE "${CMAKE_BINARY_DIR}/vitox-m6b/inv_ngs_init.cpp.tmp"
"// Generated (Vitox M6B inventory): link closure of ngs::init (no audio backend).\n\
#include <ngs/state.h>\n#include <mem/state.h>\n\
extern \"C\" __declspec(dllexport) bool vitox_m6b_ngs_init(ngs::State &ngs, MemState &mem) { return ngs::init(ngs, mem); }\n")
file(COPY_FILE "${CMAKE_BINARY_DIR}/vitox-m6b/inv_ngs_init.cpp.tmp" "${CMAKE_BINARY_DIR}/vitox-m6b/inv_ngs_init.cpp" ONLY_IF_DIFFERENT)
add_library(vitox_m6b_ngs_init SHARED EXCLUDE_FROM_ALL "${CMAKE_BINARY_DIR}/vitox-m6b/inv_ngs_init.cpp" ${_m6b_ngs} ${_m6b_codec} ${_m6b_atrac9})
vitox_m5a_flags(vitox_m6b_ngs_init)
target_include_directories(vitox_m6b_ngs_init PRIVATE "${VITA3K}/vita3k/ngs/include" "${VITA3K}/vita3k/codec/include" "${VITA3K}/external/LibAtrac9/C/src")
target_link_libraries(vitox_m6b_ngs_init PRIVATE vitox_hle
    "${VITOX_FFMPEG_PREBUILT}/avformat.lib" "${VITOX_FFMPEG_PREBUILT}/avcodec.lib" "${VITOX_FFMPEG_PREBUILT}/swscale.lib"
    "${VITOX_FFMPEG_PREBUILT}/swresample.lib" "${VITOX_FFMPEG_PREBUILT}/avutil.lib" bcrypt)
target_link_options(vitox_m6b_ngs_init PRIVATE "/NODEFAULTLIB:libcmt.lib" "/NODEFAULTLIB:oldnames.lib")

# Record exactly what was inventoried.
execute_process(COMMAND git -C "${VITA3K}" rev-parse HEAD OUTPUT_VARIABLE _v3k OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
message(STATUS "Vitox M3 inventory: Vita3K ${_v3k}")

# ---- M6C (0.0.13.0): renderer-only Xbox proof - the probe TU linked into the app with the renderer libraries ----
add_library(vitox_m6c_probe STATIC EXCLUDE_FROM_ALL "${VITOX_M3}/core-probe/vitox_renderer_probe.cpp")
vitox_m5a_flags(vitox_m6c_probe)
target_include_directories(vitox_m6c_probe PRIVATE "${VITOX_M3}/compat" "${VITOX_M3}/core-probe")
target_link_libraries(vitox_m6c_probe PRIVATE vitox_m6b_renderer vitox_m6b2_renderer_deps vitox_m6b2_shader vitox_hle)

# ---- M7A (0.0.14.x): first REAL guest SceGxm visual homebrew (VITOX0002) ----------------------------
# The real SceGxm / SceDisplay module TUs (the 8 NIDs VITOX0002 imports are bound through vitox_import_dispatch),
# Vita3K's display.cpp (vblank thread, wait_vblank; its two input-runtime calls compiled out, see below),
# float_to_half.cpp, and xxHash (SceGxm.cpp uses the x86 dispatcher). Nothing else is substituted.
# M7A display (user-approved): only the vblank thread's two input-runtime calls are compiled out and counted
vitox_substitute(vita3k/display/src/display.cpp m7a_display.cpp
[=[#include <touch/functions.h>
]=]
[=[#include <touch/functions.h>
#ifdef VITOX_UWP
#include <vitox_vblank_input_disabled.h> // Vitox shim (M7A)
#endif
]=]
[=[            touch_vsync_update(emuenv);
            refresh_motion(emuenv.motion, emuenv.ctrl);]=]
[=[#ifdef VITOX_UWP // Vitox M7A: input runtime (SDL sensor) not part of this milestone - skipped, counted
            // 0.0.24.55 (M9A.6): upstream's touch_vsync_update runs again - the virtual touch cursor feeds its
            // mouse-as-touch fields (it only reads TouchState; no SDL calls)
            touch_vsync_update(emuenv);
            vitox_vblank_input_skipped(VITOX_VBLANK_MOTION);
#else
            touch_vsync_update(emuenv);
            refresh_motion(emuenv.motion, emuenv.ctrl);
#endif]=])
# M9A.10 (0.0.24.111): after a precomputed draw, sceGxmDraw sent its uniform buffers BEFORE switching the render
# context back to the context's programs, so (memory mapping off) the uniform_buffer commands were laid out with the
# precomputed program's offsets/sizes and the draw read garbage uniforms (Gravity Rush: flickering stretched geometry).
# The program re-set now comes first; with memory mapping on the order does not matter, so upstream behaviour is kept.
vitox_substitute(vita3k/modules/SceGxm/SceGxm.cpp m9a10_SceGxm.cpp
[=[    gxmSetUniformBuffers(*emuenv.renderer, emuenv.gxm, context, vertex_program_gxp, context->state.vertex_uniform_buffers, gxm_vertex_program.renderer_data->uniform_buffer_sizes,
        emuenv.mem);
    gxmSetUniformBuffers(*emuenv.renderer, emuenv.gxm, context, fragment_program_gxp, context->state.fragment_uniform_buffers, gxm_fragment_program.renderer_data->uniform_buffer_sizes,
        emuenv.mem);

    if (context->last_precomputed) {
        // Need to re-set the data

        renderer::set_program(*emuenv.renderer, context->renderer.get(), context->state.vertex_program, false);
        renderer::set_program(*emuenv.renderer, context->renderer.get(), context->state.fragment_program, true);

        context->last_precomputed = false;
    }
]=]
[=[    // Vitox M9A.10: re-set the programs BEFORE the uniform buffers (the render thread lays them out per program)
    if (context->last_precomputed) {
        // Need to re-set the data

        renderer::set_program(*emuenv.renderer, context->renderer.get(), context->state.vertex_program, false);
        renderer::set_program(*emuenv.renderer, context->renderer.get(), context->state.fragment_program, true);

        context->last_precomputed = false;
    }

    gxmSetUniformBuffers(*emuenv.renderer, emuenv.gxm, context, vertex_program_gxp, context->state.vertex_uniform_buffers, gxm_vertex_program.renderer_data->uniform_buffer_sizes,
        emuenv.mem);
    gxmSetUniformBuffers(*emuenv.renderer, emuenv.gxm, context, fragment_program_gxp, context->state.fragment_uniform_buffers, gxm_fragment_program.renderer_data->uniform_buffer_sizes,
        emuenv.mem);
]=]
)
add_library(vitox_m7a_gxm STATIC EXCLUDE_FROM_ALL
    "${CMAKE_BINARY_DIR}/vitox-patched/m9a10_SceGxm.cpp" "${VITA3K}/vita3k/modules/SceDisplay/SceDisplay.cpp"
    "${VITA3K}/vita3k/modules/SceDriverUser/SceDisplayUser.cpp" "${VITA3K}/vita3k/util/src/float_to_half.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/m7a_display.cpp" "${VITOX_M3}/compat/vitox_vblank_input_disabled.cpp"
    "${VITA3K}/external/xxHash/xxhash.c" "${VITA3K}/external/xxHash/xxh_x86dispatch.c")
vitox_m5a_flags(vitox_m7a_gxm)
target_include_directories(vitox_m7a_gxm PRIVATE "${VITOX_M3}/compat" "${VITA3K}/vita3k/modules" "${VITA3K}/vita3k/modules/SceGxm" "${VITA3K}/external/xxHash")
set_source_files_properties("${VITA3K}/external/xxHash/xxhash.c" "${VITA3K}/external/xxHash/xxh_x86dispatch.c"
    PROPERTIES COMPILE_OPTIONS "/FI${VITOX_M3}/compat/vitox_compat.h")
# Link-closure inventory (never packaged): the 8 selected exports + the real renderer stack
file(WRITE "${CMAKE_BINARY_DIR}/vitox-m7a/inv_gxm.cpp.tmp"
"// Generated (Vitox M7A inventory): link closure of the VITOX0002 SceGxm/SceDisplay exports.\n\
#include <module/bridge.h>\n\
extern const ImportFn import_sceGxmInitialize, import_sceGxmMapMemory, import_sceGxmTerminate, import_sceGxmTransferFill,\n\
    import_sceGxmTransferFinish, import_sceGxmUnmapMemory, import_sceDisplaySetFrameBuf, import_sceDisplayWaitVblankStart;\n\
extern \"C\" __declspec(dllexport) const ImportFn *vitox_m7a_exports[] = { &import_sceGxmInitialize, &import_sceGxmMapMemory,\n\
    &import_sceGxmTerminate, &import_sceGxmTransferFill, &import_sceGxmTransferFinish, &import_sceGxmUnmapMemory,\n\
    &import_sceDisplaySetFrameBuf, &import_sceDisplayWaitVblankStart };\n")
file(COPY_FILE "${CMAKE_BINARY_DIR}/vitox-m7a/inv_gxm.cpp.tmp" "${CMAKE_BINARY_DIR}/vitox-m7a/inv_gxm.cpp" ONLY_IF_DIFFERENT)
add_library(vitox_m7a_inv SHARED EXCLUDE_FROM_ALL "${CMAKE_BINARY_DIR}/vitox-m7a/inv_gxm.cpp")
vitox_m5a_flags(vitox_m7a_inv)
target_link_libraries(vitox_m7a_inv PRIVATE vitox_m7a_gxm vitox_m6b_renderer vitox_m6b2_renderer_deps vitox_m6b2_shader vitox_hle
    vitox_emuenv vitox_core)
# M7A probe TU (linked into the app with vitox_m7a_gxm + the renderer libraries)
add_library(vitox_m7a_probe STATIC EXCLUDE_FROM_ALL "${VITOX_M3}/core-probe/vitox_gxm_homebrew_probe.cpp")
vitox_m5a_flags(vitox_m7a_probe)
target_include_directories(vitox_m7a_probe PRIVATE "${VITOX_M3}/compat" "${VITOX_M3}/core-probe")
# M7B.1 differential control: the M6B DXC SPIR-V header only (Vitox-M2/src/vitox_m6b_shaders.h)
set_property(SOURCE "${VITOX_M3}/core-probe/vitox_gxm_homebrew_probe.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES "C:/Vitox/Vitox-M2/src")
target_link_libraries(vitox_m7a_probe PRIVATE vitox_m7a_gxm vitox_m6b_renderer vitox_m6b2_renderer_deps vitox_m6b2_shader vitox_hle)

# ---- M8A: user-selectable homebrew launcher (generic; core-probe/vitox_homebrew_launcher.cpp) ----
# Inventory of every EXPORT(ret, name, ...) in vita3k/modules, used ONLY to classify a homebrew's imports that are not in
# the selected dispatcher (implemented in Vita3K / UNIMPLEMENTED stub / absent). It links nothing and calls nothing.
file(GLOB_RECURSE _m8a_mod_srcs "${VITA3K}/vita3k/modules/*.cpp")
set(_m8a_inc "// Generated by Vitox M8A (core-build/CMakeLists.txt) from vita3k/modules/**/*.cpp: VITOX_EXPORT(name, module dir, stub)\n")
set(_m8a_n 0)
set(_m8a_nstub 0)
foreach (_f ${_m8a_mod_srcs})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_f}")
    file(READ "${_f}" _t)
    string(REPLACE ";" " " _t "${_t}")       # list separator
    string(REPLACE "[" " " _t "${_t}")       # bracket list nesting
    string(REPLACE "]" " " _t "${_t}")
    get_filename_component(_d "${_f}" DIRECTORY)
    get_filename_component(_mod "${_d}" NAME)
    string(REGEX MATCHALL "EXPORT\\([^,()]+, *[A-Za-z_][A-Za-z_0-9]*" _ms "${_t}")
    string(REGEX MATCHALL "EXPORT\\([^,()]+, *[A-Za-z_][A-Za-z_0-9]*\\) *{[^}]*return UNIMPLEMENTED\\(\\)" _stubs "${_t}")
    set(_stub_names "")
    foreach (_s ${_stubs})
        string(REGEX REPLACE "^EXPORT\\([^,()]+, *([A-Za-z_][A-Za-z_0-9]*).*$" "\\1" _sn "${_s}")
        list(APPEND _stub_names "${_sn}")
    endforeach()
    foreach (_m ${_ms})
        string(REGEX REPLACE "^EXPORT\\([^,()]+, *([A-Za-z_][A-Za-z_0-9]*)$" "\\1" _name "${_m}")
        if (_name STREQUAL "name")
            continue() # the macro definition itself
        endif()
        list(FIND _stub_names "${_name}" _is_stub)
        if (_is_stub EQUAL -1)
            string(APPEND _m8a_inc "VITOX_EXPORT(\"${_name}\", \"${_mod}\", false)\n")
        else()
            string(APPEND _m8a_inc "VITOX_EXPORT(\"${_name}\", \"${_mod}\", true)\n")
            math(EXPR _m8a_nstub "${_m8a_nstub} + 1")
        endif()
        math(EXPR _m8a_n "${_m8a_n} + 1")
    endforeach()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/vitox-m8a/vitox_vita3k_exports.inc.tmp" "${_m8a_inc}")
file(COPY_FILE "${CMAKE_BINARY_DIR}/vitox-m8a/vitox_vita3k_exports.inc.tmp" "${CMAKE_BINARY_DIR}/vitox-m8a/vitox_vita3k_exports.inc" ONLY_IF_DIFFERENT)
message(STATUS "Vitox M8A: Vita3K export inventory ${_m8a_n} exports (${_m8a_nstub} UNIMPLEMENTED stubs) from vita3k/modules")
add_library(vitox_m8a_launcher STATIC EXCLUDE_FROM_ALL "${VITOX_M3}/core-probe/vitox_homebrew_launcher.cpp")
vitox_m5a_flags(vitox_m8a_launcher)
target_include_directories(vitox_m8a_launcher PRIVATE "${VITOX_M3}/compat" "${VITOX_M3}/core-probe" "${CMAKE_BINARY_DIR}/vitox-m8a" "${VITA3K}/vita3k/ctrl/include"
    "${VITA3K}/vita3k/ngs/include") # M9A.7
target_link_libraries(vitox_m8a_launcher PRIVATE vitox_m7a_gxm vitox_m6b_renderer vitox_m6b2_renderer_deps vitox_m6b2_shader vitox_hle)

# ---- M8C.1: staged firmware installer (user-approved; user-supplied PUPs, isolated staging root) ----
# Vita3K's REAL installer TUs unchanged (pup.cpp, exfat.cpp, libfat16) + the M5A-prepared sce_utils.cpp (U3 lexical
# substitution). The ONLY change for WindowsStore: <openssl/evp.h> resolves to compat/openssl-shim (8 EVP functions over
# BCrypt, compat/vitox_evp_bcrypt.cpp). That shim directory is on this target's include path only.
add_library(vitox_m8c_firmware STATIC EXCLUDE_FROM_ALL
    "${VITA3K}/vita3k/packages/src/pup.cpp"
    "${CMAKE_BINARY_DIR}/vitox-patched/sce_utils.cpp"
    "${VITA3K}/vita3k/packages/src/exfat.cpp"
    "${VITA3K}/external/libfat16/src/fat16.cpp"
    "${VITOX_M3}/compat/vitox_evp_bcrypt.cpp" # (vitox_lexical_relative.cpp: already in vitox_hostio)
    "${VITOX_M3}/core-probe/vitox_firmware_stage.cpp"
    "${VITOX_M3}/core-probe/vitox_pgf_lle_probe.cpp") # M8C.2 (user-approved): staged real LLE PGF precheck
vitox_m5a_flags(vitox_m8c_firmware)
target_include_directories(vitox_m8c_firmware BEFORE PRIVATE "${VITOX_M3}/compat/openssl-shim")
target_include_directories(vitox_m8c_firmware PRIVATE "${VITOX_M3}/compat" "${VITOX_M3}/core-probe" "${VITA3K}/vita3k/packages/include"
    "${VITA3K}/vita3k/packages/src" "${VITA3K}/external/libfat16/include" "${VITA3K}/external/vita-toolchain/src")
target_link_libraries(vitox_m8c_firmware PRIVATE miniz vitox_hle)

# ---- 0.0.29.1 (user-approved 2026-11-04): NoNpDrm install-time decryption, as upstream decrypt_install_nonpdrm ----
# psvpfstools (psvpfsparser + libzRIF + libb64 + its bundled zlib) UNCHANGED, except: OpenSSLCryptoOperations.cpp and
# CryptoOperationsFactory.cpp are replaced by compat/vitox_pfs_crypto_bcrypt.cpp (the same ICryptoOperations over BCrypt;
# OpenSSL is not available for WindowsStore). zlib's gz* file layer is not built (unused). The app links vitox_pfs.lib.
set(_pfs "${VITA3K}/external/psvpfstools")
set(_pfs_p "${_pfs}/psvpfsparser")
# Guarded substitution (the U3 rule): std::filesystem::relative() calls weakly_canonical, which opens the ancestors of
# the path (D:\, D:\DevelopmentFiles) and fails with "Access is denied" inside the app container. The source path comes
# from iterating source_root itself, so the lexical relative path is the same.
vitox_substitute(external/psvpfstools/psvpfsparser/Utils.cpp pfs_Utils.cpp
[=[   psvpfs::path dest_path = dest_root / psvpfs::relative(source_path, source_root);]=]
[=[   psvpfs::path dest_path = dest_root / source_path.lexically_relative(source_root); // Vitox shim (U3: no weakly_canonical)]=])
add_library(vitox_pfs STATIC EXCLUDE_FROM_ALL
    "${_pfs_p}/FilesDbParser.cpp" "${_pfs_p}/HashTree.cpp" "${_pfs_p}/NodeIcvCalculator.cpp" "${_pfs_p}/PfsCryptEngine.cpp"
    "${_pfs_p}/PfsCryptEngineBase.cpp" "${_pfs_p}/PfsCryptEngineSelectors.cpp" "${_pfs_p}/PfsKeyGenerator.cpp" "${_pfs_p}/PfsKeys.cpp"
    "${_pfs_p}/SceKernelUtilsForDriver.cpp" "${_pfs_p}/SceSblSsMgrForDriver.cpp" "${_pfs_p}/SecretGenerator.cpp" "${_pfs_p}/UnicvDbParser.cpp"
    "${_pfs_p}/UnicvDbTypes.cpp" "${_pfs_p}/UnicvDbUtils.cpp" "${CMAKE_BINARY_DIR}/vitox-patched/pfs_Utils.cpp" "${_pfs_p}/PsvPfsParserConfig.cpp" "${_pfs_p}/LocalKeyGenerator.cpp"
    "${_pfs_p}/LocalKeys.cpp" "${_pfs_p}/IcvPrimitives.cpp" "${_pfs_p}/FlagOperations.cpp" "${_pfs_p}/PfsPageMapper.cpp" "${_pfs_p}/PfsFilesystem.cpp"
    "${_pfs_p}/PfsFile.cpp" "${_pfs_p}/rif2zrif.cpp" "${_pfs_p}/zrif2rif.cpp" "${_pfs_p}/F00DFileKeyEncryptor.cpp" "${_pfs_p}/F00DKeyEncryptorFactory.cpp"
    "${_pfs_p}/F00DNativeKeyEncryptor.cpp"
    "${_pfs}/libzrif/src/keyflate.c" "${_pfs}/libzrif/src/licdec.cpp"
    "${_pfs}/libb64/src/cdecode.c" "${_pfs}/libb64/src/cencode.c"
    "${_pfs}/zlib/adler32.c" "${_pfs}/zlib/compress.c" "${_pfs}/zlib/crc32.c" "${_pfs}/zlib/deflate.c" "${_pfs}/zlib/infback.c" "${_pfs}/zlib/inffast.c"
    "${_pfs}/zlib/inflate.c" "${_pfs}/zlib/inftrees.c" "${_pfs}/zlib/trees.c" "${_pfs}/zlib/uncompr.c" "${_pfs}/zlib/zutil.c"
    "${VITOX_M3}/compat/vitox_pfs_crypto_bcrypt.cpp"
    "${VITOX_M3}/core-probe/vitox_nonpdrm.cpp")
target_include_directories(vitox_pfs PRIVATE "${_pfs_p}" "${_pfs}/libzrif/include" "${_pfs}/libb64/include" "${_pfs}/zlib" "${VITOX_M3}/core-probe")
target_compile_definitions(vitox_pfs PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(vitox_pfs PRIVATE "/sdl-" "/W1" "/utf-8")
target_compile_features(vitox_pfs PRIVATE cxx_std_20)
target_link_libraries(vitox_pfs PRIVATE vitox_boost_headers)

# ---- M7B Phase A (0.0.15.x): open-shacccg (MIT, frangarcj/open-shacccg, pinned) compiler core for WindowsStore ----
# User-approved download 2026-10-19, pinned commit below. Only the dependency-free core (SPIR-V -> Typed Vita IR ->
# Machine IR -> USSE -> GXP); no glslang / SPIRV-Tools / SPIRV-Cross options, no Sony ABI shim, no host tool, and none
# of the repository's research/test fixtures (the public libvita2d .gxp files are never built, linked or packaged).
# It runs only inside the signed package (Smart App Control is not changed; nothing locally built runs on the PC).
set(VITOX_OPENSHACCG "C:/Vitox/toolchain/src/open-shacccg")
set(VITOX_OPENSHACCG_COMMIT "26da8ebc44717147d002c60d4a80cd6604aace7f")
execute_process(COMMAND git -C "${VITOX_OPENSHACCG}" rev-parse HEAD OUTPUT_VARIABLE _osc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT _osc STREQUAL VITOX_OPENSHACCG_COMMIT)
    message(FATAL_ERROR "open-shacccg checkout is ${_osc}, expected the pinned ${VITOX_OPENSHACCG_COMMIT}")
endif()
set(_osc_srcs src/core/allocator.cpp src/core/compiler.cpp src/frontend/cg_compat.cpp src/spirv/spirv_cross_adapter.cpp
    src/spirv/spirv_pipeline.cpp src/spirv/spirv_reader.cpp src/backend/backend.cpp src/backend/machine_ir.cpp
    src/backend/program_builder.cpp src/backend/typed_ir.cpp src/backend/shader_profiles.cpp src/gxp/gxp_writer.cpp
    src/gxp/gxp_reader.cpp src/usse/usse.cpp)
list(TRANSFORM _osc_srcs PREPEND "${VITOX_OPENSHACCG}/")
add_library(vitox_m7b_openshacccg STATIC EXCLUDE_FROM_ALL ${_osc_srcs})
target_include_directories(vitox_m7b_openshacccg PUBLIC "${VITOX_OPENSHACCG}/include" PRIVATE "${VITOX_OPENSHACCG}/src")
target_compile_features(vitox_m7b_openshacccg PRIVATE cxx_std_17)
# MSVC: shader_profiles.cpp relies on <array> arriving transitively (GCC/libstdc++); force it instead of editing upstream
target_compile_options(vitox_m7b_openshacccg PRIVATE "/sdl-" "/bigobj" "/utf-8" "/EHsc" "/FIarray")
set_target_properties(vitox_m7b_openshacccg PROPERTIES FOLDER vitox)
message(STATUS "Vitox M7B: open-shacccg ${_osc} core (${VITOX_OPENSHACCG})")
# M7B Phase A probe TU (non-visual): our shader source -> open-shacccg -> GXP -> real Vita3K exports/recompiler/VK device
add_library(vitox_m7b_probe STATIC EXCLUDE_FROM_ALL "${VITOX_M3}/core-probe/vitox_shader_gen_probe.cpp")
vitox_m5a_flags(vitox_m7b_probe)
target_include_directories(vitox_m7b_probe PRIVATE "${VITOX_M3}/compat" "${VITOX_M3}/core-probe" "${VITOX_OPENSHACCG}/include")
target_link_libraries(vitox_m7b_probe PRIVATE vitox_m7b_openshacccg vitox_m7a_gxm vitox_m6b_renderer vitox_m6b2_renderer_deps vitox_m6b2_shader vitox_hle)
