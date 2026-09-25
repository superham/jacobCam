// SPDX-License-Identifier: GPL-2.0-or-later
#include "service.h"

#include <windows.h>

#include <mfapi.h>

#include <chrono>
#include <cstdio>
#include <vector>

#include "qcam/log.h"
#include "qcam/usb.h"

namespace qcam {
namespace {

constexpr wchar_t kServiceName[]    = L"qcamsvc";
constexpr wchar_t kServiceDisplay[] = L"qcam QuickCam Express frame broker";

// How long to wait before looking for the camera again when it is absent.
constexpr DWORD kRetryDelayMs = 3000;

// How often a streaming service checks for an unplugged camera and for
// readers having gone away.
constexpr DWORD kStreamingPollMs = 1000;

// How long the camera stays open after the last reader asked for a frame.
// Long enough to ride out an app briefly pausing its pipeline (switching
// resolution, a call being put on hold) without re-running sensor init.
constexpr ULONGLONG kIdleTimeoutMs = 10000;

// Longest exposure that still fits in one frame period. Measured on an
// HDCS-1000 at the default window: up to 128 the camera holds its full
// ~7.9 fps, and every step beyond stretches the frame (255 gives ~3.2 fps).
// Auto-exposure stops here and makes up the rest with gain, trading a noisier
// picture in dim light for motion that stays smooth.
constexpr int kFullRateExposureMax = 128;

SERVICE_STATUS         g_status = {};
SERVICE_STATUS_HANDLE  g_status_handle = nullptr;
CameraService*         g_service = nullptr;

void ReportStatus(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0) {
    static DWORD checkpoint = 1;

    g_status.dwCurrentState  = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwWaitHint      = wait_hint;
    g_status.dwControlsAccepted =
        (state == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;

    if (g_status_handle) ::SetServiceStatus(g_status_handle, &g_status);
}

void WINAPI ServiceCtrlHandler(DWORD control) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            if (g_service) g_service->Stop();
            break;
        default:
            break;
    }
}

void LogToEventLogAndDebugger(LogLevel level, const char* msg) {
    char line[1200];
    std::snprintf(line, sizeof(line), "[qcam] %s\n", msg);
    ::OutputDebugStringA(line);
    if (level == LogLevel::Error) {
        // Errors also go to stderr, which the console mode shows.
        std::fprintf(stderr, "%s", line);
    }
}

}  // namespace

CameraService::CameraService() {
    stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

CameraService::~CameraService() {
    if (stop_event_) ::CloseHandle(stop_event_);
}

void CameraService::Stop() {
    stop_.store(true);
    if (stop_event_) ::SetEvent(stop_event_);
}

void CameraService::PublishFrame(const DecodedFrame& frame) {
    // An app moved a slider. Runs on the streaming thread, so the change lands
    // at a frame boundary.
    PictureControls controls;
    if (controls_.Poll(&controls)) {
        ColorSettings color = camera_.color_settings();
        ApplyPictureControls(controls, &color);
        camera_.SetColorSettings(color);
    }

    const Status st = ring_.Publish(frame.data, frame.size, frame.sequence,
                                    frame.timestamp_100ns);
    if (Failed(st)) {
        QCAM_LOGW("publishing frame %llu failed: %s",
                  static_cast<unsigned long long>(frame.sequence), StatusName(st));
        return;
    }
    if (++published_ % 300 == 0) {
        const CameraStats s = camera_.stats();
        QCAM_LOGI("published %llu frames (%.2f fps, exposure %d, gain %d)",
                  static_cast<unsigned long long>(published_), s.measured_fps,
                  s.exposure, s.gain);
    }
}

void CameraService::ReleaseCamera() {
    camera_.Close();
    ring_.Close();   // readers see writer_alive drop and let go of the ring
    published_ = 0;
}

Status CameraService::OpenAndStream(const ServiceOptions& options) {
    CameraConfig cfg;
    cfg.format     = options.format;
    cfg.out_width  = options.out_width;
    cfg.out_height = options.out_height;
    // A short frame is padded with grey; one that ended almost at once is a
    // solid grey frame, which an app shows as a flash. Drop them: the virtual
    // camera repeats the previous frame instead.
    cfg.emit_short_frames = false;
    cfg.auto_exposure.exposure_max = kFullRateExposureMax;
    // Whatever an app last set, including before this stream started.
    ApplyPictureControls(controls_.Current(), &cfg.color);

    QCAM_TRY(camera_.OpenFirst(cfg));

    RingConfig ring_config;
    ring_config.width  = camera_.out_width();
    ring_config.height = camera_.out_height();
    ring_config.format = options.format;
    // Advertise the rate the hardware can actually sustain rather than a
    // round number an app would like to see. Media Foundation is happy with a
    // fractional rate; apps that insist on 30 fps get repeated frames from
    // the virtual camera instead of a lie told here.
    ring_config.fps_numerator   = static_cast<uint32_t>(camera_.nominal_fps() * 100.0);
    ring_config.fps_denominator = 100;

    Status st = ring_.Create(ring_config);
    if (Failed(st)) {
        camera_.Close();
        return st;
    }

    st = camera_.Start([this](const DecodedFrame& f) { PublishFrame(f); });
    if (Failed(st)) {
        ring_.Close();
        camera_.Close();
        return st;
    }

    QCAM_LOGI("streaming %ux%u %s at ~%.2f fps", camera_.out_width(),
              camera_.out_height(), PixelFormatName(options.format),
              camera_.nominal_fps());
    return Status::Ok;
}

Status CameraService::Run(const ServiceOptions& options) {
    stop_.store(false);
    if (stop_event_) ::ResetEvent(stop_event_);

    // The virtual camera (console --vcam) talks to the Frame Server over COM.
    const bool com = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        QCAM_LOGE("MFStartup failed: 0x%08lx", static_cast<unsigned long>(hr));
        if (com) ::CoUninitialize();
        return Status::Io;
    }

    // The installed camera is registered once by install.ps1 and outlives this
    // process. This one is a debugging aid and disappears when we exit.
    if (options.session_vcam) {
        Status st = vcam_.Create(options.friendly_name, VCamLifetime::Session);
        if (Succeeded(st)) {
            st = vcam_.Start();
            if (Failed(st))
                QCAM_LOGW("virtual camera did not start: %s", StatusName(st));
        } else if (st == Status::Unsupported) {
            QCAM_LOGW("continuing without a system camera; "
                      "qcamctl can still capture from the device");
        } else {
            QCAM_LOGE("virtual camera registration failed: %s", StatusName(st));
        }
    }

    // Stream only while something is reading. An open camera is a live feed
    // into shared memory, so it should not be on just because it is plugged
    // in. Readers signal the demand event on every read; see FrameDemand.
    HANDLE demand = nullptr;
    if (!options.always_on && stop_event_) {
        if (Succeeded(demand_.Create()))
            demand = static_cast<HANDLE>(demand_.wait_handle());
        else
            QCAM_LOGW("no demand event; streaming whenever the camera is present");
    }
    if (demand) QCAM_LOGI("waiting for an app to ask for frames");

    // Lives as long as the service, so app settings outlast individual streams.
    if (Failed(controls_.Create(PictureControls{})))
        QCAM_LOGW("no picture-control block; app brightness/contrast will not apply");

    bool streaming = false;
    bool demanded_ever = false;
    ULONGLONG last_demand = 0;
    while (!stop_.load()) {
        const bool wanted =
            !demand ||
            (demanded_ever && ::GetTickCount64() - last_demand < kIdleTimeoutMs);

        if (streaming && !camera_.IsStreaming()) {
            // The transport dropped the stream, which on this hardware almost
            // always means the cable came out.
            QCAM_LOGW("stream stopped unexpectedly; releasing the device");
            ReleaseCamera();
            streaming = false;
        } else if (streaming && !wanted) {
            QCAM_LOGI("no app has asked for frames in %lu s; releasing the camera",
                      static_cast<unsigned long>(kIdleTimeoutMs / 1000));
            ReleaseCamera();
            streaming = false;
        }

        if (!streaming && wanted) {
            const Status st = OpenAndStream(options);
            if (Succeeded(st)) {
                streaming = true;
            } else if (st == Status::NoDevice) {
                QCAM_LOGD("no camera present; retrying");
            } else {
                QCAM_LOGE("could not start the camera: %s", StatusName(st));
            }
        }

        // Idle: sleep until stopped or asked for frames. Otherwise wake every
        // so often, to notice an unplugged camera or the idle timeout while
        // streaming, or to retry a failed start. Demand is only polled in those
        // cases: a reader signals it on every frame, and blocking on it would
        // turn a failed start into a retry at the frame rate.
        bool demanded = false;
        if (demand && !streaming && !wanted) {
            HANDLE handles[2] = {stop_event_, demand};
            demanded = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE) ==
                       WAIT_OBJECT_0 + 1;
        } else if (stop_event_) {
            ::WaitForSingleObject(stop_event_, streaming ? kStreamingPollMs
                                                         : kRetryDelayMs);
        } else {
            ::Sleep(kRetryDelayMs);
        }
        if (demand && !demanded)
            demanded = ::WaitForSingleObject(demand, 0) == WAIT_OBJECT_0;
        if (demanded) {
            demanded_ever = true;
            last_demand   = ::GetTickCount64();
        }
    }

    QCAM_LOGI("shutting down");
    ReleaseCamera();
    demand_.Close();
    controls_.Close();
    vcam_.Stop();
    vcam_.Close();
    ::MFShutdown();
    if (com) ::CoUninitialize();
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Service control manager plumbing
// ---------------------------------------------------------------------------

namespace {

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_status_handle = ::RegisterServiceCtrlHandlerW(kServiceName, ServiceCtrlHandler);
    if (!g_status_handle) return;

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

    SetLogSink(&LogToEventLogAndDebugger);
    SetLogLevel(LogLevel::Info);

    CameraService service;
    g_service = &service;

    ReportStatus(SERVICE_RUNNING);

    ServiceOptions options;
    options.console = false;
    service.Run(options);

    g_service = nullptr;
    ReportStatus(SERVICE_STOPPED);
}

}  // namespace

int RunAsService() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    if (!::StartServiceCtrlDispatcherW(table)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::fprintf(stderr,
                         "qcamsvc is a Windows service.\n"
                         "Run 'qcamsvc --console' to run it in this window, or\n"
                         "'qcamsvc --install' (as administrator) to register it.\n");
            return 2;
        }
        std::fprintf(stderr, "StartServiceCtrlDispatcher failed: %lu\n", err);
        return 1;
    }
    return 0;
}

Status InstallService(const std::wstring& exe_path) {
    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!manager) {
        QCAM_LOGE("OpenSCManager failed: %lu (run as administrator)", ::GetLastError());
        return Status::Busy;
    }

    // Quote the path so a space in Program Files does not split the command.
    const std::wstring command = L"\"" + exe_path + L"\"";

    // LOCAL SERVICE rather than LocalSystem. All the service needs is to open
    // the WinUSB device and create the Global\ frame ring, and neither takes
    // more than an ordinary service account. The virtual camera registration,
    // which does need administrator rights, is done once by install.ps1.
    constexpr wchar_t kAccount[] = L"NT AUTHORITY\\LocalService";

    SC_HANDLE service = ::CreateServiceW(
        manager, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        command.c_str(), nullptr, nullptr, nullptr, kAccount, L"");

    if (!service) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_SERVICE_EXISTS) {
            ::CloseServiceHandle(manager);
            QCAM_LOGE("CreateService failed: %lu", err);
            return Status::Io;
        }
        // Already installed, perhaps by an older build that ran as
        // LocalSystem from the build directory. Bring it up to date rather
        // than leaving the old account and path in place.
        service = ::OpenServiceW(manager, kServiceName, SERVICE_ALL_ACCESS);
        if (!service ||
            !::ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS,
                                    SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                    command.c_str(), nullptr, nullptr, nullptr,
                                    kAccount, L"", kServiceDisplay)) {
            QCAM_LOGE("updating the existing service failed: %lu", ::GetLastError());
            if (service) ::CloseServiceHandle(service);
            ::CloseServiceHandle(manager);
            return Status::Io;
        }
        QCAM_LOGI("service already installed; configuration updated");
    }

    // Give the process its own SID (NT SERVICE\qcamsvc). The Frame Server also
    // runs as LOCAL SERVICE, so the account alone cannot tell the frame ring's
    // writer apart from its readers; the service SID can.
    SERVICE_SID_INFO sid_info = {SERVICE_SID_TYPE_UNRESTRICTED};
    ::ChangeServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO, &sid_info);

    // Strip every privilege the service does not use from its token.
    wchar_t privileges[] = L"SeCreateGlobalPrivilege\0SeChangeNotifyPrivilege\0";
    SERVICE_REQUIRED_PRIVILEGES_INFOW required = {privileges};
    ::ChangeServiceConfig2W(service, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO,
                            &required);

    SERVICE_DESCRIPTIONW description = {
        const_cast<LPWSTR>(L"Publishes frames from a Logitech QuickCam Express "
                           L"to the Windows camera stack.")};
    ::ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    // Restart on failure rather than leaving the camera dead after a hiccup.
    SC_ACTION actions[3] = {};
    for (auto& action : actions) {
        action.Type  = SC_ACTION_RESTART;
        action.Delay = 10000;
    }
    SERVICE_FAILURE_ACTIONSW failure = {};
    failure.dwResetPeriod = 86400;
    failure.cActions      = 3;
    failure.lpsaActions   = actions;
    ::ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    QCAM_LOGI("service installed");
    return Status::Ok;
}

Status UninstallService() {
    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        QCAM_LOGE("OpenSCManager failed: %lu (run as administrator)", ::GetLastError());
        return Status::Busy;
    }

    SC_HANDLE service = ::OpenServiceW(manager, kServiceName,
                                       SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!service) {
        ::CloseServiceHandle(manager);
        QCAM_LOGI("service is not installed");
        return Status::Ok;
    }

    SERVICE_STATUS status = {};
    if (::ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        // Give it a moment to come to rest before deleting the registration.
        for (int i = 0; i < 50 && status.dwCurrentState != SERVICE_STOPPED; ++i) {
            ::Sleep(100);
            if (!::QueryServiceStatus(service, &status)) break;
        }
    }

    const BOOL deleted = ::DeleteService(service);
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);

    if (!deleted) {
        QCAM_LOGE("DeleteService failed: %lu", ::GetLastError());
        return Status::Io;
    }
    QCAM_LOGI("service uninstalled");
    return Status::Ok;
}

}  // namespace qcam
