// Vitox 0.0.25.0: the quick-menu (R3 in-game overlay) side panel, drawn ONCE per game session with Direct2D into a WIC
// bitmap (CPU, premultiplied BGRA). The host presenter uploads it as a D3D12 texture and composites it over the game
// frame with shaders/overlay.hlsl, which adds every animated part (slide, dim, glows, particles, focus pulse).
// 0.0.28.0: four variants side by side, w each: statistics overlay OFF (normal | confirm), ON (normal | confirm); two
// action cards: Statistics Overlay (ON/OFF, toggles at once) and Return to Launcher (A, then A again to confirm).
#include "common.h"

#include <d2d1_1.h>
#include <dwrite_1.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <algorithm>
#include <mutex>

namespace vitox {

namespace {
std::mutex g_info_m;
OverlayInfo g_info;
} // namespace

void overlay_set_info(const OverlayInfo &info) {
    std::lock_guard<std::mutex> lock(g_info_m);
    g_info = info;
}

OverlayInfo overlay_info() {
    std::lock_guard<std::mutex> lock(g_info_m);
    return g_info;
}

namespace {

using winrt::com_ptr;

D2D1_COLOR_F rgba(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

com_ptr<ID2D1Bitmap> load_icon(ID2D1RenderTarget *rt, IWICImagingFactory *wic, const std::wstring &path) {
    com_ptr<ID2D1Bitmap> bmp;
    if (path.empty() || !wic)
        return bmp;
    com_ptr<IWICBitmapDecoder> dec;
    com_ptr<IWICBitmapFrameDecode> frame;
    com_ptr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.put())) || FAILED(dec->GetFrame(0, frame.put()))
        || FAILED(wic->CreateFormatConverter(conv.put()))
        || FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        return bmp;
    rt->CreateBitmapFromWicBitmap(conv.get(), nullptr, bmp.put());
    return bmp;
}

void text(ID2D1RenderTarget *rt, IDWriteFactory *dw, const std::wstring &s, const wchar_t *font, DWRITE_FONT_WEIGHT weight, float size, D2D1_RECT_F r,
    ID2D1Brush *brush, float spacing = 0, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false) {
    com_ptr<IDWriteTextFormat> f;
    if (FAILED(dw->CreateTextFormat(font, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", f.put())))
        return;
    f->SetTextAlignment(align);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    com_ptr<IDWriteInlineObject> ellipsis;
    dw->CreateEllipsisTrimmingSign(f.get(), ellipsis.put());
    f->SetTrimming(&trim, ellipsis.get());
    com_ptr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f.get(), r.right - r.left, r.bottom - r.top, layout.put())))
        return;
    if (spacing != 0) {
        if (auto l1 = layout.try_as<IDWriteTextLayout1>())
            l1->SetCharacterSpacing(0, spacing, 0, { 0, static_cast<UINT32>(s.size()) });
    }
    rt->DrawTextLayout({ r.left, r.top }, layout.get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// a round button glyph with a letter (controller hint)
void hint(ID2D1RenderTarget *rt, IDWriteFactory *dw, float x, float y, float k, const wchar_t *glyph, const std::wstring &label, uint32_t ring) {
    com_ptr<ID2D1SolidColorBrush> b, w, lav;
    rt->CreateSolidColorBrush(rgba(ring, 0.95f), b.put());
    rt->CreateSolidColorBrush(rgba(0xFFFFFF), w.put());
    rt->CreateSolidColorBrush(rgba(0xCDB8F5), lav.put());
    const float r = 17 * k;
    rt->FillEllipse(D2D1::Ellipse({ x + r, y + r }, r, r), b.get());
    text(rt, dw, glyph, L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, (wcslen(glyph) > 1 ? 13 : 18) * k, D2D1::RectF(x, y + r - 13 * k, x + 2 * r, y + r + 13 * k), w.get(), 0,
        DWRITE_TEXT_ALIGNMENT_CENTER);
    text(rt, dw, label, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 21 * k, D2D1::RectF(x + 2 * r + 10 * k, y + r - 15 * k, x + 2 * r + 200 * k, y + r + 15 * k), lav.get());
}

} // namespace

bool overlay_render_panel(uint32_t w, uint32_t h, float k, std::vector<uint8_t> &bgra, OverlayItemRect items[2]) {
    const OverlayInfo info = overlay_info();
    com_ptr<IWICImagingFactory> wic;
    com_ptr<ID2D1Factory> d2d;
    com_ptr<IDWriteFactory> dw;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), wic.put_void()))
        || FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), d2d.put_void()))
        || FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<::IUnknown **>(dw.put()))))
        return false;
    com_ptr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmap(w * 4, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, bmp.put())))
        return false;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    com_ptr<ID2D1RenderTarget> rt;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put())))
        return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    com_ptr<ID2D1Bitmap> icon = load_icon(rt.get(), wic.get(), info.icon_path);

    const float W = static_cast<float>(w), H = static_cast<float>(h), pad = 56 * k;
    items[0] = { pad - 8 * k, 548 * k, W - pad + 8 * k, 648 * k }; // Statistics Overlay
    items[1] = { pad - 8 * k, 668 * k, W - pad + 8 * k, 798 * k }; // Return to Launcher
    const OverlayItemRect &item = items[1];
    const bool full_mode = stats_mode() == StatsMode::Full;

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (int v = 0; v < 4; ++v) {
        const bool confirm = (v & 1) != 0, stats_on = v >= 2;
        rt->SetTransform(D2D1::Matrix3x2F::Translation(W * v, 0));
        // glass sheet: deep violet, darker at the bottom, a lit top-left corner
        com_ptr<ID2D1GradientStopCollection> stops;
        const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0x2A1250, 0.93f) }, { 0.45f, rgba(0x170A2E, 0.94f) }, { 1.0f, rgba(0x0C0618, 0.96f) } };
        rt->CreateGradientStopCollection(gs, 3, stops.put());
        com_ptr<ID2D1LinearGradientBrush> sheet;
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { W * 0.6f, H }), stops.get(), sheet.put());
        rt->FillRectangle(D2D1::RectF(0, 0, W, H), sheet.get());
        com_ptr<ID2D1SolidColorBrush> edge, white, lav, dimlav, accent, chip, chiptext, divider, card, cardedge, warn;
        rt->CreateSolidColorBrush(rgba(0xB48CFF, 0.35f), edge.put());
        rt->CreateSolidColorBrush(rgba(0xFFFFFF), white.put());
        rt->CreateSolidColorBrush(rgba(0xD9C9FF), lav.put());
        rt->CreateSolidColorBrush(rgba(0xA592CC), dimlav.put());
        rt->CreateSolidColorBrush(rgba(0xA66BFF), accent.put());
        rt->CreateSolidColorBrush(rgba(0x8A4DFF, 0.28f), chip.put());
        rt->CreateSolidColorBrush(rgba(0xE6D9FF), chiptext.put());
        rt->CreateSolidColorBrush(rgba(0xB48CFF, 0.18f), divider.put());
        rt->CreateSolidColorBrush(confirm ? rgba(0xC2338C, 0.30f) : rgba(0x8F52FF, 0.24f), card.put());
        rt->CreateSolidColorBrush(confirm ? rgba(0xFF6AC1, 0.85f) : rgba(0xB37BFF, 0.85f), cardedge.put());
        rt->CreateSolidColorBrush(rgba(0xFFB3DD), warn.put());
        rt->FillRectangle(D2D1::RectF(W - 2 * k, 0, W, H), edge.get());

        // brand
        com_ptr<ID2D1GradientStopCollection> ls;
        const D2D1_GRADIENT_STOP lg[] = { { 0.0f, rgba(0xF2E9FF) }, { 0.55f, rgba(0xC08CFF) }, { 1.0f, rgba(0x8B5CFF) } };
        rt->CreateGradientStopCollection(lg, 3, ls.put());
        com_ptr<ID2D1LinearGradientBrush> logo;
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ pad, 70 * k }, { pad + 260 * k, 130 * k }), ls.get(), logo.put());
        text(rt.get(), dw.get(), L"VITOX", L"Segoe UI", DWRITE_FONT_WEIGHT_BLACK, 54 * k, D2D1::RectF(pad, 62 * k, W - pad, 136 * k), logo.get(), 10 * k);
        text(rt.get(), dw.get(), L"QUICK MENU", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 19 * k, D2D1::RectF(pad + 3 * k, 134 * k, W - pad, 164 * k), dimlav.get(),
            6 * k);

        // now playing
        rt->FillRectangle(D2D1::RectF(pad, 206 * k, W - pad, 207 * k), divider.get());
        text(rt.get(), dw.get(), L"NOW PLAYING", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 17 * k, D2D1::RectF(pad, 236 * k, W - pad, 262 * k), accent.get(), 4 * k);
        const float ic = 128 * k, iy = 284 * k;
        const D2D1_ROUNDED_RECT icon_rr{ D2D1::RectF(pad, iy, pad + ic, iy + ic), 22 * k, 22 * k };
        if (icon) {
            com_ptr<ID2D1RoundedRectangleGeometry> clip;
            d2d->CreateRoundedRectangleGeometry(icon_rr, clip.put());
            com_ptr<ID2D1Layer> layer;
            rt->CreateLayer(layer.put());
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.get()), layer.get());
            rt->DrawBitmap(icon.get(), icon_rr.rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->PopLayer();
        } else {
            rt->FillRoundedRectangle(icon_rr, chip.get());
            text(rt.get(), dw.get(), L"V", L"Segoe UI", DWRITE_FONT_WEIGHT_BLACK, 64 * k, D2D1::RectF(pad, iy + 22 * k, pad + ic, iy + ic), lav.get(), 0,
                DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rt->DrawRoundedRectangle(icon_rr, edge.get(), 1.5f * k);
        const float tx = pad + ic + 26 * k;
        text(rt.get(), dw.get(), info.title.empty() ? info.title_id : info.title, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 31 * k,
            D2D1::RectF(tx, iy - 4 * k, W - pad, iy + 84 * k), white.get(), 0, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        const float cw = (static_cast<float>(info.title_id.size()) * 13.5f + 28) * k;
        const D2D1_ROUNDED_RECT chip_rr{ D2D1::RectF(tx, iy + 94 * k, tx + cw, iy + 126 * k), 16 * k, 16 * k };
        rt->FillRoundedRectangle(chip_rr, chip.get());
        text(rt.get(), dw.get(), info.title_id, L"Consolas", DWRITE_FONT_WEIGHT_BOLD, 19 * k, D2D1::RectF(tx, iy + 98 * k, tx + cw, iy + 124 * k), chiptext.get(), 0,
            DWRITE_TEXT_ALIGNMENT_CENTER);

        // the actions
        rt->FillRectangle(D2D1::RectF(pad, 470 * k, W - pad, 471 * k), divider.get());
        text(rt.get(), dw.get(), L"ACTIONS", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 17 * k, D2D1::RectF(pad, 500 * k, W - pad, 526 * k), accent.get(), 4 * k);
        // 0.0.28.0: Statistics Overlay - the same setting as the launcher's Settings page; A toggles it at once
        {
            const OverlayItemRect &si = items[0];
            const D2D1_ROUNDED_RECT srr{ D2D1::RectF(si.x0, si.y0, si.x1, si.y1), 20 * k, 20 * k };
            com_ptr<ID2D1SolidColorBrush> scard, sedge, bar, knob, track;
            rt->CreateSolidColorBrush(rgba(0x8F52FF, 0.24f), scard.put());
            rt->CreateSolidColorBrush(rgba(0xB37BFF, 0.85f), sedge.put());
            rt->CreateSolidColorBrush(rgba(0xFFFFFF), bar.put());
            rt->CreateSolidColorBrush(rgba(0xFFFFFF), knob.put());
            rt->CreateSolidColorBrush(stats_on ? rgba(0x9D5CFF) : rgba(0x3A2A5C), track.put());
            rt->FillRoundedRectangle(srr, scard.get());
            rt->DrawRoundedRectangle(srr, sedge.get(), 2.0f * k);
            // glyph: three rising bars
            const float gx = si.x0 + 34 * k, gb = si.y0 + 70 * k;
            for (int i = 0; i < 3; ++i) {
                const float bh = (16 + i * 13) * k;
                rt->FillRoundedRectangle({ D2D1::RectF(gx + i * 17 * k, gb - bh, gx + i * 17 * k + 11 * k, gb), 3 * k, 3 * k }, bar.get());
            }
            const float lx = si.x0 + 110 * k;
            text(rt.get(), dw.get(), stats_on ? L"Statistics Overlay: ON" : L"Statistics Overlay: OFF", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 28 * k,
                D2D1::RectF(lx, si.y0 + 16 * k, si.x1 - 120 * k, si.y0 + 58 * k), white.get());
            text(rt.get(), dw.get(), full_mode ? L"Full Performance (FPS, memory, CPU)" : L"FPS Only", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 19 * k,
                D2D1::RectF(lx, si.y0 + 60 * k, si.x1 - 20 * k, si.y0 + 88 * k), lav.get());
            // the switch
            const float sw = 74 * k, sh = 38 * k, sx = si.x1 - 26 * k - sw, sy = si.y0 + 20 * k;
            rt->FillRoundedRectangle({ D2D1::RectF(sx, sy, sx + sw, sy + sh), sh / 2, sh / 2 }, track.get());
            rt->DrawRoundedRectangle({ D2D1::RectF(sx, sy, sx + sw, sy + sh), sh / 2, sh / 2 }, sedge.get(), 1.5f * k);
            const float kx = stats_on ? sx + sw - sh / 2 : sx + sh / 2;
            rt->FillEllipse(D2D1::Ellipse({ kx, sy + sh / 2 }, sh / 2 - 5 * k, sh / 2 - 5 * k), knob.get());
        }
        const D2D1_ROUNDED_RECT card_rr{ D2D1::RectF(item.x0, item.y0, item.x1, item.y1), 20 * k, 20 * k };
        rt->FillRoundedRectangle(card_rr, card.get());
        rt->DrawRoundedRectangle(card_rr, cardedge.get(), 2.0f * k);
        // home glyph: a house outline
        {
            const float gx = item.x0 + 30 * k, gy = item.y0 + 38 * k, s = 54 * k;
            com_ptr<ID2D1PathGeometry> g;
            d2d->CreatePathGeometry(g.put());
            com_ptr<ID2D1GeometrySink> sink;
            g->Open(sink.put());
            sink->BeginFigure({ gx, gy + s * 0.45f }, D2D1_FIGURE_BEGIN_HOLLOW);
            const D2D1_POINT_2F pts[] = { { gx + s * 0.5f, gy }, { gx + s, gy + s * 0.45f } };
            sink->AddLines(pts, 2);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure({ gx + s * 0.15f, gy + s * 0.32f }, D2D1_FIGURE_BEGIN_HOLLOW);
            const D2D1_POINT_2F body[] = { { gx + s * 0.15f, gy + s * 0.95f }, { gx + s * 0.85f, gy + s * 0.95f }, { gx + s * 0.85f, gy + s * 0.32f } };
            sink->AddLines(body, 3);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure({ gx + s * 0.4f, gy + s * 0.95f }, D2D1_FIGURE_BEGIN_HOLLOW);
            const D2D1_POINT_2F door[] = { { gx + s * 0.4f, gy + s * 0.62f }, { gx + s * 0.6f, gy + s * 0.62f }, { gx + s * 0.6f, gy + s * 0.95f } };
            sink->AddLines(door, 3);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            com_ptr<ID2D1StrokeStyle> round;
            d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0,
                round.put());
            rt->DrawGeometry(g.get(), white.get(), 4.5f * k, round.get());
        }
        const float lx = item.x0 + 110 * k;
        text(rt.get(), dw.get(), L"Return to Launcher", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 31 * k, D2D1::RectF(lx, item.y0 + 22 * k, item.x1 - 20 * k, item.y0 + 66 * k),
            white.get());
        text(rt.get(), dw.get(), confirm ? L"Press A again to close the game" : L"Close the game and go back to your library", L"Segoe UI",
            DWRITE_FONT_WEIGHT_NORMAL, 20 * k, D2D1::RectF(lx, item.y0 + 70 * k, item.x1 - 20 * k, item.y0 + 100 * k), confirm ? warn.get() : lav.get());
        if (confirm)
            text(rt.get(), dw.get(), L"Progress that was not saved in the game will be lost.", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 19 * k,
                D2D1::RectF(pad, item.y1 + 22 * k, W - pad, item.y1 + 80 * k), warn.get(), 0, DWRITE_TEXT_ALIGNMENT_LEADING, true);

        // controller hints
        rt->FillRectangle(D2D1::RectF(pad, H - 132 * k, W - pad, H - 131 * k), divider.get());
        const float hy = H - 100 * k;
        hint(rt.get(), dw.get(), pad, hy, k, L"A", confirm ? L"Confirm" : L"Select", 0x3E9A4F);
        hint(rt.get(), dw.get(), pad + 165 * k, hy, k, L"B", confirm ? L"Cancel" : L"Resume", 0xB23A48);
        hint(rt.get(), dw.get(), pad + 335 * k, hy, k, L"R3", L"Close", 0x6B3FC9);
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (FAILED(rt->EndDraw()))
        return false;
    bgra.resize(static_cast<size_t>(w) * 4 * h * 4);
    const WICRect all{ 0, 0, static_cast<INT>(w * 4), static_cast<INT>(h) };
    return SUCCEEDED(bmp->CopyPixels(&all, w * 4 * 4, static_cast<UINT>(bgra.size()), bgra.data()));
}

} // namespace vitox
