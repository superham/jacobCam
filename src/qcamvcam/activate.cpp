// SPDX-License-Identifier: GPL-2.0-or-later
#include "qcamvcam.h"

#include <mferror.h>
#include <mfvirtualcamera.h>

#include <new>

#include "qcam/log.h"

namespace qcam {
namespace vcam {

QcamActivate::QcamActivate() { ++g_object_count; }

QcamActivate::~QcamActivate() {
    // Only ShutdownObject() shuts the source down. The Frame Server releases
    // the activator as soon as ActivateObject returns and keeps using the
    // source it got; shutting it down here made every call after that fail
    // with MF_E_SHUTDOWN.
    if (source_) {
        source_->Release();
        source_ = nullptr;
    }
    --g_object_count;
}

HRESULT QcamActivate::Initialize() {
    HRESULT hr = MFCreateAttributes(&attributes_, 4);
    if (FAILED(hr)) return hr;
    // Every public Media Foundation virtual camera sample sets this on the
    // activator before the Frame Server ever calls ActivateObject.
    return attributes_->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1);
}

// --- IUnknown --------------------------------------------------------------

IFACEMETHODIMP QcamActivate::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IMFAttributes || riid == IID_IMFActivate) {
        *ppv = static_cast<IMFActivate*>(this);
        AddRef();
        return S_OK;
    }
    QCAM_LOGT("QcamActivate: interface {%08lx-...} not implemented",
              (unsigned long)riid.Data1);
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) QcamActivate::AddRef() { return ++ref_count_; }

IFACEMETHODIMP_(ULONG) QcamActivate::Release() {
    const ULONG count = --ref_count_;
    if (count == 0) delete this;
    return count;
}

// --- IMFActivate -----------------------------------------------------------

IFACEMETHODIMP QcamActivate::ActivateObject(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    // Callers may shut the source down directly rather than through
    // ShutdownObject, then activate again; a dead source is no use to them.
    if (source_ && source_->IsShutdown()) {
        source_->Release();
        source_ = nullptr;
    }
    if (!source_) {
        auto* source = new (std::nothrow) QcamMediaSource();
        if (!source) return E_OUTOFMEMORY;
        const HRESULT hr = source->Initialize();
        if (FAILED(hr)) {
            QCAM_LOGE("media source initialisation failed: 0x%08lx",
                      static_cast<unsigned long>(hr));
            source->Release();
            return hr;
        }
        source_ = source;
    }
    return source_->QueryInterface(riid, ppv);
}

IFACEMETHODIMP QcamActivate::ShutdownObject() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source_) {
        source_->Shutdown();
        source_->Release();
        source_ = nullptr;
    }
    return S_OK;
}

IFACEMETHODIMP QcamActivate::DetachObject() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source_) {
        source_->Release();
        source_ = nullptr;
    }
    return S_OK;
}

// --- IMFAttributes ---------------------------------------------------------

IFACEMETHODIMP QcamActivate::GetItem(REFGUID key, PROPVARIANT* value) {
    return attributes_->GetItem(key, value);
}
IFACEMETHODIMP QcamActivate::GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) {
    return attributes_->GetItemType(key, type);
}
IFACEMETHODIMP QcamActivate::CompareItem(REFGUID key, REFPROPVARIANT value,
                                         BOOL* result) {
    return attributes_->CompareItem(key, value, result);
}
IFACEMETHODIMP QcamActivate::Compare(IMFAttributes* theirs,
                                     MF_ATTRIBUTES_MATCH_TYPE match, BOOL* result) {
    return attributes_->Compare(theirs, match, result);
}
IFACEMETHODIMP QcamActivate::GetUINT32(REFGUID key, UINT32* value) {
    return attributes_->GetUINT32(key, value);
}
IFACEMETHODIMP QcamActivate::GetUINT64(REFGUID key, UINT64* value) {
    return attributes_->GetUINT64(key, value);
}
IFACEMETHODIMP QcamActivate::GetDouble(REFGUID key, double* value) {
    return attributes_->GetDouble(key, value);
}
IFACEMETHODIMP QcamActivate::GetGUID(REFGUID key, GUID* value) {
    return attributes_->GetGUID(key, value);
}
IFACEMETHODIMP QcamActivate::GetStringLength(REFGUID key, UINT32* length) {
    return attributes_->GetStringLength(key, length);
}
IFACEMETHODIMP QcamActivate::GetString(REFGUID key, LPWSTR value, UINT32 size,
                                       UINT32* length) {
    return attributes_->GetString(key, value, size, length);
}
IFACEMETHODIMP QcamActivate::GetAllocatedString(REFGUID key, LPWSTR* value,
                                                UINT32* length) {
    return attributes_->GetAllocatedString(key, value, length);
}
IFACEMETHODIMP QcamActivate::GetBlobSize(REFGUID key, UINT32* size) {
    return attributes_->GetBlobSize(key, size);
}
IFACEMETHODIMP QcamActivate::GetBlob(REFGUID key, UINT8* buf, UINT32 size,
                                     UINT32* blob_size) {
    return attributes_->GetBlob(key, buf, size, blob_size);
}
IFACEMETHODIMP QcamActivate::GetAllocatedBlob(REFGUID key, UINT8** buf, UINT32* size) {
    return attributes_->GetAllocatedBlob(key, buf, size);
}
IFACEMETHODIMP QcamActivate::GetUnknown(REFGUID key, REFIID riid, LPVOID* ppv) {
    return attributes_->GetUnknown(key, riid, ppv);
}
IFACEMETHODIMP QcamActivate::SetItem(REFGUID key, REFPROPVARIANT value) {
    return attributes_->SetItem(key, value);
}
IFACEMETHODIMP QcamActivate::DeleteItem(REFGUID key) {
    return attributes_->DeleteItem(key);
}
IFACEMETHODIMP QcamActivate::DeleteAllItems() { return attributes_->DeleteAllItems(); }
IFACEMETHODIMP QcamActivate::SetUINT32(REFGUID key, UINT32 value) {
    return attributes_->SetUINT32(key, value);
}
IFACEMETHODIMP QcamActivate::SetUINT64(REFGUID key, UINT64 value) {
    return attributes_->SetUINT64(key, value);
}
IFACEMETHODIMP QcamActivate::SetDouble(REFGUID key, double value) {
    return attributes_->SetDouble(key, value);
}
IFACEMETHODIMP QcamActivate::SetGUID(REFGUID key, REFGUID value) {
    return attributes_->SetGUID(key, value);
}
IFACEMETHODIMP QcamActivate::SetString(REFGUID key, LPCWSTR value) {
    return attributes_->SetString(key, value);
}
IFACEMETHODIMP QcamActivate::SetBlob(REFGUID key, const UINT8* buf, UINT32 size) {
    return attributes_->SetBlob(key, buf, size);
}
IFACEMETHODIMP QcamActivate::SetUnknown(REFGUID key, IUnknown* unknown) {
    return attributes_->SetUnknown(key, unknown);
}
IFACEMETHODIMP QcamActivate::LockStore() { return attributes_->LockStore(); }
IFACEMETHODIMP QcamActivate::UnlockStore() { return attributes_->UnlockStore(); }
IFACEMETHODIMP QcamActivate::GetCount(UINT32* count) {
    return attributes_->GetCount(count);
}
IFACEMETHODIMP QcamActivate::GetItemByIndex(UINT32 index, GUID* key,
                                            PROPVARIANT* value) {
    return attributes_->GetItemByIndex(index, key, value);
}
IFACEMETHODIMP QcamActivate::CopyAllItems(IMFAttributes* dest) {
    return attributes_->CopyAllItems(dest);
}

}  // namespace vcam
}  // namespace qcam
