// SPDX-License-Identifier: GPL-2.0-or-later
//
// WinUSB transport.
//
// Vendor control transfers are straightforward. The interesting part is
// isochronous input: WinUSB has supported it from user mode since Windows
// 8.1, which is what makes a fully user-mode driver for this camera possible
// at all. The camera is a full-speed device, so the host schedules one packet
// per 1 ms frame; to keep that pipeline fed we register one large buffer and
// keep several multi-packet transfers in flight at once.

#include <windows.h>

#include <winusb.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "qcam/log.h"
#include "qcam/usb.h"
#include "qcam/win_guids.h"

namespace qcam {
namespace {

// Eight transfers of 32 packets each: ~256 ms of buffering at full speed,
// which is enough to ride out a scheduler hiccup without adding noticeable
// latency to a camera that only produces about eight frames a second.
constexpr size_t kTransfersInFlight   = 8;
constexpr size_t kPacketsPerTransfer  = 32;

Status StatusFromLastError(DWORD err) {
    switch (err) {
        case ERROR_SUCCESS:            return Status::Ok;
        case ERROR_SEM_TIMEOUT:
        case WAIT_TIMEOUT:             return Status::Timeout;
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_NO_SUCH_DEVICE:
        case ERROR_FILE_NOT_FOUND:     return Status::NoDevice;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:  return Status::Busy;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:        return Status::NoMemory;
        case ERROR_OPERATION_ABORTED:  return Status::Cancelled;
        case ERROR_NOT_SUPPORTED:      return Status::Unsupported;
        default:                       return Status::Io;
    }
}

std::wstring Widen(const std::string& narrow) {
    if (narrow.empty()) return std::wstring();
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
                                             static_cast<int>(narrow.size()),
                                             nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
                          static_cast<int>(narrow.size()), &out[0], needed);
    return out;
}

class WinUsbTransport final : public IUsbTransport {
public:
    ~WinUsbTransport() override {
        StopIso();
        if (winusb_ != INVALID_HANDLE_VALUE && winusb_ != nullptr)
            ::WinUsb_Free(winusb_);
        if (device_ != INVALID_HANDLE_VALUE && device_ != nullptr)
            ::CloseHandle(device_);
    }

    Status Open(const std::string& path) {
        info_.device_path = path;

        // Two things about this open:
        //
        // FILE_FLAG_OVERLAPPED is mandatory, because the isochronous reads
        // below are all asynchronous.
        //
        // The share mode is zero, i.e. exclusive. WinUSB itself would permit
        // several handles to the same interface, but this camera cannot
        // survive it: opening the device runs the sensor init sequence, which
        // resets the sensor and reprograms its timing. A second process doing
        // that to a live stream produces garbage in the first one. Exclusive
        // access turns that race into an honest ERROR_SHARING_VIOLATION, which
        // surfaces as Status::Busy.
        device_ = ::CreateFileW(Widen(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                                /*dwShareMode=*/0, nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                nullptr);
        if (device_ == INVALID_HANDLE_VALUE) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) {
                QCAM_LOGE("the camera is already open in another process "
                          "(the qcam service, most likely). Stop it with "
                          "'Stop-Service qcamsvc', or use 'qcamctl attach' to "
                          "read the frames it is already publishing.");
            } else {
                QCAM_LOGE("CreateFile on %s failed: %lu", path.c_str(), err);
            }
            return StatusFromLastError(err);
        }

        if (!::WinUsb_Initialize(device_, &winusb_)) {
            const DWORD err = ::GetLastError();
            QCAM_LOGE("WinUsb_Initialize failed: %lu", err);
            ::CloseHandle(device_);
            device_ = INVALID_HANDLE_VALUE;
            return StatusFromLastError(err);
        }

        USB_DEVICE_DESCRIPTOR desc = {};
        ULONG transferred = 0;
        if (::WinUsb_GetDescriptor(winusb_, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                                   reinterpret_cast<PUCHAR>(&desc), sizeof(desc),
                                   &transferred) &&
            transferred == sizeof(desc)) {
            info_.vid = desc.idVendor;
            info_.pid = desc.idProduct;
        }

        // DEVICE_SPEED yields a single byte: 0x01 low/full, 0x03 high.
        UCHAR speed = 0;
        ULONG len = sizeof(speed);
        if (::WinUsb_QueryDeviceInformation(winusb_, DEVICE_SPEED, &len, &speed))
            info_.bus_speed = static_cast<uint8_t>(speed);

        const DeviceId* known = LookupDevice(info_.vid, info_.pid);
        info_.friendly_name = known ? known->marketing_name : "Unknown STV06xx";

        QCAM_LOGI("opened %s (%04x:%04x)", info_.friendly_name.c_str(), info_.vid,
                  info_.pid);
        return Status::Ok;
    }

    // -- Control endpoint --------------------------------------------------

    Status ControlOut(uint8_t request, uint16_t value, uint16_t index,
                      const uint8_t* data, size_t len) override {
        WINUSB_SETUP_PACKET setup = {};
        setup.RequestType = kVendorOut;
        setup.Request     = request;
        setup.Value       = value;
        setup.Index       = index;
        setup.Length      = static_cast<USHORT>(len);

        ULONG transferred = 0;
        // The buffer is not modified for a host-to-device transfer, but the
        // API is not const-correct.
        if (!::WinUsb_ControlTransfer(winusb_, setup,
                                      const_cast<PUCHAR>(data),
                                      static_cast<ULONG>(len), &transferred,
                                      nullptr)) {
            const DWORD err = ::GetLastError();
            QCAM_LOGD("control out req 0x%02x value 0x%04x failed: %lu", request,
                      value, err);
            return StatusFromLastError(err);
        }
        return (transferred == len) ? Status::Ok : Status::Protocol;
    }

    Status ControlIn(uint8_t request, uint16_t value, uint16_t index,
                     uint8_t* data, size_t len, size_t* transferred) override {
        WINUSB_SETUP_PACKET setup = {};
        setup.RequestType = kVendorIn;
        setup.Request     = request;
        setup.Value       = value;
        setup.Index       = index;
        setup.Length      = static_cast<USHORT>(len);

        ULONG got = 0;
        if (!::WinUsb_ControlTransfer(winusb_, setup, data,
                                      static_cast<ULONG>(len), &got, nullptr)) {
            return StatusFromLastError(::GetLastError());
        }
        if (transferred) *transferred = got;
        return Status::Ok;
    }

    // -- Interface state ---------------------------------------------------

    Status SetAltSetting(uint8_t alt) override {
        if (!::WinUsb_SetCurrentAlternateSetting(winusb_, alt)) {
            const DWORD err = ::GetLastError();
            QCAM_LOGE("WinUsb_SetCurrentAlternateSetting(%u) failed: %lu", alt, err);
            return StatusFromLastError(err);
        }
        alt_ = alt;
        QCAM_LOGD("alternate setting %u selected", alt);
        return Status::Ok;
    }

    Status GetIsoMaxPacketSize(uint8_t alt, uint16_t* max_packet) override {
        if (!max_packet) return Status::InvalidArg;

        USB_INTERFACE_DESCRIPTOR iface = {};
        if (!::WinUsb_QueryInterfaceSettings(winusb_, alt, &iface))
            return StatusFromLastError(::GetLastError());

        for (UCHAR i = 0; i < iface.bNumEndpoints; ++i) {
            WINUSB_PIPE_INFORMATION_EX pipe = {};
            if (!::WinUsb_QueryPipeEx(winusb_, alt, i, &pipe)) continue;
            if (pipe.PipeType != UsbdPipeTypeIsochronous) continue;
            if (!USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) continue;

            *max_packet = static_cast<uint16_t>(pipe.MaximumPacketSize);
            iso_pipe_id_ = pipe.PipeId;
            QCAM_LOGD("iso pipe 0x%02x, wMaxPacketSize %u", pipe.PipeId,
                      pipe.MaximumPacketSize);
            return Status::Ok;
        }
        return Status::NoDevice;
    }

    // -- Isochronous streaming --------------------------------------------

    Status StartIso(IIsoSink* sink) override {
        if (!sink) return Status::InvalidArg;
        if (streaming_.load()) return Status::Busy;

        uint16_t packet_size = 0;
        QCAM_TRY(GetIsoMaxPacketSize(alt_, &packet_size));
        if (packet_size == 0) return Status::Protocol;

        packet_size_ = packet_size;
        transfer_bytes_ = static_cast<size_t>(packet_size) * kPacketsPerTransfer;
        buffer_.assign(transfer_bytes_ * kTransfersInFlight, 0);

        // A single registration covers the whole ring; each transfer then
        // addresses its own slice by offset.
        if (!::WinUsb_RegisterIsochBuffer(winusb_, iso_pipe_id_, buffer_.data(),
                                          static_cast<ULONG>(buffer_.size()),
                                          &iso_buffer_)) {
            const DWORD err = ::GetLastError();
            QCAM_LOGE("WinUsb_RegisterIsochBuffer failed: %lu", err);
            return StatusFromLastError(err);
        }

        transfers_.clear();
        transfers_.resize(kTransfersInFlight);
        for (size_t i = 0; i < kTransfersInFlight; ++i) {
            transfers_[i].descriptors.resize(kPacketsPerTransfer);
            transfers_[i].overlapped = {};
            transfers_[i].overlapped.hEvent =
                ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!transfers_[i].overlapped.hEvent) {
                const DWORD err = ::GetLastError();
                TeardownIso();
                return StatusFromLastError(err);
            }
        }

        sink_ = sink;
        stop_.store(false);
        streaming_.store(true);

        // Prime the pipeline before the reader thread starts, so the host
        // controller never runs dry between submissions.
        for (size_t i = 0; i < kTransfersInFlight; ++i) {
            const Status st = Submit(i, /*continue_stream=*/i != 0);
            if (Failed(st)) {
                // Transfers 0..i-1 are already queued and the host controller
                // owns the buffer they point into. Cancel and drain them
                // before releasing anything.
                streaming_.store(false);
                AbortAndDrain(i);
                TeardownIso();
                return st;
            }
        }

        thread_ = std::thread([this] { StreamLoop(); });
        return Status::Ok;
    }

    void StopIso() override {
        if (!streaming_.exchange(false)) return;

        stop_.store(true);
        // Cancel everything queued on the pipe so the in-flight waits return.
        if (winusb_ && winusb_ != INVALID_HANDLE_VALUE)
            ::WinUsb_AbortPipe(winusb_, iso_pipe_id_);

        if (thread_.joinable()) thread_.join();

        // The reader thread stops at whichever transfer it was on, leaving
        // the rest still queued. Drain them before the buffer goes away.
        AbortAndDrain(transfers_.size());
        TeardownIso();
        sink_ = nullptr;
    }

    bool IsStreaming() const override { return streaming_.load(); }

    const UsbDeviceInfo& Info() const override { return info_; }

private:
    struct Transfer {
        OVERLAPPED                            overlapped{};
        std::vector<USBD_ISO_PACKET_DESCRIPTOR> descriptors;
    };

    size_t OffsetOf(size_t index) const { return index * transfer_bytes_; }

    Status Submit(size_t index, bool continue_stream) {
        Transfer& t = transfers_[index];
        ::ResetEvent(t.overlapped.hEvent);

        // ReadIsochPipeAsap lets the driver pick the start frame, which is
        // what we want for a continuous capture: it keeps the stream packed
        // rather than aligning every transfer to an explicit frame number.
        if (!::WinUsb_ReadIsochPipeAsap(iso_buffer_,
                                        static_cast<ULONG>(OffsetOf(index)),
                                        static_cast<ULONG>(transfer_bytes_),
                                        continue_stream ? TRUE : FALSE,
                                        static_cast<ULONG>(t.descriptors.size()),
                                        t.descriptors.data(),
                                        &t.overlapped)) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                QCAM_LOGE("WinUsb_ReadIsochPipeAsap failed: %lu", err);
                return StatusFromLastError(err);
            }
        }
        return Status::Ok;
    }

    void StreamLoop() {
        // Transfers are completed strictly in submission order. Isochronous
        // data is only meaningful in order, and reaping round-robin keeps it
        // that way without any resequencing.
        size_t index = 0;
        while (!stop_.load()) {
            Transfer& t = transfers_[index];

            DWORD transferred = 0;
            if (!::WinUsb_GetOverlappedResult(winusb_, &t.overlapped, &transferred,
                                              TRUE)) {
                const DWORD err = ::GetLastError();
                if (stop_.load() || err == ERROR_OPERATION_ABORTED) break;
                QCAM_LOGW("isochronous transfer failed: %lu", err);
                if (sink_) sink_->OnIsoError(StatusFromLastError(err));
                break;
            }

            DeliverPackets(index, t);

            if (stop_.load()) break;
            if (Failed(Submit(index, /*continue_stream=*/true))) break;

            index = (index + 1) % transfers_.size();
        }
        QCAM_LOGD("isochronous reader thread exiting after %zu-byte packets",
                  static_cast<size_t>(packet_size_));
    }

    void DeliverPackets(size_t index, const Transfer& t) {
        const uint8_t* base = buffer_.data() + OffsetOf(index);
        for (const auto& desc : t.descriptors) {
            // A zero-length packet means the camera had nothing ready for
            // that bus frame, which is normal; a non-zero USBD status means
            // the packet was damaged and must not reach the framer.
            if (desc.Length == 0) continue;
            if (desc.Status != 0) {
                QCAM_LOGT("dropping iso packet with status 0x%08lx",
                          static_cast<unsigned long>(desc.Status));
                continue;
            }
            if (static_cast<size_t>(desc.Offset) + desc.Length > transfer_bytes_) {
                QCAM_LOGW("iso descriptor out of range: offset %lu length %lu",
                          static_cast<unsigned long>(desc.Offset),
                          static_cast<unsigned long>(desc.Length));
                continue;
            }
            if (sink_) sink_->OnIsoPacket(base + desc.Offset, desc.Length);
        }
    }

    // Cancels queued transfers and waits for the host controller to give the
    // buffer back. Safe to call with `count` == 0.
    void AbortAndDrain(size_t count) {
        if (winusb_ && winusb_ != INVALID_HANDLE_VALUE)
            ::WinUsb_AbortPipe(winusb_, iso_pipe_id_);

        for (size_t i = 0; i < count && i < transfers_.size(); ++i) {
            DWORD transferred = 0;
            // Blocking wait: an aborted transfer completes promptly with
            // ERROR_OPERATION_ABORTED, and we must not free the buffer first.
            ::WinUsb_GetOverlappedResult(winusb_, &transfers_[i].overlapped,
                                         &transferred, TRUE);
        }
    }

    void TeardownIso() {
        if (iso_buffer_) {
            ::WinUsb_UnregisterIsochBuffer(iso_buffer_);
            iso_buffer_ = nullptr;
        }
        for (auto& t : transfers_) {
            if (t.overlapped.hEvent) {
                ::CloseHandle(t.overlapped.hEvent);
                t.overlapped.hEvent = nullptr;
            }
        }
        transfers_.clear();
        buffer_.clear();
    }

    HANDLE                    device_  = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE   winusb_  = nullptr;
    WINUSB_ISOCH_BUFFER_HANDLE iso_buffer_ = nullptr;
    UCHAR                     iso_pipe_id_ = kIsoEndpoint;
    uint8_t                   alt_ = 0;
    uint16_t                  packet_size_ = 0;
    size_t                    transfer_bytes_ = 0;

    std::vector<uint8_t>      buffer_;
    std::vector<Transfer>     transfers_;
    IIsoSink*                 sink_ = nullptr;
    std::thread               thread_;
    std::atomic<bool>         streaming_{false};
    std::atomic<bool>         stop_{false};
    UsbDeviceInfo             info_;
};

}  // namespace

Status OpenDevice(const std::string& device_path,
                  std::unique_ptr<IUsbTransport>* out) {
    if (!out) return Status::InvalidArg;

    std::unique_ptr<WinUsbTransport> transport(new WinUsbTransport());
    QCAM_TRY(transport->Open(device_path));
    *out = std::move(transport);
    return Status::Ok;
}

}  // namespace qcam
