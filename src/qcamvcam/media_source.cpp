// SPDX-License-Identifier: GPL-2.0-or-later
#include "qcamvcam.h"

#include <mferror.h>

#include <algorithm>
#include <new>
#include <vector>

#include "qcam/log.h"

namespace qcam {
namespace vcam {

std::atomic<long> g_object_count{0};

namespace {

// Standard VideoProcAmp ranges. The HDCS-1000 has no hardware for most of
// these; brightness and contrast are applied in the decoder's tone curve, and
// the rest are reported so that app settings panels have something coherent
// to show rather than failing to open.
struct ProcAmpRange {
    ULONG property;
    LONG  min;
    LONG  max;
    LONG  step;
    LONG  default_value;
    ULONG capabilities;
};

constexpr ProcAmpRange kProcAmpRanges[] = {
    {KSPROPERTY_VIDEOPROCAMP_BRIGHTNESS, -100, 100, 1,   0, KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL},
    {KSPROPERTY_VIDEOPROCAMP_CONTRAST,     20, 200, 1, 100, KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL},
    {KSPROPERTY_VIDEOPROCAMP_SATURATION,    0, 200, 1, 115, KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL},
    {KSPROPERTY_VIDEOPROCAMP_GAMMA,        20, 300, 1,  65, KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL},
    {KSPROPERTY_VIDEOPROCAMP_GAIN,          0, 255, 1,  50, KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL |
                                                            KSPROPERTY_VIDEOPROCAMP_FLAGS_AUTO},
    {KSPROPERTY_VIDEOPROCAMP_WHITEBALANCE,  0, 255, 1, 128, KSPROPERTY_VIDEOPROCAMP_FLAGS_AUTO},
};

const ProcAmpRange* FindProcAmp(ULONG property) {
    for (const auto& range : kProcAmpRanges)
        if (range.property == property) return &range;
    return nullptr;
}

}  // namespace

QcamMediaSource::QcamMediaSource() { ++g_object_count; }

QcamMediaSource::~QcamMediaSource() {
    if (stream_) {
        stream_->Release();
        stream_ = nullptr;
    }
    --g_object_count;
}

HRESULT QcamMediaSource::CheckShutdown() const {
    return shutdown_ ? MF_E_SHUTDOWN : S_OK;
}

bool QcamMediaSource::IsShutdown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_;
}

HRESULT QcamMediaSource::CreateMediaType(UINT32 width, UINT32 height,
                                         IMFMediaType** out) const {
    if (!out) return E_POINTER;
    *out = nullptr;

    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;

    hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;
    hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) return hr;

    hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps_num_, fps_den_);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) return hr;

    hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return hr;
    // NV12 here is packed, so the stride is just the width.
    hr = type->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_SAMPLE_SIZE,
                         static_cast<UINT32>(ImageSize(PixelFormat::Nv12,
                                                       static_cast<uint16_t>(width),
                                                       static_cast<uint16_t>(height))));
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr)) return hr;

    // Frame rate bounds: this camera is bandwidth-limited to a single rate.
    hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE_RANGE_MIN, fps_num_, fps_den_);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE_RANGE_MAX, fps_num_, fps_den_);
    if (FAILED(hr)) return hr;

    *out = type.Detach();
    return S_OK;
}

HRESULT QcamMediaSource::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Match whatever the service is actually publishing, so the advertised
    // media type and the frames in the ring cannot disagree. If the service
    // is not up yet, fall back to the defaults it uses.
    {
        FrameRingReader probe;
        if (Succeeded(probe.Open())) {
            RingConfig config;
            if (Succeeded(probe.GetConfig(&config)) && config.width && config.height) {
                width_   = config.width;
                height_  = config.height;
                fps_num_ = config.fps_numerator ? config.fps_numerator : kDefaultFpsNum;
                fps_den_ = config.fps_denominator ? config.fps_denominator : kDefaultFpsDen;
                QCAM_LOGI("virtual camera adopting live format %ux%u", width_, height_);
            }
            probe.Close();
        } else {
            QCAM_LOGD("service not running; advertising the default format");
        }
    }

    HRESULT hr = MFCreateEventQueue(&event_queue_);
    if (FAILED(hr)) return hr;

    // Native size first, so it is the default; then the standard sizes the
    // stream scales to for apps that insist on one.
    std::vector<ComPtr<IMFMediaType>> types;
    types.emplace_back();
    hr = CreateMediaType(width_, height_, &types.back());
    if (FAILED(hr)) return hr;
    for (const auto& size : kExtraSizes) {
        if (size.width == width_ && size.height == height_) continue;
        types.emplace_back();
        hr = CreateMediaType(size.width, size.height, &types.back());
        if (FAILED(hr)) return hr;
    }

    std::vector<IMFMediaType*> raw_types;
    for (const auto& t : types) raw_types.push_back(t.Get());
    ComPtr<IMFStreamDescriptor> stream_descriptor;
    hr = MFCreateStreamDescriptor(/*streamId=*/0, static_cast<DWORD>(raw_types.size()),
                                  raw_types.data(), &stream_descriptor);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaTypeHandler> handler;
    hr = stream_descriptor->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    hr = handler->SetCurrentMediaType(types.front().Get());
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor* descriptors[] = {stream_descriptor.Get()};
    hr = MFCreatePresentationDescriptor(1, descriptors, &descriptor_);
    if (FAILED(hr)) return hr;
    hr = descriptor_->SelectStream(0);
    if (FAILED(hr)) return hr;

    // Attributes the capture pipeline looks for when deciding what this
    // source is. Without the frame-source type and stream category, the Frame
    // Server will not treat it as a colour camera.
    hr = MFCreateAttributes(&source_attributes_, 4);
    if (FAILED(hr)) return hr;
    hr = source_attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                                       MFFrameSourceTypes_Color);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&stream_attributes_, 4);
    if (FAILED(hr)) return hr;
    hr = stream_attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    if (FAILED(hr)) return hr;
    hr = stream_attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY,
                                     PINNAME_VIDEO_CAPTURE);
    if (FAILED(hr)) return hr;
    hr = stream_attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                                       MFFrameSourceTypes_Color);
    if (FAILED(hr)) return hr;

    // Undocumented, but the Frame Server client reads both off every stream
    // during activation (confirmed via mftrace); leaving them unset may be
    // read as "hidden" rather than defaulted to visible.
    hr = stream_attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 0);
    if (FAILED(hr)) return hr;
    hr = stream_attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_HIDDEN, 0);
    if (FAILED(hr)) return hr;

    // The Frame Server queries this during activation, before ever starting
    // the source, and appears to require it: without a sensor profile
    // collection the camera activates but Start() fails with MF_E_SHUTDOWN.
    // This camera has one fixed frame rate, so a single generic "normal
    // speed" profile is enough; there is no high-frame-rate mode to declare.
    {
        ComPtr<IMFSensorProfileCollection> profiles;
        ComPtr<IMFSensorProfile> profile;
        hr = MFCreateSensorProfileCollection(&profiles);
        if (FAILED(hr)) return hr;
        hr = MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile);
        if (FAILED(hr)) return hr;
        hr = profile->AddProfileFilter(/*streamId=*/0, L"((RES==;FRT<=30,1;SUT==))");
        if (FAILED(hr)) return hr;
        hr = profiles->AddProfile(profile.Get());
        if (FAILED(hr)) return hr;
        hr = source_attributes_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION,
                                            profiles.Get());
        if (FAILED(hr)) return hr;
    }

    stream_ = new (std::nothrow) QcamMediaStream();
    if (!stream_) return E_OUTOFMEMORY;

    hr = stream_->Initialize(this, stream_descriptor.Get(), width_, height_,
                             fps_num_, fps_den_);
    if (FAILED(hr)) {
        stream_->Release();
        stream_ = nullptr;
        return hr;
    }

    QCAM_LOGI("virtual camera source ready: %ux%u NV12 @ %.2f fps", width_, height_,
              fps_den_ ? static_cast<double>(fps_num_) / fps_den_ : 0.0);
    return S_OK;
}

// --- IUnknown --------------------------------------------------------------

IFACEMETHODIMP QcamMediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == IID_IMFGetService) {
        *ppv = static_cast<IMFGetService*>(this);
    } else if (riid == IID_IMFSampleAllocatorControl) {
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl*>(this);
    } else {
        QCAM_LOGT("QcamMediaSource: interface {%08lx-...} not implemented",
                  (unsigned long)riid.Data1);
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) QcamMediaSource::AddRef() { return ++ref_count_; }

IFACEMETHODIMP_(ULONG) QcamMediaSource::Release() {
    const ULONG count = --ref_count_;
    if (count == 0) delete this;
    return count;
}

// --- IMFMediaEventGenerator ------------------------------------------------

IFACEMETHODIMP QcamMediaSource::BeginGetEvent(IMFAsyncCallback* callback,
                                              IUnknown* state) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP QcamMediaSource::EndGetEvent(IMFAsyncResult* result,
                                            IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->EndGetEvent(result, event) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP QcamMediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->GetEvent(flags, event) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP QcamMediaSource::QueueEvent(MediaEventType type, REFGUID extended,
                                           HRESULT status,
                                           const PROPVARIANT* value) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FAILED(CheckShutdown())) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->QueueEventParamVar(type, extended, status, value)
                 : MF_E_SHUTDOWN;
}

// --- IMFMediaSource --------------------------------------------------------

IFACEMETHODIMP QcamMediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;

    // A live capture source: no seeking, no duration, and it can pause.
    *characteristics = MFMEDIASOURCE_IS_LIVE | MFMEDIASOURCE_CAN_PAUSE;
    return S_OK;
}

IFACEMETHODIMP QcamMediaSource::CreatePresentationDescriptor(
    IMFPresentationDescriptor** pd) {
    if (!pd) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    if (!descriptor_) return E_UNEXPECTED;

    // Each caller gets its own copy: the presentation descriptor carries
    // mutable per-session state such as stream selection.
    return descriptor_->Clone(pd);
}

IFACEMETHODIMP QcamMediaSource::Start(IMFPresentationDescriptor* pd,
                                      const GUID* time_format,
                                      const PROPVARIANT* start_position) {
    if (!pd) return E_INVALIDARG;
    // Only the default (100 ns) time format is meaningful for a live source.
    if (time_format && *time_format != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

    QcamMediaStream*           stream = nullptr;
    ComPtr<IMFMediaEventQueue> queue;
    bool                       first_start = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        stream = stream_;
        if (stream) stream->AddRef();
        queue = event_queue_;
        first_start = !stream_announced_;
        stream_announced_ = true;
    }
    if (!stream) return E_UNEXPECTED;
    if (!queue) {
        stream->Release();
        return MF_E_SHUTDOWN;
    }

    // The pipeline discovers streams from MENewStream / MEUpdatedStream, and
    // those must be queued on the *source* with the stream object as the
    // event payload, before MESourceStarted. Getting this order wrong is why
    // a source enumerates but never delivers a sample.
    HRESULT hr = queue->QueueEventParamUnk(
        first_start ? MENewStream : MEUpdatedStream, GUID_NULL, S_OK,
        static_cast<IMFMediaStream*>(stream));

    if (SUCCEEDED(hr)) {
        PROPVARIANT start;
        PropVariantInit(&start);
        start.vt = VT_I8;
        start.hVal.QuadPart = (start_position && start_position->vt == VT_I8)
                                  ? start_position->hVal.QuadPart
                                  : 0;
        hr = queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &start);
        PropVariantClear(&start);
    }

    if (FAILED(hr))
        QCAM_LOGE("Camera::Start: queuing the new-stream event failed: 0x%08lx",
                  static_cast<unsigned long>(hr));

    // The stream queues MEStreamStarted on its own queue.
    if (SUCCEEDED(hr)) hr = stream->Start();
    if (FAILED(hr))
        QCAM_LOGE("Camera::Start: stream failed to start: 0x%08lx",
                  static_cast<unsigned long>(hr));

    stream->Release();
    return hr;
}

IFACEMETHODIMP QcamMediaSource::Pause() {
    QcamMediaStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        stream = stream_;
        if (stream) stream->AddRef();
    }
    HRESULT hr = S_OK;
    if (stream) {
        hr = stream->Pause();
        stream->Release();
    }
    if (SUCCEEDED(hr)) hr = QueueEvent(MESourcePaused, GUID_NULL, S_OK, nullptr);
    return hr;
}

IFACEMETHODIMP QcamMediaSource::Stop() {
    QcamMediaStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        stream = stream_;
        if (stream) stream->AddRef();
    }
    HRESULT hr = S_OK;
    if (stream) {
        hr = stream->StopStream();
        stream->Release();
    }
    if (SUCCEEDED(hr)) hr = QueueEvent(MESourceStopped, GUID_NULL, S_OK, nullptr);
    return hr;
}

IFACEMETHODIMP QcamMediaSource::Shutdown() {
    QcamMediaStream* stream = nullptr;
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        shutdown_ = true;
        stream = stream_;
        stream_ = nullptr;
        queue = event_queue_;
        event_queue_.Reset();
        descriptor_.Reset();
    }

    if (stream) {
        stream->Shutdown();
        stream->Release();
    }
    if (queue) queue->Shutdown();
    return S_OK;
}

// --- IMFMediaSourceEx ------------------------------------------------------

IFACEMETHODIMP QcamMediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    if (!source_attributes_) return E_UNEXPECTED;

    *attributes = source_attributes_.Get();
    (*attributes)->AddRef();
    return S_OK;
}

IFACEMETHODIMP QcamMediaSource::GetStreamAttributes(DWORD stream_id,
                                                    IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    if (stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;

    std::lock_guard<std::mutex> lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    if (!stream_attributes_) return E_UNEXPECTED;

    *attributes = stream_attributes_.Get();
    (*attributes)->AddRef();
    return S_OK;
}

IFACEMETHODIMP QcamMediaSource::SetD3DManager(IUnknown*) {
    // Frames arrive as system memory from a USB 1.1 camera; there is nothing
    // for a D3D device to accelerate here.
    return E_NOTIMPL;
}

// --- IMFGetService ---------------------------------------------------------

IFACEMETHODIMP QcamMediaSource::GetService(REFGUID service, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (service == GUID_NULL || service == MF_MEDIASOURCE_SERVICE)
        return QueryInterface(riid, ppv);

    return MF_E_UNSUPPORTED_SERVICE;
}

// --- IMFSampleAllocatorControl ---------------------------------------------

IFACEMETHODIMP QcamMediaSource::SetDefaultAllocator(DWORD output_stream_id,
                                                    IUnknown* allocator) {
    if (output_stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;

    QcamMediaStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        stream = stream_;
        if (stream) stream->AddRef();
    }
    if (!stream) return E_UNEXPECTED;
    const HRESULT hr = stream->SetAllocator(allocator);
    stream->Release();
    return hr;
}

IFACEMETHODIMP QcamMediaSource::GetAllocatorUsage(DWORD output_stream_id,
                                                  DWORD* input_stream_id,
                                                  MFSampleAllocatorUsage* usage) {
    if (!input_stream_id || !usage) return E_POINTER;
    if (output_stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;
    // A source has no input streams; echo the output id as Microsoft's own
    // virtual camera sample does.
    *input_stream_id = output_stream_id;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

// --- IKsControl ------------------------------------------------------------

HRESULT QcamMediaSource::HandleVideoProcAmp(PKSPROPERTY property, void* data,
                                            ULONG data_length,
                                            ULONG* bytes_returned) {
    const ProcAmpRange* range = FindProcAmp(property->Id);
    if (!range) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

    if (property->Flags & KSPROPERTY_TYPE_BASICSUPPORT) {
        // Describe the control's range so settings panels can draw a slider.
        if (data_length < sizeof(KSPROPERTY_DESCRIPTION)) {
            if (bytes_returned) *bytes_returned = sizeof(KSPROPERTY_MEMBERSHEADER) +
                                                  sizeof(KSPROPERTY_STEPPING_LONG) +
                                                  sizeof(KSPROPERTY_DESCRIPTION);
            return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
        }

        auto* description = static_cast<PKSPROPERTY_DESCRIPTION>(data);
        description->AccessFlags   = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET |
                                     KSPROPERTY_TYPE_BASICSUPPORT;
        description->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION) +
                                       sizeof(KSPROPERTY_MEMBERSHEADER) +
                                       sizeof(KSPROPERTY_STEPPING_LONG);
        description->PropTypeSet.Set = KSPROPTYPESETID_General;
        description->PropTypeSet.Id  = VT_I4;
        description->PropTypeSet.Flags = 0;
        description->MembersListCount = 1;
        description->Reserved = 0;

        if (data_length >= description->DescriptionSize) {
            auto* members = reinterpret_cast<PKSPROPERTY_MEMBERSHEADER>(description + 1);
            members->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
            members->MembersSize  = sizeof(KSPROPERTY_STEPPING_LONG);
            members->MembersCount = 1;
            members->Flags = 0;

            auto* stepping = reinterpret_cast<PKSPROPERTY_STEPPING_LONG>(members + 1);
            stepping->Bounds.SignedMinimum = range->min;
            stepping->Bounds.SignedMaximum = range->max;
            stepping->SteppingDelta        = static_cast<ULONG>(range->step);
            stepping->Reserved = 0;
            if (bytes_returned) *bytes_returned = description->DescriptionSize;
        } else {
            if (bytes_returned) *bytes_returned = sizeof(KSPROPERTY_DESCRIPTION);
        }
        return S_OK;
    }

    if (data_length < sizeof(KSPROPERTY_VIDEOPROCAMP_S))
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    auto* procamp = static_cast<PKSPROPERTY_VIDEOPROCAMP_S>(data);

    // The four controls the decoder applies; gain and white balance stay
    // automatic and just report their defaults.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!controls_.IsOpen() && Succeeded(controls_.Open()))
        controls_.Set(local_controls_);   // settings made while qcamsvc was down
    PictureControls current = controls_.IsOpen() ? controls_.Get() : local_controls_;
    int32_t* field = nullptr;
    switch (property->Id) {
        case KSPROPERTY_VIDEOPROCAMP_BRIGHTNESS: field = &current.brightness; break;
        case KSPROPERTY_VIDEOPROCAMP_CONTRAST:   field = &current.contrast;   break;
        case KSPROPERTY_VIDEOPROCAMP_SATURATION: field = &current.saturation; break;
        case KSPROPERTY_VIDEOPROCAMP_GAMMA:      field = &current.gamma;      break;
        default: break;
    }

    if (property->Flags & KSPROPERTY_TYPE_GET) {
        procamp->Value        = field ? *field : range->default_value;
        procamp->Flags        = range->capabilities;
        procamp->Capabilities = range->capabilities;
        if (bytes_returned) *bytes_returned = sizeof(KSPROPERTY_VIDEOPROCAMP_S);
        return S_OK;
    }

    if (property->Flags & KSPROPERTY_TYPE_SET) {
        if (field) {
            *field = std::clamp(static_cast<int32_t>(procamp->Value),
                                static_cast<int32_t>(range->min),
                                static_cast<int32_t>(range->max));
            local_controls_ = current;
            // qcamsvc picks this up at its next frame.
            if (controls_.IsOpen()) controls_.Set(current);
        }
        if (bytes_returned) *bytes_returned = sizeof(KSPROPERTY_VIDEOPROCAMP_S);
        return S_OK;
    }

    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

IFACEMETHODIMP QcamMediaSource::KsProperty(PKSPROPERTY property,
                                           ULONG property_length, void* data,
                                           ULONG data_length,
                                           ULONG* bytes_returned) {
    if (!property || property_length < sizeof(KSPROPERTY)) return E_INVALIDARG;
    if (bytes_returned) *bytes_returned = 0;

    if (property->Set == PROPSETID_VIDCAP_VIDEOPROCAMP)
        return HandleVideoProcAmp(property, data, data_length, bytes_returned);

    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP QcamMediaSource::KsMethod(PKSMETHOD, ULONG, void*, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP QcamMediaSource::KsEvent(PKSEVENT, ULONG, void*, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

}  // namespace vcam
}  // namespace qcam
