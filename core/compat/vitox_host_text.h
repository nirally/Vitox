// Vitox M8F.2B: the host's text source for Vita3K's IME dialog (plain C ABI, no Vita3K/WinRT types). The UWP app
// implements it over Windows.UI.Text.Core.CoreTextEditContext + the system keyboard (CoreInputView); the launcher
// mirrors the host text into Vita3K's shared Ime state through the REAL helpers (ime_commit_text / ime_backspace /
// ime_cursor_*), exactly what the SDL / Qt frontends do with their text events. Completion stays with Vita3K's
// common_dialog_overlay (Cross = submit, Circle = cancel). Every call is made on the CoreWindow UI thread (the launcher's
// run loop pumps that window). Without a registered host there is no host text input.
#pragma once

#include <cstdint>

struct VitoxHostText {
    // the IME dialog went RUNNING: seed the edit context with `text` (UTF-16), focus it and request the system keyboard.
    // Returns the keyboard show request's result (CoreInputView::TryShow, InputPane::TryShow as fallback).
    bool (*show)(const char16_t *text, uint32_t len, uint32_t max_len);
    void (*hide)(); // the dialog left RUNNING: focus leave + hide request
    uint32_t (*generation)(); // changes on every host text change
    uint32_t (*text)(char16_t *out, uint32_t cap); // the host text (UTF-16 code units, no NUL); returns its length
    bool (*keyboard_visible)(); // between the system keyboard's show and hide events
    // M8F.2C: Enter was pressed on the keyboard since the last call (the launcher then calls submit_current_ime, what the
    // SDL / Qt frontends do for Return). Backspace / Left / Right are applied to the host text itself.
    bool (*take_enter)();
};

void vitox_set_host_text(const VitoxHostText *host); // nullptr = none
const VitoxHostText *vitox_host_text();
