// Vitox M6C (0.0.13.0) / M6C.1 (0.0.13.1, 0.0.13.2) - host side of the REAL Vita3K Vulkan renderer on Xbox. VISUAL.
//
// Implements vitox::ExternalPresenter (Vitox-M3/compat/vitox_external_present.h) on a host D3D12 device opened on
// the Vulkan adapter's LUID, with a CoreWindow flip-model swapchain (the M6A/M6B presentation bridge):
//   Vita3K ScreenRenderer image i (exported D3D12_RESOURCE) + timeline value v (exported D3D12 fence)
//   -> host queue Wait(fence, v) -> CopyResource(back buffer, image i) -> Signal(copy fence) -> Present(1, 0)
//   -> Signal(frame fence)
// and then calls the real renderer probe (vitox_m6c_probe.lib, vitox_renderer_probe.cpp).
//
// M6C.1: 0.0.13.0 stalled with the host completion fence at 8 while 11 frames were submitted. This build splits the
// host completion into a copy fence (after the copy, before Present) and a frame fence (after Present), records the
// HRESULT of every host operation per frame, logs one host snapshot at the first wait > 100 ms (the renderer logs
// its Vulkan half under the same id), and runs the control variants A..D, each with a fresh renderer and presenter.
// 0.0.13.2: a pre-copy fence right after the shared-fence Wait, order B, C, D, A, a host wedge does not end the matrix,
// and a same-value CPU re-signal of the shared fence tests for a missed wake-up.
// M6C.2 (0.0.13.4 repro, 0.0.13.5 acceptance): production variant P - the frame-latency waitable object
// (MaxFrameLatency 1) paces the host, image reuse is gated on the copy fence, and a labelled, counted watchdog
// switches to a CPU wait on the shared fence if the host queue stops advancing although the value is visible.
#include "common.h"

#include <vitox_external_present.h>
#include <vitox_host_input.h> // 0.0.24.54: touch cursor
#include <vitox_gxm_homebrew_probe.h>
#include <vitox_homebrew_launcher.h> // M8A
#include <vitox_firmware_stage.h>    // M8C.1
#include <vitox_pgf_lle_probe.h>     // M8C.2
#include <vitox_shader_gen_probe.h>
#include <vitox_renderer_probe.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include "vitox_touch_cursor_shaders.h" // 0.0.24.54: shaders/touch_cursor.hlsl (fxc)
#include "vitox_loading_shaders.h" // 0.0.24.198: shaders/loading_screen.hlsl (fxc)
#include "vitox_ui_shaders.h"      // 0.0.25.0: shaders/overlay.hlsl (fxc) - the R3 quick menu

#include <cmath>
#include <mutex>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

std::string vitox_sha256_hex(const void *data, size_t size); // Vitox-M3 compat/vitox_sha256.cpp (vitox_hle.lib)

using winrt::com_ptr;

namespace vitox {

namespace {

void m6c_log(const char *line) { log_line(Log::Core, "%s", line); }

void plog(const char *fmt, ...) {
    char buf[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line(Log::Core, "m6c host %s", buf);
}

// M8E: the periodic per-frame samples only with VITOX_DIAG_VERBOSE (default OFF); the first frames and failures always
#ifdef VITOX_DIAG_VERBOSE
bool logged(uint64_t n) { return n <= 16 || n % 300 == 0; }
#else
bool logged(uint64_t n) { return n <= 16; }
#endif

unsigned long hx(HRESULT hr) { return static_cast<unsigned long>(hr); }
unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }

// Every host operation of one present() call, in queue order.
struct FrameOps {
    uint64_t frame = 0;
    uint32_t image = 0;
    uint64_t wait_value = 0; // 0 = no queue Wait (variant B)
    HRESULT wait = S_OK;
    uint32_t back_buffer = 0;
    HRESULT get_buffer = S_OK, alloc_reset = S_OK, list_reset = S_OK, close = S_OK;
    bool executed = false;
    uint64_t pre_value = 0; // 0.0.13.2: pre-copy fence, right after the shared-fence Wait
    HRESULT signal_pre = S_OK;
    uint64_t copy_value = 0;
    HRESULT signal_copy = S_OK;
    double pace_ms = 0; // variant C: CPU wait for the previous frame before Present
    bool pace_ok = true;
    HRESULT present = S_OK;
    uint64_t frame_value = 0;
    HRESULT signal_frame = S_OK;
    HRESULT removed = S_OK;
};

// 0.0.24.75: LocalState folder for the headless frame captures (set by probe_m8a)
std::wstring &shot_dir() {
    static std::wstring dir;
    return dir;
}

void mem_breakdown(const char *tag); // 0.0.25.1

class HostPresenter final : public ExternalPresenter {
public:
    // headless (M7A non-visual precheck): no CoreWindow swapchain and no Present - the copy goes into a host-owned
    // offscreen texture, paced to ~60 Hz on the frame fence; everything before Present is the production path
    HostPresenter(void *core_window, uint32_t w, uint32_t h, uint32_t n, VitoxXpVariant variant, bool headless = false)
        : core_window_(core_window)
        , w_(w)
        , h_(h)
        , n_(n)
        , variant_(variant)
        , headless_(headless) {}
    ~HostPresenter() override {
        if (!wedged_)
            shutdown();
    }

    uint32_t width() const override { return w_; }
    uint32_t height() const override { return h_; }
    uint32_t image_count() const override { return n_; }

    bool bind(const uint8_t luid[8]) override {
        HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory_.put_void());
        LUID l;
        memcpy(&l, luid, sizeof(l));
        com_ptr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(hr))
            hr = factory_->EnumAdapterByLuid(l, __uuidof(IDXGIAdapter1), adapter.put_void());
        if (SUCCEEDED(hr))
            hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), d12_.put_void());
        plog("bind: EnumAdapterByLuid + D3D12CreateDevice(FL 11_0) hr=0x%08lX device=%p", hx(hr), static_cast<void *>(d12_.get()));
        if (FAILED(hr))
            return false;
        D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
        hr = d12_->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), queue_.put_void());
        plog("bind: host DIRECT queue %p hr=0x%08lX", static_cast<void *>(queue_.get()), hx(hr));
        if (headless_) {
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = w_;
            td.Height = h_;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            hr = d12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                offscreen_.put_void());
            plog("bind: HEADLESS (non-visual precheck): offscreen %ux%u B8G8R8A8 copy target hr=0x%08lX - no CoreWindow swapchain, no Present", w_, h_, hx(hr));
            if (FAILED(hr))
                return false;
        }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w_;
        sd.Height = h_;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        const bool production = variant_ == VitoxXpVariant::P_Production && !headless_;
        if (production)
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        com_ptr<IDXGISwapChain1> sc1;
        if (!headless_) {
            hr = factory_->CreateSwapChainForCoreWindow(queue_.get(), static_cast<::IUnknown *>(core_window_), &sd, nullptr, sc1.put());
            plog("bind: CoreWindow swapchain %ux%u B8G8R8A8 FLIP_DISCARD x2%s hr=0x%08lX", w_, h_, production ? " FRAME_LATENCY_WAITABLE_OBJECT" : "", hx(hr));
            if (FAILED(hr))
                return false;
            sc_ = sc1.as<IDXGISwapChain3>();
        }
        if (production) {
            // M6C.2 production pacing: the host never runs more than one frame ahead of the flip queue
            const HRESULT hl = sc_->SetMaximumFrameLatency(1);
            UINT latency = 0;
            sc_->GetMaximumFrameLatency(&latency);
            waitable_ = sc_->GetFrameLatencyWaitableObject();
            plog("bind: production pacing SetMaximumFrameLatency(1)=0x%08lX -> %u | waitable object %s", hx(hl), latency, waitable_ ? "OK" : "MISSING");
            if (FAILED(hl) || latency != 1 || !waitable_)
                return false;
        }
        allocs_.resize(n_);
        lists_.resize(n_);
        list_open_.assign(n_, false);
        images_.resize(n_);
        done_.assign(n_, 0);
        done_copy_.assign(n_, 0);
        for (uint32_t i = 0; i < n_; ++i) {
            d12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), allocs_[i].put_void());
            d12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs_[i].get(), nullptr, __uuidof(ID3D12GraphicsCommandList), lists_[i].put_void());
            lists_[i]->Close();
        }
        d12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), frame_fence_.put_void());
        d12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), copy_fence_.put_void());
        d12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), pre_fence_.put_void());
        event_ = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (!headless_)
            cursor_init();
        plog("bind: host fences frame=%p copy=%p | variant %s", static_cast<void *>(frame_fence_.get()), static_cast<void *>(copy_fence_.get()),
            vitox_xp_variant_name(variant_));
        return true;
    }

    bool import_image(uint32_t index, void *nt_handle) override {
        if (index >= n_ || !d12_)
            return false;
        const HRESULT hr = d12_->OpenSharedHandle(static_cast<HANDLE>(nt_handle), __uuidof(ID3D12Resource), images_[index].put_void());
        D3D12_RESOURCE_DESC d{};
        if (SUCCEEDED(hr))
            d = images_[index]->GetDesc();
        plog("import_image %u: OpenSharedHandle(ID3D12Resource) hr=0x%08lX -> %llux%u format=%d", index, hx(hr), ull(d.Width), d.Height,
            static_cast<int>(d.Format));
        return SUCCEEDED(hr) && d.Width == w_ && d.Height == h_;
    }

    bool import_fence(void *nt_handle) override {
        const HRESULT hr = d12_ ? d12_->OpenSharedHandle(static_cast<HANDLE>(nt_handle), __uuidof(ID3D12Fence), vk_fence_.put_void()) : E_FAIL;
        ++fence_imports_;
        plog("import_fence #%u: OpenSharedHandle(ID3D12Fence) hr=0x%08lX object=%p (completed value %llu)", fence_imports_, hx(hr),
            static_cast<void *>(vk_fence_.get()), vk_fence_ ? ull(vk_fence_->GetCompletedValue()) : 0ull);
        return SUCCEEDED(hr);
    }

    bool wait_image_free(uint32_t index, uint32_t timeout_ms) override {
        if (index >= n_ || !frame_fence_)
            return false;
        if (variant_ == VitoxXpVariant::P_Production)
            return wait_copy_done(index, timeout_ms);
        if (frame_fence_->GetCompletedValue() >= done_[index])
            return true;
        frame_fence_->SetEventOnCompletion(done_[index], event_);
        const DWORD w = WaitForSingleObjectEx(event_, timeout_ms, FALSE);
        if (w != WAIT_OBJECT_0 && timeout_ms > 1000)
            plog("wait_image_free %u: frame fence %llu not reached in %u ms (frame fence %llu, copy fence %llu / image copy value %llu, shared Vulkan fence %llu) "
                 "removed=0x%08lX",
                index, ull(done_[index]), timeout_ms, ull(frame_fence_->GetCompletedValue()), ull(copy_fence_->GetCompletedValue()), ull(done_copy_[index]),
                vk_fence_ ? ull(vk_fence_->GetCompletedValue()) : 0ull, hx(d12_->GetDeviceRemovedReason()));
        return w == WAIT_OBJECT_0;
    }

    bool present(uint32_t index, uint64_t value) override {
        if (index >= n_ || !images_[index])
            return false;
        std::lock_guard<std::mutex> present_lock(present_m_); // 0.0.25.0: present_overlay() runs on the launcher thread
        if (frames_ == 1800)
            mem_breakdown("in session (host frame 1800)");
        FrameOps op;
        op.frame = ++frames_;
        op.image = index;
        // headless: one frame ahead on the frame fence + ~60 Hz (there is no flip queue to pace against)
        if (headless_) {
            const auto t0 = std::chrono::steady_clock::now();
            if (frame_value_ && frame_fence_->GetCompletedValue() < frame_value_) {
                frame_fence_->SetEventOnCompletion(frame_value_, event_);
                op.pace_ok = WaitForSingleObjectEx(event_, 2000, FALSE) == WAIT_OBJECT_0;
            }
            const auto due = last_headless_ + std::chrono::microseconds(16667);
            if (std::chrono::steady_clock::now() < due)
                std::this_thread::sleep_until(due);
            last_headless_ = std::chrono::steady_clock::now();
            op.pace_ms = std::chrono::duration<double, std::milli>(last_headless_ - t0).count();
            pace_total_ms_ += op.pace_ms;
            pace_max_ms_ = std::max(pace_max_ms_, op.pace_ms);
            pace_timeouts_ += !op.pace_ok;
        }
        // production: wait for the frame-latency waitable (MaxFrameLatency 1) before any host work for this frame
        if (variant_ == VitoxXpVariant::P_Production && !headless_) {
            const auto t0 = std::chrono::steady_clock::now();
            DWORD w = WaitForSingleObjectEx(waitable_, 100, FALSE);
            if (w != WAIT_OBJECT_0) {
                watchdog_check("latency waitable not signalled within 100 ms");
                w = WaitForSingleObjectEx(waitable_, 1900, FALSE);
            }
            op.pace_ok = w == WAIT_OBJECT_0;
            op.pace_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            pace_total_ms_ += op.pace_ms;
            pace_max_ms_ = std::max(pace_max_ms_, op.pace_ms);
            pace_timeouts_ += !op.pace_ok;
        }
        // variant C: at most one frame ahead - the previous frame (Present + frame fence) must have completed
        if (variant_ == VitoxXpVariant::C_Paced && frame_value_ && frame_fence_->GetCompletedValue() < frame_value_) {
            const auto t0 = std::chrono::steady_clock::now();
            frame_fence_->SetEventOnCompletion(frame_value_, event_);
            op.pace_ok = WaitForSingleObjectEx(event_, 2000, FALSE) == WAIT_OBJECT_0;
            op.pace_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            pace_total_ms_ += op.pace_ms;
            pace_max_ms_ = std::max(pace_max_ms_, op.pace_ms);
            pace_timeouts_ += !op.pace_ok;
        }
        if (vk_fence_ && value && cpu_fallback_) {
            // LABELLED CPU-WAIT FALLBACK (the production watchdog fired earlier): the host CPU waits for the shared
            // fence instead of the host queue - counted, never a final pass
            const auto t0 = std::chrono::steady_clock::now();
            if (vk_fence_->GetCompletedValue() < value) {
                vk_fence_->SetEventOnCompletion(value, event_);
                WaitForSingleObjectEx(event_, 2000, FALSE);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++fallback_frames_;
            fallback_ms_ += ms;
            fallback_max_ms_ = std::max(fallback_max_ms_, ms);
            last_wait_value_ = value;
        } else if (vk_fence_ && value) {
            op.wait_value = value;
            op.wait = queue_->Wait(vk_fence_.get(), value); // GPU-GPU: the copy starts after Vita3K's frame completes
            last_wait_value_ = value;
        }
        // 0.0.13.2 pre-copy fence: reached = the shared-fence Wait was satisfied (whatever happens to the copy after it)
        op.pre_value = ++pre_value_;
        op.signal_pre = queue_->Signal(pre_fence_.get(), op.pre_value);
        com_ptr<ID3D12Resource> back;
        if (headless_) {
            back = offscreen_;
        } else {
            op.back_buffer = sc_->GetCurrentBackBufferIndex();
            op.get_buffer = sc_->GetBuffer(op.back_buffer, __uuidof(ID3D12Resource), back.put_void());
        }
        op.alloc_reset = allocs_[index]->Reset();
        auto &cl = lists_[index];
        op.list_reset = cl->Reset(allocs_[index].get(), nullptr);
        list_open_[index] = SUCCEEDED(op.list_reset);
        if (back && headless_) {
            cl->CopyResource(back.get(), images_[index].get()); // the offscreen target stays in COPY_DEST
            m7b5_readback_record(cl.get(), op.frame);
            shot_record(cl.get()); // 0.0.24.75: periodic full-frame captures (headless)
        } else if (back) {
            D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            b.Transition = { back.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST };
            cl->ResourceBarrier(1, &b);
            cl->CopyResource(back.get(), images_[index].get());
            // 0.0.25.0: the last game frame, kept for the quick menu's repaints while the guest is paused
            if (frozen_) {
                cl->CopyResource(frozen_.get(), images_[index].get());
                frozen_valid_ = true;
            }
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            VitoxOverlayTick ov{};
            if (overlay_ok_)
                overlay_state(&ov);
            stats_frame_presented(); // 0.0.28.0: one game frame reaches the screen (the overlay's FPS)
            if (ov.visible && op.back_buffer < 2) {
                // 0.0.25.0: the quick menu over the copied frame (COPY_DEST -> RENDER_TARGET -> PRESENT)
                D3D12_RESOURCE_BARRIER rb = b;
                rb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                cl->ResourceBarrier(1, &rb);
                overlay_record(cl.get(), op.back_buffer, ov);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            } else if (cursor_record(cl.get(), back.get(), op.back_buffer, b)) {
                // 0.0.24.54: the touch cursor is drawn over the copied frame (COPY_DEST -> RENDER_TARGET -> PRESENT)
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            // 0.0.28.0: the performance statistics panel, top-right, over the game (and the quick menu)
            if (op.back_buffer < 2 && stats_prepare(cl.get())) {
                if (b.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                    D3D12_RESOURCE_BARRIER rb = b;
                    rb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    cl->ResourceBarrier(1, &rb);
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
                stats_record(cl.get(), op.back_buffer);
            }
            cl->ResourceBarrier(1, &b);
            last_game_present_ = std::chrono::steady_clock::now();
        }
        op.close = cl->Close();
        list_open_[index] = false;
        ID3D12CommandList *lists[] = { cl.get() };
        if (SUCCEEDED(op.close) && back) {
            queue_->ExecuteCommandLists(1, lists);
            op.executed = true;
        }
        // copy fence: in queue order right after the copy, before anything Present inserts
        op.copy_value = ++copy_value_;
        op.signal_copy = queue_->Signal(copy_fence_.get(), op.copy_value);
        done_copy_[index] = op.copy_value;
        if (!headless_)
            op.present = sc_->Present(1, 0);
        op.frame_value = ++frame_value_;
        op.signal_frame = queue_->Signal(frame_fence_.get(), op.frame_value);
        done_[index] = op.frame_value;
        op.removed = d12_->GetDeviceRemovedReason();
        ops_[ops_n_++ % ops_.size()] = op;
        const bool ok = SUCCEEDED(op.wait) && SUCCEEDED(op.get_buffer) && SUCCEEDED(op.alloc_reset) && SUCCEEDED(op.list_reset) && SUCCEEDED(op.close)
            && op.executed && SUCCEEDED(op.signal_pre) && SUCCEEDED(op.signal_copy) && SUCCEEDED(op.present) && SUCCEEDED(op.signal_frame) && op.pace_ok;
        if (!ok) {
            ++present_failures_;
            if (first_failure_ == S_OK)
                first_failure_ = FAILED(op.present) ? op.present : FAILED(op.close) ? op.close : FAILED(op.get_buffer) ? op.get_buffer : FAILED(op.wait) ? op.wait : E_FAIL;
        }
        if (logged(op.frame) || !ok)
            log_ops(op, "present");
        return ok;
    }

    void snapshot(uint32_t id, const char *reason) override {
        if (!d12_)
            return;
        const uint32_t bb = sc_ ? sc_->GetCurrentBackBufferIndex() : 0;
        std::string lists;
        for (uint32_t i = 0; i < n_; ++i) {
            char b[96];
            snprintf(b, sizeof(b), "%u:%s/copy %llu/frame %llu ", i, list_open_[i] ? "OPEN" : "closed", ull(done_copy_[i]), ull(done_[i]));
            lists += b;
        }
        plog("[M6C.1] SNAPSHOT %u host (%s), variant %s: shared Vulkan fence %p GetCompletedValue=%llu (expected wait value %llu) | pre-copy fence completed=%llu (last signalled %llu) | copy fence completed=%llu "
             "(last signalled %llu) | frame fence completed=%llu (last signalled %llu) | current back buffer index=%u | command lists (image:state/copy/frame "
             "value) %s| frames presented=%llu failures=%llu | variant C pacing total %.1f ms max %.1f ms timeouts %u | fence imports=%u | removed=0x%08lX",
            id, reason, vitox_xp_variant_name(variant_), static_cast<void *>(vk_fence_.get()), vk_fence_ ? ull(vk_fence_->GetCompletedValue()) : 0ull,
            ull(last_wait_value_), ull(pre_fence_->GetCompletedValue()), ull(pre_value_), ull(copy_fence_->GetCompletedValue()), ull(copy_value_), ull(frame_fence_->GetCompletedValue()), ull(frame_value_), bb,
            lists.c_str(), ull(frames_), ull(present_failures_), pace_total_ms_, pace_max_ms_, pace_timeouts_, fence_imports_, hx(d12_->GetDeviceRemovedReason()));
        const uint64_t first = ops_n_ > ops_.size() ? ops_n_ - ops_.size() : 0;
        for (uint64_t k = first; k < ops_n_; ++k)
            log_ops(ops_[k % ops_.size()], "SNAPSHOT op record");
    }

    void release() override {
        if (!idle(3000)) {
            snapshot(0, "release: host queue not idle after 3 s");
            // diagnostics only: if the host queue sits on the shared-fence Wait, release it from the CPU so the queue can drain
            if (vk_fence_ && vk_fence_->GetCompletedValue() < last_wait_value_) {
                const HRESULT hs = vk_fence_->Signal(last_wait_value_);
                plog("release: RECOVERY (diagnostic) CPU Signal(shared Vulkan fence, %llu) hr=0x%08lX", ull(last_wait_value_), hx(hs));
            } else if (vk_fence_) {
                // 0.0.13.2 wake-up test: the value is already reached; re-signal the SAME value from the CPU. If the host
                // queue then drains, its GPU Wait on the shared fence missed the wake-up.
                const uint64_t cur = vk_fence_->GetCompletedValue();
                const uint64_t pre0 = pre_fence_->GetCompletedValue(), copy0 = copy_fence_->GetCompletedValue();
                const HRESULT hs = vk_fence_->Signal(cur);
                Sleep(500);
                plog("release: WAKE-UP TEST (diagnostic) CPU Signal(shared Vulkan fence, %llu = current value) hr=0x%08lX -> after 500 ms pre-copy fence "
                     "%llu -> %llu, copy fence %llu -> %llu, frame fence %llu",
                    ull(cur), hx(hs), ull(pre0), ull(pre_fence_->GetCompletedValue()), ull(copy0), ull(copy_fence_->GetCompletedValue()),
                    ull(frame_fence_->GetCompletedValue()));
            }
            if (!idle(3000)) {
                wedged_ = true;
                snapshot(0, "release: host queue STILL not idle - host objects leaked, remaining variants skipped");
                return;
            }
            plog("release: host queue drained after recovery");
        }
        for (auto &img : images_)
            img = nullptr;
        vk_fence_ = nullptr;
        plog("release: shared images + Vulkan fence dropped (host idle)");
    }

    void shutdown() {
        if (!d12_ || wedged_)
            return;
        idle(3000);
        m7b5_readback_log(); // M7B.5: the last pending sample, now that the queue is idle
        removed_ = d12_->GetDeviceRemovedReason();
        lists_.clear();
        allocs_.clear();
        images_.clear();
        vk_fence_ = nullptr;
        sc_ = nullptr;
        offscreen_ = nullptr;
        queue_ = nullptr;
        frame_fence_ = nullptr;
        copy_fence_ = nullptr;
        pre_fence_ = nullptr;
        if (event_)
            CloseHandle(event_);
        event_ = nullptr;
        if (waitable_)
            CloseHandle(waitable_);
        waitable_ = nullptr;
        d12_ = nullptr;
        factory_ = nullptr;
    }

    uint64_t frames() const { return frames_; }
    uint64_t present_failures() const { return present_failures_; }
    HRESULT first_failure() const { return first_failure_; }
    HRESULT removed() const { return d12_ ? d12_->GetDeviceRemovedReason() : removed_; }
    bool wedged() const { return wedged_; }
    double pace_avg_ms() const { return frames_ ? pace_total_ms_ / frames_ : 0; }
    double pace_max_ms() const { return pace_max_ms_; }
    uint32_t pace_timeouts() const { return pace_timeouts_; }
    uint32_t watchdog_count() const { return watchdog_; }
    uint64_t fallback_frames() const { return fallback_frames_; }
    double fallback_avg_ms() const { return fallback_frames_ ? fallback_ms_ / fallback_frames_ : 0; }
    double fallback_max_ms() const { return fallback_max_ms_; }
    double copy_wait_avg_ms() const { return copy_waits_ ? copy_wait_total_ms_ / copy_waits_ : 0; }
    double copy_wait_max_ms() const { return copy_wait_max_ms_; }
    std::string fence_values() const {
        char b[220];
        snprintf(b, sizeof(b), "shared %llu (last wait %llu) | pre-copy %llu/%llu | copy %llu/%llu | frame %llu/%llu",
            vk_fence_ ? ull(vk_fence_->GetCompletedValue()) : 0ull, ull(last_wait_value_), pre_fence_ ? ull(pre_fence_->GetCompletedValue()) : 0ull,
            ull(pre_value_), copy_fence_ ? ull(copy_fence_->GetCompletedValue()) : 0ull, ull(copy_value_),
            frame_fence_ ? ull(frame_fence_->GetCompletedValue()) : 0ull, ull(frame_value_));
        return b;
    }

private:
    // production: an image may be rendered again once the host COPY out of it completed (Present is not in the path)
    bool wait_copy_done(uint32_t index, uint32_t timeout_ms) {
        if (copy_fence_->GetCompletedValue() >= done_copy_[index])
            return true;
        const auto t0 = std::chrono::steady_clock::now();
        copy_fence_->SetEventOnCompletion(done_copy_[index], event_);
        DWORD w = WaitForSingleObjectEx(event_, std::min<uint32_t>(timeout_ms, 100), FALSE);
        if (w != WAIT_OBJECT_0)
            watchdog_check("copy fence not reached within 100 ms");
        if (w != WAIT_OBJECT_0 && timeout_ms > 100) {
            copy_fence_->SetEventOnCompletion(done_copy_[index], event_);
            w = WaitForSingleObjectEx(event_, timeout_ms - 100, FALSE);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++copy_waits_;
        copy_wait_total_ms_ += ms;
        copy_wait_max_ms_ = std::max(copy_wait_max_ms_, ms);
        return w == WAIT_OBJECT_0;
    }

    // M6C.2 labelled watchdog: the shared Vulkan value is already visible on the CPU, yet the host queue's pre-copy /
    // copy completion has not advanced. Logged, counted, a wake-up test, then the CPU-wait fallback.
    void watchdog_check(const char *why) {
        const uint64_t shared = vk_fence_ ? vk_fence_->GetCompletedValue() : 0;
        const uint64_t pre = pre_fence_->GetCompletedValue(), copy = copy_fence_->GetCompletedValue();
        const bool stuck = last_wait_value_ && shared >= last_wait_value_ && (pre < pre_value_ || copy < copy_value_);
        plog("[M6C.2] watchdog probe (%s): shared fence %llu (last wait %llu) pre-copy %llu/%llu copy %llu/%llu frame %llu/%llu -> %s", why, ull(shared),
            ull(last_wait_value_), ull(pre), ull(pre_value_), ull(copy), ull(copy_value_), ull(frame_fence_->GetCompletedValue()), ull(frame_value_),
            stuck ? "FIRED" : "not stuck (shared value not reached yet, or the host already caught up)");
        if (!stuck || watchdog_frame_ == frames_) // one count per stuck frame
            return;
        watchdog_frame_ = frames_;
        ++watchdog_;
        snapshot(1000 + watchdog_, "M6C.2 WATCHDOG");
        if (!cpu_fallback_) {
            const HRESULT hs = vk_fence_->Signal(shared); // wake-up test: same value
            Sleep(200);
            plog("[M6C.2] watchdog wake-up test: CPU Signal(shared, %llu = current) hr=0x%08lX -> pre-copy %llu -> %llu, copy %llu -> %llu", ull(shared), hx(hs),
                ull(pre), ull(pre_fence_->GetCompletedValue()), ull(copy), ull(copy_fence_->GetCompletedValue()));
            cpu_fallback_ = true;
            plog("[M6C.2] watchdog: switching to the LABELLED CPU-wait fallback for the following frames (the run is DIAGNOSTIC, not a final pass)");
        }
    }

    void log_ops(const FrameOps &o, const char *what) {
        plog("%s #%llu image %u: [pace %.2f ms %s] Wait(shared, %llu)=0x%08lX Signal(pre, %llu)=0x%08lX | back buffer %u GetBuffer=0x%08lX | allocator Reset=0x%08lX list Reset=0x%08lX "
             "Close=0x%08lX executed=%d | Signal(copy, %llu)=0x%08lX | Present(1,0)=0x%08lX | Signal(frame, %llu)=0x%08lX | removed=0x%08lX",
            what, ull(o.frame), o.image, o.pace_ms, o.pace_ok ? "ok" : "TIMEOUT", ull(o.wait_value), hx(o.wait), ull(o.pre_value), hx(o.signal_pre), o.back_buffer, hx(o.get_buffer),
            hx(o.alloc_reset), hx(o.list_reset), hx(o.close), o.executed, ull(o.copy_value), hx(o.signal_copy), hx(o.present), ull(o.frame_value),
            hx(o.signal_frame), hx(o.removed));
    }

    bool idle(DWORD timeout_ms) {
        if (!queue_ || !frame_fence_)
            return true;
        queue_->Signal(frame_fence_.get(), ++frame_value_);
        if (frame_fence_->GetCompletedValue() >= frame_value_)
            return true;
        frame_fence_->SetEventOnCompletion(frame_value_, event_);
        return WaitForSingleObjectEx(event_, timeout_ms, FALSE) == WAIT_OBJECT_0;
    }

    // ---- M7B.5 DIAGNOSTIC checkpoint C (headless, read-only): every 60th frame, after the copy into the host-owned offscreen
    //      target, copy three 1x1 texels of it into a readback buffer on the host's own list (offscreen: COPY_DEST ->
    //      COPY_SOURCE -> COPY_DEST). The shared image itself is not touched. Logged once the copy fence has passed.
    void m7b5_readback_log() {
        if (!rb_frame_ || !copy_fence_ || copy_fence_->GetCompletedValue() < rb_copy_value_)
            return;
        uint8_t *p = nullptr;
        const D3D12_RANGE r{ 0, kRbSlots * 512 };
        if (SUCCEEDED(rb_->Map(0, &r, reinterpret_cast<void **>(&p))) && p) {
            uint32_t v[kRbSlots];
            for (uint32_t i = 0; i < kRbSlots; i++)
                memcpy(&v[i], p + i * 512, 4);
            const D3D12_RANGE none{ 0, 0 };
            rb_->Unmap(0, &none);
            plog("[M7B.5] C presented image (host copy, B8G8R8A8 as BGRA bytes LE) frame %llu: centre(%u,%u)=0x%08X inner-corner(40,40)=0x%08X "
                 "upper(%u,%u)=0x%08X",
                ull(rb_frame_), w_ / 2, h_ / 2, v[0], v[1], w_ / 2, h_ / 4, v[2]);
        }
        rb_frame_ = 0;
    }
    // ---- 0.0.24.75 (M9A.6 DIAGNOSTIC, headless only, read-only): every 30 s from 120 s, the whole host copy of the presented
    //      frame (offscreen: COPY_DEST -> COPY_SOURCE -> COPY_DEST) into a readback buffer; once its copy fence passed, written
    //      half-size as LocalStaterame_NN.bmp (24-bit, at most 12) - shows what a headless run is on screen
    void shot_write_pending() {
        if (!shot_pending_ || !copy_fence_ || copy_fence_->GetCompletedValue() < shot_copy_value_)
            return;
        shot_pending_ = false;
        uint8_t *p = nullptr;
        const D3D12_RANGE r{ 0, static_cast<SIZE_T>(shot_pitch_) * h_ };
        if (FAILED(shot_rb_->Map(0, &r, reinterpret_cast<void **>(&p))) || !p)
            return;
        const uint32_t ow = w_ / 2, oh = h_ / 2, row = (ow * 3 + 3) & ~3u;
        std::vector<uint8_t> bmp(54 + static_cast<size_t>(row) * oh);
        const auto put32 = [&](size_t o, uint32_t v) { memcpy(&bmp[o], &v, 4); };
        bmp[0] = 'B', bmp[1] = 'M';
        put32(2, static_cast<uint32_t>(bmp.size())), put32(10, 54), put32(14, 40), put32(18, ow), put32(22, oh);
        bmp[26] = 1, bmp[28] = 24;
        put32(34, static_cast<uint32_t>(row) * oh);
        for (uint32_t y = 0; y < oh; ++y) {
            const uint8_t *src = p + static_cast<size_t>(shot_pitch_) * (y * 2);
            uint8_t *dst = &bmp[54 + static_cast<size_t>(row) * (oh - 1 - y)]; // bottom-up rows
            for (uint32_t x = 0; x < ow; ++x)
                memcpy(dst + x * 3, src + x * 2 * 4, 3); // BGRA -> BGR
        }
        const D3D12_RANGE none{ 0, 0 };
        shot_rb_->Unmap(0, &none);
        wchar_t name[32];
        swprintf_s(name, L"\\frame_%02u.bmp", shot_n_ - 1);
        const std::wstring path = shot_dir() + name;
        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
            fwrite(bmp.data(), 1, bmp.size(), f);
            fclose(f);
        }
        plog("[M9A.6] frame capture %u written (%ux%u)", shot_n_ - 1, ow, oh);
    }
    void shot_record(ID3D12GraphicsCommandList *cl) {
        shot_write_pending();
        const auto now = std::chrono::steady_clock::now();
        if (shot_t0_ == std::chrono::steady_clock::time_point{})
            shot_t0_ = now;
        if (shot_pending_ || shot_n_ >= 12 || shot_dir().empty() || now - shot_t0_ < std::chrono::seconds(120 + 30 * shot_n_))
            return;
        if (!shot_rb_) {
            shot_pitch_ = (w_ * 4 + 255) & ~255u;
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = static_cast<UINT64>(shot_pitch_) * h_;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(d12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                    shot_rb_.put_void())))
                return;
        }
        D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = { offscreen_.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
        cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{ shot_rb_.get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, w_, h_, 1, shot_pitch_ };
        D3D12_TEXTURE_COPY_LOCATION src{ offscreen_.get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &b);
        shot_pending_ = true;
        shot_copy_value_ = copy_value_ + 1;
        ++shot_n_;
    }
    com_ptr<ID3D12Resource> shot_rb_;
    uint32_t shot_pitch_ = 0, shot_n_ = 0;
    uint64_t shot_copy_value_ = 0;
    bool shot_pending_ = false;
    std::chrono::steady_clock::time_point shot_t0_{};

    void m7b5_readback_record(ID3D12GraphicsCommandList *cl, uint64_t frame) {
        if (!headless_ || frame % 60 != 0)
            return;
        m7b5_readback_log(); // the previous sample (60 frames ago) has long completed
        if (rb_frame_)
            return; // still pending: skip this sample rather than overwrite it
        if (!rb_) {
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = kRbSlots * 512;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const HRESULT hr = d12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                rb_.put_void());
            plog("[M7B.5] C readback buffer %u B hr=0x%08lX", kRbSlots * 512, hx(hr));
            if (FAILED(hr))
                return;
        }
        D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = { offscreen_.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
        cl->ResourceBarrier(1, &b);
        const uint32_t pts[kRbSlots][2] = { { w_ / 2, h_ / 2 }, { 40, 40 }, { w_ / 2, h_ / 4 } };
        for (uint32_t i = 0; i < kRbSlots; i++) {
            D3D12_TEXTURE_COPY_LOCATION dst{ rb_.get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Offset = i * 512;
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 256 };
            D3D12_TEXTURE_COPY_LOCATION src{ offscreen_.get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            src.SubresourceIndex = 0;
            const D3D12_BOX box{ pts[i][0], pts[i][1], 0, pts[i][0] + 1, pts[i][1] + 1, 1 };
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &b);
        rb_frame_ = frame;
        rb_copy_value_ = copy_value_ + 1; // this frame's copy-fence value (signalled right after this list executes)
    }

    // ---- 0.0.24.54 (M9A.6) virtual touch cursor overlay (host_input.cpp owns the state; visual presenter only) ----
    void cursor_init() {
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp.Constants = { 0, 0, 8 };
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{ 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        com_ptr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put());
        if (SUCCEEDED(hr))
            hr = d12_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), cursor_rs_.put_void());
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = cursor_rs_.get();
        pd.VS = { kVitoxCursorVs, sizeof(kVitoxCursorVs) };
        pd.PS = { kVitoxCursorPs, sizeof(kVitoxCursorPs) };
        D3D12_RENDER_TARGET_BLEND_DESC &bl = pd.BlendState.RenderTarget[0];
        bl.BlendEnable = TRUE; // premultiplied alpha
        bl.SrcBlend = D3D12_BLEND_ONE;
        bl.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOp = D3D12_BLEND_OP_ADD;
        bl.SrcBlendAlpha = D3D12_BLEND_ONE;
        bl.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bl.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pd.SampleDesc.Count = 1;
        if (SUCCEEDED(hr))
            hr = d12_->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), cursor_pso_.put_void());
        D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
        if (SUCCEEDED(hr))
            hr = d12_->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), cursor_rtv_.put_void());
        cursor_rtv_size_ = d12_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; SUCCEEDED(hr) && i < 2; ++i) {
            com_ptr<ID3D12Resource> bb;
            hr = sc_->GetBuffer(i, __uuidof(ID3D12Resource), bb.put_void());
            if (SUCCEEDED(hr)) {
                D3D12_CPU_DESCRIPTOR_HANDLE h = cursor_rtv_->GetCPUDescriptorHandleForHeapStart();
                h.ptr += i * cursor_rtv_size_;
                d12_->CreateRenderTargetView(bb.get(), nullptr, h);
            }
        }
        cursor_ok_ = SUCCEEDED(hr);
        plog("bind: touch cursor overlay (root constants + PSO + 2 RTVs) hr=0x%08lX -> %s", hx(hr), cursor_ok_ ? "ready" : "DISABLED");
        if (cursor_ok_) {
            loading_init();
            overlay_init();
            stats_init();
        }
    }

    // ---- 0.0.25.0 Vitox quick menu (R3): panel texture (overlay_panel.cpp) + shaders/overlay.hlsl, frozen last frame ----
    void overlay_init() {
        const auto t0 = std::chrono::steady_clock::now();
        const float k = static_cast<float>(h_) / 1080.0f;
        panel_w_ = static_cast<uint32_t>(640.0f * k + 0.5f);
        panel_h_ = h_;
        std::vector<uint8_t> pixels;
        const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC; RPC_E_CHANGED_MODE = already initialised
        const bool drawn = overlay_render_panel(panel_w_, panel_h_, k, pixels, panel_items_);
        if (SUCCEEDED(ci))
            CoUninitialize();
        if (!drawn) {
            plog("bind: quick menu panel draw (Direct2D/WIC) FAILED -> quick menu DISABLED");
            return;
        }
        // root: 16 constants (b0) + one SRV table (t0) + a static linear clamp sampler (s0)
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants = { 0, 0, 20 };
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable = { 1, &range };
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{ 2, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        com_ptr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put());
        if (SUCCEEDED(hr))
            hr = d12_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), overlay_rs_.put_void());
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = overlay_rs_.get();
        pd.VS = { kVitoxOverlayVs, sizeof(kVitoxOverlayVs) };
        pd.PS = { kVitoxOverlayPs, sizeof(kVitoxOverlayPs) };
        D3D12_RENDER_TARGET_BLEND_DESC &bl = pd.BlendState.RenderTarget[0];
        bl.BlendEnable = TRUE; // premultiplied alpha
        bl.SrcBlend = D3D12_BLEND_ONE;
        bl.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOp = D3D12_BLEND_OP_ADD;
        bl.SrcBlendAlpha = D3D12_BLEND_ONE;
        bl.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bl.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pd.SampleDesc.Count = 1;
        if (SUCCEEDED(hr))
            hr = d12_->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), overlay_pso_.put_void());
        // the panel texture (2 variants side by side) and the frozen-frame copy target
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = panel_w_ * 4; // 0.0.28.0: OFF normal | OFF confirm | ON normal | ON confirm
        td.Height = panel_h_;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        if (SUCCEEDED(hr))
            hr = d12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), panel_tex_.put_void());
        D3D12_RESOURCE_DESC fd = td;
        fd.Width = w_;
        fd.Height = h_;
        if (SUCCEEDED(hr))
            hr = d12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &fd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), frozen_.put_void());
        D3D12_DESCRIPTOR_HEAP_DESC dh{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        if (SUCCEEDED(hr))
            hr = d12_->CreateDescriptorHeap(&dh, __uuidof(ID3D12DescriptorHeap), overlay_srv_.put_void());
        if (SUCCEEDED(hr))
            d12_->CreateShaderResourceView(panel_tex_.get(), nullptr, overlay_srv_->GetCPUDescriptorHandleForHeapStart());
        // upload: one placed footprint, row pitch aligned to 256
        const uint32_t row = panel_w_ * 4 * 4, pitch = (row + 255) & ~255u;
        com_ptr<ID3D12Resource> upload;
        D3D12_HEAP_PROPERTIES uh{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(pitch) * panel_h_;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(hr))
            hr = d12_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), upload.put_void());
        uint8_t *dst = nullptr;
        if (SUCCEEDED(hr))
            hr = upload->Map(0, nullptr, reinterpret_cast<void **>(&dst));
        if (SUCCEEDED(hr)) {
            for (uint32_t y = 0; y < panel_h_; ++y)
                memcpy(dst + static_cast<size_t>(pitch) * y, pixels.data() + static_cast<size_t>(row) * y, row);
            upload->Unmap(0, nullptr);
        }
        if (SUCCEEDED(hr))
            hr = d12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), overlay_alloc_.put_void());
        if (SUCCEEDED(hr))
            hr = d12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, overlay_alloc_.get(), nullptr, __uuidof(ID3D12GraphicsCommandList), overlay_list_.put_void());
        if (SUCCEEDED(hr)) {
            D3D12_TEXTURE_COPY_LOCATION d{ panel_tex_.get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_TEXTURE_COPY_LOCATION s{ upload.get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            s.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, panel_w_ * 4, panel_h_, 1, pitch };
            overlay_list_->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            b.Transition = { panel_tex_.get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
            overlay_list_->ResourceBarrier(1, &b);
            hr = overlay_list_->Close();
        }
        if (SUCCEEDED(hr)) {
            ID3D12CommandList *lists[] = { overlay_list_.get() };
            queue_->ExecuteCommandLists(1, lists);
            overlay_value_ = ++frame_value_;
            queue_->Signal(frame_fence_.get(), overlay_value_);
            frame_fence_->SetEventOnCompletion(overlay_value_, event_);
            if (WaitForSingleObjectEx(event_, 3000, FALSE) != WAIT_OBJECT_0)
                hr = E_FAIL;
        }
        overlay_ok_ = SUCCEEDED(hr);
        plog("bind: quick menu (R3): panel %ux%u x2 (Direct2D) + PSO + SRV + frozen frame %ux%u, built in %.0f ms hr=0x%08lX -> %s", panel_w_, panel_h_, w_, h_,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), hx(hr), overlay_ok_ ? "ready" : "DISABLED");
    }

    // records the quick menu over back buffer `bi` (already in RENDER_TARGET)
    void overlay_record(ID3D12GraphicsCommandList *cl, UINT bi, const VitoxOverlayTick &ov) {
        const float fw = static_cast<float>(w_), fh = static_cast<float>(h_);
        const float gt = std::chrono::duration<float>(std::chrono::steady_clock::now() - overlay_epoch_).count();
        const OverlayItemRect &it = panel_items_[ov.selected == 0 ? 0 : 1];
        const float k[20] = { fw, fh, ov.anim, ov.time_s, static_cast<float>(panel_w_), static_cast<float>(panel_h_), static_cast<float>(panel_w_ * 4),
            static_cast<float>(panel_h_), it.x0, it.y0, it.x1, it.y1, ov.confirm_anim, ov.press_s, std::fmod(gt, 3600.0f), fh / 1080.0f,
            stats_enabled() ? 0.5f : 0.0f, 0, 0, 0 };
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = cursor_rtv_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += bi * cursor_rtv_size_;
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT view{ 0, 0, fw, fh, 0, 1 };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(w_), static_cast<LONG>(h_) };
        cl->RSSetViewports(1, &view);
        cl->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap *heaps[] = { overlay_srv_.get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootSignature(overlay_rs_.get());
        cl->SetPipelineState(overlay_pso_.get());
        cl->SetGraphicsRoot32BitConstants(0, 20, k, 0);
        cl->SetGraphicsRootDescriptorTable(1, overlay_srv_->GetGPUDescriptorHandleForHeapStart());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
        ++overlay_frames_;
    }

    bool present_overlay() override {
        std::lock_guard<std::mutex> present_lock(present_m_);
        if (headless_ || !overlay_ok_ || !sc_ || !frozen_valid_)
            return false;
        const auto now = std::chrono::steady_clock::now();
        // the game still presents (the pause has not landed yet, or the menu is closing): those frames carry the menu
        if (now - last_game_present_ < std::chrono::milliseconds(40) || now - last_overlay_present_ < std::chrono::milliseconds(8))
            return false;
        VitoxOverlayTick ov{};
        overlay_state(&ov);
        if (!ov.visible)
            return false;
        if (variant_ == VitoxXpVariant::P_Production && waitable_)
            WaitForSingleObjectEx(waitable_, 100, FALSE);
        if (overlay_value_ && frame_fence_->GetCompletedValue() < overlay_value_) {
            frame_fence_->SetEventOnCompletion(overlay_value_, event_);
            WaitForSingleObjectEx(event_, 1000, FALSE);
        }
        const UINT bi = sc_->GetCurrentBackBufferIndex();
        com_ptr<ID3D12Resource> back;
        if (bi >= 2 || FAILED(sc_->GetBuffer(bi, __uuidof(ID3D12Resource), back.put_void())))
            return false;
        if (FAILED(overlay_alloc_->Reset()) || FAILED(overlay_list_->Reset(overlay_alloc_.get(), nullptr)))
            return false;
        auto &cl = overlay_list_;
        D3D12_RESOURCE_BARRIER b[2]{};
        b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition = { frozen_.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
        b[1].Transition = { back.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST };
        cl->ResourceBarrier(2, b);
        cl->CopyResource(back.get(), frozen_.get());
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(2, b);
        overlay_record(cl.get(), bi, ov);
        if (stats_prepare(cl.get())) // 0.0.28.0: the statistics stay visible (and toggle live) while the menu is open
            stats_record(cl.get(), bi);
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b[1]);
        if (FAILED(cl->Close()))
            return false;
        ID3D12CommandList *lists[] = { cl.get() };
        queue_->ExecuteCommandLists(1, lists);
        const HRESULT hp = sc_->Present(1, 0);
        overlay_value_ = ++frame_value_;
        queue_->Signal(frame_fence_.get(), overlay_value_);
        last_overlay_present_ = std::chrono::steady_clock::now();
        if (++overlay_repaints_ == 1 || FAILED(hp) || overlay_repaints_ % 600 == 0)
            plog("present_overlay #%llu (paused-guest repaint, %llu menu frames total) Present hr=0x%08lX", ull(overlay_repaints_), ull(overlay_frames_), hx(hp));
        return SUCCEEDED(hp);
    }

    // ---- 0.0.28.0 performance statistics overlay (perf_stats.cpp draws the panel on the CPU; shaders/stats.hlsl) ----
    static constexpr uint32_t kStatsTexW = 400, kStatsTexH = 300; // at 1080p; scaled with the output height
    void stats_init() {
        const float k = static_cast<float>(h_) / 1080.0f;
        stats_tw_ = static_cast<uint32_t>(kStatsTexW * k + 0.5f);
        stats_th_ = static_cast<uint32_t>(kStatsTexH * k + 0.5f);
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants = { 0, 0, 8 };
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable = { 1, &range };
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{ 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        com_ptr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put());
        if (SUCCEEDED(hr))
            hr = d12_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), stats_rs_.put_void());
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = stats_rs_.get();
        pd.VS = { kVitoxStatsVs, sizeof(kVitoxStatsVs) };
        pd.PS = { kVitoxStatsPs, sizeof(kVitoxStatsPs) };
        D3D12_RENDER_TARGET_BLEND_DESC &bl = pd.BlendState.RenderTarget[0];
        bl.BlendEnable = TRUE; // premultiplied alpha
        bl.SrcBlend = D3D12_BLEND_ONE;
        bl.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOp = D3D12_BLEND_OP_ADD;
        bl.SrcBlendAlpha = D3D12_BLEND_ONE;
        bl.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bl.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pd.SampleDesc.Count = 1;
        if (SUCCEEDED(hr))
            hr = d12_->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), stats_pso_.put_void());
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = stats_tw_;
        td.Height = stats_th_;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        if (SUCCEEDED(hr))
            hr = d12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, __uuidof(ID3D12Resource),
                stats_tex_.put_void());
        D3D12_DESCRIPTOR_HEAP_DESC dh{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        if (SUCCEEDED(hr))
            hr = d12_->CreateDescriptorHeap(&dh, __uuidof(ID3D12DescriptorHeap), stats_srv_.put_void());
        if (SUCCEEDED(hr))
            d12_->CreateShaderResourceView(stats_tex_.get(), nullptr, stats_srv_->GetCPUDescriptorHandleForHeapStart());
        // two persistently mapped upload buffers, used alternately (one update per ~500 ms: the older one is long done)
        stats_pitch_ = (stats_tw_ * 4 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES uh{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(stats_pitch_) * stats_th_;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
            hr = d12_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
                stats_up_[i].put_void());
            if (SUCCEEDED(hr))
                hr = stats_up_[i]->Map(0, nullptr, reinterpret_cast<void **>(&stats_map_[i]));
        }
        stats_ok_ = SUCCEEDED(hr);
        plog("bind: statistics overlay (texture %ux%u, 2 upload buffers, PSO) hr=0x%08lX -> %s", stats_tw_, stats_th_, hx(hr), stats_ok_ ? "ready" : "DISABLED");
    }
    // per frame: the fade, and a new panel from the sampler (copied into the texture in this command list). true = draw
    bool stats_prepare(ID3D12GraphicsCommandList *cl) {
        if (!stats_ok_ || headless_)
            return false;
        const auto now = std::chrono::steady_clock::now();
        const float dt = stats_last_ == std::chrono::steady_clock::time_point{} ? 0.0f : std::min(0.1f, std::chrono::duration<float>(now - stats_last_).count());
        stats_last_ = now;
        const bool want = stats_enabled();
        // a 250 ms fade in / out; nothing is shown until the first real sample exists
        stats_alpha_ = want ? std::min(1.0f, stats_alpha_ + dt / 0.25f) : std::max(0.0f, stats_alpha_ - dt / 0.25f);
        uint32_t w = 0, h = 0, v = 0;
        if (want && stats_take_panel(stats_version_, stats_px_, w, h, v) && w <= stats_tw_ && h <= stats_th_) {
            const int u = stats_next_up_;
            stats_next_up_ ^= 1;
            for (uint32_t y = 0; y < h; ++y)
                memcpy(stats_map_[u] + static_cast<size_t>(stats_pitch_) * y, stats_px_.data() + static_cast<size_t>(w) * 4 * y, w * 4);
            D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            b.Transition = { stats_tex_.get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
            cl->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION d{ stats_tex_.get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_TEXTURE_COPY_LOCATION s{ stats_up_[u].get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            s.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, stats_pitch_ };
            const D3D12_BOX box{ 0, 0, 0, w, h, 1 };
            cl->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            cl->ResourceBarrier(1, &b);
            stats_w_ = w;
            stats_h_ = h;
            stats_version_ = v;
            if (++stats_updates_ == 1)
                plog("statistics overlay: first panel %ux%u uploaded", w, h);
        }
        return stats_alpha_ > 0.001f && stats_w_ > 0;
    }
    // draws the panel into back buffer `bi` (already in RENDER_TARGET), top-right, inside the screen
    void stats_record(ID3D12GraphicsCommandList *cl, UINT bi) {
        const float k = static_cast<float>(h_) / 1080.0f, margin = 28.0f * k;
        const float x0 = std::max(0.0f, static_cast<float>(w_) - margin - static_cast<float>(stats_w_)), y0 = margin;
        const float c[8] = { std::floor(x0), std::floor(y0), static_cast<float>(stats_w_), static_cast<float>(stats_h_), stats_alpha_ * stats_alpha_ * (3 - 2 * stats_alpha_), 0, 0, 0 };
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = cursor_rtv_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += bi * cursor_rtv_size_;
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT view{ 0, 0, static_cast<float>(w_), static_cast<float>(h_), 0, 1 };
        const D3D12_RECT scissor{ static_cast<LONG>(c[0]), static_cast<LONG>(c[1]), static_cast<LONG>(c[0] + c[2]), static_cast<LONG>(c[1] + c[3]) };
        cl->RSSetViewports(1, &view);
        cl->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap *heaps[] = { stats_srv_.get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootSignature(stats_rs_.get());
        cl->SetPipelineState(stats_pso_.get());
        cl->SetGraphicsRoot32BitConstants(0, 8, c, 0);
        cl->SetGraphicsRootDescriptorTable(1, stats_srv_->GetGPUDescriptorHandleForHeapStart());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
    }
    com_ptr<ID3D12RootSignature> stats_rs_;
    com_ptr<ID3D12PipelineState> stats_pso_;
    com_ptr<ID3D12DescriptorHeap> stats_srv_;
    com_ptr<ID3D12Resource> stats_tex_, stats_up_[2];
    uint8_t *stats_map_[2]{};
    std::vector<uint8_t> stats_px_;
    uint32_t stats_tw_ = 0, stats_th_ = 0, stats_pitch_ = 0, stats_w_ = 0, stats_h_ = 0, stats_version_ = 0;
    int stats_next_up_ = 0;
    uint64_t stats_updates_ = 0;
    float stats_alpha_ = 0;
    bool stats_ok_ = false;
    std::chrono::steady_clock::time_point stats_last_{};
    com_ptr<ID3D12RootSignature> overlay_rs_;
    com_ptr<ID3D12PipelineState> overlay_pso_;
    com_ptr<ID3D12DescriptorHeap> overlay_srv_;
    com_ptr<ID3D12Resource> panel_tex_, frozen_;
    com_ptr<ID3D12CommandAllocator> overlay_alloc_;
    com_ptr<ID3D12GraphicsCommandList> overlay_list_;
    OverlayItemRect panel_items_[2]{};
    uint32_t panel_w_ = 0, panel_h_ = 0;
    bool overlay_ok_ = false, frozen_valid_ = false;
    uint64_t overlay_value_ = 0, overlay_frames_ = 0, overlay_repaints_ = 0;
    std::chrono::steady_clock::time_point last_game_present_{}, last_overlay_present_{}, overlay_epoch_ = std::chrono::steady_clock::now();
    std::mutex present_m_;
    // ---- 0.0.24.198 pipeline pre-warm loading screen (shaders/loading_screen.hlsl; reuses the cursor's back-buffer RTVs) ----
    void loading_init() {
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp.Constants = { 0, 0, 32 };
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{ 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        com_ptr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put());
        if (SUCCEEDED(hr))
            hr = d12_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), loading_rs_.put_void());
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = loading_rs_.get();
        pd.VS = { kVitoxLoadingVs, sizeof(kVitoxLoadingVs) };
        pd.PS = { kVitoxLoadingPs, sizeof(kVitoxLoadingPs) };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pd.SampleDesc.Count = 1;
        if (SUCCEEDED(hr))
            hr = d12_->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), loading_pso_.put_void());
        if (SUCCEEDED(hr))
            hr = d12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), loading_alloc_.put_void());
        if (SUCCEEDED(hr))
            hr = d12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, loading_alloc_.get(), nullptr, __uuidof(ID3D12GraphicsCommandList), loading_list_.put_void());
        if (SUCCEEDED(hr))
            hr = loading_list_->Close();
        loading_ok_ = SUCCEEDED(hr);
        plog("bind: pre-warm loading screen (32 root constants + PSO + own command list) hr=0x%08lX -> %s", hx(hr), loading_ok_ ? "ready" : "DISABLED");
    }
    bool present_loading(uint32_t done, uint32_t total, const char *title) override {
        if (headless_ || !sc_ || !loading_ok_ || !cursor_ok_)
            return false;
        std::lock_guard<std::mutex> present_lock(present_m_);
        const auto now = std::chrono::steady_clock::now();
        if (!loading_frames_)
            loading_t0_ = now;
        if (variant_ == VitoxXpVariant::P_Production && waitable_)
            WaitForSingleObjectEx(waitable_, 100, FALSE);
        // the previous loading frame must be done before its allocator is reset
        if (loading_value_ && frame_fence_->GetCompletedValue() < loading_value_) {
            frame_fence_->SetEventOnCompletion(loading_value_, event_);
            WaitForSingleObjectEx(event_, 1000, FALSE);
        }
        const UINT bi = sc_->GetCurrentBackBufferIndex();
        com_ptr<ID3D12Resource> back;
        if (bi >= 2 || FAILED(sc_->GetBuffer(bi, __uuidof(ID3D12Resource), back.put_void())))
            return false;
        if (FAILED(loading_alloc_->Reset()) || FAILED(loading_list_->Reset(loading_alloc_.get(), loading_pso_.get())))
            return false;
        // root constants: screen, progress, time, line lengths, 4 lines x 24 ASCII characters
        uint32_t k[32] = {};
        const float fw = static_cast<float>(w_), fh = static_cast<float>(h_);
        const float progress = total ? std::min(1.0f, static_cast<float>(done) / static_cast<float>(total)) : 1.0f;
        const float secs = std::chrono::duration<float>(now - loading_t0_).count();
        memcpy(&k[0], &fw, 4);
        memcpy(&k[1], &fh, 4);
        memcpy(&k[2], &progress, 4);
        memcpy(&k[3], &secs, 4);
        char lines[4][25] = {};
        snprintf(lines[0], 25, "%s", title ? title : "");
        snprintf(lines[1], 25, "PREPARING SHADERS");
        snprintf(lines[2], 25, "%u / %u   %u%%", done, total, static_cast<unsigned>(progress * 100.0f + 0.5f));
        snprintf(lines[3], 25, "SAVED FOR FASTER BOOTS");
        uint8_t *chars = reinterpret_cast<uint8_t *>(&k[8]);
        for (int l = 0; l < 4; ++l) {
            const size_t n = strlen(lines[l]);
            k[4 + l] = static_cast<uint32_t>(n);
            memcpy(chars + l * 24, lines[l], n);
        }
        auto &cl = loading_list_;
        D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = { back.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = cursor_rtv_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += bi * cursor_rtv_size_;
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT view{ 0, 0, fw, fh, 0, 1 };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(w_), static_cast<LONG>(h_) };
        cl->RSSetViewports(1, &view);
        cl->RSSetScissorRects(1, &scissor);
        cl->SetGraphicsRootSignature(loading_rs_.get());
        cl->SetGraphicsRoot32BitConstants(0, 32, k, 0);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        if (FAILED(cl->Close()))
            return false;
        ID3D12CommandList *lists[] = { cl.get() };
        queue_->ExecuteCommandLists(1, lists);
        const HRESULT hp = sc_->Present(1, 0);
        loading_value_ = ++frame_value_;
        queue_->Signal(frame_fence_.get(), loading_value_);
        if (loading_frames_++ == 0 || FAILED(hp) || done == total)
            plog("present_loading #%u: %u/%u title '%s' Present hr=0x%08lX", loading_frames_, done, total, title ? title : "", hx(hp));
        return SUCCEEDED(hp);
    }
    com_ptr<ID3D12RootSignature> loading_rs_;
    com_ptr<ID3D12PipelineState> loading_pso_;
    com_ptr<ID3D12CommandAllocator> loading_alloc_;
    com_ptr<ID3D12GraphicsCommandList> loading_list_;
    bool loading_ok_ = false;
    uint32_t loading_frames_ = 0;
    uint64_t loading_value_ = 0;
    std::chrono::steady_clock::time_point loading_t0_{};
    // records the cursor draw when it is on; returns true when the back buffer was left in RENDER_TARGET
    bool cursor_record(ID3D12GraphicsCommandList *cl, ID3D12Resource *back, UINT back_index, D3D12_RESOURCE_BARRIER b) {
        VitoxTouchCursor c{};
        touch_cursor_state(&c);
        if (!cursor_ok_ || !c.active || back_index >= 2)
            return false;
        // the Vita screen inside the presented frame, as Vita3K's renderer letterboxes it (vk_renderer.cpp viewport)
        const float fw = static_cast<float>(w_), fh = static_cast<float>(h_), vita = 960.0f / 544.0f;
        float vw = fw, vh = fh, vx = 0, vy = 0;
        if (fw / fh > vita)
            vw = fh * vita, vx = (fw - vw) / 2;
        else
            vh = fw / vita, vy = (fh - vh) / 2;
        const auto ease = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return 1 - (1 - t) * (1 - t) * (1 - t); };
        cursor_press_ += ((c.pressed ? 1.0f : 0.0f) - cursor_press_) * 0.45f; // eased toward the button state per frame
        const float k[8] = { vx + c.x * vw, vy + c.y * vh, fw, fh, 26.0f * fh / 1080.0f, cursor_press_, ease(c.shown_s / 0.18f),
            c.press_s >= 0 ? c.press_s / 0.42f : 1.0f };
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.pResource = back;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = cursor_rtv_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += back_index * cursor_rtv_size_;
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT view{ 0, 0, fw, fh, 0, 1 };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(w_), static_cast<LONG>(h_) };
        cl->RSSetViewports(1, &view);
        cl->RSSetScissorRects(1, &scissor);
        cl->SetGraphicsRootSignature(cursor_rs_.get());
        cl->SetPipelineState(cursor_pso_.get());
        cl->SetGraphicsRoot32BitConstants(0, 8, k, 0);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cl->DrawInstanced(4, 1, 0, 0);
        return true;
    }
    com_ptr<ID3D12RootSignature> cursor_rs_;
    com_ptr<ID3D12PipelineState> cursor_pso_;
    com_ptr<ID3D12DescriptorHeap> cursor_rtv_;
    UINT cursor_rtv_size_ = 0;
    bool cursor_ok_ = false;
    float cursor_press_ = 0;

    void *core_window_;
    uint32_t w_, h_, n_;
    VitoxXpVariant variant_;
    bool headless_ = false;
    com_ptr<ID3D12Resource> offscreen_;
    // M7B.5 DIAGNOSTIC checkpoint C (headless only, read-only): pixels of the host copy of the presented image
    com_ptr<ID3D12Resource> rb_;
    uint64_t rb_frame_ = 0, rb_copy_value_ = 0;
    static constexpr uint32_t kRbSlots = 3; // centre, inner corner (40,40), upper (w/2, h/4); 512 B per placed 1x1 footprint
    std::chrono::steady_clock::time_point last_headless_{};
    com_ptr<IDXGIFactory4> factory_;
    com_ptr<ID3D12Device> d12_;
    com_ptr<ID3D12CommandQueue> queue_;
    com_ptr<IDXGISwapChain3> sc_;
    std::vector<com_ptr<ID3D12CommandAllocator>> allocs_;
    std::vector<com_ptr<ID3D12GraphicsCommandList>> lists_;
    std::vector<bool> list_open_;
    std::vector<com_ptr<ID3D12Resource>> images_;
    std::vector<uint64_t> done_, done_copy_; // per image: frame / copy fence value of its last host use
    com_ptr<ID3D12Fence> vk_fence_, frame_fence_, copy_fence_, pre_fence_;
    HANDLE event_ = nullptr;
    HANDLE waitable_ = nullptr; // production: frame-latency waitable object
    uint32_t watchdog_ = 0;
    uint64_t watchdog_frame_ = 0;
    bool cpu_fallback_ = false;
    uint64_t fallback_frames_ = 0, copy_waits_ = 0;
    double fallback_ms_ = 0, fallback_max_ms_ = 0, copy_wait_total_ms_ = 0, copy_wait_max_ms_ = 0;
    uint64_t frame_value_ = 0, copy_value_ = 0, pre_value_ = 0, last_wait_value_ = 0, frames_ = 0, present_failures_ = 0;
    std::array<FrameOps, 8> ops_{};
    uint64_t ops_n_ = 0;
    uint32_t fence_imports_ = 0, pace_timeouts_ = 0;
    double pace_total_ms_ = 0, pace_max_ms_ = 0;
    bool wedged_ = false;
    HRESULT first_failure_ = S_OK, removed_ = S_OK;
};

// 0.0.25.1: where the process memory sits between launcher sessions: CRT/process heaps (committed), the GPU (DXGI local
// segment usage of this process on the console's unified memory) and the rest (private commit)
void mem_breakdown(const char *tag) {
    PROCESS_MEMORY_COUNTERS_EX pmc{ sizeof(pmc) };
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
    HANDLE heaps[256];
    const DWORD n = GetProcessHeaps(256, heaps);
    unsigned long long heap_commit = 0;
    // HeapSummary is not in the app partition's import library: resolved at run time (0 when absent)
    using HeapSummaryFn = BOOL(WINAPI *)(HANDLE, DWORD, LPHEAP_SUMMARY);
    static const auto heap_summary = reinterpret_cast<HeapSummaryFn>(resolve(L"kernelbase.dll", "HeapSummary"));
    for (DWORD i = 0; heap_summary && i < n && i < 256; ++i) {
        HEAP_SUMMARY hs{ sizeof(hs) };
        if (heap_summary(heaps[i], 0, &hs))
            heap_commit += hs.cbCommitted;
    }
    unsigned long long gpu_local = 0, gpu_nonlocal = 0;
    com_ptr<IDXGIFactory4> f;
    com_ptr<IDXGIAdapter1> a;
    if (SUCCEEDED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), f.put_void())) && SUCCEEDED(f->EnumAdapters1(0, a.put()))) {
        if (auto a3 = a.try_as<IDXGIAdapter3>()) {
            DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
                gpu_local = mi.CurrentUsage;
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &mi)))
                gpu_nonlocal = mi.CurrentUsage;
        }
    }
    log_line(Log::Core, "vitox mem breakdown %s: private_mib=%.1f heaps=%lu heap_committed_mib=%.1f gpu_local_mib=%.1f gpu_nonlocal_mib=%.1f", tag,
        pmc.PrivateUsage / 1048576.0, n, heap_commit / 1048576.0, gpu_local / 1048576.0, gpu_nonlocal / 1048576.0);
}

// M6C.2: false = 0.0.13.4 (controlled stale-process repro), true = 0.0.13.5 (clean-process acceptance run)
constexpr bool kM6c2Acceptance = true;

std::string dzn_hash() {
    const std::wstring dir(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    std::ifstream f(dir + L"\\vulkan_dzn.dll", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return bytes.empty() ? std::string("unreadable") : vitox_sha256_hex(bytes.data(), bytes.size()) + " (" + std::to_string(bytes.size()) + " B)";
}

// Section 5 inventory only (no Present): can a CoreWindow swapchain on this console use the frame-latency waitable object?
[[maybe_unused]] void inventory_frame_latency(void *core_window, uint32_t w, uint32_t h) {
    com_ptr<IDXGIFactory4> factory;
    com_ptr<ID3D12Device> dev;
    com_ptr<ID3D12CommandQueue> queue;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void());
    if (SUCCEEDED(hr))
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), dev.put_void());
    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), queue.put_void());
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    com_ptr<IDXGISwapChain1> sc1;
    HRESULT hc = E_FAIL, hs = E_FAIL, hg = E_FAIL;
    HANDLE waitable = nullptr;
    UINT latency = 0;
    if (SUCCEEDED(hr))
        hc = factory->CreateSwapChainForCoreWindow(queue.get(), static_cast<::IUnknown *>(core_window), &sd, nullptr, sc1.put());
    if (SUCCEEDED(hc)) {
        auto sc2 = sc1.as<IDXGISwapChain2>();
        hs = sc2->SetMaximumFrameLatency(1);
        hg = sc2->GetMaximumFrameLatency(&latency);
        waitable = sc2->GetFrameLatencyWaitableObject();
        if (waitable)
            CloseHandle(waitable);
    }
    plog("[M6C.1] inventory: CoreWindow swapchain with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT create=0x%08lX | SetMaximumFrameLatency(1)=0x%08lX | "
         "GetMaximumFrameLatency=0x%08lX -> %u | GetFrameLatencyWaitableObject=%s (no Present issued; released)",
        hx(hc), hx(hs), hx(hg), latency, waitable ? "handle" : "null");
}

} // namespace

std::string probe_m6c(void *core_window, unsigned width, unsigned height, void (*pump)(), const std::wstring &local_state) {
    struct Cycle {
        VitoxXpVariant variant;
        unsigned seconds;
    };
    // M6C.2: 0.0.13.4 = the controlled stale-process repro (A first, pre-copy fence + wake-up test armed, then the
    // production path P); 0.0.13.5 = the clean-process ACCEPTANCE run (P only, 40 s).
    const std::vector<Cycle> cycles = kM6c2Acceptance ? std::vector<Cycle>{ { VitoxXpVariant::P_Production, 40 } }
                                                      : std::vector<Cycle>{ { VitoxXpVariant::A_GpuWait, 12 }, { VitoxXpVariant::P_Production, 40 } };
    plog("begin: M6C.2 %s - %s (each cycle: fresh renderer::init + late_init + display path + presenter), %ux%u, 3 images",
        kM6c2Acceptance ? "ACCEPTANCE run (clean process expected; the harness logs whether a previous Vitox process was running)"
                        : "CONTROLLED STALE-PROCESS REPRO (the previous Vitox instance was deliberately left running)",
        kM6c2Acceptance ? "P 40 s" : "A 12 s, then P 40 s", width, height);
    plog("vulkan_dzn.dll (packaged ICD) SHA-256 %s", dzn_hash().c_str());

    // CRT stderr -> LocalState (dzn shares the UCRT, so Mesa's stderr log sink lands here). Unbuffered. No D3D12 debug layer.
    const std::wstring err_path = local_state + L"\\m6c1_stderr.txt";
    FILE *ef = nullptr;
    const errno_t fe = _wfreopen_s(&ef, err_path.c_str(), L"w", stderr);
    if (fe == 0 && ef)
        setvbuf(stderr, nullptr, _IONBF, 0);
    _putenv_s("MESA_LOG_LEVEL", "debug");
    plog("stderr -> %s: %s | MESA_LOG_LEVEL=debug | D3D12 debug layer NOT enabled", narrow(err_path).c_str(), fe == 0 ? "ok" : "FAILED");
    fprintf(stderr, "[vitox] m6c1 stderr capture start\n");

    const VitoxCoreHost host{ &m6c_log, &mem_snapshot };
    const std::wstring install_dir(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    mem_snapshot("m6c1_baseline_before_any_renderer");
    std::string matrix;
    bool aborted = false;
    unsigned cycle = 0;
    for (const Cycle &c : cycles) {
        const VitoxXpVariant v = c.variant;
        const char letter = "ABCDP"[static_cast<uint32_t>(v)];
        if (aborted) {
            matrix += std::string(1, letter) + "=SKIPPED ";
            continue;
        }
        ++cycle;
        auto *presenter = new HostPresenter(core_window, width, height, 3, v);
        const VitoxRendererResult r =
            vitox_renderer_probe_run(&host, presenter, pump, local_state.c_str(), L"D:\\DevelopmentFiles\\Vitox", install_dir.c_str(), c.seconds, v, cycle);
        const VitoxExternalPresentStats s = vitox_external_present_stats();
        const HRESULT removed = presenter->removed();
        const bool wedged = presenter->wedged() || r.wedged;
        plog("variant %c end: host presents=%llu failures=%llu first_failure=0x%08lX removed=0x%08lX | pacing avg %.2f ms max %.2f ms timeouts %u | host wedged=%d "
             "vulkan wedged=%d",
            letter, ull(presenter->frames()), ull(presenter->present_failures()), hx(presenter->first_failure()), hx(removed), presenter->pace_avg_ms(),
            presenter->pace_max_ms(), presenter->pace_timeouts(), presenter->wedged(), r.wedged);
        const bool production = v == VitoxXpVariant::P_Production;
        bool accept = false;
        if (production) {
            accept = r.renderer_init && r.late_init && r.mapping_disabled && r.bridge && r.gpu_sync && r.frames_ok && r.seconds >= 30.0 && !wedged
                && presenter->watchdog_count() == 0 && presenter->fallback_frames() == 0 && s.cpu_wait_fallback == 0 && s.submits_failed == 0
                && s.presents_failed == 0 && presenter->present_failures() == 0 && presenter->pace_timeouts() == 0 && removed == S_OK && r.counters_zero;
            plog("[M6C.2] P end: frames=%llu in %.1f s (%.1f fps) | latency waitable wait avg %.2f ms max %.2f ms timeouts %u | copy-fence image reuse wait avg %.2f "
                 "ms max %.2f ms | fences: %s | watchdog_count=%u | CPU fallback frames=%llu (avg %.2f ms max %.2f ms) + renderer cpu_wait=%llu | submits failed=%llu "
                 "presents failed=%llu host failures=%llu | device_removed_reason=0x%08lX | counters_zero=%d -> %s",
                ull(r.frames), r.seconds, r.seconds > 0 ? r.frames / r.seconds : 0.0, presenter->pace_avg_ms(), presenter->pace_max_ms(), presenter->pace_timeouts(),
                presenter->copy_wait_avg_ms(), presenter->copy_wait_max_ms(), presenter->fence_values().c_str(), presenter->watchdog_count(),
                ull(presenter->fallback_frames()), presenter->fallback_avg_ms(), presenter->fallback_max_ms(), ull(s.cpu_wait_fallback), ull(s.submits_failed),
                ull(s.presents_failed), ull(presenter->present_failures()), hx(removed), r.counters_zero,
                accept ? "ACCEPTANCE CRITERIA MET (pending the user's visual confirmation)" : "NOT accepted");
        }
        if (wedged) {
            // leaked on purpose: releasing objects a stuck queue still references is unsafe. A fresh presenter (new
            // queue + CoreWindow swapchain) still works afterwards (0.0.13.1: the status display's swapchain was created
            // with the wedged one leaked); only a Vulkan-side wedge or a removed device ends the matrix.
            aborted = r.wedged || removed != S_OK;
            plog("variant %c: host presenter LEAKED (wedged)%s", letter, aborted ? " - matrix aborted" : " - continuing with a fresh presenter");
        }
        const bool ok = production ? accept : r.renderer_init && r.late_init && r.bridge && r.frames_ok && !wedged && removed == S_OK;
        char b[200];
        snprintf(b, sizeof(b), "%c=%s(%llu fr %.0fs stalls %u%s) ", letter, ok ? "OK" : r.stalled ? "STALL" : "FAIL", ull(r.frames), r.seconds, s.stalls,
            production ? (", watchdog " + std::to_string(presenter->watchdog_count()) + ", fallback " + std::to_string(presenter->fallback_frames())).c_str() : "");
        matrix += b;
        if (!wedged) {
            delete presenter;
            mem_snapshot(("m6c1_c" + std::to_string(cycle) + "_after_host_presenter_released").c_str());
        }
    }
    fprintf(stderr, "[vitox] m6c1 stderr capture end\n");
    fflush(stderr);
    plog("end: %s %s%s", kM6c2Acceptance ? "acceptance" : "stale-process repro", matrix.c_str(), aborted ? "(aborted after a wedge)" : "");
    std::string summary = std::string(kM6c2Acceptance ? "M6C.2 ACCEPT " : "M6C.2 REPRO ") + matrix;
    log_line(Log::Core, "m6c %s", summary.c_str());
    return summary;
}

// M7A: the first REAL guest SceGxm visual homebrew (VITOX0002) through the real Vita3K loader/HLE/renderer, presented by
// the accepted P_Production presenter. headless = the non-visual precheck (offscreen copy, no Present).
// homebrew 2 = VITOX0002 (M7A transfer fills), 3 = VITOX0003 (M7B Phase B, the first real GXM draw)
std::string probe_m7a(void *core_window, unsigned width, unsigned height, void (*pump)(), const std::wstring &local_state, bool headless, unsigned homebrew) {
    const char *label = homebrew == 3 ? "M7B" : "M7A";
    plog("begin: %s %s - VITOX000%u (self-made VitaSDK GXM homebrew) through the real Vita3K core, %ux%u, presenter %s", label,
        headless ? "NON-VISUAL PRECHECK" : "VISUAL", homebrew, width, height, headless ? "HEADLESS (offscreen copy, no Present)" : "P_Production");
    plog("vulkan_dzn.dll (packaged ICD) SHA-256 %s", dzn_hash().c_str());
    const std::wstring err_path = local_state + L"\\m7a_stderr.txt";
    FILE *ef = nullptr;
    const errno_t fe = _wfreopen_s(&ef, err_path.c_str(), L"w", stderr);
    if (fe == 0 && ef)
        setvbuf(stderr, nullptr, _IONBF, 0);
    _putenv_s("MESA_LOG_LEVEL", "debug");
    plog("stderr -> %s: %s | MESA_LOG_LEVEL=debug | D3D12 debug layer NOT enabled", narrow(err_path).c_str(), fe == 0 ? "ok" : "FAILED");
    fprintf(stderr, "[vitox] m7a stderr capture start\n");

    const VitoxCoreHost host{ &m6c_log, &mem_snapshot };
    const std::wstring install_dir(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    mem_snapshot("m7a_baseline");
    auto *presenter = new HostPresenter(core_window, width, height, 3, VitoxXpVariant::P_Production, headless);
    const VitoxGxmHomebrewResult r = vitox_gxm_homebrew_probe_run(&host, presenter, pump,
        (install_dir + (homebrew == 3 ? L"\\homebrew\\VITOX0003" : L"\\homebrew\\VITOX0002")).c_str(), local_state.c_str(),
        L"D:\\DevelopmentFiles\\Vitox", install_dir.c_str(), 60, homebrew);
    const HRESULT removed = presenter->removed();
    const bool wedged = presenter->wedged();
    plog("[%s] presenter end: host presents=%llu failures=%llu removed=0x%08lX | pacing avg %.2f ms max %.2f ms timeouts %u | fences: %s | watchdog_count=%u | "
         "CPU fallback frames=%llu | wedged=%d",
        label, ull(presenter->frames()), ull(presenter->present_failures()), hx(removed), presenter->pace_avg_ms(), presenter->pace_max_ms(),
        presenter->pace_timeouts(), presenter->fence_values().c_str(), presenter->watchdog_count(), ull(presenter->fallback_frames()), wedged);
    const bool presenter_ok = !wedged && presenter->watchdog_count() == 0 && presenter->fallback_frames() == 0 && presenter->present_failures() == 0
        && presenter->pace_timeouts() == 0 && removed == S_OK;
    if (!wedged)
        delete presenter;
    mem_snapshot("m7a_after_presenter_released");
    fprintf(stderr, "[vitox] m7a stderr capture end\n");
    fflush(stderr);
    const bool ok = r.installed && r.sfo && r.renderer && r.loaded && r.imports_bound && r.thread && r.exited && r.exit_ok && r.imports_real && r.gxm_calls
        && r.frames && r.gpu && r.sentinel && r.counters_zero && r.cleanup && r.root_safety && presenter_ok;
    char b[360];
    if (homebrew == 3)
        snprintf(b, sizeof(b), "M7B %s %s (presents=%llu %.0fs guest_draws=%llu gpu_draws=%llu pipelines=%llu flips=%llu exit=%d sentinel=%d imports_real=%d "
                               "presenter_ok=%d errors=%llu)",
            headless ? "PRECHECK" : "VISUAL", ok ? "PASS" : "FAIL", ull(r.presents), r.present_seconds, ull(r.fill_calls), ull(r.gpu_draws), ull(r.pipelines),
            ull(r.setframebuf_calls), r.exit_ok, r.sentinel, r.imports_real, presenter_ok, ull(r.vita3k_errors));
    else
        snprintf(b, sizeof(b), "M7A %s %s (presents=%llu %.0fs fills=%llu flips=%llu vblanks=%llu exit=%d sentinel=%d imports_real=%d presenter_ok=%d)",
            headless ? "PRECHECK" : "VISUAL", ok ? "PASS" : "FAIL", ull(r.presents), r.present_seconds, ull(r.fill_calls), ull(r.setframebuf_calls),
            ull(r.vblank_calls), r.exit_ok, r.sentinel, r.imports_real, presenter_ok);
    log_line(Log::Core, "m7 %s", b);
    return b;
}

// M8A: the user-selectable homebrew launcher. Which app runs, and whether the presenter is visual or headless, comes
// from D:\DevelopmentFiles\Vitox\homebrew\vitox-launch.txt at run time - nothing about the app is in this package.
const wchar_t *const kDevRoot = L"D:\\DevelopmentFiles\\Vitox";
static const wchar_t *kM8aDevRoot = kDevRoot;
bool m8a_launch_headless() {
    return vitox_launcher_read_config(kM8aDevRoot).headless;
}
std::string probe_m8a(void *core_window, unsigned width, unsigned height, void (*pump)(), const std::wstring &local_state) {
    return probe_m8a_with(core_window, width, height, pump, local_state, nullptr, nullptr);
}
std::string probe_m8a_with(void *core_window, unsigned width, unsigned height, void (*pump)(), const std::wstring &local_state,
    const VitoxLaunchConfig *cfg_in, bool *user_quit) {
    shot_dir() = local_state; // 0.0.24.75 frame captures
    const VitoxLaunchConfig cfg = cfg_in ? *cfg_in : vitox_launcher_read_config(kM8aDevRoot);
    plog("begin: M8A homebrew launcher - app from %ls\\homebrew (config %s: app='%s' presenter=%s timeout_s=%u app_root=%s licensed_load=%s%s%s), %ux%u",
        kM8aDevRoot, cfg.found ? "vitox-launch.txt" : "absent", cfg.app, cfg.headless ? "HEADLESS (offscreen copy, no Present)" : "P_Production (visual)",
        cfg.timeout_s, cfg.app_root[0] ? cfg.app_root : "homebrew", cfg.licensed_load ? "staged-license" : "off", cfg.error[0] ? " | CONFIG ERROR: " : "",
        cfg.error, width, height);
    plog("vulkan_dzn.dll (packaged ICD) SHA-256 %s", dzn_hash().c_str());
    const std::wstring err_path = local_state + L"\\m8a_stderr.txt";
    FILE *ef = nullptr;
    const errno_t fe = _wfreopen_s(&ef, err_path.c_str(), L"w", stderr);
    if (fe == 0 && ef)
        setvbuf(stderr, nullptr, _IONBF, 0);
    plog("stderr -> %s: %s | D3D12 debug layer NOT enabled", narrow(err_path).c_str(), fe == 0 ? "ok" : "FAILED");
    fprintf(stderr, "[vitox] m8a stderr capture start\n");

    const VitoxCoreHost host{ &m6c_log, &mem_snapshot, &log_checkpoint }; // 0.0.24.3: durable launcher checkpoints
    const std::wstring install_dir(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    mem_snapshot("m8a_baseline");
    mem_breakdown("session begin");
    HostPresenter *presenter = nullptr;
    host_input_start(); // M8B.2: Xbox controller -> REAL Vita3K ctrl/SceCtrl
    host_text_start();  // M8F.2B: system keyboard text -> REAL Vita3K IME dialog state
    overlay_session(!cfg.headless); // 0.0.25.0: the R3 quick menu (visual sessions)
    if (!cfg.headless)
        stats_session_begin(height); // 0.0.28.0: the statistics sampler (idle while the overlay is off)
    // 0.0.29.3: sceAppMgrLoadExec ends the process with a relaunch request; like upstream's frontend (sdl-frontend
    // session.cpp), the same app runs again with the requested executable + argv, as a fresh emulator session
    VitoxLaunchConfig run_cfg = cfg;
    VitoxLaunchResult r{};
    for (int hop = 0;; ++hop) {
        presenter = new HostPresenter(core_window, width, height, 3, VitoxXpVariant::P_Production, cfg.headless);
        r = vitox_homebrew_launch_run(&host, presenter, pump, &run_cfg, local_state.c_str(), kM8aDevRoot, install_dir.c_str());
        if (!r.load_exec || r.user_quit || hop >= 3 || presenter->wedged())
            break;
        plog("[M8A] load_exec %d: the guest asked for app0:%s - relaunching the app with it", hop + 1, r.exec_self);
        delete presenter;
        presenter = nullptr;
        strncpy_s(run_cfg.exec_self, r.exec_self, _TRUNCATE);
        strncpy_s(run_cfg.exec_args, r.exec_args, _TRUNCATE);
    }
    stats_session_end();
    overlay_session(false);
    if (user_quit)
        *user_quit = r.user_quit;
    host_text_stop();
    host_input_stop();
    const HRESULT removed = presenter->removed();
    const bool wedged = presenter->wedged();
    plog("[M8A] presenter end: host presents=%llu failures=%llu removed=0x%08lX | pacing avg %.2f ms max %.2f ms timeouts %u | fences: %s | watchdog_count=%u | "
         "CPU fallback frames=%llu | wedged=%d",
        ull(presenter->frames()), ull(presenter->present_failures()), hx(removed), presenter->pace_avg_ms(), presenter->pace_max_ms(), presenter->pace_timeouts(),
        presenter->fence_values().c_str(), presenter->watchdog_count(), ull(presenter->fallback_frames()), wedged);
    const bool presenter_ok = !wedged && presenter->watchdog_count() == 0 && presenter->fallback_frames() == 0 && presenter->present_failures() == 0
        && presenter->pace_timeouts() == 0 && removed == S_OK;
    if (!wedged)
        delete presenter;
    mem_snapshot("m8a_after_presenter_released");
    mem_breakdown("session end (renderer + presenter released)");
    fprintf(stderr, "[vitox] m8a stderr capture end\n");
    fflush(stderr);
    log_checkpoint("m9a4 cp host: launcher returned, presenter released");
    // a guest that runs until the timeout (or until the user returns to the launcher) is a valid outcome (most homebrew
    // never exits); an exit must be code 0
    const bool ran = r.thread && (r.exit_ok || r.timed_out);
    // M8E fail-closed: unselected imports the guest never calls are allowed; a call to one traps and fails the run
    const bool imports_ok = r.imports_supported || (cfg.fail_closed && !r.trapped);
    const bool ok = r.discovered && r.selected && r.installed && r.sfo && r.renderer && r.loaded && imports_ok && ran && r.unknown_zero && r.cleanup
        && r.root_safety && presenter_ok && r.pso_failures <= 0;
    // M9A.2C: app-staging content check (protected executables fail the launch closed before anything is installed)
    // M9A.3A: licensed_load=staged-license validates and always stops before guest code in this build
    char content[160] = "";
    if (r.licensed_requested)
        snprintf(content, sizeof(content), " licensed_load=STOP(validation=%s staging_unchanged=%d protected=%u, nothing run)",
            r.licensed_valid ? "PASS" : "FAIL", r.staging_unchanged, r.content_protected);
    else if (r.stop_reason[0])
        snprintf(content, sizeof(content), " STOP(%.100s)", r.stop_reason);
    else if (r.content_checked)
        snprintf(content, sizeof(content), r.content_readable ? " content=loader-readable" : " content=PROTECTED x%u (fail-closed, nothing run)",
            r.content_protected);
    char b[520];
    snprintf(b, sizeof(b), "M8A %s %s %s '%s' from '%s' (candidates=%u rejected=%u imports=%u unsupported=%u %s %.0fs presents=%llu draws=%llu pipelines=%llu "
                           "pso_failures=%ld unknown_zero=%d presenter_ok=%d cleanup=%d root_safety=%d errors=%llu%s)",
        cfg.trap ? "TRAP-DISCOVERY" : cfg.headless ? (cfg.fail_closed ? "PRECHECK(fail-closed)" : "PRECHECK") : (cfg.fail_closed ? "VISUAL(fail-closed)" : "VISUAL"), ok ? "PASS" : (r.trapped ? "FAIL/NEEDS_CLOSURE" : "FAIL"),
        r.title_id[0] ? r.title_id : "-", r.title, r.folder, r.candidates, r.rejected, r.bound_imports, r.unsupported_imports,
        r.trapped ? "TRAPPED" : r.exited ? (r.exit_ok ? "exit=0" : "exit!=0") : r.user_quit ? "user-quit" : (r.timed_out ? "timeout" : "not-run"), r.run_seconds, ull(r.presents),
        ull(r.gpu_draws), ull(r.pipelines), r.pso_failures, r.unknown_zero, presenter_ok, r.cleanup, r.root_safety, ull(r.vita3k_errors), content);
    log_line(Log::Core, "m8 %s", b);
    log_checkpoint((std::string("m9a4 cp result: ") + b).c_str());
    return b;
}

// M8C.1 (NON-VISUAL): Vita3K's real install_pup of the user's three approved PUPs into the isolated
// D:\DevelopmentFiles\Vitox\firmware-staging\vita-fs (never the active vita-fs). Runs once; a no-op without inputs.
std::string probe_m8c() {
    const VitoxCoreHost host{ &m6c_log, &mem_snapshot };
    mem_snapshot("m8c_baseline");
    const VitoxFirmwareStageResult r = vitox_firmware_stage_run(&host, kM8aDevRoot);
    mem_snapshot("m8c_after_stage");
    char b[300];
    if (!r.requested)
        snprintf(b, sizeof(b), "M8C.1 firmware staging: not requested / already staged");
    else
        snprintf(b, sizeof(b), "M8C.1 firmware staging %s (selftest=%d hashes=%d installed=%d partitions=%d libpgf=%d fonts=%d contained=%d files=%u bytes=%llu)",
            r.selftest && r.hashes && r.installed && r.partitions && r.libpgf && r.fonts && r.contained ? "PASS" : "FAIL", r.selftest, r.hashes, r.installed,
            r.partitions, r.libpgf, r.fonts, r.contained, r.files, ull(r.bytes));
    log_line(Log::Core, "m8c %s", b);
    return b;
}

// M8C.2 (NON-VISUAL, user-approved): Vita3K's real LLE load of libpgf from the STAGED firmware tree (read-only use),
// MANUAL modules mode with [libpgf] only. Runs only when the staging is complete.
std::string probe_m8c2(const std::wstring &local_state) {
    const VitoxCoreHost host{ &m6c_log, &mem_snapshot };
    mem_snapshot("m8c2_baseline");
    const VitoxPgfLleResult r = vitox_pgf_lle_probe_run(&host, kM8aDevRoot, local_state.c_str());
    mem_snapshot("m8c2_after");
    char b[300];
    snprintf(b, sizeof(b), "M8C.2 PGF LLE precheck (staged=%d policy=%d decrypt=%d loaded=%d module_start_ok=%d imports=%u unselected=%u traps=%u no_network=%d "
                           "staging_unchanged=%d)",
        r.staged, r.policy, r.decrypt, r.loaded, r.module_start_ok, r.imports, r.unselected_imports, r.traps, r.no_network, r.staging_unchanged);
    log_line(Log::Core, "m8c %s", b);
    return b;
}

// M7B Phase A (NON-VISUAL): shader generation with open-shacccg inside this package + Vita3K validation. The headless
// presenter only gives renderer::init its external-present bridge (no swapchain, no Present, no frames).
std::string probe_m7b(void *core_window, const std::wstring &local_state) {
    plog("begin: M7B Phase A NON-VISUAL shader generation/validation (open-shacccg in-package, headless presenter)");
    plog("vulkan_dzn.dll (packaged ICD) SHA-256 %s", dzn_hash().c_str());
    const std::wstring err_path = local_state + L"\\m7b_stderr.txt";
    FILE *ef = nullptr;
    const errno_t fe = _wfreopen_s(&ef, err_path.c_str(), L"w", stderr);
    if (fe == 0 && ef)
        setvbuf(stderr, nullptr, _IONBF, 0);
    _putenv_s("MESA_LOG_LEVEL", "debug");
    plog("stderr -> %s: %s | MESA_LOG_LEVEL=debug | D3D12 debug layer NOT enabled", narrow(err_path).c_str(), fe == 0 ? "ok" : "FAILED");
    fprintf(stderr, "[vitox] m7b stderr capture start\n");
    const VitoxCoreHost host{ &m6c_log, &mem_snapshot };
    const std::wstring install_dir(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    auto *presenter = new HostPresenter(core_window, 1920, 1080, 3, VitoxXpVariant::P_Production, true);
    const VitoxShaderGenResult r = vitox_shader_gen_probe_run(&host, presenter, local_state.c_str(), L"D:\\DevelopmentFiles\\Vitox", install_dir.c_str());
    const bool wedged = presenter->wedged();
    if (!wedged)
        delete presenter;
    fprintf(stderr, "[vitox] m7b stderr capture end\n");
    fflush(stderr);
    const bool ok = r.generated && r.deterministic && r.program_check && r.types && r.parameters && r.recompiled && r.vk_modules && r.written && r.cleanup;
    char b[260];
    snprintf(b, sizeof(b), "M7B-A %s (gen=%d det=%d check=%d types=%d params=%d recomp=%d vkmod=%d written=%d clean=%d)", ok ? "PASS" : "FAIL", r.generated,
        r.deterministic, r.program_check, r.types, r.parameters, r.recompiled, r.vk_modules, r.written, r.cleanup);
    log_line(Log::Core, "m7b %s", b);
    return b;
}

} // namespace vitox
