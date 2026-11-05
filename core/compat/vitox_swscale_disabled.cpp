// Vitox (M6C): counters for the compiled-out swscale call sites (see vitox_swscale_disabled.h).
#include "vitox_swscale_disabled.h"

#include <atomic>

namespace {
std::atomic<unsigned> g_hits[VITOX_SWS_SITE_COUNT];
}

extern "C" void vitox_swscale_disabled_hit(unsigned site) {
    if (site < VITOX_SWS_SITE_COUNT)
        ++g_hits[site];
}

extern "C" unsigned vitox_swscale_disabled_count(unsigned site) { return site < VITOX_SWS_SITE_COUNT ? g_hits[site].load() : 0; }
