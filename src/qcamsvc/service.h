// SPDX-License-Identifier: GPL-2.0-or-later
//
// qcamsvc - the frame broker.
//
// One process owns the USB device, because WinUSB hands out exclusive access
// to the interface and because the isochronous pipeline should be set up once
// rather than torn down every time an app opens the camera. Decoded frames go
// into a shared-memory ring that the Media Foundation virtual camera (and
// qcamctl) read from.

#ifndef QCAM_SERVICE_H_
#define QCAM_SERVICE_H_

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "qcam/device.h"
#include "qcam/ring.h"
#include "qcam/types.h"
#include "qcam/vcam.h"

namespace qcam {

struct ServiceOptions {
    PixelFormat format     = PixelFormat::Nv12;
    // 0 => the sensor's native size (360x296 on the HDCS-1000). The virtual
    // camera scales that to whatever size an app picks.
    uint16_t    out_width  = 0;
    uint16_t    out_height = 0;
    // The installed service never registers a camera: that needs
    // administrator rights, which it does not have, so install.ps1 registers
    // a persistent one instead. This is for debugging in console mode from
    // an elevated prompt, and lasts only as long as the process.
    bool        session_vcam = false;
    // Stream whenever the camera is plugged in, rather than only while a
    // reader is asking for frames. Console debugging aid.
    bool        always_on = false;
    bool        console = false;
    std::wstring friendly_name = L"Logitech QuickCam Express (qcam)";
};

class CameraService {
public:
    CameraService();
    ~CameraService();

    // Runs until Stop() is called. Opens the camera only while a reader is
    // asking for frames (unless always_on), handles it not being plugged in
    // yet, and reconnects if it is unplugged and plugged back in.
    Status Run(const ServiceOptions& options);
    void   Stop();

private:
    Status OpenAndStream(const ServiceOptions& options);
    void   ReleaseCamera();
    void   PublishFrame(const DecodedFrame& frame);

    Camera            camera_;
    FrameRingWriter   ring_;
    FrameDemand       demand_;
    PictureControlHost controls_;
    VirtualCamera     vcam_;
    std::atomic<bool> stop_{false};
    HANDLE            stop_event_ = nullptr;
    uint64_t          published_  = 0;
};

// Service control plumbing.
Status InstallService(const std::wstring& exe_path);
Status UninstallService();
int    RunAsService();

}  // namespace qcam

#endif  // QCAM_SERVICE_H_
