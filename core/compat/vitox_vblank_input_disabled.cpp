// Vitox (M7A): counters for the compiled-out vblank input refresh calls (see vitox_vblank_input_disabled.h).
#include "vitox_vblank_input_disabled.h"

#include <atomic>

namespace {
std::atomic<unsigned long long> g_skipped[VITOX_VBLANK_SITE_COUNT];
}

extern "C" void vitox_vblank_input_skipped(unsigned site) {
    if (site < VITOX_VBLANK_SITE_COUNT)
        ++g_skipped[site];
}

extern "C" unsigned long long vitox_vblank_input_skipped_count(unsigned site) {
    return site < VITOX_VBLANK_SITE_COUNT ? g_skipped[site].load() : 0;
}
