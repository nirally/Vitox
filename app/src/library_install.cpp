// Vitox 0.0.29.0: library settings + the USB archive installer (see library_install.h).
#include "library_install.h"

#include "common.h"

#include <miniz.h>
#include <vitox_nonpdrm.h> // 0.0.29.1 (core vitox_pfs.lib)
#include <vitox_self_check.h>

#include <fileapifromapp.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace vitox {

namespace fs = std::filesystem;

// ================================================================ settings
namespace {
std::mutex g_lm;
std::wstring g_ldir;
bool g_show_homebrew = true;

void library_save_locked() {
    if (g_ldir.empty())
        return;
    std::ofstream f(g_ldir + L"\\vitox-library.txt", std::ios::trunc);
    f << "# Vitox library settings (launcher Settings > Library)\nshow_homebrew=" << (g_show_homebrew ? 1 : 0) << "\n";
}
} // namespace

void library_load(const std::wstring &local_state) {
    std::lock_guard<std::mutex> lock(g_lm);
    g_ldir = local_state;
    std::ifstream f(g_ldir + L"\\vitox-library.txt");
    bool found = false;
    for (std::string line; f && std::getline(f, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line == "show_homebrew=0" || line == "show_homebrew=1")
            g_show_homebrew = line.back() == '1', found = true;
    }
    log_line(Log::Lifecycle, "vitox library: show homebrew %s%s", g_show_homebrew ? "ON" : "OFF", found ? "" : " [default]");
}

bool library_show_homebrew() {
    std::lock_guard<std::mutex> lock(g_lm);
    return g_show_homebrew;
}

void library_set_show_homebrew(bool on) {
    std::lock_guard<std::mutex> lock(g_lm);
    g_show_homebrew = on;
    library_save_locked();
}

namespace install {

const wchar_t *kind_name(Kind k) {
    switch (k) {
    case Kind::Game: return L"Game";
    case Kind::Homebrew: return L"Homebrew";
    case Kind::Update: return L"Update";
    case Kind::Dlc: return L"DLC";
    }
    return L"?";
}

std::wstring format_bytes(uint64_t b) {
    wchar_t buf[48];
    if (b >= 1024ull * 1024 * 1024)
        swprintf_s(buf, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ull * 1024)
        swprintf_s(buf, L"%.1f MB", b / (1024.0 * 1024));
    else
        swprintf_s(buf, L"%.0f KB", b / 1024.0);
    return buf;
}

namespace {

constexpr uint32_t kMaxFiles = 400; // archives listed per drive
constexpr uint64_t kSpaceMargin = 256ull * 1024 * 1024;

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

bool valid_title_id(const std::string &id) {
    return id.size() == 9 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}

bool ends_with_ci(const std::wstring &s, const wchar_t *suffix) {
    const size_t n = wcslen(suffix);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (towlower(s[s.size() - n + i]) != towlower(suffix[i]))
            return false;
    return true;
}

// param.sfo: "\0PSF", key table / data table offsets, entry count, 16-byte entries (vita-toolchain sfo.h)
std::map<std::string, std::string> parse_sfo(const uint8_t *b, size_t n) {
    std::map<std::string, std::string> out;
    const auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= n ? static_cast<uint32_t>(b[o] | (b[o + 1] << 8)) : 0; };
    const auto u32 = [&](size_t o) -> uint32_t {
        return o + 4 <= n ? static_cast<uint32_t>(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<uint32_t>(b[o + 3]) << 24)) : 0;
    };
    if (n < 0x14 || memcmp(b, "\0PSF", 4) != 0)
        return out;
    const uint32_t keys = u32(8), data = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && i < 256; ++i) {
        const size_t e = 0x14 + static_cast<size_t>(i) * 16;
        if (e + 16 > n)
            break;
        const uint32_t key_off = u16(e), fmt = u16(e + 2), len = u32(e + 4), data_off = u32(e + 12);
        const size_t ko = static_cast<size_t>(keys) + key_off, dof = static_cast<size_t>(data) + data_off;
        if (ko >= n || dof > n)
            continue;
        std::string key;
        for (size_t p = ko; p < n && b[p]; ++p)
            key += static_cast<char>(b[p]);
        if (fmt == 0x0404) {
            out[key] = std::to_string(u32(dof));
        } else if (dof + len <= n) {
            out[key] = trim(std::string(reinterpret_cast<const char *>(b + dof), len));
        }
    }
    return out;
}

std::map<std::string, std::string> read_sfo_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse_sfo(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

bool path_exists(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// a zip entry name is used under a destination folder only when it cannot leave it
bool safe_relative(const std::string &rel) {
    if (rel.empty() || rel[0] == '/' || rel.find(':') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t slash = rel.find('/', start);
        const std::string part = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..")
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

// the read side of the archive: a Win32 handle when the brokered path opens directly, else the WinRT stream
struct Reader {
    HANDLE h = INVALID_HANDLE_VALUE;
    winrt::Windows::Storage::Streams::IRandomAccessStream stream{ nullptr };
    uint64_t size = 0;
    ~Reader() {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        if (stream)
            stream.Close();
    }
    static size_t read(void *op, mz_uint64 ofs, void *buf, size_t n) {
        Reader *r = static_cast<Reader *>(op);
        size_t got = 0;
        uint8_t *dst = static_cast<uint8_t *>(buf);
        try {
            while (got < n) {
                const uint32_t want = static_cast<uint32_t>(std::min<size_t>(n - got, 16u * 1024 * 1024));
                const uint64_t at = ofs + got;
                uint32_t len = 0;
                if (r->h != INVALID_HANDLE_VALUE) {
                    OVERLAPPED o{};
                    o.Offset = static_cast<DWORD>(at);
                    o.OffsetHigh = static_cast<DWORD>(at >> 32);
                    DWORD d = 0;
                    if (!ReadFile(r->h, dst + got, want, &d, &o))
                        break;
                    len = d;
                } else {
                    r->stream.Seek(at);
                    winrt::Windows::Storage::Streams::Buffer b(want);
                    const auto res = r->stream.ReadAsync(b, want, winrt::Windows::Storage::Streams::InputStreamOptions::None).get();
                    len = res.Length();
                    if (len)
                        memcpy(dst + got, res.data(), len);
                }
                if (len == 0)
                    break;
                got += len;
            }
        } catch (...) {
        }
        return got;
    }
};

struct Source {
    ArchiveFile info;
    winrt::Windows::Storage::StorageFile file{ nullptr };
    std::wstring path;
};

struct Entry {
    mz_uint index = 0;
    std::string name; // '/' separators
    uint64_t size = 0;
    bool dir = false;
};

struct Root {
    std::string prefix;
    std::vector<size_t> entries; // into Impl::entries (files only)
};

} // namespace

struct Installer::Impl {
    std::wstring dev;
    mutable std::mutex m;
    Snapshot s; // guarded by m
    std::atomic<uint64_t> done{ 0 };
    std::atomic<bool> stop{ false };
    std::thread worker;
    std::atomic<bool> running{ false };
    std::vector<Source> sources; // written by the scan worker only
    size_t current = SIZE_MAX;   // 0.0.30.0: the archive being analysed / installed (index into sources)
    // the open archive (worker-owned between analyze and close)
    std::unique_ptr<Reader> reader;
    mz_zip_archive zip{};
    bool zip_open = false;
    std::vector<Entry> entries;
    std::vector<Root> roots; // parallel to s.items

    explicit Impl(std::wstring d) : dev(std::move(d)) {}
    ~Impl() {
        stop = true;
        join();
        close_zip();
    }
    void join() {
        if (worker.joinable())
            worker.join();
    }
    void close_zip() {
        if (zip_open)
            mz_zip_reader_end(&zip);
        zip_open = false;
        memset(&zip, 0, sizeof(zip));
        reader.reset();
        entries.clear();
        roots.clear();
    }
    void set_phase(Phase p, const std::wstring &msg = std::wstring(), bool keep_msg = false) {
        std::lock_guard<std::mutex> lock(m);
        s.phase = p;
        if (!keep_msg)
            s.message = msg;
        ++s.version;
    }
    template <typename F>
    void run(F f) {
        join();
        stop = false;
        running = true;
        worker = std::thread([this, f] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try {
                f();
            } catch (winrt::hresult_error const &e) {
                log_line(Log::Lifecycle, "installer: WinRT error 0x%08lX %s", static_cast<unsigned long>(e.code()), narrow(std::wstring(e.message())).c_str());
                set_phase(Phase::Done, L"Error: " + std::wstring(e.message()));
            } catch (std::exception const &e) {
                log_line(Log::Lifecycle, "installer: error %s", e.what());
                set_phase(Phase::Done, L"Error: " + widen(e.what()));
            }
            winrt::uninit_apartment();
            running = false;
        });
    }

    // ---------------------------------------------------------------- scan
    void walk(winrt::Windows::Storage::StorageFolder const &folder, int depth, std::vector<winrt::Windows::Storage::StorageFile> &out) {
        if (depth > 4 || stop || out.size() >= kMaxFiles)
            return;
        try {
            for (auto const &f : folder.GetFilesAsync().get()) {
                const std::wstring n(f.Name());
                if ((ends_with_ci(n, L".zip") || ends_with_ci(n, L".vpk")) && out.size() < kMaxFiles)
                    out.push_back(f);
            }
            for (auto const &d : folder.GetFoldersAsync().get()) {
                const std::wstring n(d.Name());
                if (n.empty() || n[0] == L'$' || n[0] == L'.' || n == L"System Volume Information")
                    continue;
                walk(d, depth + 1, out);
            }
        } catch (...) {
        }
    }
    void do_scan() {
        using namespace winrt::Windows::Storage;
        std::error_code ec;
        fs::remove_all(fs::path(dev) / L".vitox-install", ec); // what an interrupted installation left
        std::vector<Source> found;
        unsigned drives = 0;
        const auto t0 = GetTickCount64();
        for (StorageFolder const &d : KnownFolders::RemovableDevices().GetFoldersAsync().get()) {
            if (stop)
                return;
            ++drives;
            std::vector<StorageFile> files;
            bool queried = false;
            try {
                Search::QueryOptions opts(Search::CommonFileQuery::DefaultQuery, winrt::single_threaded_vector<winrt::hstring>({ L".zip", L".vpk" }));
                opts.FolderDepth(Search::FolderDepth::Deep);
                opts.IndexerOption(Search::IndexerOption::DoNotUseIndexer);
                for (auto const &f : d.CreateFileQueryWithOptions(opts).GetFilesAsync(0, kMaxFiles).get())
                    files.push_back(f);
                queried = true;
            } catch (winrt::hresult_error const &e) {
                log_line(Log::Lifecycle, "installer: deep query on '%s' failed 0x%08lX - walking the folders", narrow(std::wstring(d.DisplayName())).c_str(),
                    static_cast<unsigned long>(e.code()));
            }
            if (!queried)
                walk(d, 0, files);
            const std::wstring root(d.Path());
            for (StorageFile const &f : files) {
                Source src;
                src.file = f;
                src.path = f.Path();
                src.info.name = f.Name();
                src.info.drive = d.DisplayName();
                std::wstring folder = src.path.size() > src.info.name.size() ? src.path.substr(0, src.path.size() - src.info.name.size()) : L"";
                if (!root.empty() && folder.size() >= root.size() && _wcsnicmp(folder.c_str(), root.c_str(), root.size()) == 0)
                    folder = L"\\" + folder.substr(root.size());
                while (folder.size() > 1 && (folder.back() == L'\\' || folder.back() == L'/'))
                    folder.pop_back();
                while (folder.size() > 1 && folder[0] == L'\\' && folder[1] == L'\\')
                    folder.erase(0, 1);
                src.info.folder = folder.empty() ? L"\\" : folder;
                try {
                    src.info.size = f.GetBasicPropertiesAsync().get().Size();
                } catch (...) {
                }
                found.push_back(std::move(src));
            }
            log_line(Log::Lifecycle, "installer: drive '%s' (%s): %u archive(s)%s", narrow(std::wstring(d.DisplayName())).c_str(), narrow(root).c_str(),
                static_cast<unsigned>(files.size()), queried ? "" : " (folder walk)");
        }
        std::sort(found.begin(), found.end(), [](const Source &a, const Source &b) {
            const int d = _wcsicmp(a.info.drive.c_str(), b.info.drive.c_str());
            if (d)
                return d < 0;
            const int f = _wcsicmp(a.info.folder.c_str(), b.info.folder.c_str());
            return f ? f < 0 : _wcsicmp(a.info.name.c_str(), b.info.name.c_str()) < 0;
        });
        log_line(Log::Lifecycle, "installer: scan %u drive(s), %u archive(s) in %llu ms", drives, static_cast<unsigned>(found.size()),
            static_cast<unsigned long long>(GetTickCount64() - t0));
        sources = std::move(found);
        std::lock_guard<std::mutex> lock(m);
        s.files.clear();
        for (const Source &src : sources)
            s.files.push_back(src.info);
        s.drives = drives;
        s.message = drives == 0 ? L"No USB drive found. Connect a USB drive to the console, then press Y to look again."
            : s.files.empty()   ? L"No .zip or .vpk files on the USB drive. Copy your archives to it, then press Y to look again."
                                : L"";
        s.phase = Phase::Files;
        ++s.version;
    }

    // ---------------------------------------------------------------- analysis
    bool head(size_t e, std::vector<uint8_t> &out, size_t max) {
        out.clear();
        mz_zip_reader_extract_iter_state *it = mz_zip_reader_extract_iter_new(&zip, entries[e].index, 0);
        if (!it)
            return false;
        out.resize(static_cast<size_t>(std::min<uint64_t>(entries[e].size, max)));
        const size_t got = out.empty() ? 0 : mz_zip_reader_extract_iter_read(it, out.data(), out.size());
        mz_zip_reader_extract_iter_free(it);
        out.resize(got);
        return got > 0;
    }
    std::wstring miniz_error() {
        return widen(mz_zip_get_error_string(mz_zip_peek_last_error(&zip)));
    }
    fs::path homebrew_dest(const std::string &tid) {
        const fs::path root = fs::path(dev) / L"homebrew";
        std::error_code ec;
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2))
                continue;
            auto sfo = read_sfo_file(it->path() / L"sce_sys" / L"param.sfo");
            if (trim(sfo["TITLE_ID"]) == tid)
                return it->path();
        }
        fs::path p = root / widen(tid);
        for (int i = 1; path_exists(p) && i < 100; ++i) // a folder of that name holding another app: never overwritten
            p = root / (widen(tid) + L"_" + std::to_wstring(i));
        return p;
    }
    // every executable a launch needs must be loader-readable (vitox_self_check over the header)
    std::wstring exec_problem(const Root &r, bool modules) {
        std::vector<uint8_t> b;
        for (size_t e : r.entries) {
            const std::string rel = entries[e].name.substr(r.prefix.size());
            bool want = rel == "eboot.bin";
            if (!want && modules && rel.rfind("sce_module/", 0) == 0 && rel.find('/', 11) == std::string::npos) {
                const std::wstring w = widen(rel);
                want = ends_with_ci(w, L".suprx") || ends_with_ci(w, L".self") || ends_with_ci(w, L".sprx");
            }
            if (!want)
                continue;
            if (!head(e, b, 1024 * 1024))
                return widen(rel) + L" could not be read from the archive (" + miniz_error() + L")";
            const VitoxSelfCheck c = vitox_self_check(b.data(), b.size());
            log_line(Log::Lifecycle, "installer:   %s%s: %s %s -> %s", r.prefix.c_str(), rel.c_str(), c.kind.c_str(), c.detail.c_str(),
                c.readable ? "loader-readable" : "PROTECTED");
            if (!c.readable)
                return widen(rel) + L" is encrypted. Vitox installs decrypted (loader-readable) dumps only.";
        }
        return {};
    }
    void do_analyze(size_t file) {
        close_zip();
        if (file >= sources.size()) {
            set_phase(Phase::Files);
            return;
        }
        const Source &src = sources[file];
        current = file;
        {
            std::lock_guard<std::mutex> lock(m);
            s.archive = src.info.name;
            s.items.clear();
            s.installed = s.failed = 0;
            s.cancelled = s.cancelling = false;
            s.select_title_id.clear();
            s.done_bytes = s.total_bytes = 0;
            ++s.version;
        }
        reader = std::make_unique<Reader>();
        reader->h = CreateFileFromAppW(src.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD open_err = reader->h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        if (reader->h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            GetFileSizeEx(reader->h, &sz);
            reader->size = static_cast<uint64_t>(sz.QuadPart);
        } else {
            reader->stream = src.file.OpenReadAsync().get();
            reader->size = reader->stream.Size();
        }
        log_line(Log::Lifecycle, "installer: open '%s' %llu B via %s", narrow(src.path).c_str(), static_cast<unsigned long long>(reader->size),
            open_err ? ("WinRT stream (CreateFileFromApp error " + std::to_string(open_err) + ")").c_str() : "CreateFileFromApp");
        memset(&zip, 0, sizeof(zip));
        zip.m_pRead = &Reader::read;
        zip.m_pIO_opaque = reader.get();
        if (!mz_zip_reader_init(&zip, reader->size, 0)) {
            const std::wstring why = miniz_error();
            close_zip();
            log_line(Log::Lifecycle, "installer: not a zip archive: %s", narrow(why).c_str());
            set_phase(Phase::Done, L"“" + src.info.name + L"” is not a readable zip archive (" + why + L").");
            return;
        }
        zip_open = true;
        // every entry, then the content roots (folders holding sce_sys/param.sfo; none inside another)
        const mz_uint n = mz_zip_reader_get_num_files(&zip);
        std::unordered_map<std::string, size_t> by_name;
        std::vector<std::string> prefixes;
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st))
                continue;
            Entry e;
            e.index = i;
            e.name = st.m_filename;
            std::replace(e.name.begin(), e.name.end(), '\\', '/');
            e.size = st.m_uncomp_size;
            e.dir = mz_zip_reader_is_file_a_directory(&zip, i) || (!e.name.empty() && e.name.back() == '/');
            if (e.dir)
                continue;
            static const std::string kSfo = "sce_sys/param.sfo";
            if (e.name.size() >= kSfo.size() && e.name.compare(e.name.size() - kSfo.size(), kSfo.size(), kSfo) == 0
                && (e.name.size() == kSfo.size() || e.name[e.name.size() - kSfo.size() - 1] == '/'))
                prefixes.push_back(e.name.substr(0, e.name.size() - kSfo.size()));
            by_name[e.name] = entries.size();
            entries.push_back(std::move(e));
        }
        std::sort(prefixes.begin(), prefixes.end(), [](const std::string &a, const std::string &b) { return a.size() != b.size() ? a.size() < b.size() : a < b; });
        std::vector<std::string> kept;
        for (const std::string &p : prefixes)
            if (std::none_of(kept.begin(), kept.end(), [&](const std::string &q) { return p.rfind(q, 0) == 0; }))
                kept.push_back(p);
        log_line(Log::Lifecycle, "installer: '%s' %u entr(ies), %u content root(s)", narrow(src.info.name).c_str(), static_cast<unsigned>(entries.size()),
            static_cast<unsigned>(kept.size()));
        if (kept.empty()) {
            set_phase(Phase::Done, L"No Vita game, update, DLC or homebrew app in “" + src.info.name + L"” (no sce_sys/param.sfo inside).");
            return;
        }
        // pass 1: identity of every root
        std::vector<Item> items;
        for (const std::string &p : kept) {
            Root r{ p, {} };
            Item it;
            for (size_t e = 0; e < entries.size(); ++e)
                if (entries[e].name.rfind(p, 0) == 0) {
                    r.entries.push_back(e);
                    it.bytes += entries[e].size;
                    ++it.files;
                    if (it.installable && !safe_relative(entries[e].name.substr(p.size())))
                        it.installable = false, it.problem = L"The archive holds an unsafe path (" + widen(entries[e].name) + L")";
                }
            std::vector<uint8_t> sfo_bytes;
            const auto sfo_it = by_name.find(p + "sce_sys/param.sfo");
            if (sfo_it == by_name.end() || !head(sfo_it->second, sfo_bytes, 1024 * 1024)) {
                it.installable = false;
                it.problem = L"param.sfo could not be read (" + miniz_error() + L")";
            }
            auto sfo = parse_sfo(sfo_bytes.data(), sfo_bytes.size());
            const std::string cat = sfo["CATEGORY"], cid = sfo["CONTENT_ID"];
            std::string tid = sfo["TITLE_ID"];
            it.content_id = widen(cid);
            it.version = widen(sfo["APP_VER"].empty() ? sfo["VERSION"] : sfo["APP_VER"]);
            if (cat == "ac") {
                it.kind = Kind::Dlc;
                if (tid.empty() && cid.size() >= 16)
                    tid = cid.substr(7, 9);
            } else if (cat.find("gp") != std::string::npos) {
                it.kind = Kind::Update;
            } else {
                it.kind = tid.rfind("PCS", 0) == 0 ? Kind::Game : Kind::Homebrew;
            }
            it.title_id = widen(tid);
            it.title = widen(sfo["TITLE"].empty() ? tid : sfo["TITLE"]);
            for (wchar_t &c : it.title)
                if (c == L'\n' || c == L'\r')
                    c = L' ';
            if (it.installable && sfo.empty())
                it.installable = false, it.problem = L"param.sfo does not parse";
            if (it.installable && !valid_title_id(tid))
                it.installable = false, it.problem = L"The TITLE_ID “" + widen(tid) + L"” is not valid";
            if (it.installable && by_name.count(p + "sce_module/steroid.suprx"))
                it.installable = false, it.problem = L"This is a Vitamin dump, which is not supported";
            items.push_back(std::move(it));
            roots.push_back(std::move(r));
        }
        const auto game_in_zip = [&](const std::wstring &tid) {
            return std::any_of(items.begin(), items.end(), [&](const Item &x) { return x.kind == Kind::Game && x.title_id == tid && x.installable; });
        };
        // pass 2: destination + the checks that need it
        const fs::path staging = fs::path(dev) / L"app-staging";
        const fs::path licenses = fs::path(dev) / L"vita-fs" / L"ux0" / L"license";
        static const wchar_t kNonPdrmNote[] = L"Encrypted (NoNpDrm): decrypted while installing";
        for (size_t i = 0; i < items.size(); ++i) {
            Item &it = items[i];
            const Root &r = roots[i];
            const std::string tid = narrow(it.title_id);
            // 0.0.29.1: a NoNpDrm dump carries its license; upstream decrypts it at install (PCS titles only)
            const bool work_bin = by_name.count(r.prefix + "sce_sys/package/work.bin") != 0 && tid.rfind("PCS", 0) == 0;
            if (it.installable && work_bin && it.content_id.empty())
                it.installable = false, it.problem = L"The NoNpDrm dump's param.sfo has no CONTENT_ID";
            fs::path dest;
            switch (it.kind) {
            case Kind::Game:
            case Kind::Homebrew:
                dest = it.kind == Kind::Game ? staging / it.title_id : homebrew_dest(tid);
                if (it.installable && !by_name.count(r.prefix + "eboot.bin"))
                    it.installable = false, it.problem = L"There is no eboot.bin in this app";
                if (it.installable && work_bin)
                    it.nonpdrm = true, it.note = kNonPdrmNote; // checked after decryption (the PFS layer hides the SELF header)
                else if (it.installable)
                    if (std::wstring e = exec_problem(r, it.kind == Kind::Game); !e.empty())
                        it.installable = false, it.problem = e;
                it.replaces = path_exists(dest);
                break;
            case Kind::Update:
                dest = staging / it.title_id;
                it.replaces = true;
                if (it.installable && !path_exists(dest / L"eboot.bin") && !game_in_zip(it.title_id))
                    it.installable = false, it.problem = L"Install the game first: " + it.title_id + L" is not in the library";
                if (it.installable && work_bin) {
                    it.nonpdrm = true, it.note = kNonPdrmNote;
                } else if (it.installable) {
                    if (std::wstring e = exec_problem(r, true); !e.empty()) {
                        // upstream: an encrypted patch is decrypted with its GAME's license (ux0:license/<TITLE_ID>/<app CONTENT_ID>.rif)
                        std::wstring game_cid = widen(trim(read_sfo_file(dest / L"sce_sys" / L"param.sfo")["CONTENT_ID"]));
                        bool game_nonpdrm_here = false;
                        for (const Item &g : items)
                            if (g.kind == Kind::Game && g.title_id == it.title_id && g.installable)
                                game_cid = g.content_id, game_nonpdrm_here = g.nonpdrm || by_name.count(roots[&g - items.data()].prefix + "sce_sys/package/work.bin");
                        const fs::path rif = licenses / it.title_id / (game_cid + L".rif");
                        if (!game_cid.empty() && (game_nonpdrm_here || path_exists(rif)))
                            it.nonpdrm = true, it.license_src = rif.wstring(), it.note = L"Encrypted: decrypted with its game's license";
                        else
                            it.installable = false, it.problem = e;
                    }
                }
                break;
            case Kind::Dlc: {
                const std::string cid = narrow(it.content_id);
                const std::string leaf = cid.size() > 20 ? cid.substr(20) : std::string();
                const bool leaf_ok = !leaf.empty() && leaf.size() <= 32
                    && std::all_of(leaf.begin(), leaf.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
                if (it.installable && !leaf_ok)
                    it.installable = false, it.problem = L"The DLC's CONTENT_ID “" + it.content_id + L"” is not valid";
                if (it.installable && work_bin)
                    it.nonpdrm = true;
                dest = fs::path(dev) / L"vita-fs" / L"ux0" / L"addcont" / it.title_id / widen(leaf_ok ? leaf : "-");
                it.replaces = path_exists(dest);
                if (it.installable && !path_exists(staging / it.title_id / L"eboot.bin") && !game_in_zip(it.title_id))
                    it.note = L"Its game (" + it.title_id + L") is not installed yet";
                else if (it.nonpdrm)
                    it.note = kNonPdrmNote;
                break;
            }
            }
            it.dest = dest.wstring();
            log_line(Log::Lifecycle, "installer: item %s %s '%s' v%s %llu B %u file(s) -> %s%s%s%s", narrow(kind_name(it.kind)).c_str(), tid.c_str(),
                narrow(it.title).c_str(), narrow(it.version).c_str(), static_cast<unsigned long long>(it.bytes), it.files, narrow(it.dest).c_str(),
                it.replaces ? " (replaces/merges)" : "", it.nonpdrm ? (" | NoNpDrm, license " + (it.license_src.empty() ? std::string("work.bin") : narrow(it.license_src))).c_str() : "",
                it.installable ? "" : (" | NOT INSTALLABLE: " + narrow(it.problem)).c_str());
        }
        uint64_t need = 0;
        for (const Item &it : items)
            if (it.installable)
                need += it.bytes * (it.nonpdrm ? 2 : 1); // the encrypted copy is kept until its decryption finished
        ULARGE_INTEGER avail{}, total{}, free_all{};
        const bool got_space = GetDiskFreeSpaceExW(dev.c_str(), &avail, &total, &free_all) != 0;
        std::lock_guard<std::mutex> lock(m);
        s.items = std::move(items);
        s.need_bytes = need;
        s.free_bytes = got_space ? avail.QuadPart : 0;
        s.space_ok = !got_space || need + kSpaceMargin <= avail.QuadPart;
        s.message.clear();
        s.phase = Phase::Confirm;
        ++s.version;
    }

    // ---------------------------------------------------------------- installation
    struct WriteCtx {
        HANDLE h;
        Impl *impl;
        bool failed;
    };
    static size_t write_cb(void *op, mz_uint64, const void *buf, size_t n) {
        WriteCtx *c = static_cast<WriteCtx *>(op);
        if (c->impl->stop)
            return 0;
        DWORD w = 0;
        if (!WriteFile(c->h, buf, static_cast<DWORD>(n), &w, nullptr) || w != n) {
            c->failed = true;
            return 0;
        }
        c->impl->done += n;
        return n;
    }
    // extracts root i into `tmp`; empty = success
    std::wstring extract(size_t i, const fs::path &tmp) {
        const Root &r = roots[i];
        for (size_t e : r.entries) {
            if (stop)
                return L"cancelled";
            const std::string rel = entries[e].name.substr(r.prefix.size());
            const fs::path out = (tmp / widen(rel)).make_preferred();
            std::error_code ec;
            fs::create_directories(out.parent_path(), ec);
            {
                std::lock_guard<std::mutex> lock(m);
                s.current = widen(rel);
                ++s.version;
            }
            CREATEFILE2_EXTENDED_PARAMETERS cp{ sizeof(cp) };
            cp.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            cp.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
            const HANDLE h = CreateFile2(out.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, &cp);
            if (h == INVALID_HANDLE_VALUE)
                return L"could not create " + widen(rel) + L" (" + widen(win32_err(GetLastError())) + L")";
            WriteCtx ctx{ h, this, false };
            const bool ok = mz_zip_reader_extract_to_callback(&zip, entries[e].index, &write_cb, &ctx, 0) != 0;
            CloseHandle(h);
            if (!ok)
                return stop ? L"cancelled"
                    : ctx.failed ? L"writing " + widen(rel) + L" failed (is the console storage full?)"
                                 : L"the archive is damaged at " + widen(rel) + L" (" + miniz_error() + L")";
        }
        return {};
    }
    // moves every file of `from` over `to` (an update over its game)
    std::wstring merge(const fs::path &from, const fs::path &to) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2))
                continue;
            const fs::path dst = to / it->path().lexically_relative(from);
            fs::create_directories(dst.parent_path(), e2);
            if (!MoveFileExW(it->path().c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
                return L"could not replace " + dst.lexically_relative(to).wstring() + L" (" + widen(win32_err(GetLastError())) + L")";
        }
        return ec ? L"could not read the extracted update" : L"";
    }
    // 0.0.29.1: NoNpDrm -> decrypted copy in `dec` (core vitox_nonpdrm_decrypt = upstream decrypt_install_nonpdrm's
    // psvpfsparser step), then upstream copy_license (not for an update) and a check that the eboot is now a SELF the
    // loader can open (an NPDRM SELF: decrypted at load with the license). Empty = success.
    std::wstring decrypt(const Item &it, const fs::path &enc, const fs::path &dec) {
        const fs::path lic = it.license_src.empty() ? enc / L"sce_sys" / L"package" / L"work.bin" : fs::path(it.license_src);
        {
            std::lock_guard<std::mutex> lock(m);
            s.current = L"Decrypting (NoNpDrm)…";
            ++s.version;
        }
        std::error_code ec;
        fs::remove_all(dec, ec);
        struct P {
            Impl *impl;
            uint64_t base, bytes;
        } p{ this, done.load(), it.bytes };
        char e[256] = {};
        const auto t0 = GetTickCount64();
        const bool ok = vitox_nonpdrm_decrypt(enc.c_str(), dec.c_str(), lic.c_str(),
            [](void *u, uint64_t a, uint64_t t) {
                P *q = static_cast<P *>(u);
                if (t)
                    q->impl->done = q->base + static_cast<uint64_t>(static_cast<double>(q->bytes) * std::min(1.0, static_cast<double>(a) / static_cast<double>(t)));
                return !q->impl->stop.load();
            },
            &p, e, sizeof(e));
        done = p.base + it.bytes;
        log_line(Log::Lifecycle, "installer: NoNpDrm decrypt %s (license %s) -> %s in %llu ms", narrow(it.title_id).c_str(), narrow(lic.wstring()).c_str(),
            ok ? "OK" : e, static_cast<unsigned long long>(GetTickCount64() - t0));
        if (!ok)
            return strcmp(e, "cancelled") == 0 ? L"cancelled" : widen(e);
        if (it.kind != Kind::Update) {
            // upstream copy_license: <license content_id>.rif in ux0:license/<content_id[7:16]>
            std::ifstream f(lic, std::ios::binary);
            const std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string cid = b.size() >= 0x40 ? std::string(b.data() + 0x10, strnlen(b.data() + 0x10, 0x30)) : std::string();
            if (cid.size() < 16)
                return L"the license (work.bin) has no CONTENT_ID";
            const fs::path dst = fs::path(dev) / L"vita-fs" / L"ux0" / L"license" / widen(cid.substr(7, 9));
            fs::create_directories(dst, ec);
            fs::copy_file(lic, dst / (widen(cid) + L".rif"), fs::copy_options::overwrite_existing, ec);
            log_line(Log::Lifecycle, "installer: license %s -> %s\\%s.rif %s", cid.c_str(), narrow(dst.wstring()).c_str(), cid.c_str(), ec ? ec.message().c_str() : "OK");
            if (ec)
                return L"the license could not be installed (" + widen(ec.message()) + L")";
        }
        if (it.kind == Kind::Game || path_exists(dec / L"eboot.bin")) {
            std::ifstream f(dec / L"eboot.bin", std::ios::binary);
            std::vector<uint8_t> h(1024 * 1024);
            f.read(reinterpret_cast<char *>(h.data()), static_cast<std::streamsize>(h.size()));
            h.resize(static_cast<size_t>(f.gcount()));
            const VitoxSelfCheck c = vitox_self_check(h.data(), h.size());
            log_line(Log::Lifecycle, "installer:   decrypted eboot.bin: %s %s", c.kind.c_str(), c.detail.c_str());
            if (!c.readable && !(c.kind == "SELF" && c.segments > 0))
                return L"the decrypted eboot.bin is not a loadable program (" + widen(c.detail) + L")";
        }
        return {};
    }
    void do_install() {
        std::vector<size_t> order;
        std::vector<Item> items;
        {
            std::lock_guard<std::mutex> lock(m);
            items = s.items;
            s.total_bytes = 0;
            for (size_t i = 0; i < items.size(); ++i)
                if (items[i].installable) {
                    order.push_back(i);
                    s.total_bytes += items[i].bytes * (items[i].nonpdrm ? 2 : 1); // extraction + decryption
                }
            s.done_bytes = 0;
            s.installed = s.failed = 0;
            s.phase = Phase::Installing;
            ++s.version;
        }
        done = 0;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return static_cast<int>(items[a].kind) < static_cast<int>(items[b].kind); });
        const fs::path work = fs::path(dev) / L".vitox-install";
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        log_line(Log::Lifecycle, "installer: install %u item(s) from '%s'", static_cast<unsigned>(order.size()), narrow(s.archive).c_str());
        unsigned n = 0;
        for (size_t i : order) {
            Item &it = items[i];
            std::wstring err;
            const fs::path dest(it.dest), tmp = work / (L"new-" + std::to_wstring(n)), old = work / (L"old-" + std::to_wstring(n)),
                                          dec = work / (L"dec-" + std::to_wstring(n));
            ++n;
            if (stop) {
                err = L"cancelled";
            } else {
                fs::remove_all(tmp, ec);
                fs::create_directories(tmp, ec);
                const auto t0 = GetTickCount64();
                err = extract(i, tmp);
                // 0.0.29.1: a NoNpDrm dump -> its decrypted copy (upstream decrypt_install_nonpdrm), which is what is installed
                fs::path src = tmp;
                if (err.empty() && it.nonpdrm) {
                    err = decrypt(it, tmp, dec);
                    if (err.empty())
                        src = dec;
                }
                if (err.empty() && it.kind == Kind::Update) {
                    // merged over the game (installed before it when both are in this archive)
                    err = path_exists(dest / L"eboot.bin") ? merge(src, dest) : L"its game is not installed";
                } else if (err.empty()) {
                    fs::create_directories(dest.parent_path(), ec);
                    const bool had = path_exists(dest);
                    std::error_code e1, e2;
                    if (had)
                        fs::rename(dest, old, e1);
                    if (e1) {
                        err = L"the installed copy could not be replaced (" + widen(e1.message()) + L")";
                    } else {
                        fs::rename(src, dest, e2);
                        if (e2) {
                            err = L"could not move the files into place (" + widen(e2.message()) + L")";
                            if (had)
                                fs::rename(old, dest, e1); // the previous install comes back
                        }
                    }
                }
                log_line(Log::Lifecycle, "installer: %s %s -> %s in %llu ms%s", narrow(kind_name(it.kind)).c_str(), narrow(it.title_id).c_str(),
                    err.empty() ? "INSTALLED" : "FAILED", static_cast<unsigned long long>(GetTickCount64() - t0), err.empty() ? "" : (" | " + narrow(err)).c_str());
            }
            fs::remove_all(tmp, ec);
            fs::remove_all(dec, ec);
            fs::remove_all(old, ec);
            std::lock_guard<std::mutex> lock(m);
            Item &si = s.items[i];
            si.done = true;
            si.ok = err.empty();
            si.result = err.empty() ? (it.kind == Kind::Update ? L"Update installed" : it.replaces ? L"Reinstalled" : L"Installed") : (err == L"cancelled" ? L"Not installed (cancelled)" : err);
            if (si.ok) {
                ++s.installed;
                if (s.select_title_id.empty() && (it.kind == Kind::Game || it.kind == Kind::Homebrew))
                    s.select_title_id = narrow(it.title_id);
            } else {
                ++s.failed;
            }
            ++s.version;
        }
        fs::remove_all(work, ec);
        close_zip();
        std::lock_guard<std::mutex> lock(m);
        s.cancelled = stop;
        s.cancelling = false;
        s.done_bytes = done;
        s.current.clear();
        s.message = s.cancelled ? L"Installation cancelled" : s.failed == 0 ? L"Installation complete" : s.installed ? L"Installed with errors" : L"Installation failed";
        // 0.0.30.0: the archive is offered for deletion only when everything in it is installed
        s.can_delete_archive = !s.cancelled && s.installed > 0 && std::all_of(s.items.begin(), s.items.end(), [](const Item &x) { return x.ok; });
        s.archive_deleted = s.deleting_archive = false;
        s.delete_error.clear();
        s.phase = Phase::Done;
        ++s.version;
    }
};

Installer::Installer(std::wstring dev_root) : impl_(std::make_unique<Impl>(std::move(dev_root))) {}
Installer::~Installer() = default;

void Installer::scan() {
    Impl &m = *impl_;
    m.stop = true;
    m.join();
    m.close_zip();
    {
        std::lock_guard<std::mutex> lock(m.m);
        m.s = Snapshot{ Phase::Scanning, m.s.version + 1 };
    }
    m.run([&m] { m.do_scan(); });
}

void Installer::analyze(size_t file) {
    Impl &m = *impl_;
    m.join();
    m.set_phase(Phase::Analyzing);
    m.run([&m, file] { m.do_analyze(file); });
}

void Installer::back() {
    Impl &m = *impl_;
    m.join();
    m.close_zip();
    std::lock_guard<std::mutex> lock(m.m);
    m.s.items.clear();
    m.s.installed = m.s.failed = 0; // (the launcher took the results before going back)
    m.s.can_delete_archive = m.s.archive_deleted = m.s.deleting_archive = false;
    m.s.delete_error.clear();
    m.s.select_title_id.clear();
    m.s.message = m.s.drives == 0 ? L"No USB drive found. Connect a USB drive to the console, then press Y to look again."
        : m.s.files.empty()       ? L"No .zip or .vpk files on the USB drive. Copy your archives to it, then press Y to look again."
                                  : L"";
    m.s.phase = Phase::Files;
    ++m.s.version;
}

void Installer::install() {
    Impl &m = *impl_;
    m.join();
    m.run([&m] { m.do_install(); });
}

void Installer::delete_archive() {
    Impl &m = *impl_;
    {
        std::lock_guard<std::mutex> lock(m.m);
        if (m.s.phase != Phase::Done || !m.s.can_delete_archive || m.s.archive_deleted || m.s.deleting_archive || m.current >= m.sources.size())
            return;
        m.s.deleting_archive = true;
        ++m.s.version;
    }
    m.join();
    m.run([&m] {
        const Source &src = m.sources[m.current];
        std::wstring err;
        try {
            // the archive is closed (do_install ended with close_zip); removableStorage grants .zip/.vpk on the drive
            src.file.DeleteAsync(winrt::Windows::Storage::StorageDeleteOption::PermanentDelete).get();
        } catch (winrt::hresult_error const &e) {
            err = std::wstring(e.message());
            if (err.empty())
                err = L"error 0x" + std::to_wstring(static_cast<unsigned long>(e.code()));
        }
        log_line(Log::Lifecycle, "installer: delete '%s' from the USB drive -> %s", narrow(src.path).c_str(), err.empty() ? "DELETED" : narrow(err).c_str());
        std::lock_guard<std::mutex> lock(m.m);
        m.s.deleting_archive = false;
        m.s.archive_deleted = err.empty();
        m.s.delete_error = err;
        if (err.empty()) {
            // gone from the file list too (the next Files view)
            for (size_t i = 0; i < m.s.files.size(); ++i)
                if (m.s.files[i].name == src.info.name && m.s.files[i].folder == src.info.folder && m.s.files[i].drive == src.info.drive) {
                    m.s.files.erase(m.s.files.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            m.sources.erase(m.sources.begin() + static_cast<std::ptrdiff_t>(m.current));
            m.current = SIZE_MAX;
        }
        ++m.s.version;
    });
}

uint64_t dir_size(const std::wstring &dir) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::none, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2))
            n += it->file_size(e2);
    }
    return n;
}

std::wstring uninstall(const std::wstring &dev_root, const std::wstring &local_state, const std::string &app_root, const std::string &folder,
    const std::string &title_id, bool shared_title_id, bool delete_saves, uint64_t *freed) {
    // every path is built from validated names below fixed roots inside dev_root (no separators, no "..")
    const auto plain = [](const std::string &s) {
        return !s.empty() && s != "." && s != ".." && s.find_first_of("/\\:") == std::string::npos;
    };
    if ((app_root != "homebrew" && app_root != "app-staging") || !plain(folder) || !valid_title_id(title_id))
        return L"refused: not an installed library app";
    const fs::path dev(dev_root), ux0 = dev / L"vita-fs" / L"ux0";
    const fs::path tid = widen(title_id);
    std::vector<fs::path> targets{ dev / widen(app_root) / widen(folder) };
    if (!shared_title_id) {
        targets.push_back(ux0 / L"app" / tid);
        targets.push_back(ux0 / L"addcont" / tid);
        if (app_root == "app-staging")
            targets.push_back(ux0 / L"license" / tid);
        if (delete_saves)
            targets.push_back(ux0 / L"user" / L"00" / L"savedata" / tid);
        if (!local_state.empty())
            targets.push_back(fs::path(local_state) / L"artwork" / tid);
    }
    uint64_t total = 0;
    std::wstring err;
    for (const fs::path &t : targets) {
        if (!path_exists(t))
            continue;
        const uint64_t sz = dir_size(t.wstring());
        std::error_code ec;
        fs::remove_all(t, ec);
        log_line(Log::Lifecycle, "uninstall %s: remove '%s' (%llu B) -> %s", title_id.c_str(), narrow(t.wstring()).c_str(), static_cast<unsigned long long>(sz),
            ec ? ec.message().c_str() : "OK");
        if (ec && err.empty())
            err = L"could not remove " + t.wstring() + L" (" + widen(ec.message()) + L")";
        else if (!ec)
            total += sz;
    }
    if (freed)
        *freed = total;
    return err;
}

void Installer::cancel() {
    Impl &m = *impl_;
    m.stop = true;
    std::lock_guard<std::mutex> lock(m.m);
    m.s.cancelling = true;
    ++m.s.version;
}

void Installer::close() {
    Impl &m = *impl_;
    m.stop = true;
    m.join();
    m.close_zip();
    std::lock_guard<std::mutex> lock(m.m);
    m.s = Snapshot{ Phase::Idle, m.s.version + 1 };
}

bool Installer::busy() const { return impl_->running; }

bool Installer::poll(uint64_t have, Snapshot &out) {
    Impl &m = *impl_;
    std::lock_guard<std::mutex> lock(m.m);
    m.s.done_bytes = m.done;
    if (m.s.version == have && m.s.phase != Phase::Installing)
        return false;
    out = m.s;
    return true;
}

} // namespace install
} // namespace vitox
