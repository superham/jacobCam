// SPDX-License-Identifier: GPL-2.0-or-later
//
// qcamsvc entry point. Runs as a Windows service by default; --console runs
// the same code in the foreground, which is how you debug it.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "qcam/log.h"
#include "qcam/vcam.h"

#include "service.h"

using namespace qcam;

namespace {

void LogToStderr(LogLevel level, const char* msg) {
    static const char* kNames[] = {"error", "warn", "info", "debug", "trace"};
    int idx = static_cast<int>(level);
    if (idx < 0 || idx > 4) idx = 2;
    std::fprintf(stderr, "%-5s %s\n", kNames[idx], msg);
}

std::wstring ExecutablePath() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

CameraService* g_console_service = nullptr;

BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_console_service) g_console_service->Stop();
        return TRUE;
    }
    return FALSE;
}

int Usage() {
    std::printf(
        "qcamsvc - frame broker for the Logitech QuickCam Express driver\n"
        "\n"
        "usage: qcamsvc [command] [options]\n"
        "\n"
        "commands:\n"
        "  (none)              run as a Windows service (started by the SCM)\n"
        "  --console           run in the foreground; Ctrl-C to stop\n"
        "  --install           register the service (administrator)\n"
        "  --uninstall         remove the service (administrator)\n"
        "  --register-vcam     register the system-wide camera (administrator)\n"
        "  --remove-vcam       remove the registered virtual camera\n"
        "\n"
        "options:\n"
        "  --size WxH          published frame size (default: sensor native,\n"
        "                      360x296; apps can still pick other sizes)\n"
        "  --vcam              console mode: also register a temporary camera\n"
        "                      that lasts until Ctrl-C (administrator)\n"
        "  --always-on         console mode: stream whenever the camera is\n"
        "                      plugged in, not only while an app reads frames\n"
        "  --name \"TEXT\"       friendly name shown in app camera pickers\n"
        "  -v, --verbose       verbose logging\n");
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ServiceOptions options;
    bool console = false;
    bool verbose = false;
    std::wstring command;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--console")        console = true;
        else if (arg == L"-v" || arg == L"--verbose") verbose = true;
        else if (arg == L"--vcam")      options.session_vcam = true;
        else if (arg == L"--always-on") options.always_on = true;
        // Older install scripts passed this; the service no longer registers
        // a camera on its own, so it is accepted and ignored.
        else if (arg == L"--no-vcam")   {}
        else if (arg == L"--name" && i + 1 < argc) options.friendly_name = argv[++i];
        else if (arg == L"--size" && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::swscanf(argv[++i], L"%dx%d", &w, &h) == 2) {
                options.out_width  = static_cast<uint16_t>(w);
                options.out_height = static_cast<uint16_t>(h);
            }
        }
        else if (arg == L"--install" || arg == L"--uninstall" ||
                 arg == L"--register-vcam" || arg == L"--remove-vcam") {
            command = arg;
        }
        else if (arg == L"-h" || arg == L"--help") return Usage();
        else {
            std::fwprintf(stderr, L"unknown argument '%ls'\n\n", arg.c_str());
            return Usage();
        }
    }

    // One-shot commands run after parsing, so --name applies wherever it
    // appears on the command line.
    if (!command.empty()) {
        SetLogSink(&LogToStderr);
        Status st = Status::Ok;
        if (command == L"--install")          st = InstallService(ExecutablePath());
        else if (command == L"--uninstall")   st = UninstallService();
        else if (command == L"--register-vcam")
            st = RegisterPersistentVirtualCamera(options.friendly_name);
        else if (command == L"--remove-vcam")
            st = RemoveVirtualCamera(options.friendly_name);
        return Failed(st) ? 1 : 0;
    }

    if (!console) return RunAsService();

    SetLogSink(&LogToStderr);
    SetLogLevel(verbose ? LogLevel::Trace : LogLevel::Info);

    CameraService service;
    g_console_service = &service;
    ::SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    options.console = true;
    std::printf("running in console mode; Ctrl-C to stop\n");
    const Status st = service.Run(options);

    g_console_service = nullptr;
    return Failed(st) ? 1 : 0;
}
