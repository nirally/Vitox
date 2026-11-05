// Vitox (M6C, approved): FFmpeg swscale is not linked on WindowsStore yet. The three renderer call sites that
// need it are compiled out under VITOX_UWP (core-build/CMakeLists.txt, "M6C swscale"); each disabled path logs,
// calls vitox_swscale_disabled_hit() and skips the operation through its existing error/skip path.
// The first renderer/GXM proofs require every counter to stay 0.
#pragma once

enum VitoxSwsSite : unsigned {
    VITOX_SWS_YUV420_TO_RGB = 0, // texture/yuv.cpp yuv420_texture_to_rgb (YUV420P/NV12 textures)
    VITOX_SWS_SURFACE_SYNC_RGB24 = 1, // vulkan/surface_cache.cpp perform_post_surface_sync (24-bit colour surface readback)
    VITOX_SWS_TRANSFER_DOWNSCALE = 2, // transfer.cpp cmd_handle_transfer_downscale (ffmpeg SWS_AREA branch)
    VITOX_SWS_UNEXPECTED_CONTEXT = 3, // a destructor found a non-null sws context (cannot happen when compiled out)
    VITOX_SWS_SITE_COUNT = 4,
};

extern "C" void vitox_swscale_disabled_hit(unsigned site);
extern "C" unsigned vitox_swscale_disabled_count(unsigned site);
