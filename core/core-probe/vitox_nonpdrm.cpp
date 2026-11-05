// Vitox 0.0.29.1: see vitox_nonpdrm.h.
#include "vitox_nonpdrm.h"

#include <F00DKeyEncryptorFactory.h>
#include <PsvPfsParserConfig.h>
#include <rif2zrif.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
struct Cancelled {};

std::string narrow_ascii(const wchar_t *w) {
    std::string s;
    for (; w && *w; ++w)
        s += *w < 128 ? static_cast<char>(*w) : '?';
    return s;
}
} // namespace

extern "C" bool vitox_nonpdrm_decrypt(const wchar_t *src_dir, const wchar_t *dst_dir, const wchar_t *license_file,
    bool (*progress)(void *user, uint64_t processed, uint64_t total), void *user, char *error, unsigned error_size) {
    const auto fail = [&](const std::string &why) {
        if (error && error_size)
            snprintf(error, error_size, "%s", why.c_str());
        return false;
    };
    try {
        // upstream: fs::ifstream binfile(drmlicpath, std::ios::in | std::ios::binary | std::ios::ate); rif2zrif(binfile)
        std::ifstream binfile(std::filesystem::path(license_file), std::ios::in | std::ios::binary | std::ios::ate);
        if (!binfile.is_open())
            return fail("the license (work.bin) could not be opened");
        std::string zrif = rif2zrif(binfile);
        if (zrif.empty())
            return fail("the license (work.bin) is not a valid NoNpDrm license");
        std::string src = narrow_ascii(src_dir), dst = narrow_ascii(dst_dir), f00d_arg;
        PfsProgressCallback cb = nullptr;
        if (progress)
            cb = [progress, user](std::uint64_t processed, std::uint64_t total, const std::string &) {
                if (!progress(user, processed, total))
                    throw Cancelled{};
            };
        if (execute(zrif, src, dst, F00DEncryptorTypes::native, f00d_arg, cb) < 0)
            return fail("the PFS decryption failed (the license does not match this content, or the dump is damaged)");
        return true;
    } catch (const Cancelled &) {
        return fail("cancelled");
    } catch (const std::exception &e) {
        return fail(std::string("the PFS decryption failed: ") + e.what());
    } catch (...) {
        return fail("the PFS decryption failed");
    }
}
