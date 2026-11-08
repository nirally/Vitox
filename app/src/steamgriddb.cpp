// Vitox 0.0.26.0: SteamGridDB client (see steamgriddb.h).
#include "steamgriddb.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <algorithm>
#include <chrono>
#include <fstream>

using winrt::com_ptr;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Web::Http;

namespace vitox::sgdb {

namespace {

constexpr const wchar_t *kApi = L"https://www.steamgriddb.com/api/v2";

std::wstring hex(uint32_t v) {
    wchar_t b[16];
    swprintf_s(b, L"0x%08X", v);
    return b;
}

// a user-facing message for a transport failure
std::wstring net_error(const winrt::hresult_error &e) {
    const uint32_t c = static_cast<uint32_t>(e.code());
    switch (c) {
    case 0x80072EE7: // WININET_E_NAME_NOT_RESOLVED
    case 0x80072EFD: // cannot connect
    case 0x80072EFE: // connection aborted
    case 0x80072EFF: // connection reset
        return L"No internet connection (" + hex(c) + L")";
    case 0x80072EE2:
        return L"SteamGridDB did not answer in time";
    case 0x80072F8F:
    case 0x80072F0D:
        return L"Secure connection failed - check the console's date and time (" + hex(c) + L")";
    default:
        return L"Network error " + hex(c);
    }
}

struct Response {
    int status = 0;
    std::vector<uint8_t> body;
};

// GET with a timeout; throws winrt::hresult_error on transport failure
Response http_get(HttpClient &client, const std::wstring &url, const std::wstring &key) {
    HttpRequestMessage req(HttpMethod::Get(), Uri(url));
    if (!key.empty())
        req.Headers().Authorization(Headers::HttpCredentialsHeaderValue(L"Bearer", key));
    req.Headers().UserAgent().TryParseAdd(L"Vitox/0.0.26");
    auto op = client.SendRequestAsync(req, HttpCompletionOption::ResponseContentRead);
    if (op.wait_for(std::chrono::seconds(20)) != AsyncStatus::Completed) {
        if (op.Status() == AsyncStatus::Started) {
            op.Cancel();
            throw winrt::hresult_error(static_cast<winrt::hresult>(0x80072EE2));
        }
    }
    HttpResponseMessage resp = op.GetResults();
    Response r;
    r.status = static_cast<int>(resp.StatusCode());
    auto rd = resp.Content().ReadAsBufferAsync();
    if (rd.wait_for(std::chrono::seconds(30)) != AsyncStatus::Completed) {
        rd.Cancel();
        throw winrt::hresult_error(static_cast<winrt::hresult>(0x80072EE2));
    }
    auto buf = rd.GetResults();
    r.body.assign(buf.data(), buf.data() + buf.Length());
    return r;
}

std::wstring utf8_to_w(const std::vector<uint8_t> &b) {
    if (b.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char *>(b.data()), static_cast<int>(b.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char *>(b.data()), static_cast<int>(b.size()), w.data(), n);
    return w;
}

std::wstring jstr(const JsonObject &o, const wchar_t *k) {
    if (!o.HasKey(k))
        return {};
    const IJsonValue v = o.Lookup(k);
    return v.ValueType() == JsonValueType::String ? std::wstring(v.GetString()) : std::wstring();
}
double jnum(const JsonObject &o, const wchar_t *k) {
    if (!o.HasKey(k))
        return 0;
    const IJsonValue v = o.Lookup(k);
    return v.ValueType() == JsonValueType::Number ? v.GetNumber() : 0;
}
bool jbool(const JsonObject &o, const wchar_t *k) {
    if (!o.HasKey(k))
        return false;
    const IJsonValue v = o.Lookup(k);
    return v.ValueType() == JsonValueType::Boolean && v.GetBoolean();
}

// parses an API answer; false + r.error on failure (HTTP status / "success": false / not JSON)
bool api_json(const Response &resp, Result &r, JsonArray &data) {
    r.http = resp.status;
    if (resp.status == 401 || resp.status == 403) {
        r.error = L"SteamGridDB rejected the API key";
        return false;
    }
    if (resp.status == 404) {
        data = JsonArray();
        return true; // "game not found" = nothing to show
    }
    JsonObject o{ nullptr };
    if (!JsonObject::TryParse(utf8_to_w(resp.body), o)) {
        r.error = L"SteamGridDB answered HTTP " + std::to_wstring(resp.status) + L" (not JSON)";
        return false;
    }
    if (resp.status != 200 || !jbool(o, L"success")) {
        std::wstring why;
        if (o.HasKey(L"errors") && o.Lookup(L"errors").ValueType() == JsonValueType::Array) {
            const JsonArray e = o.GetNamedArray(L"errors");
            if (e.Size() > 0 && e.GetAt(0).ValueType() == JsonValueType::String)
                why = e.GetAt(0).GetString();
        }
        r.error = L"SteamGridDB error (HTTP " + std::to_wstring(resp.status) + L")" + (why.empty() ? L"" : L": " + why);
        return false;
    }
    if (o.HasKey(L"data") && o.Lookup(L"data").ValueType() == JsonValueType::Array)
        data = o.GetNamedArray(L"data");
    else
        data = JsonArray();
    return true;
}

com_ptr<IWICBitmap> decode_memory(IWICImagingFactory *f, const std::vector<uint8_t> &bytes, uint32_t max_w) {
    com_ptr<IWICBitmap> out;
    if (!f || bytes.empty())
        return out;
    com_ptr<IWICStream> stream;
    com_ptr<IWICBitmapDecoder> dec;
    com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(f->CreateStream(stream.put())) || FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()), static_cast<DWORD>(bytes.size())))
        || FAILED(f->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put())) || FAILED(dec->GetFrame(0, frame.put())))
        return out;
    com_ptr<IWICBitmapSource> src = frame;
    UINT fw = 0, fh = 0;
    frame->GetSize(&fw, &fh);
    com_ptr<IWICBitmapScaler> scaler;
    if (max_w && fw > max_w && SUCCEEDED(f->CreateBitmapScaler(scaler.put()))
        && SUCCEEDED(scaler->Initialize(frame.get(), max_w, std::max<UINT>(1, fh * max_w / fw), WICBitmapInterpolationModeFant)))
        src = scaler;
    com_ptr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(conv.put()))
        || FAILED(conv->Initialize(src.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        return out;
    f->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, out.put());
    return out;
}

void parse_images(const JsonArray &data, bool icon, std::vector<Image> &out) {
    for (const IJsonValue &v : data) {
        if (v.ValueType() != JsonValueType::Object)
            continue;
        const JsonObject o = v.GetObject();
        Image im;
        im.id = static_cast<int64_t>(jnum(o, L"id"));
        im.url = jstr(o, L"url");
        im.thumb = jstr(o, L"thumb");
        im.style = jstr(o, L"style");
        im.width = static_cast<int>(jnum(o, L"width"));
        im.height = static_cast<int>(jnum(o, L"height"));
        im.icon = icon;
        if (o.HasKey(L"author") && o.Lookup(L"author").ValueType() == JsonValueType::Object)
            im.author = jstr(o.GetNamedObject(L"author"), L"name");
        if (!im.url.empty())
            out.push_back(std::move(im));
    }
}

thread_local HttpClient *t_client = nullptr;
thread_local IWICImagingFactory *t_wic = nullptr;

} // namespace

Client::Client() {
    for (int i = 0; i < 3; ++i)
        threads_.emplace_back([this] { worker(); });
}

Client::~Client() {
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    for (std::thread &t : threads_)
        if (t.joinable())
            t.join();
}

void Client::set_key(const std::wstring &key) {
    std::lock_guard<std::mutex> lock(m_);
    key_ = key;
}
std::wstring Client::key_copy() {
    std::lock_guard<std::mutex> lock(m_);
    return key_;
}

void Client::cancel_pending() {
    std::lock_guard<std::mutex> lock(m_);
    for (Job &j : jobs_) {
        j.h->error = L"cancelled";
        j.h->done = true;
    }
    jobs_.clear();
}

void Client::submit(Handle h, std::function<void(Result &)> run, bool urgent) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (urgent)
            jobs_.push_front({ h, std::move(run) });
        else
            jobs_.push_back({ h, std::move(run) });
    }
    cv_.notify_one();
}

void Client::worker() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    HttpClient client;
    com_ptr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), wic.put_void());
    t_client = &client;
    t_wic = wic.get();
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_)
                break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            job.run(*job.h);
        } catch (const winrt::hresult_error &e) {
            job.h->ok = false;
            job.h->error = net_error(e);
            log_line(Log::Lifecycle, "launcher sgdb: request failed hr=0x%08X %s", static_cast<unsigned>(e.code()), winrt::to_string(e.message()).c_str());
        } catch (...) {
            job.h->ok = false;
            job.h->error = L"Unexpected error";
        }
        job.h->done = true;
    }
    t_client = nullptr;
    t_wic = nullptr;
    wic = nullptr;
    client.Close();
    winrt::uninit_apartment();
}

Handle Client::validate() {
    auto h = std::make_shared<Result>();
    const std::wstring key = key_copy();
    submit(h, [key](Result &r) {
        if (key.empty()) {
            r.error = L"No API key";
            return;
        }
        const Response resp = http_get(*t_client, std::wstring(kApi) + L"/search/autocomplete/vita", key);
        JsonArray data{ nullptr };
        r.ok = api_json(resp, r, data);
        log_line(Log::Lifecycle, "launcher sgdb: key check HTTP %d -> %s", resp.status, r.ok ? "valid" : "REJECTED");
    }, true);
    return h;
}

Handle Client::search(const std::wstring &term) {
    auto h = std::make_shared<Result>();
    const std::wstring key = key_copy();
    submit(h, [key, term](Result &r) {
        const std::wstring url = std::wstring(kApi) + L"/search/autocomplete/" + std::wstring(Uri::EscapeComponent(term));
        const Response resp = http_get(*t_client, url, key);
        JsonArray data{ nullptr };
        if (!api_json(resp, r, data))
            return;
        for (const IJsonValue &v : data) {
            if (v.ValueType() != JsonValueType::Object)
                continue;
            const JsonObject o = v.GetObject();
            Game g;
            g.id = static_cast<int64_t>(jnum(o, L"id"));
            g.name = jstr(o, L"name");
            g.verified = jbool(o, L"verified");
            const double rel = jnum(o, L"release_date");
            if (rel > 0) {
                const time_t t = static_cast<time_t>(rel);
                tm u{};
                gmtime_s(&u, &t);
                g.year = u.tm_year + 1900;
            }
            if (g.id)
                r.games.push_back(std::move(g));
        }
        r.ok = true;
        log_line(Log::Lifecycle, "launcher sgdb: search '%s' -> %u game(s)", narrow(term).c_str(), static_cast<unsigned>(r.games.size()));
    }, true);
    return h;
}

Handle Client::images(int64_t game_id, Kind kind) {
    auto h = std::make_shared<Result>();
    const std::wstring key = key_copy();
    submit(h, [key, game_id, kind](Result &r) {
        const std::wstring id = std::to_wstring(game_id);
        const std::wstring common = L"mimes=image/png,image/jpeg&types=static&nsfw=false";
        JsonArray data{ nullptr };
        if (kind == Kind::Card) {
            // square grids fit the library's square cards; icons are the fallback for games without square grids
            Response resp = http_get(*t_client, std::wstring(kApi) + L"/grids/game/" + id + L"?dimensions=512x512,1024x1024&" + common, key);
            if (!api_json(resp, r, data))
                return;
            parse_images(data, false, r.images);
            resp = http_get(*t_client, std::wstring(kApi) + L"/icons/game/" + id + L"?mimes=image/png&types=static&nsfw=false", key); // icons: png only
            if (api_json(resp, r, data))
                parse_images(data, true, r.images);
        } else {
            const Response resp = http_get(*t_client, std::wstring(kApi) + L"/heroes/game/" + id + L"?" + common, key);
            if (!api_json(resp, r, data))
                return;
            parse_images(data, false, r.images);
        }
        if (r.images.size() > 60)
            r.images.resize(60);
        r.ok = true;
        r.error.clear();
        log_line(Log::Lifecycle, "launcher sgdb: game %lld %s -> %u image(s)", static_cast<long long>(game_id), kind == Kind::Card ? "card art" : "backgrounds",
            static_cast<unsigned>(r.images.size()));
    }, true);
    return h;
}

Handle Client::thumb(const std::wstring &url, uint32_t max_w, const std::wstring &fallback_url) {
    auto h = std::make_shared<Result>();
    submit(h, [url, max_w, fallback_url](Result &r) {
        for (const std::wstring &u : { url, fallback_url }) {
            if (u.empty())
                continue;
            const Response resp = http_get(*t_client, u, {}); // the CDN: no key
            r.http = resp.status;
            if (resp.status == 200)
                r.bitmap = decode_memory(t_wic, resp.body, max_w);
            if (r.bitmap)
                break;
        }
        r.ok = r.bitmap != nullptr;
        if (!r.ok)
            r.error = L"thumbnail unavailable";
    }, false);
    return h;
}

Handle Client::download(const std::wstring &url, const std::wstring &dest_path) {
    auto h = std::make_shared<Result>();
    submit(h, [url, dest_path](Result &r) {
        const Response resp = http_get(*t_client, url, {});
        r.http = resp.status;
        if (resp.status != 200) {
            r.error = L"Download failed (HTTP " + std::to_wstring(resp.status) + L")";
            return;
        }
        r.bitmap = decode_memory(t_wic, resp.body, 1280);
        if (!r.bitmap) {
            r.error = L"The image could not be decoded";
            return;
        }
        // create the folders, then write a temp file and move it over the old artwork
        for (size_t p = dest_path.find(L'\\', 3); p != std::wstring::npos; p = dest_path.find(L'\\', p + 1))
            CreateDirectoryW(dest_path.substr(0, p).c_str(), nullptr);
        const std::wstring tmp = dest_path + L".part";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char *>(resp.body.data()), static_cast<std::streamsize>(resp.body.size()));
            if (!f) {
                r.error = L"Could not save the artwork";
                return;
            }
        }
        if (!MoveFileExW(tmp.c_str(), dest_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            r.error = L"Could not save the artwork";
            return;
        }
        r.ok = true;
        log_line(Log::Lifecycle, "launcher sgdb: saved %u bytes -> %s", static_cast<unsigned>(resp.body.size()), narrow(dest_path).c_str());
    }, true);
    return h;
}

std::wstring load_key(const std::wstring &local_state) {
    std::ifstream f(local_state + L"\\steamgriddb-key.txt");
    std::string s;
    std::getline(f, s);
    std::string k;
    for (char c : s)
        if (c > ' ' && c < 127)
            k += c;
    return widen(k);
}

bool save_key(const std::wstring &local_state, const std::wstring &key) {
    const std::wstring path = local_state + L"\\steamgriddb-key.txt";
    if (key.empty())
        return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    std::ofstream f(path, std::ios::trunc);
    f << narrow(key) << "\n";
    return static_cast<bool>(f);
}

} // namespace vitox::sgdb
