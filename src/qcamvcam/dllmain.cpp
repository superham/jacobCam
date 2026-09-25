// SPDX-License-Identifier: GPL-2.0-or-later
//
// COM in-process server entry points and self-registration.

#include "qcamvcam.h"

#include <mferror.h>
#include <shlwapi.h>

#include <atomic>
#include <cstdio>
#include <new>
#include <string>

#include "qcam/log.h"
#include "qcam/win_guids.h"

namespace qcam {
namespace vcam {
namespace {

HMODULE g_module = nullptr;

void LogToDebugger(LogLevel, const char* msg) {
    char line[1200];
    std::snprintf(line, sizeof(line), "[qcamvcam] %s\n", msg);
    ::OutputDebugStringA(line);
}

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() { ++g_object_count; }
    ~ClassFactory() { --g_object_count; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG count = --ref_count_;
        if (count == 0) delete this;
        return count;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        auto* activate = new (std::nothrow) QcamActivate();
        if (!activate) return E_OUTOFMEMORY;

        HRESULT hr = activate->Initialize();
        if (SUCCEEDED(hr)) hr = activate->QueryInterface(riid, ppv);
        activate->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_object_count;
        else      --g_object_count;
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1};
};

std::wstring ModulePath() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(g_module, path, MAX_PATH);
    return path;
}

std::wstring ClsidString() {
    wchar_t buffer[64] = {};
    ::StringFromGUID2(CLSID_QcamMediaSource, buffer, 64);
    return buffer;
}

LONG SetStringValue(HKEY key, const wchar_t* name, const std::wstring& value) {
    return ::RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace
}  // namespace vcam
}  // namespace qcam

using namespace qcam;
using namespace qcam::vcam;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_module = instance;
            ::DisableThreadLibraryCalls(instance);
            SetLogSink(&LogToDebugger);
            SetLogLevel(LogLevel::Info);
            break;
        default:
            break;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (clsid != CLSID_QcamMediaSource) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory) return E_OUTOFMEMORY;

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_object_count.load() == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    const std::wstring clsid = ClsidString();
    const std::wstring key_path = L"Software\\Classes\\CLSID\\" + clsid;

    HKEY clsid_key = nullptr;
    LONG result = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, nullptr,
                                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                    &clsid_key, nullptr);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);

    SetStringValue(clsid_key, nullptr, L"qcam QuickCam Express Media Source");

    HKEY inproc_key = nullptr;
    result = ::RegCreateKeyExW(clsid_key, L"InprocServer32", 0, nullptr,
                               REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                               &inproc_key, nullptr);
    if (result != ERROR_SUCCESS) {
        ::RegCloseKey(clsid_key);
        return HRESULT_FROM_WIN32(result);
    }

    SetStringValue(inproc_key, nullptr, ModulePath());
    // "Both" so the Frame Server can host the source on whichever apartment
    // it happens to be using.
    SetStringValue(inproc_key, L"ThreadingModel", L"Both");

    ::RegCloseKey(inproc_key);
    ::RegCloseKey(clsid_key);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    const std::wstring key_path = L"Software\\Classes\\CLSID\\" + ClsidString();
    const LONG result =
        ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, key_path.c_str());
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
        return HRESULT_FROM_WIN32(result);
    return S_OK;
}
