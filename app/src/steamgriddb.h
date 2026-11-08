// Vitox 0.0.26.0: a minimal SteamGridDB (https://www.steamgriddb.com/api/v2) client for the launcher's custom artwork.
// Requests run on worker threads (Windows.Web.Http + Windows.Data.Json, no extra libraries); the UI thread polls each
// request's Result (`done`). The user's API key is sent only to www.steamgriddb.com, never to the image CDN.
#pragma once

#include "common.h"

#include <wincodec.h>

#include <winrt/base.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vitox::sgdb {

struct Game {
    int64_t id = 0;
    std::wstring name;
    bool verified = false;
    int year = 0;
};

struct Image {
    int64_t id = 0;
    std::wstring url, thumb, author, style;
    int width = 0, height = 0;
    bool icon = false; // from /icons (card art fallback), not /grids
};

enum class Kind { Card, Hero };

struct Result {
    std::atomic<bool> done{ false };
    bool ok = false;
    int http = 0;
    std::wstring error; // user-facing
    std::vector<Game> games;
    std::vector<Image> images;
    winrt::com_ptr<IWICBitmap> bitmap; // thumb / download preview
};
using Handle = std::shared_ptr<Result>;

class Client {
public:
    Client();
    ~Client();
    void set_key(const std::wstring &key);
    Handle validate();                                  // ok = the key works
    Handle search(const std::wstring &term);            // games
    Handle images(int64_t game_id, Kind kind);          // images (Card: square grids, then icons; Hero: heroes)
    Handle thumb(const std::wstring &url, uint32_t max_w, const std::wstring &fallback_url = {}); // bitmap
    Handle download(const std::wstring &url, const std::wstring &dest_path); // ok = saved (and decodable)
    void cancel_pending(); // drops the queued (not yet started) requests

private:
    struct Job {
        Handle h;
        std::function<void(Result &)> run;
    };
    void submit(Handle h, std::function<void(Result &)> run, bool urgent);
    void worker();
    std::wstring key_copy();
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    std::wstring key_;
};

// the key file in LocalState (plain text, one line)
std::wstring load_key(const std::wstring &local_state);
bool save_key(const std::wstring &local_state, const std::wstring &key);

} // namespace vitox::sgdb
