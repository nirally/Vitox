// Counters of the inert camera substitute (vitox_camera_inert.cpp).
#pragma once

struct VitoxCameraReport {
    unsigned ctor, dtor, unexpected_use;
};

extern "C" VitoxCameraReport vitox_camera_report();
