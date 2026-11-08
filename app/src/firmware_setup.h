// Vitox 0.0.33.0: first-run firmware setup (launcher). The three firmware packages Vita3K needs are installed from
// .PUP files on a USB drive: each .PUP found on a removable drive is identified by its SHA-256 (the approved packages
// of M8C.1), copied to <fw_root>\firmware-staging\input\<NAME>.PUP and then Vita3K's real install_pup runs through the
// core's vitox_firmware_stage_run. Done = <fw_root>\firmware-staging\stage-complete.txt exists.
// fw_root = the dev root; the side-by-side test package (Identity Vitox.FirmwareTest) uses <dev_root>\fwtest so the
// real firmware staging is never touched while the flow is tested.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace vitox::firmware {

constexpr int kPackages = 3;
struct PackageInfo {
    const wchar_t *name;    // shown in the list
    const wchar_t *file;    // canonical file name in the staging input
    const wchar_t *detail;  // version / purpose
    const char *sha256;     // upper-case hex
};
const PackageInfo &package(int i);

std::wstring root(const std::wstring &dev_root);
bool installed(const std::wstring &dev_root);

enum class Phase : int { Idle, Scanning, Copying, Installing, Done, Failed };
struct Snapshot {
    Phase phase = Phase::Idle;
    uint64_t version = 0;
    bool have[kPackages] = {};      // already in the staging input (verified)
    unsigned pups_seen = 0, drives = 0;
    std::wstring message, current;
    uint64_t done_bytes = 0, total_bytes = 0;
};

class Setup {
public:
    explicit Setup(std::wstring dev_root);
    ~Setup();
    void start();                                // scan the USB drives, copy, install (one worker)
    bool poll(uint64_t have, Snapshot &out);     // true when changed
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vitox::firmware
