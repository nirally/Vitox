// Vitox M9A.10 EVIDENCE (0.0.24.112, logging only): are per-draw vertex/index copies stale or short?
//
// With memory mapping off, sceGxmDraw only records guest POINTERS to the vertex streams and the index buffer; the render
// thread copies the bytes later (scene.cpp draw / bind_vertex_streams). Gravity Rush shows flickering stretched geometry.
// This header checks, per draw:
//   1. stale: the bytes hashed at sceGxmDraw time (game thread) differ from the bytes the render thread copies
//   2. short: the stream copy (sized from the draw-time max index) does not cover the max index the render thread sees
// It also reports, once per program/attribute, vertex fetches wider than the GXM attribute (vk_wide), and uniform buffers
// whose size is the parser's SCE_GXM_MAX_UB_IN_FLOAT_UNIT guess (ub_max). Nothing is changed: lines go to stderr as
// [vitox-ev] (first 64 of each kind + a summary every 4096 draws).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <utility>

struct VitoxEvRec {
    uint32_t ib_addr = 0, ib_count = 0, max_index = 0, program = 0;
    uint64_t ib_hash = 0;
    uint32_t vb_addr[16] = {}, vb_len[16] = {};
    uint64_t vb_hash[16] = {};
    bool precomputed = false;
};

inline uint64_t vitox_ev_hash(const void *data, size_t n) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

struct VitoxEvState {
    std::mutex lock;
    std::deque<VitoxEvRec> queue;
    std::atomic<uint64_t> recorded{ 0 }, draws{ 0 }, matched{ 0 }, unmatched{ 0 }, dropped{ 0 }, ib_stale{ 0 }, vb_stale{ 0 },
        vb_moved{ 0 }, short_copy{ 0 }, vk_wide{ 0 }, ub_max{ 0 };
    std::set<std::pair<uint64_t, uint32_t>> once; // (program, key) already reported
};
inline VitoxEvState g_vitox_ev;

// render-thread state of the draw being recorded
inline thread_local VitoxEvRec t_vitox_ev_cur;
inline thread_local bool t_vitox_ev_have = false;
inline thread_local uint32_t t_vitox_ev_max_index = 0;

#define VITOX_EV_LOG(counter, ...)                                    \
    do {                                                              \
        if (g_vitox_ev.counter.fetch_add(1) < 64) {                   \
            std::fprintf(stderr, "[vitox-ev] " #counter " " __VA_ARGS__); \
            std::fflush(stderr);                                      \
        }                                                             \
    } while (0)

inline bool vitox_ev_once(uint64_t program, uint32_t key) {
    std::lock_guard<std::mutex> guard(g_vitox_ev.lock);
    return g_vitox_ev.once.insert({ program, key }).second;
}

// game thread (SceGxm draw paths), right before renderer::draw
inline void vitox_ev_record(const VitoxEvRec &rec) {
    g_vitox_ev.recorded++;
    std::lock_guard<std::mutex> guard(g_vitox_ev.lock);
    g_vitox_ev.queue.push_back(rec);
    while (g_vitox_ev.queue.size() > 32768) {
        g_vitox_ev.queue.pop_front();
        g_vitox_ev.dropped++;
    }
}

inline void vitox_ev_summary() {
    std::fprintf(stderr,
        "[vitox-ev] summary draws=%llu recorded=%llu matched=%llu unmatched=%llu dropped=%llu ib_stale=%llu vb_stale=%llu "
        "vb_moved=%llu short_copy=%llu vk_wide=%llu ub_max=%llu\n",
        (unsigned long long)g_vitox_ev.draws.load(), (unsigned long long)g_vitox_ev.recorded.load(),
        (unsigned long long)g_vitox_ev.matched.load(), (unsigned long long)g_vitox_ev.unmatched.load(),
        (unsigned long long)g_vitox_ev.dropped.load(), (unsigned long long)g_vitox_ev.ib_stale.load(),
        (unsigned long long)g_vitox_ev.vb_stale.load(), (unsigned long long)g_vitox_ev.vb_moved.load(),
        (unsigned long long)g_vitox_ev.short_copy.load(), (unsigned long long)g_vitox_ev.vk_wide.load(),
        (unsigned long long)g_vitox_ev.ub_max.load());
    std::fflush(stderr);
}

// render thread (scene.cpp draw, memory mapping off), before the index copy
inline void vitox_ev_begin_draw(uint32_t ib_addr, uint32_t ib_count, const void *indices, bool u16, uint32_t program) {
    const uint64_t n = g_vitox_ev.draws.fetch_add(1) + 1;
    if (n % 4096 == 0)
        vitox_ev_summary();

    uint32_t max_index = 0;
    if (u16) {
        const uint16_t *d = static_cast<const uint16_t *>(indices);
        for (uint32_t k = 0; k < ib_count; k++)
            max_index = std::max<uint32_t>(max_index, d[k]);
    } else {
        const uint32_t *d = static_cast<const uint32_t *>(indices);
        for (uint32_t k = 0; k < ib_count; k++)
            max_index = std::max<uint32_t>(max_index, d[k]);
    }
    t_vitox_ev_max_index = max_index;

    t_vitox_ev_have = false;
    {
        std::lock_guard<std::mutex> guard(g_vitox_ev.lock);
        for (int skips = 0; !g_vitox_ev.queue.empty() && skips < 256; skips++) {
            VitoxEvRec rec = g_vitox_ev.queue.front();
            g_vitox_ev.queue.pop_front();
            if (rec.ib_addr == ib_addr && rec.ib_count == ib_count) {
                t_vitox_ev_cur = rec;
                t_vitox_ev_have = true;
                break;
            }
            g_vitox_ev.unmatched++; // recorded at draw time but never reached the render thread's draw (or out of order)
        }
    }
    if (!t_vitox_ev_have)
        return;
    g_vitox_ev.matched++;

    const uint64_t ib_hash = vitox_ev_hash(indices, (size_t)ib_count * (u16 ? 2 : 4));
    if (ib_hash != t_vitox_ev_cur.ib_hash)
        VITOX_EV_LOG(ib_stale, "draw=%llu ib=0x%08x count=%u max_index draw=%u now=%u prog=0x%08x pre=%d\n",
            (unsigned long long)n, ib_addr, ib_count, t_vitox_ev_cur.max_index, max_index, program, t_vitox_ev_cur.precomputed);
}

// render thread (bind_vertex_streams, memory mapping off), before the stream copy. `required` is the copy length the
// render-time max index needs (same formula as SceGxm.cpp).
inline void vitox_ev_stream(int i, uint32_t addr, uint32_t size, const void *data, uint32_t stride, uint32_t required, uint32_t program) {
    if (required > size)
        VITOX_EV_LOG(short_copy, "draw=%llu stream=%d addr=0x%08x size=%u required=%u stride=%u max_index now=%u draw=%u prog=0x%08x\n",
            (unsigned long long)g_vitox_ev.draws.load(), i, addr, size, required, stride, t_vitox_ev_max_index,
            t_vitox_ev_have ? t_vitox_ev_cur.max_index : 0, program);
    if (!t_vitox_ev_have || i >= 16)
        return;
    if (t_vitox_ev_cur.vb_addr[i] != addr || t_vitox_ev_cur.vb_len[i] != size) {
        VITOX_EV_LOG(vb_moved, "draw=%llu stream=%d render addr=0x%08x size=%u | draw-time addr=0x%08x size=%u prog=0x%08x\n",
            (unsigned long long)g_vitox_ev.draws.load(), i, addr, size, t_vitox_ev_cur.vb_addr[i], t_vitox_ev_cur.vb_len[i], program);
        return;
    }
    if (vitox_ev_hash(data, size) != t_vitox_ev_cur.vb_hash[i])
        VITOX_EV_LOG(vb_stale, "draw=%llu stream=%d addr=0x%08x size=%u stride=%u prog=0x%08x pre=%d\n",
            (unsigned long long)g_vitox_ev.draws.load(), i, addr, size, stride, program, t_vitox_ev_cur.precomputed);
}
