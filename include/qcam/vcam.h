// SPDX-License-Identifier: GPL-2.0-or-later
//
// Virtual camera registration.
//
// Windows 11 (build 22000 and later) can publish a user-mode media source as
// a real camera that every app sees: Teams, Zoom, OBS, the Camera app. That
// is what lets this driver present a 1999 QuickCam to software written a
// quarter of a century later, without a kernel AVStream driver.

#ifndef QCAM_VCAM_H_
#define QCAM_VCAM_H_

#ifdef _WIN32

#include <memory>
#include <string>

#include "qcam/types.h"

namespace qcam {

enum class VCamLifetime {
    // Lives as long as the registering process holds it. Good for debugging.
    Session,
    // Survives process exit and reboot until explicitly removed. The installer
    // registers one of these so the camera is present at the sign-in screen.
    System,
};

// Owns a registered virtual camera. Destroying it stops the camera when the
// lifetime is Session; a System camera keeps running and must be removed with
// RemoveVirtualCamera().
class VirtualCamera {
public:
    VirtualCamera();
    ~VirtualCamera();

    VirtualCamera(const VirtualCamera&) = delete;
    VirtualCamera& operator=(const VirtualCamera&) = delete;

    // `friendly_name` is what the user sees in an app's camera picker.
    Status Create(const std::wstring& friendly_name, VCamLifetime lifetime);
    Status Start();
    Status Stop();
    void   Close();
    bool   IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Registers and starts a System-lifetime virtual camera, then lets go of it
// without stopping it, so it stays in app pickers after this process exits.
// Needs administrator rights; the installer calls it once, which is what lets
// the service itself run without them.
Status RegisterPersistentVirtualCamera(const std::wstring& friendly_name);

// Removes a previously registered System-lifetime virtual camera. Pass the
// same friendly name it was registered with. Safe to call when none is
// registered.
Status RemoveVirtualCamera(const std::wstring& friendly_name);

// True when this Windows build has the virtual camera API at all.
bool VirtualCameraSupported();

}  // namespace qcam

#endif  // _WIN32
#endif  // QCAM_VCAM_H_
