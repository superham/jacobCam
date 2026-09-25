# Architecture

## The shape of the problem

A modern Windows camera is a UVC device: Windows has a class driver, and
nothing else is needed. This camera is not one, so something has to translate
between a vendor-specific USB protocol and what `Media Foundation` expects.

The traditional answer is an **AVStream minidriver** — kernel-mode, and the
way webcam drivers were written when this camera was new. That route is bad
here for concrete reasons:

- Kernel drivers on 64-bit Windows need attestation or WHQL signing before
  they will load, and an EV code-signing certificate to get it.
- A bug in the isochronous path bugchecks the machine rather than crashing a
  process.
- Debugging needs a second machine and a kernel debugger.
- Demosaicing and an auto-exposure control loop in kernel mode is exactly the
  sort of thing that should not be in kernel mode.

So: **no custom kernel code at all**.

## The user-mode route

Two capabilities Windows gained after this camera was discontinued make it
possible:

**WinUSB isochronous transfers (Windows 8.1).** WinUSB originally handled only
control, bulk and interrupt transfers, which would have ruled out a camera.
Since 8.1 it exposes `WinUsb_RegisterIsochBuffer` and
`WinUsb_ReadIsochPipeAsap`, so a user-mode process can drive an isochronous
video endpoint directly.

**`MFCreateVirtualCamera` (Windows 11, build 22000).** A user-mode media source
can be registered as a camera that every application sees — not a
DirectShow-only filter, not something each app needs to opt into. Teams, Zoom,
OBS and the Camera app all just see a camera.

The only driver binary in the stack is `WinUSB.sys`, which ships with Windows
and is already Microsoft-signed. `qcamusb.inf` contains no code; it names the
devices and hands them to WinUSB.

## Process layout

```
┌─────────────────────────────┐   ┌──────────────────────────────┐
│ qcamsvc.exe                 │   │ Frame Server (LOCAL SERVICE) │
│ NT SERVICE\qcamsvc          │   │                              │
│                             │   │  ┌────────────────────────┐  │
│  Camera                     │   │  │ qcamvcam.dll           │  │
│   ├ WinUsbTransport ────────┼───┼─▶│  QcamMediaSource       │  │
│   ├ Stv06xxBridge           │   │  │  QcamMediaStream       │  │
│   ├ HdcsSensor              │   │  └───────────▲────────────┘  │
│   ├ ChunkFramer             │   │              │               │
│   ├ Decoder                 │   └──────────────┼───────────────┘
│   └ AutoExposure            │                  │
│           │                 │                  │
│           ▼                 │                  │
│   FrameRingWriter ──────────┼──▶ shared memory ┘
└─────────────────────────────┘     (4 × NV12 slots)
```

### Why a separate service rather than opening the device in the media source

Three reasons, in order of how much they matter.

**WinUSB access is exclusive.** Only one process can hold the interface. If the
media source opened the device directly, then `qcamctl` could not run at the
same time, and two applications opening the camera at once would fight over
it.

**The isochronous pipeline should not churn.** Alternate-setting switches
and bandwidth reservation are not free, and this camera takes about 250 ms
after enumeration before it will answer I2C at all. The service opens the
camera when the first reader asks and keeps it open until readers have been
gone for ten seconds. An app restarting its pipeline, or a second app joining,
reuses the running stream rather than re-running sensor init.

It does *not* keep the camera open all the time. An open camera is a live
feed into shared memory, and streaming only on demand means a plugged-in,
unused camera is actually off. The cost is a second or two of grey frames when
an app first opens it.

**The Frame Server is not a good place to own hardware.** It loads the source
into a shared, sandboxed service process. Keeping USB ownership, sensor
control and a control loop out of there means a failure in any of them cannot
take down the Frame Server.

### Why shared memory rather than a pipe

The frames are ~150 KB at 7.5 fps. That is trivial either way; the reason is
latency behaviour under contention, not throughput. A reader that stalls must
never be able to block the writer, because the writer is servicing a live
isochronous pipeline where a late frame is a lost frame.

Each slot is protected by a **seqlock** rather than a mutex:

```
writer:   version += 1  (now odd — "in flux")
          write payload
          version += 1  (now even — "stable")

reader:   read version  (must be even)
          copy payload
          re-read version  → unchanged means the copy is coherent
```

The writer never waits. A reader that loses the race retries, and by then the
writer has moved on to a newer frame — which is what a live capture wants
anyway. Readers always take the newest frame rather than draining a backlog.

The shared objects carry explicit security descriptors. The Frame Server runs
as **LOCAL SERVICE**, and a default DACL would grant access only to SYSTEM and
Administrators — the camera would enumerate and then never produce a frame.
Sections grant `GENERIC_READ`; events additionally need `SYNCHRONIZE`, which
`GENERIC_READ` does not include.

## The core library

`src/core/` has no Windows dependency at all. It talks to hardware through one
interface:

```cpp
class IUsbTransport {
    virtual Status ControlOut(...) = 0;
    virtual Status ControlIn(...) = 0;
    virtual Status SetAltSetting(uint8_t alt) = 0;
    virtual Status GetIsoMaxPacketSize(uint8_t alt, uint16_t* max) = 0;
    virtual Status StartIso(IIsoSink* sink) = 0;
    virtual void   StopIso() = 0;
};
```

Two implementations: `WinUsbTransport`, and `MockTransport`, which records
every control transfer, emulates the bridge's I2C read window well enough for
sensor probing to succeed, and replays canned isochronous packets.

That seam is why 96 tests run on a machine with no camera and no Windows, and
why `qcamctl selftest` can exercise the whole stack end to end. The parts of a
device driver that are usually hardest to test — register sequencing, framing,
error recovery — are the parts under test here.

## The imaging pipeline

```
Bayer GRBG 8-bit, 360×296
        │
        ├──▶ grey-world white balance estimator (updates gains, slewed)
        │
        ├──▶ auto-exposure metering (green sites, centre-weighted)
        │        └──▶ sensor exposure/gain registers
        │
        ▼
    demosaic  (bilinear by default, Malvar-He-Cutler optional)
        │
        ▼
    channel gains → tone curve (gamma, contrast, brightness) → saturation
        │
        ▼
    crop or scale to the output size
        │
        ▼
    NV12 / BGRA / YUY2
```

A few decisions worth spelling out.

**Metering on green sites only.** Green carries most of the luminance and there
are twice as many green sites, so metering the mosaic directly is both cheaper
and less noisy than demosaicing first. Metering is centre-weighted — the outer
eighth is ignored — so a bright window behind the subject does not drive the
whole picture dark.

**Exposure before gain.** The controller spends exposure headroom first and
only reaches for analogue gain at the rail, because gain on this sensor is
visibly noisy above about 120. Coming back down it unwinds gain first, for the
same reason.

**The step is clamped in the multiplicative domain.** Without a clamp, a
pitch-black frame produces an unbounded correction factor and the loop slams
to both rails in two frames, then crawls back down — the ratio is unbounded
upward but bounded below by 1, so the loop is asymmetric and hunts. Clamping
to ×2/÷2 per frame makes correcting a two-stop error take the same number of
frames in either direction.

**White balance holds still on a dark frame.** With nothing to measure, a
grey-world estimator will happily chase sensor noise into a magenta cast.

## Output geometry

Native is 360 × 296. The service publishes exactly that, every sensor pixel
and nothing resampled, and the virtual camera offers it to apps as the
default media type.

Not every application accepts a size that unusual, so the virtual camera also
offers **640 × 480** and **320 × 240**. For those it centre-crops the native
frame to 4:3 and scales it bilinearly, per stream, in the Frame Server
process. That adds no detail, but an app that insists on VGA still opens the
camera instead of failing.

## Frame rate honesty

The camera sustains about 7.9 fps and the driver advertises that, as a
fraction, rather than claiming 15 or 30. Media Foundation is perfectly happy
with a fractional rate.

It only sustains that while the exposure fits inside one frame period. On
the HDCS-1000 that is an exposure setting of 128 or less; beyond that every
step lengthens the frame, down to about 3 fps at the maximum. The service
therefore caps auto-exposure at 128 and makes up the rest with gain. In a dim
room the picture is noisier than it could be, but motion stays smooth, and
the advertised rate stays true.

## Picture controls

Brightness, contrast, saturation and gamma arrive at the virtual camera as
the standard VideoProcAmp properties apps' settings panels use. The virtual
camera writes them into a small shared block (`Global\qcam.controls.*`),
which is the one object the Frame Server may write. qcamsvc checks it on
every frame and hands changes to the decoder at the next frame boundary. The
block lives as long as the service, so a setting outlasts the camera being
closed and reopened between apps.

The alternative — advertising 30 fps and repeating each frame four times —
makes the camera look better in a device-properties dialog and worse
everywhere else: encoders allocate bitrate for motion that is not there, and
frame-drop diagnostics stop meaning anything. If a specific application
refuses a sub-15-fps source, repeating frames is the right fix *at that
boundary*, in the virtual camera, not a lie told by the capture layer.

## Threading

| Thread | Owns | Notes |
| --- | --- | --- |
| WinUSB reader | isochronous transfers | Reaps transfers round-robin, preserving packet order; calls into the framer |
| Framer/decoder | runs on the reader thread | Decode is ~1 ms per frame at this resolution; a handoff would cost more than it saves |
| Service main | lifecycle | Waits on a stop event, reconnects the device |
| vcam worker | sample delivery | Pairs each `RequestSample` with the next ring frame |

`Camera` uses three locks with a documented order: `mutex_` (lifecycle) →
`ctrl_mutex_` (register access and the AE state) → `settings_mutex_` /
`stats_mutex_`.

The rule that matters: **no lock is ever held across
`IUsbTransport::StopIso()`**, because that joins the reader thread, and the
reader thread takes `ctrl_mutex_` when auto-exposure writes to the sensor.
Holding a lock across the join deadlocks. `Camera::Stop()` therefore joins
first, then takes its locks for teardown; once the join returns, no callback
can be in flight.

Colour settings are staged rather than applied under a lock, so a caller
changing gamma never contends with the decode path: the setter drops a copy
behind an atomic flag and the streaming thread picks it up at a frame
boundary.
