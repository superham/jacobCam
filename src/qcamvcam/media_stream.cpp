// SPDX-License-Identifier: GPL-2.0-or-later
#include "qcamvcam.h"

#include <mferror.h>

#include <algorithm>
#include <cstring>

#include "qcam/log.h"

namespace qcam {
namespace vcam {

QcamMediaStream::QcamMediaStream() {
    ++g_object_count;
    work_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

QcamMediaStream::~QcamMediaStream() {
    Shutdown();
    if (work_event_) ::CloseHandle(work_event_);
    --g_object_count;
}

HRESULT QcamMediaStream::Initialize(QcamMediaSource* source,
                                    IMFStreamDescriptor* descriptor, UINT32 width,
                                    UINT32 height, UINT32 fps_num, UINT32 fps_den) {
    if (!source || !descriptor) return E_INVALIDARG;
    if (!work_event_) return E_FAIL;

    std::lock_guard<std::mutex> lock(mutex_);
    source_     = source;
    descriptor_ = descriptor;
    width_      = width;
    height_     = height;
    fps_num_    = fps_num ? fps_num : kDefaultFpsNum;
    fps_den_    = fps_den ? fps_den : kDefaultFpsDen;

    frame_bytes_ = ImageSize(PixelFormat::Nv12, static_cast<uint16_t>(width_),
                             static_cast<uint16_t>(height_));
    // One second in 100 ns units, divided by the frame rate.
    frame_duration_100ns_ =
        static_cast<LONGLONG>(10'000'000.0 * fps_den_ / fps_num_);

    return MFCreateEventQueue(&event_queue_);
}

HRESULT QcamMediaStream::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return S_OK;
        shutdown_ = true;
        state_ = MF_STREAM_STATE_STOPPED;
    }

    worker_stop_.store(true);
    if (work_event_) ::SetEvent(work_event_);
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    if (allocator_) {
        if (allocator_ready_) allocator_->UninitializeSampleAllocator();
        allocator_.Reset();
        allocator_ready_ = false;
    }
    if (event_queue_) {
        event_queue_->Shutdown();
        event_queue_.Reset();
    }
    ring_.Close();
    ring_open_ = false;
    descriptor_.Reset();
    source_ = nullptr;
    return S_OK;
}

// --- IUnknown --------------------------------------------------------------

IFACEMETHODIMP QcamMediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaStream || riid == IID_IMFMediaStream2) {
        *ppv = static_cast<IMFMediaStream2*>(this);
    } else {
        QCAM_LOGT("QcamMediaStream: interface {%08lx-...} not implemented",
                  (unsigned long)riid.Data1);
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) QcamMediaStream::AddRef() {
    return ++ref_count_;
}

IFACEMETHODIMP_(ULONG) QcamMediaStream::Release() {
    const ULONG count = --ref_count_;
    if (count == 0) delete this;
    return count;
}

// --- IMFMediaEventGenerator ------------------------------------------------

IFACEMETHODIMP QcamMediaStream::BeginGetEvent(IMFAsyncCallback* callback,
                                              IUnknown* state) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP QcamMediaStream::EndGetEvent(IMFAsyncResult* result,
                                            IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->EndGetEvent(result, event) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP QcamMediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    // GetEvent can block, so the queue reference is taken under the lock and
    // the call itself is made without it held.
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->GetEvent(flags, event) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP QcamMediaStream::QueueEvent(MediaEventType type,
                                           REFGUID extended_type, HRESULT status,
                                           const PROPVARIANT* value) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->QueueEventParamVar(type, extended_type, status, value)
                 : MF_E_SHUTDOWN;
}

// --- IMFMediaStream --------------------------------------------------------

IFACEMETHODIMP QcamMediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_ || !source_) return MF_E_SHUTDOWN;
    return source_->QueryInterface(IID_PPV_ARGS(source));
}

IFACEMETHODIMP QcamMediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!descriptor_) return E_UNEXPECTED;

    *descriptor = descriptor_.Get();
    (*descriptor)->AddRef();
    return S_OK;
}

IFACEMETHODIMP QcamMediaStream::RequestSample(IUnknown* token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
        pending_.emplace_back(token);
    }
    // Hand the work to the worker thread: RequestSample must return promptly,
    // and pulling a frame can block for up to a frame interval.
    ::SetEvent(work_event_);
    return S_OK;
}

// --- IMFMediaStream2 -------------------------------------------------------

IFACEMETHODIMP QcamMediaStream::SetStreamState(MF_STREAM_STATE state) {
    switch (state) {
        case MF_STREAM_STATE_RUNNING: return Start();
        case MF_STREAM_STATE_PAUSED:  return Pause();
        case MF_STREAM_STATE_STOPPED: return StopStream();
        default:                      return E_INVALIDARG;
    }
}

IFACEMETHODIMP QcamMediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
}

// --- Streaming -------------------------------------------------------------

HRESULT QcamMediaStream::Start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ == MF_STREAM_STATE_RUNNING) return S_OK;
        state_ = MF_STREAM_STATE_RUNNING;
        next_timestamp_ = 0;
        UpdateOutputSize();

        if (!ring_open_) {
            // The service may not be running yet. That is not fatal: the
            // stream produces blank frames until it appears, which keeps
            // conferencing apps from erroring out at open time.
            ring_open_ = Succeeded(ring_.Open());
            if (!ring_open_)
                QCAM_LOGW("frame ring unavailable; emitting blank frames");
        }

        if (allocator_ && !allocator_ready_) {
            ComPtr<IMFMediaTypeHandler> handler;
            ComPtr<IMFMediaType> type;
            HRESULT hr = descriptor_ ? descriptor_->GetMediaTypeHandler(&handler) : E_UNEXPECTED;
            if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
            // A handful of samples in flight is plenty at under 8 fps.
            if (SUCCEEDED(hr)) hr = allocator_->InitializeSampleAllocator(8, type.Get());
            allocator_ready_ = SUCCEEDED(hr);
            if (FAILED(hr))
                QCAM_LOGE("initialising the Frame Server's sample allocator failed: 0x%08lx",
                          static_cast<unsigned long>(hr));
        }

        if (!worker_.joinable()) {
            worker_stop_.store(false);
            worker_ = std::thread([this] { WorkerLoop(); });
        }
    }
    ::SetEvent(work_event_);
    const HRESULT hr = QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    if (FAILED(hr))
        QCAM_LOGE("QcamMediaStream::Start: QueueEvent(MEStreamStarted) failed 0x%08lx",
                  static_cast<unsigned long>(hr));
    return hr;
}

HRESULT QcamMediaStream::Pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        state_ = MF_STREAM_STATE_PAUSED;
    }
    return QueueEvent(MEStreamPaused, GUID_NULL, S_OK, nullptr);
}

HRESULT QcamMediaStream::StopStream() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        state_ = MF_STREAM_STATE_STOPPED;
        pending_.clear();
    }
    return QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT QcamMediaStream::SetAllocator(IUnknown* allocator) {
    ComPtr<IMFVideoSampleAllocatorEx> video_allocator;
    if (allocator) {
        const HRESULT hr = allocator->QueryInterface(IID_PPV_ARGS(&video_allocator));
        if (FAILED(hr)) return hr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (allocator_ && allocator_ready_) allocator_->UninitializeSampleAllocator();
    allocator_ = video_allocator;
    allocator_ready_ = false;
    return S_OK;
}

void QcamMediaStream::UpdateOutputSize() {
    // mutex_ is held by the caller.
    ComPtr<IMFMediaTypeHandler> handler;
    ComPtr<IMFMediaType> type;
    UINT32 w = 0, h = 0;
    if (!descriptor_ || FAILED(descriptor_->GetMediaTypeHandler(&handler)) ||
        FAILED(handler->GetCurrentMediaType(&type)) ||
        FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) {
        return;
    }
    if (w == width_ && h == height_) return;

    width_  = w;
    height_ = h;
    frame_bytes_ = ImageSize(PixelFormat::Nv12, static_cast<uint16_t>(w),
                             static_cast<uint16_t>(h));
    // A frame kept for repeating is the old size; the allocator's samples
    // are too, so re-initialise it for the new type.
    last_frame_.clear();
    if (allocator_ && allocator_ready_) {
        allocator_->UninitializeSampleAllocator();
        allocator_ready_ = false;
    }
    QCAM_LOGI("app selected %ux%u", w, h);
}

HRESULT QcamMediaStream::SetRate(float) {
    // The camera runs at one fixed rate; there is nothing to vary.
    return S_OK;
}

void QcamMediaStream::WorkerLoop() {
    while (!worker_stop_.load()) {
        ComPtr<IUnknown> token;
        bool have_request = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!shutdown_ && state_ == MF_STREAM_STATE_RUNNING && !pending_.empty()) {
                token = pending_.front();
                pending_.erase(pending_.begin());
                have_request = true;
            }
        }

        if (!have_request) {
            ::WaitForSingleObject(work_event_, 100);
            continue;
        }

        const HRESULT hr = DeliverSample(token.Get());
        if (FAILED(hr) && hr != MF_E_SHUTDOWN) {
            QCAM_LOGW("delivering a sample failed: 0x%08lx",
                      static_cast<unsigned long>(hr));
            QueueEvent(MEError, GUID_NULL, hr, nullptr);
        }
    }
}

HRESULT QcamMediaStream::DeliverSample(IUnknown* token) {
    ComPtr<IMFSample> sample;
    HRESULT hr = CreateSampleFromRing(&sample);
    if (hr == MF_E_SHUTDOWN) return hr;
    if (FAILED(hr) || !sample) {
        // No new frame in time. Pace the stand-in at the frame rate: answering
        // at once would flood the consumer with hundreds of frames a second
        // while the service is still starting the camera.
        const LONGLONG due = last_fill_time_ + frame_duration_100ns_;
        const LONGLONG now = MFGetSystemTime();
        if (last_fill_time_ && now < due)
            ::Sleep(static_cast<DWORD>((due - now) / 10'000));
        last_fill_time_ = MFGetSystemTime();

        // Repeat the last real frame if there is one: in low light the camera
        // runs well under its nominal rate, and a grey frame between two real
        // ones reads as flicker. Grey only until the first frame arrives.
        hr = last_frame_.empty()
                 ? CreateBlankSample(&sample)
                 : WrapBuffer(last_frame_.data(), last_frame_.size(), next_timestamp_, &sample);
        if (FAILED(hr)) return hr;
    }

    if (token) {
        // The Frame Server matches the token it handed to RequestSample
        // against the one that comes back on the sample.
        hr = sample->SetUnknown(MFSampleExtension_Token, token);
        if (FAILED(hr)) return hr;
    }

    // A sample is delivered as an MEMediaSample event carrying the sample as
    // its IUnknown payload; there is no separate delivery call.
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    if (!queue) return MF_E_SHUTDOWN;

    return queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

HRESULT QcamMediaStream::CreateSampleFromRing(IMFSample** out) {
    if (!out) return E_POINTER;
    *out = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (!ring_open_) {
            // Retry cheaply: the service may have started since we last looked.
            ring_open_ = Succeeded(ring_.Open());
            if (!ring_open_) return E_PENDING;
        }
    }

    std::vector<uint8_t> frame;
    FrameMeta meta;
    // Low light stretches exposure past the frame period and the camera drops
    // to a few frames a second, so allow well over the nominal interval
    // before falling back to repeating the last frame.
    constexpr uint32_t kFrameWaitMs = 1000;
    const Status st = ring_.Read(&frame, &meta, kFrameWaitMs);

    if (st == Status::NoDevice) {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.Close();
        ring_open_ = false;
        return E_PENDING;
    }
    if (Failed(st)) return E_PENDING;

    if (meta.format != PixelFormat::Nv12 || (meta.width & 1) || (meta.height & 1) ||
        frame.size() != ImageSize(PixelFormat::Nv12, static_cast<uint16_t>(meta.width),
                                  static_cast<uint16_t>(meta.height))) {
        QCAM_LOGW("ring frame %ux%u (%zu bytes) is not usable NV12", meta.width,
                  meta.height, frame.size());
        return E_PENDING;
    }

    // The ring carries the native size; scale to the one the app picked.
    if (meta.width != width_ || meta.height != height_) {
        scaled_.resize(frame_bytes_);
        ScaleNv12(frame.data(), static_cast<uint16_t>(meta.width),
                  static_cast<uint16_t>(meta.height), scaled_.data(),
                  static_cast<uint16_t>(width_), static_cast<uint16_t>(height_));
        frame.swap(scaled_);
    }

    const HRESULT hr = WrapBuffer(frame.data(), frame.size(), next_timestamp_, out);
    if (SUCCEEDED(hr)) last_frame_ = std::move(frame);
    return hr;
}

HRESULT QcamMediaStream::CreateBlankSample(IMFSample** out) {
    // Mid-grey NV12: Y at 16 (studio black would be harsher), chroma neutral.
    std::vector<uint8_t> blank(frame_bytes_);
    const size_t luma = static_cast<size_t>(width_) * height_;
    std::memset(blank.data(), 32, luma);
    std::memset(blank.data() + luma, 128, blank.size() - luma);

    ++blanks_;
    if (blanks_ == 1 || blanks_ % 150 == 0)
        QCAM_LOGD("emitting blank frames (%llu so far)",
                  static_cast<unsigned long long>(blanks_));

    return WrapBuffer(blank.data(), blank.size(), next_timestamp_, out);
}

HRESULT QcamMediaStream::CopyIntoAllocatedSample(IMFVideoSampleAllocatorEx* allocator,
                                                 const uint8_t* data, size_t size,
                                                 IMFSample** out) {
    ComPtr<IMFSample> sample;
    HRESULT hr = allocator->AllocateSample(&sample);
    // Every sample is still with the consumer; give it a moment to return one.
    for (int tries = 0; hr == MF_E_SAMPLEALLOCATOR_EMPTY && tries < 20; ++tries) {
        ::Sleep(10);
        hr = allocator->AllocateSample(&sample);
    }
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) return hr;

    ComPtr<IMF2DBuffer2> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        // The allocator's buffers are pitched; copy row by row. NV12 in a 2D
        // buffer is the Y plane followed by the interleaved UV plane, both at
        // the same pitch.
        BYTE* scanline0 = nullptr;
        LONG  pitch = 0;
        BYTE* start = nullptr;
        DWORD length = 0;
        hr = buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch,
                                  &start, &length);
        if (FAILED(hr)) return hr;
        const uint8_t* src = data;
        for (UINT32 y = 0; y < height_; ++y, src += width_)
            std::memcpy(scanline0 + static_cast<ptrdiff_t>(pitch) * y, src, width_);
        BYTE* uv = scanline0 + static_cast<ptrdiff_t>(pitch) * height_;
        for (UINT32 y = 0; y < height_ / 2; ++y, src += width_)
            std::memcpy(uv + static_cast<ptrdiff_t>(pitch) * y, src, width_);
        buffer2d->Unlock2D();
    } else {
        BYTE* dst = nullptr;
        DWORD max_len = 0;
        hr = buffer->Lock(&dst, &max_len, nullptr);
        if (FAILED(hr)) return hr;
        if (max_len < size) {
            buffer->Unlock();
            return E_UNEXPECTED;
        }
        std::memcpy(dst, data, size);
        buffer->Unlock();
        hr = buffer->SetCurrentLength(static_cast<DWORD>(size));
        if (FAILED(hr)) return hr;
    }

    *out = sample.Detach();
    return S_OK;
}

HRESULT QcamMediaStream::WrapBuffer(const uint8_t* data, size_t size,
                                    LONGLONG timestamp, IMFSample** out) {
    ComPtr<IMFVideoSampleAllocatorEx> allocator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (allocator_ready_) allocator = allocator_;
    }

    ComPtr<IMFSample> sample;
    HRESULT hr;
    if (allocator) {
        hr = CopyIntoAllocatedSample(allocator.Get(), data, size, &sample);
        if (FAILED(hr)) return hr;
    } else {
        // No Frame Server allocator (e.g. an in-process consumer): plain
        // system-memory buffer.
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
        if (FAILED(hr)) return hr;

        BYTE* dst = nullptr;
        DWORD max_len = 0;
        hr = buffer->Lock(&dst, &max_len, nullptr);
        if (FAILED(hr)) return hr;
        if (max_len < size) {
            buffer->Unlock();
            return E_UNEXPECTED;
        }
        std::memcpy(dst, data, size);
        buffer->Unlock();

        hr = buffer->SetCurrentLength(static_cast<DWORD>(size));
        if (FAILED(hr)) return hr;

        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return hr;

        hr = sample->AddBuffer(buffer.Get());
        if (FAILED(hr)) return hr;
    }

    // A live source stamps samples on the Media Foundation system clock, which
    // is what the Frame Server and every consumer compare against; a series
    // starting at zero reads as hopelessly late and is dropped before it
    // reaches the app. Kept monotonic in case the clock and the frame
    // interval disagree.
    (void)timestamp;
    LONGLONG now = MFGetSystemTime();
    if (now <= last_sample_time_) now = last_sample_time_ + 1;
    last_sample_time_ = now;
    hr = sample->SetSampleTime(now);
    if (FAILED(hr)) return hr;
    hr = sample->SetSampleDuration(frame_duration_100ns_);
    if (FAILED(hr)) return hr;

    next_timestamp_ = timestamp + frame_duration_100ns_;
    ++delivered_;

    *out = sample.Detach();
    return S_OK;
}

}  // namespace vcam
}  // namespace qcam
