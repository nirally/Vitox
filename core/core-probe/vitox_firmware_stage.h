// Vitox M8C.1 - STAGED FIRMWARE INSTALLER (non-visual). Vita3K's REAL install_pup (packages/src/pup.cpp +
// sce_utils.cpp + exfat.cpp + libfat16) with the VITOX_UWP BCrypt EVP substitution, run against an isolated staging
// root - never the active vita-fs. The inputs are the user's own firmware packages, approved by SHA-256:
//   inputs:  <dev_root>\firmware-staging\input\{PSVUPDAT,PREINST,FONTPKG}.PUP (SHA-256 verified right before use)
//   output:  <dev_root>\firmware-staging\vita-fs\{os0,vs0,sa0,pd0}
// Runs once: skipped when the staging output already exists (a completed stage is never redone implicitly).
#pragma once

#include "vitox_core_probe.h" // VitoxCoreHost

struct VitoxFirmwareStageResult {
    bool requested;    // input dir present and no staged output yet
    bool selftest;     // NIST AES-128-CBC / AES-256-CBC / AES-128-CTR known answers through the EVP shim
    bool hashes;       // all three inputs match their approved SHA-256
    bool installed;    // install_pup returned for all three without exception; 0 EVP failures; 0 decrypt/decompress errors
    bool partitions;   // os0, vs0, sa0, pd0 exist and are non-empty
    bool libpgf;       // vs0/sys/external/libpgf.suprx exists
    bool fonts;        // sa0 font resources present
    bool contained;    // every staged file under the staging root; active vita-fs + homebrew root unchanged
    unsigned files;
    unsigned long long bytes;
};

extern "C" VitoxFirmwareStageResult vitox_firmware_stage_run(const VitoxCoreHost *host, const wchar_t *dev_root);
