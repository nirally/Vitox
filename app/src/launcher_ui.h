// Vitox 0.0.25.0: the Vitox launcher - the library of installed Vita games / homebrew (found by the launcher's own
// discovery, vitox_launcher_discover) and a Settings tab, drawn on the CoreWindow with D3D11 (the animated background,
// shaders/launcher_bg.hlsl) + Direct2D/DirectWrite (cards, text, artwork). Controller only (Windows.Gaming.Input).
#pragma once

#include "common.h"

#include <vitox_homebrew_launcher.h>

#include <winrt/Windows.UI.Core.h>

#include <functional>
#include <memory>
#include <string>

namespace vitox {

struct LauncherHost {
    std::wstring version;            // the installed package version (Package.Current), shown in Settings
    std::wstring local_state;        // 0.0.26.0: LocalState (vitox-input.txt, steamgriddb-key.txt, artwork\<TITLE_ID>\)
    std::function<bool()> closed;    // the window was closed: leave
    std::function<bool()> visible;   // false: no frames (the app is in the background)
};

struct LauncherChoice {
    bool launch = false;
    VitoxAppInfo app{};
};

class LauncherUi {
public:
    LauncherUi();
    ~LauncherUi();
    // the CoreWindow holds one swapchain at a time: init before run, release before a game takes the window
    bool init(winrt::Windows::UI::Core::CoreWindow const &window);
    void release();
    // frames until the user starts an app (or the window closes). `preselect` = TITLE_ID or folder to highlight;
    // `toast` = a message shown briefly (e.g. how the last session ended)
    LauncherChoice run(winrt::Windows::UI::Core::CoreWindow const &window, const LauncherHost &host, const std::string &preselect, const std::wstring &toast);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vitox
