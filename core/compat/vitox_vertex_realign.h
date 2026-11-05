// Vitox (M9A.8, 0.0.24.94): 4-byte aligned vertex streams for the Xbox D3D12 driver.
//
// The Xbox driver rejects a vertex input element whose byte offset is not a multiple of 4 (CreatePipelineState
// E_INVALIDARG; the 0.0.24.92 bisect accepted the same PSO only with the offsets rounded down). It also ignores the low
// bits of a vertex buffer address (0.0.24.93 shifted-view aliases drew garbage). With memory mapping off, Vita3K copies
// every vertex stream per draw, so a stream with an unaligned attribute offset or stride is repacked on that copy: each
// attribute moves to a 4-byte aligned offset and the stride grows to match. get_vertex_input_state (pipeline) and
// bind_vertex_streams (upload) compute the same layout from the same vertex program and the same attribute filter.
#pragma once

#include <gxm/functions.h>
#include <gxm/types.h>
#include <renderer/gxm_types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

struct VitoxStreamLayout {
    bool repack = false;
    uint32_t stride = 0; // new stride (0 stays 0: one element shared by every vertex)
    uint32_t ranges = 0;
    uint16_t src[32] = {}, dst[32] = {}, len[32] = {};
};

inline uint32_t vitox_align4(uint32_t v) { return (v + 3) & ~3u; }

// new_offsets[a] receives the repacked offset of vertex_program.attributes[a] (for the attributes of this stream that
// pass `used`); it is left untouched for the others.
template <typename Used>
inline VitoxStreamLayout vitox_realign_stream(const SceGxmVertexProgram &vertex_program, uint32_t stream_index, Used used, uint16_t *new_offsets) {
    VitoxStreamLayout l;
    const uint32_t old_stride = vertex_program.streams[stream_index].stride;
    struct Attr {
        uint32_t index, offset, size;
    };
    std::vector<Attr> attrs;
    bool unaligned = old_stride % 4 != 0;
    for (uint32_t a = 0; a < vertex_program.attributes.size(); a++) {
        const SceGxmVertexAttribute &attribute = vertex_program.attributes[a];
        if (attribute.streamIndex != stream_index || !used(attribute))
            continue;
        const uint32_t size = gxm::attribute_format_size(static_cast<SceGxmAttributeFormat>(attribute.format)) * attribute.componentCount;
        attrs.push_back({ a, attribute.offset, size });
        unaligned |= attribute.offset % 4 != 0;
    }
    if (!unaligned || attrs.empty())
        return l;
    std::stable_sort(attrs.begin(), attrs.end(), [](const Attr &x, const Attr &y) { return x.offset < y.offset; });
    uint32_t cursor = 0;
    for (const Attr &at : attrs) {
        // attributes at the same source offset share one range
        uint32_t r = 0;
        while (r < l.ranges && l.src[r] != at.offset)
            r++;
        if (r == l.ranges) {
            if (l.ranges == 32)
                return VitoxStreamLayout{}; // too many ranges: leave the stream as it is
            l.src[r] = static_cast<uint16_t>(at.offset);
            l.dst[r] = static_cast<uint16_t>(cursor);
            l.len[r] = static_cast<uint16_t>(at.size);
            l.ranges++;
            cursor += vitox_align4(std::max<uint32_t>(at.size, 1));
        } else if (at.size > l.len[r]) {
            // a wider attribute at an offset already placed: grow that range (rare; recompute the cursor)
            l.len[r] = static_cast<uint16_t>(at.size);
            cursor = 0;
            for (uint32_t k = 0; k < l.ranges; k++) {
                l.dst[k] = static_cast<uint16_t>(cursor);
                cursor += vitox_align4(std::max<uint32_t>(l.len[k], 1));
            }
        }
    }
    for (const Attr &at : attrs) {
        for (uint32_t r = 0; r < l.ranges; r++)
            if (l.src[r] == at.offset)
                new_offsets[at.index] = l.dst[r];
    }
    l.stride = old_stride == 0 ? 0 : std::max<uint32_t>(vitox_align4(cursor), 4);
    l.repack = true;
    return l;
}

// Copies `size` bytes of a stream with `old_stride` into the layout; returns a new[] buffer (caller delete[]s it).
inline uint8_t *vitox_repack_stream(const uint8_t *stream, uint32_t size, uint32_t old_stride, const VitoxStreamLayout &l, uint32_t &new_size) {
    const uint32_t vertices = old_stride ? (size + old_stride - 1) / old_stride : 1;
    uint32_t elem = l.stride;
    if (!elem)
        for (uint32_t r = 0; r < l.ranges; r++)
            elem = std::max<uint32_t>(elem, l.dst[r] + vitox_align4(l.len[r]));
    new_size = std::max<uint32_t>(vertices * elem, 4);
    uint8_t *out = new uint8_t[new_size];
    std::memset(out, 0, new_size);
    for (uint32_t v = 0; v < vertices; v++) {
        for (uint32_t r = 0; r < l.ranges; r++) {
            const uint32_t from = v * old_stride + l.src[r];
            if (from >= size)
                continue;
            std::memcpy(out + v * elem + l.dst[r], stream + from, std::min<uint32_t>(l.len[r], size - from));
        }
    }
    return out;
}
