// Vitox M9A.2C - is an executable directly loadable by the real Vita3K loader (kernel/src/load_self.cpp)? That is a
// plain ELF32, or a SELF (version 3, header type 1) whose every segment is plain (segment_info.encryption == 2, the
// loader's own test). A protected SELF (NPDRM / encrypted segments) is not: it needs a license-based decrypt, and no
// supported licensed-load mode exists in this build. Header-only and allocation-free over the bytes, so a host-side
// test can include it unchanged. Layouts: vita-toolchain src/self.h (SCE_header, SCE_appinfo, segment_info).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

struct VitoxSelfCheck {
    bool readable = false;
    std::string kind;   // "ELF", "SELF", "unknown"
    std::string detail; // why (segment counts / the first protected segment / malformed header)
    uint32_t self_type = 0;
    unsigned segments = 0, encrypted = 0;
};

inline VitoxSelfCheck vitox_self_check(const uint8_t *b, size_t n) {
    VitoxSelfCheck r;
    const auto u16 = [&](uint64_t o, uint64_t &v) { if (o + 2 > n) return false; uint16_t x; memcpy(&x, b + o, 2); v = x; return true; };
    const auto u32 = [&](uint64_t o, uint64_t &v) { if (o + 4 > n) return false; uint32_t x; memcpy(&x, b + o, 4); v = x; return true; };
    const auto u64 = [&](uint64_t o, uint64_t &v) { if (o + 8 > n) return false; memcpy(&v, b + o, 8); return true; };
    const auto is_elf32 = [&](uint64_t o) { return o + 0x34 <= n && b[o] == 0x7F && b[o + 1] == 'E' && b[o + 2] == 'L' && b[o + 3] == 'F' && b[o + 4] == 1; };
    if (n >= 4 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        r.kind = "ELF";
        r.readable = is_elf32(0);
        r.detail = r.readable ? "plain ELF32" : "ELF header is not ELF32 / truncated";
        return r;
    }
    if (n < 4 || memcmp(b, "SCE\0", 4) != 0) {
        r.kind = "unknown";
        r.detail = "neither ELF nor SELF magic";
        return r;
    }
    r.kind = "SELF";
    uint64_t version = 0, header_type = 0, appinfo = 0, elf_off = 0, seg_off = 0, phnum = 0, st = 0;
    if (!u32(0x04, version) || !u16(0x0A, header_type) || !u64(0x38, appinfo) || !u64(0x40, elf_off) || !u64(0x58, seg_off)) {
        r.detail = "truncated SELF header";
        return r;
    }
    if (version != 3 || header_type != 1) {
        r.detail = "SELF version " + std::to_string(version) + " / header type " + std::to_string(header_type) + " (the loader takes 3 / 1 only)";
        return r;
    }
    if (u32(appinfo + 12, st))
        r.self_type = static_cast<uint32_t>(st);
    if (elf_off >= n || !is_elf32(elf_off) || !u16(elf_off + 0x2C, phnum)) {
        r.detail = "no ELF32 header at the SELF elf_offset (encrypted or malformed)";
        return r;
    }
    r.segments = static_cast<unsigned>(phnum);
    for (uint64_t i = 0; i < phnum; ++i) {
        uint64_t enc = 0;
        if (seg_off > n || !u64(seg_off + i * 32 + 24, enc)) {
            r.detail = "segment_info table truncated";
            return r;
        }
        if (enc != 2 && r.encrypted++ == 0)
            r.detail = "segment " + std::to_string(i) + " encryption=" + std::to_string(enc) + " (protected: the loader needs 2 = plain)";
    }
    r.readable = r.encrypted == 0 && r.segments > 0;
    if (r.readable)
        r.detail = "SELF, " + std::to_string(r.segments) + " plain segment(s)";
    else if (r.segments == 0)
        r.detail = "SELF without segments";
    else
        r.detail += " | " + std::to_string(r.encrypted) + " of " + std::to_string(r.segments) + " segment(s) protected";
    return r;
}
