// Vitox M8C.3 - see vitox_firmware_root.h.
#include "vitox_firmware_root.h"

#include <atomic>

namespace {
std::filesystem::path g_root;
std::atomic<bool> g_set{ false };
} // namespace

const std::filesystem::path *vitox_firmware_root() { return g_set ? &g_root : nullptr; }

void vitox_set_firmware_root(const std::filesystem::path *root) {
    g_set = false;
    if (root) {
        g_root = *root;
        g_set = true;
    }
}
