// Vitox M8F.2B: the host text source for Vita3K's IME dialog (compat/vitox_host_text.h), the smallest CoreWindow path:
// Windows.UI.Text.Core.CoreTextEditContext (InputScope Default, InputPaneDisplayPolicy Manual) for the text, and the
// system keyboard requested with CoreInputView::TryShow(Keyboard) (InputPane::TryShow only as fallback). No XAML.
// Everything runs on the CoreWindow UI thread (created in host_text_start, called by the launcher's run loop, events
// dispatched by its CoreWindow pump). The launcher mirrors the text into Vita3K's shared Ime state through the REAL
// ime_* helpers; completion stays with Vita3K's common_dialog_overlay (Cross / Circle).
#include "common.h"

#include <vitox_host_text.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text::Core;
using namespace winrt::Windows::UI::ViewManagement;
using namespace winrt::Windows::UI::ViewManagement::Core;

namespace {

CoreTextEditContext g_ctx{ nullptr };
CoreInputView g_view{ nullptr };
InputPane g_pane{ nullptr };
// the keyboard view / input pane are per-view singletons: their handlers are revoked on stop
CoreInputView::PrimaryViewShowing_revoker g_rev_showing;
CoreInputView::PrimaryViewHiding_revoker g_rev_hiding;
InputPane::Showing_revoker g_rev_pane_showing;
InputPane::Hiding_revoker g_rev_pane_hiding;
CoreWindow::KeyDown_revoker g_rev_keydown;
bool g_focused = false; // between show() and hide()
std::atomic<bool> g_enter{ false };
std::u16string g_text;
CoreTextRange g_sel{ 0, 0 };
uint32_t g_max = 0;
std::atomic<uint32_t> g_gen{ 0 };
std::atomic<bool> g_visible{ false };
bool g_started = false;
unsigned g_updates = 0, g_rejected = 0;

std::string u8(const std::u16string &s) {
    return vitox::narrow(std::wstring(s.begin(), s.end()));
}
std::string u8(winrt::hstring const &s) { return winrt::to_string(s); }

void log_hr(const char *what, winrt::hresult_error const &e) {
    vitox::log_line(vitox::Log::Core, "m8f.2b host text: %s FAILED hr=0x%08X %s", what, static_cast<unsigned>(e.code()), u8(e.message()).c_str());
}

bool show(const char16_t *text, uint32_t len, uint32_t max_len) {
    if (!g_ctx)
        return false;
    g_focused = true;
    g_enter = false;
    const int32_t old_len = static_cast<int32_t>(g_text.size());
    g_text.assign(text, len);
    g_max = max_len;
    g_sel = { static_cast<int32_t>(len), static_cast<int32_t>(len) };
    bool shown = false, fallback = false;
    try {
        g_ctx.NotifyFocusEnter();
        g_ctx.NotifyTextChanged({ 0, old_len }, static_cast<int32_t>(len), g_sel);
        shown = g_view && g_view.TryShow(CoreInputViewKind::Keyboard);
        if (!shown && g_pane) {
            fallback = true;
            shown = g_pane.TryShow();
        }
    } catch (winrt::hresult_error const &e) {
        log_hr("show", e);
    }
    vitox::log_line(vitox::Log::Core, "m8f.2b host text: NotifyFocusEnter + NotifyTextChanged(seed '%s', %u units, max %u) | %s -> %d",
        u8(g_text).c_str(), len, max_len, fallback ? "CoreInputView::TryShow(Keyboard) false, InputPane::TryShow" : "CoreInputView::TryShow(Keyboard)", shown);
    return shown;
}

void hide() {
    if (!g_ctx)
        return;
    g_focused = false;
    bool hidden = false;
    try {
        g_ctx.NotifyFocusLeave();
        hidden = g_view ? g_view.TryHide() : false;
        if (!hidden && g_pane)
            hidden = g_pane.TryHide();
    } catch (winrt::hresult_error const &e) {
        log_hr("hide", e);
    }
    vitox::log_line(vitox::Log::Core, "m8f.2b host text: NotifyFocusLeave + TryHide -> %d | text updates %u rejected (max length) %u", hidden, g_updates,
        g_rejected);
}

uint32_t generation() { return g_gen.load(); }

// the text without CR/LF (the SDL frontend strips them from text input too)
uint32_t text(char16_t *out, uint32_t cap) {
    uint32_t n = 0;
    for (char16_t c : g_text)
        if (c != u'\r' && c != u'\n' && n < cap)
            out[n++] = c;
    return n;
}

bool keyboard_visible() { return g_visible.load(); }
bool take_enter() { return g_enter.exchange(false); }

const VitoxHostText kHost{ &show, &hide, &generation, &text, &keyboard_visible, &take_enter };

int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return std::max(lo, std::min(v, hi)); }

// M8F.2C: the keys CoreTextEditContext does not turn into TextUpdating (Microsoft's CustomEditControl sample handles them
// in CoreWindow.KeyDown too): the Xbox keyboard's backspace and Enter arrive here. Gamepad X / Menu are accepted as the
// same two keys only while the system keyboard is up, with one action per 80 ms (the keyboard may report both).
void apply_backspace() {
    const int32_t n = static_cast<int32_t>(g_text.size());
    int32_t b = clampi(std::min(g_sel.StartCaretPosition, g_sel.EndCaretPosition), 0, n);
    const int32_t e = clampi(std::max(g_sel.StartCaretPosition, g_sel.EndCaretPosition), b, n);
    if (b == e) {
        if (b == 0)
            return;
        --b;
    }
    g_text.erase(b, e - b);
    g_sel = { b, b };
    g_ctx.NotifyTextChanged({ b, e }, 0, g_sel);
    ++g_updates;
    ++g_gen;
}
void move_caret(int32_t delta) {
    const int32_t n = static_cast<int32_t>(g_text.size());
    const int32_t lo = std::min(g_sel.StartCaretPosition, g_sel.EndCaretPosition), hi = std::max(g_sel.StartCaretPosition, g_sel.EndCaretPosition);
    const int32_t c = lo != hi ? (delta < 0 ? lo : hi) : clampi(lo + delta, 0, n);
    g_sel = { c, c };
    g_ctx.NotifySelectionChanged(g_sel);
}
void on_key(KeyEventArgs const &a) {
    if (!g_focused || !g_ctx)
        return;
    const VirtualKey k = a.VirtualKey();
    static auto last_action = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    const bool visible = g_visible.load();
    const bool is_back = k == VirtualKey::Back || (visible && k == VirtualKey::GamepadX);
    const bool is_enter = k == VirtualKey::Enter || (visible && k == VirtualKey::GamepadMenu);
    const bool is_left = k == VirtualKey::Left, is_right = k == VirtualKey::Right;
    const bool act = (is_back || is_enter || is_left || is_right) && now - last_action > std::chrono::milliseconds(80);
    vitox::log_line(vitox::Log::Core, "m8f.2c host text: KeyDown VirtualKey %d (keyboard visible %d) -> %s", static_cast<int>(k), visible,
        !act ? "ignored" : is_back ? "backspace" : is_enter ? "enter" : is_left ? "caret left" : "caret right");
    if (!act)
        return;
    last_action = now;
    a.Handled(true);
    try {
        if (is_back)
            apply_backspace();
        else if (is_enter)
            g_enter = true;
        else
            move_caret(is_left ? -1 : 1);
    } catch (winrt::hresult_error const &e) {
        log_hr("KeyDown", e);
    }
    if (is_back)
        vitox::log_line(vitox::Log::Core, "m8f.2c host text: backspace -> text '%s'", u8(g_text).c_str());
}

} // namespace

namespace vitox {

void host_text_start() {
    if (g_started)
        return;
    try {
        g_ctx = CoreTextServicesManager::GetForCurrentView().CreateEditContext();
        g_ctx.InputScope(CoreTextInputScope::Default);
        g_ctx.InputPaneDisplayPolicy(CoreTextInputPaneDisplayPolicy::Manual);
        g_ctx.Name(L"Vitox IME dialog");
        g_ctx.TextRequested([](auto &&, CoreTextTextRequestedEventArgs const &a) {
            const CoreTextRange r = a.Request().Range();
            const int32_t n = static_cast<int32_t>(g_text.size());
            const int32_t b = clampi(r.StartCaretPosition, 0, n), e = clampi(r.EndCaretPosition, b, n);
            a.Request().Text(winrt::hstring(std::wstring(g_text.begin() + b, g_text.begin() + e)));
        });
        g_ctx.SelectionRequested([](auto &&, CoreTextSelectionRequestedEventArgs const &a) { a.Request().Selection(g_sel); });
        g_ctx.TextUpdating([](auto &&, CoreTextTextUpdatingEventArgs const &a) {
            const CoreTextRange r = a.Range();
            const std::wstring w(a.Text());
            const int32_t n = static_cast<int32_t>(g_text.size());
            const int32_t b = clampi(r.StartCaretPosition, 0, n), e = clampi(r.EndCaretPosition, b, n);
            std::u16string next = g_text;
            next.replace(b, e - b, std::u16string(w.begin(), w.end()));
            if (g_max && next.size() > g_max) {
                ++g_rejected;
                a.Result(CoreTextTextUpdatingResult::Failed);
                log_line(Log::Core, "m8f.2b host text: TextUpdating range [%d,%d) '%s' REJECTED (would be %u > max %u)", b, e, winrt::to_string(a.Text()).c_str(),
                    static_cast<unsigned>(next.size()), g_max);
                return;
            }
            g_text = std::move(next);
            g_sel = a.NewSelection();
            ++g_updates;
            ++g_gen;
            log_line(Log::Core, "m8f.2b host text: TextUpdating range [%d,%d) '%s' -> text '%s'", b, e, winrt::to_string(a.Text()).c_str(), u8(g_text).c_str());
        });
        g_ctx.SelectionUpdating([](auto &&, CoreTextSelectionUpdatingEventArgs const &a) { g_sel = a.Selection(); });
        g_ctx.FormatUpdating([](auto &&, CoreTextFormatUpdatingEventArgs const &) {});
        g_ctx.LayoutRequested([](auto &&, CoreTextLayoutRequestedEventArgs const &a) {
            // the overlay's text bar sits mid-screen; the bounds only steer where a keyboard / candidate UI may go
            a.Request().LayoutBounds().TextBounds({ 480.f, 400.f, 960.f, 60.f });
            a.Request().LayoutBounds().ControlBounds({ 480.f, 400.f, 960.f, 60.f });
        });
        g_ctx.CompositionStarted([](auto &&, auto &&) { log_line(Log::Core, "m8f.2b host text: CompositionStarted"); });
        g_ctx.CompositionCompleted([](auto &&, auto &&) { log_line(Log::Core, "m8f.2b host text: CompositionCompleted"); });
        g_ctx.FocusRemoved([](auto &&, auto &&) { log_line(Log::Core, "m8f.2b host text: FocusRemoved (the system took the edit focus)"); });
    } catch (winrt::hresult_error const &e) {
        log_hr("CoreTextServicesManager::CreateEditContext", e);
        g_ctx = nullptr;
    }
    try {
        g_view = CoreInputView::GetForCurrentView();
        g_rev_showing = g_view.PrimaryViewShowing(winrt::auto_revoke, [](auto &&, CoreInputViewShowingEventArgs const &) {
            g_visible = true;
            log_line(Log::Core, "m8f.2b host text: CoreInputView PrimaryViewShowing (system keyboard shown)");
        });
        g_rev_hiding = g_view.PrimaryViewHiding(winrt::auto_revoke, [](auto &&, CoreInputViewHidingEventArgs const &) {
            g_visible = false;
            log_line(Log::Core, "m8f.2b host text: CoreInputView PrimaryViewHiding (system keyboard hidden)");
        });
    } catch (winrt::hresult_error const &e) {
        log_hr("CoreInputView::GetForCurrentView", e);
        g_view = nullptr;
    }
    try {
        g_rev_keydown = CoreWindow::GetForCurrentThread().KeyDown(winrt::auto_revoke, [](auto &&, KeyEventArgs const &a) { on_key(a); });
    } catch (winrt::hresult_error const &e) {
        log_hr("CoreWindow::KeyDown", e);
    }
    try {
        g_pane = InputPane::GetForCurrentView();
        // on this Xbox the keyboard shows as the InputPane (CoreInputView's PrimaryViewShowing did not fire): both drive
        // keyboard_visible
        g_rev_pane_showing = g_pane.Showing(winrt::auto_revoke, [](auto &&, auto &&) {
            g_visible = true;
            log_line(Log::Core, "m8f.2b host text: InputPane Showing (system keyboard shown)");
        });
        g_rev_pane_hiding = g_pane.Hiding(winrt::auto_revoke, [](auto &&, auto &&) {
            g_visible = false;
            log_line(Log::Core, "m8f.2b host text: InputPane Hiding (system keyboard hidden)");
        });
    } catch (winrt::hresult_error const &e) {
        log_hr("InputPane::GetForCurrentView", e);
        g_pane = nullptr;
    }
    g_started = true;
    if (g_ctx)
        vitox_set_host_text(&kHost);
    log_line(Log::Core, "m8f.2b host text: CoreTextEditContext %s | CoreInputView %s | InputPane %s", g_ctx ? "created" : "UNAVAILABLE",
        g_view ? "ok" : "UNAVAILABLE", g_pane ? "ok" : "UNAVAILABLE");
}

void host_text_stop() {
    if (!g_started)
        return;
    vitox_set_host_text(nullptr);
    g_rev_showing.revoke();
    g_rev_hiding.revoke();
    g_rev_pane_showing.revoke();
    g_rev_pane_hiding.revoke();
    g_rev_keydown.revoke();
    g_ctx = nullptr; // the edit context's handlers go with it
    g_view = nullptr;
    g_pane = nullptr;
    g_text.clear();
    g_visible = false;
    g_started = false;
}

} // namespace vitox
