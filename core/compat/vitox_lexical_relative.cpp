// Vitox U3: containment-checked lexical relative path (see vitox_lexical_relative.h).
#include "vitox_lexical_relative.h"

#include <atomic>
#include <cwctype>
#include <vector>

namespace vitox {

namespace {

std::atomic<LexicalRelativeTrace> g_trace{ nullptr };

bool same_element(const fs::path &a, const fs::path &b) {
    const auto &x = a.native();
    const auto &y = b.native();
    if (x.size() != y.size())
        return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (std::towlower(x[i]) != std::towlower(y[i]))
            return false;
    return true;
}

// Elements below the root, without "." (lexically_normal leaves a trailing "." for "dir\").
bool elements(const fs::path &p, std::vector<fs::path> &out, std::string *why) {
    for (const auto &e : p.relative_path()) {
        if (e == ".")
            continue;
        if (e == "..") {
            if (why)
                *why = "'..' remains after lexical normalisation (escapes the root)";
            return false;
        }
        out.push_back(e);
    }
    return true;
}

bool fail(fs::path &out, std::string *why, const char *reason) {
    out.clear();
    if (why)
        *why = reason;
    return false;
}

} // namespace

bool lexical_relative(const fs::path &child, const fs::path &base, fs::path &out, std::string *why) {
    std::string local_why;
    std::string *w = why ? why : &local_why;
    w->clear();
    bool ok = false;
    [&] {
        if (!child.is_absolute() || !base.is_absolute())
            return (void)fail(out, w, "child and base must both be absolute");
        const fs::path c = child.lexically_normal(), b = base.lexically_normal();
        if (!same_element(c.root_name(), b.root_name()))
            return (void)fail(out, w, "different root name (drive)");
        if (c.root_directory() != b.root_directory())
            return (void)fail(out, w, "different root directory");
        std::vector<fs::path> ce, be;
        if (!elements(c, ce, w) || !elements(b, be, w)) {
            out.clear();
            return;
        }
        if (ce.size() <= be.size())
            return (void)fail(out, w, ce.size() == be.size() ? "child is the base itself (not strictly contained)" : "child is not under base");
        for (size_t i = 0; i < be.size(); ++i)
            if (!same_element(ce[i], be[i]))
                return (void)fail(out, w, "child is not under base (sibling or other subtree)");
        out.clear();
        for (size_t i = be.size(); i < ce.size(); ++i)
            out /= ce[i];
        ok = true;
    }();
    if (const auto t = g_trace.load())
        t(child, base, out, ok, *w);
    return ok;
}

void set_lexical_relative_trace(LexicalRelativeTrace trace) { g_trace = trace; }

} // namespace vitox
