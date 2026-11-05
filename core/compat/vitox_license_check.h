// Vitox M9A.3A - licensed-load VALIDATION only. Identity and structure checks of an app-staging app and its separately
// staged license, in the generic PS Vita formats; nothing here is specific to any title. No key is used: the key bytes
// are only tested for being non-zero, nothing is decrypted, nothing is registered. Header-only, so a host-side test
// includes it unchanged. License layout: vita3k/packages/include/packages/license.h (SceNpDrmLicense, 512 B).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

inline bool vitox_valid_title_id(const std::string &id) {
    if (id.size() != 9)
        return false;
    for (char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    return true;
}

// XX0000-TTTTTTTTT_00-KKKKKKKKKKKKKKKK (36 characters)
inline bool vitox_valid_content_id(const std::string &s) {
    if (s.size() != 36 || s[6] != '-' || s[16] != '_' || s[19] != '-')
        return false;
    const auto up = [](char c) { return c >= 'A' && c <= 'Z'; };
    const auto dig = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 6 || i == 16 || i == 19)
            continue;
        const bool ok = i < 2 ? up(c) : i < 6 ? dig(c) : (i >= 17 && i < 19) ? dig(c) : (up(c) || dig(c));
        if (!ok)
            return false;
    }
    return true;
}

// App identity: folder == TITLE_ID, a well-formed CONTENT_ID that names this TITLE_ID, and a retail TITLE_ID ("PCS"
// prefix: upstream get_license() handles only those and silently skips anything else). Empty = OK, else the reason.
inline std::string vitox_app_identity_error(const std::string &folder, const std::string &title_id, const std::string &content_id) {
    if (!vitox_valid_title_id(title_id))
        return "param.sfo TITLE_ID '" + title_id + "' is not 9 characters A-Z/0-9";
    if (folder != title_id)
        return "app-staging folder '" + folder + "' is not its param.sfo TITLE_ID '" + title_id + "'";
    if (content_id.empty())
        return "param.sfo has no CONTENT_ID";
    if (!vitox_valid_content_id(content_id))
        return "param.sfo CONTENT_ID '" + content_id + "' is malformed";
    if (content_id.compare(7, 9, title_id) != 0)
        return "param.sfo CONTENT_ID '" + content_id + "' does not name TITLE_ID '" + title_id + "'";
    if (title_id.compare(0, 3, "PCS") != 0)
        return "TITLE_ID '" + title_id + "' is not a retail (PCS*) title; upstream get_license() would skip it";
    return {};
}

struct VitoxLicenseCheck {
    bool ok = false;
    std::string reason;     // set when !ok
    std::string content_id; // as found in the license (when well-formed)
};

// The staged license: exactly 512 B, version 1 header (big-endian 0x0001), a well-formed NUL-padded content ID at 0x10
// equal to the app's CONTENT_ID (so its TITLE_ID part is the app's), and a key field (0x50..0x5F) that is not all zero.
inline VitoxLicenseCheck vitox_license_check(const uint8_t *b, size_t n, const std::string &app_content_id, const std::string &app_title_id) {
    VitoxLicenseCheck r;
    if (n != 512) {
        r.reason = "license is " + std::to_string(n) + " B, expected exactly 512 B";
        return r;
    }
    if (b[0] != 0 || b[1] != 1) {
        r.reason = "license header is not version 1";
        return r;
    }
    for (size_t i = 0x10 + 36; i < 0x40; ++i)
        if (b[i] != 0) {
            r.reason = "license content ID field is not NUL-padded after 36 characters";
            return r;
        }
    const std::string cid(reinterpret_cast<const char *>(b + 0x10), 36);
    if (!vitox_valid_content_id(cid)) {
        r.reason = "license content ID is malformed";
        return r;
    }
    r.content_id = cid;
    if (cid != app_content_id) {
        r.reason = "license content ID '" + cid + "' differs from the app CONTENT_ID '" + app_content_id + "'";
        return r;
    }
    if (cid.compare(7, 9, app_title_id) != 0) {
        r.reason = "license TITLE_ID '" + cid.substr(7, 9) + "' differs from the app TITLE_ID '" + app_title_id + "'";
        return r;
    }
    bool any = false;
    for (size_t i = 0x50; i < 0x60; ++i)
        any |= b[i] != 0;
    if (!any) {
        r.reason = "license key field is all zero";
        return r;
    }
    r.ok = true;
    return r;
}

// vitox-launch.txt: licensed_load=staged-license is only valid together with these (else the launch fails closed)
inline const char *vitox_licensed_load_config_error(bool licensed_load, const char *app_root, bool fail_closed, bool headless) {
    if (!licensed_load)
        return nullptr;
    if (!app_root || strcmp(app_root, "app-staging") != 0)
        return "licensed_load=staged-license requires app_root=app-staging";
    if (!fail_closed)
        return "licensed_load=staged-license requires unsupported_imports=fail-closed";
    if (!headless)
        return "licensed_load=staged-license requires presenter=headless";
    return nullptr;
}
