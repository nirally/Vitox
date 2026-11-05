// Vitox M4/0.0.6.0 - INERT out-of-tree substitute for vita3k/camera/src/camera.cpp (approved).
// camera.cpp is a real SDL3 camera/surface/timer backend; EmuEnvState only needs Camera's
// constructor/destructor (CameraState owns Camera objects). This file defines exactly the symbols
// camera.cpp defines, with no SDL and no device: the ctor/dtor work, every runtime entry point is
// counted as UNEXPECTED USE and fails with SCE_CAMERA_ERROR_NOT_OPEN.
// Drift guard: core-build pins the SHA-256 of upstream camera.h and camera.cpp (configure fails on change).
// Future upstream-style refactor candidate: split camera.cpp into camera.cpp (object) + camera_sdl.cpp (backend).
#include <config/state.h> // camera.h needs Config declared (upstream camera.cpp includes it first too)

#include <camera/camera.h>
#include <camera/state.h>

#include "vitox_camera_report.h"

#include <atomic>

class Camera::CameraImpl {}; // no backend

namespace {
std::atomic<unsigned> g_ctor{ 0 }, g_dtor{ 0 }, g_unexpected{ 0 };

int unexpected() {
    ++g_unexpected;
    return static_cast<int>(SCE_CAMERA_ERROR_NOT_OPEN);
}
} // namespace

Camera::Camera()
    : pImpl(std::make_unique<CameraImpl>()) { ++g_ctor; }
Camera::~Camera() { ++g_dtor; }

int Camera::get_attribute(CameraAttributes) { return unexpected(); }
int Camera::set_attribute(CameraAttributes, int) { return unexpected(); }
int Camera::open(SceCameraInfo *, uint64_t, uint64_t) { return unexpected(); }
int Camera::close() { return unexpected(); }
int Camera::start() { return unexpected(); }
int Camera::stop() { return unexpected(); }
int Camera::read(SceCameraRead *, void *, void *, void *, SceSize, SceSize, SceSize) { return unexpected(); }
void Camera::update_config(int, const std::string &, const std::string &, uint32_t) { unexpected(); }
void CameraState::deinit() { unexpected(); }
void init_default_cameras(Config &) { unexpected(); }

extern "C" VitoxCameraReport vitox_camera_report() { return { g_ctor.load(), g_dtor.load(), g_unexpected.load() }; }
