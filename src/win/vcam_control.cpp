// SPDX-License-Identifier: GPL-2.0-or-later
//
// Virtual camera registration, via MFCreateVirtualCamera.

#include "qcam/vcam.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>

#include <iterator>
#include <string>

#include "qcam/log.h"
#include "qcam/win_guids.h"

namespace qcam {
namespace {

Status StatusFromHr(HRESULT hr) {
    if (SUCCEEDED(hr)) return Status::Ok;
    switch (hr) {
        case E_INVALIDARG:                     return Status::InvalidArg;
        case E_OUTOFMEMORY:                    return Status::NoMemory;
        case E_NOTIMPL:                        return Status::Unsupported;
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED): return Status::Busy;
        default:                               return Status::Io;
    }
}

// The CLSID of the media source, spelled the way MFCreateVirtualCamera wants
// it: a braced registry-style string.
std::wstring SourceClsidString() {
    wchar_t buffer[64] = {};
    ::StringFromGUID2(CLSID_QcamMediaSource, buffer,
                      static_cast<int>(std::size(buffer)));
    return buffer;
}

// MFCreateVirtualCamera talks to the Frame Server over COM and needs Media
// Foundation started on the calling thread. The one-shot register/remove
// commands run on a bare main thread, so they set both up themselves.
class ScopedMediaFoundation {
public:
    ScopedMediaFoundation() {
        const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // RPC_E_CHANGED_MODE: COM is already up in another apartment, which
        // is fine to use but not ours to tear down.
        com_ = SUCCEEDED(co);
        mf_ = SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    }
    ~ScopedMediaFoundation() {
        if (mf_) ::MFShutdown();
        if (com_) ::CoUninitialize();
    }

    ScopedMediaFoundation(const ScopedMediaFoundation&) = delete;
    ScopedMediaFoundation& operator=(const ScopedMediaFoundation&) = delete;

private:
    bool com_ = false;
    bool mf_  = false;
};

}  // namespace

struct VirtualCamera::Impl {
    IMFVirtualCamera* camera = nullptr;
    bool running = false;

    ~Impl() { Release(); }

    void Release() {
        if (camera) {
            if (running) camera->Stop();
            camera->Release();
            camera = nullptr;
        }
        running = false;
    }
};

VirtualCamera::VirtualCamera() : impl_(new Impl()) {}
VirtualCamera::~VirtualCamera() = default;

bool VirtualCamera::IsRunning() const { return impl_ && impl_->running; }

bool VirtualCameraSupported() {
    // MFCreateVirtualCamera arrived in Windows 11 21H2 (build 22000). Resolve
    // it dynamically so this binary still loads on Windows 10, where the rest
    // of the stack (qcamctl, the service, the frame ring) works fine and only
    // the system-wide camera is unavailable.
    const HMODULE mf = ::GetModuleHandleW(L"mfsensorgroup.dll");
    if (mf && ::GetProcAddress(mf, "MFCreateVirtualCamera")) return true;

    const HMODULE loaded = ::LoadLibraryW(L"mfsensorgroup.dll");
    if (!loaded) return false;
    const bool present = ::GetProcAddress(loaded, "MFCreateVirtualCamera") != nullptr;
    ::FreeLibrary(loaded);
    return present;
}

Status VirtualCamera::Create(const std::wstring& friendly_name,
                             VCamLifetime lifetime) {
    Close();

    if (!VirtualCameraSupported()) {
        QCAM_LOGE("this Windows build has no virtual camera support "
                  "(Windows 11 build 22000 or later is required); "
                  "use 'qcamctl stream' or the DirectShow path instead");
        return Status::Unsupported;
    }

    const std::wstring clsid = SourceClsidString();

    // The source id is the CLSID of the COM server the Frame Server will
    // instantiate; it must already be registered (regsvr32 qcamvcam.dll).
    const HRESULT hr = ::MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        lifetime == VCamLifetime::System ? MFVirtualCameraLifetime_System
                                         : MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_AllUsers,
        friendly_name.c_str(),
        clsid.c_str(),
        /*Categories=*/nullptr,
        /*CategoryCount=*/0,
        &impl_->camera);

    if (FAILED(hr)) {
        QCAM_LOGE("MFCreateVirtualCamera failed: 0x%08lx", static_cast<unsigned long>(hr));
        return StatusFromHr(hr);
    }

    QCAM_LOGI("virtual camera registered against source %ls", clsid.c_str());
    return Status::Ok;
}

Status VirtualCamera::Start() {
    if (!impl_->camera) return Status::NoDevice;
    if (impl_->running) return Status::Ok;

    const HRESULT hr = impl_->camera->Start(nullptr);
    if (FAILED(hr)) {
        QCAM_LOGE("IMFVirtualCamera::Start failed: 0x%08lx",
                  static_cast<unsigned long>(hr));
        return StatusFromHr(hr);
    }
    impl_->running = true;
    QCAM_LOGI("virtual camera started");
    return Status::Ok;
}

Status VirtualCamera::Stop() {
    if (!impl_->camera || !impl_->running) return Status::Ok;
    const HRESULT hr = impl_->camera->Stop();
    impl_->running = false;
    return StatusFromHr(hr);
}

void VirtualCamera::Close() {
    if (impl_) impl_->Release();
}

Status RegisterPersistentVirtualCamera(const std::wstring& friendly_name) {
    if (!VirtualCameraSupported()) return Status::Unsupported;

    ScopedMediaFoundation mf;
    IMFVirtualCamera* camera = nullptr;
    const std::wstring clsid = SourceClsidString();
    HRESULT hr = ::MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                         MFVirtualCameraLifetime_System,
                                         MFVirtualCameraAccess_AllUsers,
                                         friendly_name.c_str(), clsid.c_str(),
                                         nullptr, 0, &camera);
    if (FAILED(hr)) {
        QCAM_LOGE("MFCreateVirtualCamera failed: 0x%08lx (run as administrator)",
                  static_cast<unsigned long>(hr));
        return StatusFromHr(hr);
    }

    // Release without Stop(): stopping would disable the registration that
    // this call exists to leave behind.
    hr = camera->Start(nullptr);
    camera->Release();
    if (FAILED(hr)) {
        QCAM_LOGE("IMFVirtualCamera::Start failed: 0x%08lx",
                  static_cast<unsigned long>(hr));
        return StatusFromHr(hr);
    }
    QCAM_LOGI("virtual camera registered against source %ls", clsid.c_str());
    return Status::Ok;
}

Status RemoveVirtualCamera(const std::wstring& friendly_name) {
    if (!VirtualCameraSupported()) return Status::Unsupported;

    ScopedMediaFoundation mf;
    // Re-create a handle to the same registration so it can be removed. A
    // System-lifetime camera outlives whichever process registered it. Every
    // argument, the name included, has to match the registration's.
    IMFVirtualCamera* camera = nullptr;
    const std::wstring clsid = SourceClsidString();
    HRESULT hr = ::MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                         MFVirtualCameraLifetime_System,
                                         MFVirtualCameraAccess_AllUsers,
                                         friendly_name.c_str(),
                                         clsid.c_str(), nullptr, 0, &camera);
    if (FAILED(hr)) return StatusFromHr(hr);

    hr = camera->Remove();
    camera->Release();
    if (FAILED(hr)) {
        QCAM_LOGE("IMFVirtualCamera::Remove failed: 0x%08lx",
                  static_cast<unsigned long>(hr));
        return StatusFromHr(hr);
    }
    QCAM_LOGI("virtual camera removed");
    return Status::Ok;
}

}  // namespace qcam
