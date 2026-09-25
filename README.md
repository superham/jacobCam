# qcam — a modern Windows driver for the Logitech QuickCam Express

A user-mode Windows 10/11 driver for a 1999 USB webcam, so it shows up as an
ordinary camera in Teams, Zoom, OBS and the Windows Camera app.

The camera this was written for, identified from the label:

| Field | Value |
| --- | --- |
| Model | **V-UB2** — Logitech QuickCam Express |
| Part number | **861037-0000** |
| USB ID | `046D:0840` |
| Bridge ASIC | STMicroelectronics **STV0600** |
| Image sensor | Agilent/HP **HDCS-1000**, 1/4" CMOS |
| Native output | 360×296, 8-bit Bayer GRBG |
| Frame rate | ~7.9 fps (USB full speed, bandwidth-limited) |
| Bus power | 5 V, 100 mA |

The part number is decisive: `861037` appears verbatim in the STV06xx family's
published hardware table as *"Sensor HDCS1000, ASIC STV0600"*. There is no
guesswork about what is inside this particular camera.

## Using it with the camera

Read this first. The Windows half of this stack has never been compiled or run
against hardware — see [State of the code](#state-of-the-code) — so bring it up
in the order below and verify each layer before adding the next. Every step
tells you what "working" looks like, so when something breaks you know which
layer broke.

Budget an hour for the first run, most of it in steps 2 and 3.

### What you need

- The camera, and a USB port **directly on the machine** — not a hub. It draws
  100 mA and an unpowered hub with anything else on it can starve it.
- **Windows 11**, build 22000 or later, for the camera to appear in apps.
  Windows 10 works for everything except that last step; `MFCreateVirtualCamera`
  is Win11-only.
- **Visual Studio 2022 or newer** with "Desktop development with C++", and
  the **Windows 11 SDK** (10.0.22000+). Older SDKs lack `mfvirtualcamera.h`.
  No WDK needed to build.
- An elevated PowerShell for steps 3 and 5.

### 1. Check the label matches

Look at the sticker on the cable. You want **`P/N: 861037-0000`**.

That part number — not the model name — is what says an HDCS-1000 sensor sits
behind the STV0600 bridge. Logitech shipped the *same* "QuickCam Express"
with three different sensors over its life. If your part number differs, keep
going anyway: step 4 reads the sensor's identity register and will tell you
exactly what you have. See [`docs/hardware.md`](docs/hardware.md) for the full
part-number table.

### 2. Build, and prove the protocol stack before touching hardware

```powershell
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
```

Leaving the generator out is deliberate: CMake picks the newest Visual Studio
it finds, so this keeps working as you upgrade. Pass
`-G "Visual Studio 17 2022"` only if you need to pin a specific one.

This should build clean — CI compiles x64 and ARM64 on every push — but it
has only ever been built by CI, never by a person. If MSVC does complain,
the portable core is the well-covered part; look in `src/win/`,
`src/qcamsvc/` or `src/qcamvcam/` first.

Once it builds, run the two checks that need no camera:

```powershell
.\build\RelWithDebInfo\qcam_tests.exe      # 96 tests
.\build\RelWithDebInfo\qcamctl.exe selftest
```

Both should pass. This is the useful checkpoint: `selftest` drives the entire
stack — bridge protocol, I2C, sensor init, chunk framing, demosaic — against a
mock device and decodes a synthetic frame. If it passes, everything above the
USB layer is intact, and any remaining problem is Windows plumbing or the
hardware itself. That is a much smaller search space.

> No Windows machine, or the camera isn't to hand? These two also run on Linux
> and macOS: `cmake -S . -B build && cmake --build build && ./build/qcam_tests`.

### 3. Bind the camera to WinUSB

`qcamusb.inf` contains no code — it just hands the device to the inbox WinUSB
driver — but Windows still requires the **package** to be signed before it
will install it. The installer signs it with a single-use certificate whose
private key is destroyed straight afterwards. No test-signing mode, no reboot,
and Secure Boot stays on. [`docs/installing.md`](docs/installing.md) explains
why that is safe.

```powershell
.\scripts\install.ps1 -BinDir .\build\RelWithDebInfo -DriverOnly   # elevated
```

`-DriverOnly` installs just the binding and not the service, which would take
the camera for itself before step 4 can use it.

**Working looks like:** in Device Manager, under **Universal Serial Bus
devices**, an entry named *"Logitech QuickCam Express (qcam)"*.

If it is still under **Other devices** with a yellow mark, the INF did not
take. `Get-Content C:\Windows\INF\setupapi.dev.log -Tail 80` names the actual
reason, and it is almost always the signature.

### 4. Bring it up layer by layer with `qcamctl`

**Do not install the service yet.** WinUSB access is exclusive — the service
would take the device and `qcamctl` could not open it. Drive the camera
directly first.

```powershell
cd build\RelWithDebInfo
```

**Is the device bound?**

```powershell
.\qcamctl.exe list
```
```
1 camera(s):
  046d:0840  Logitech QuickCam Express (qcam)   STV0600
             \\?\usb#vid_046d&pid_0840#5&1b2c3d4e&0&2#{b17cb711-...}
```

Empty means step 3 did not finish. The command prints the table of device ids
it knows, so you can compare.

**Does the sensor answer?**

```powershell
.\qcamctl.exe probe
```
```
camera opened.
  device      : 046d:0840  Logitech QuickCam Express (qcam)
  sensor      : Agilent HDCS-1000/1100
  native      : 360x296 Bayer GRBG
  output      : 360x296 NV12
  nominal fps : 7.91
```

This is the first real proof of life: the bridge accepted control transfers,
the I2C master worked, and the sensor's identity register read back `0x08`.

If it says *no supported sensor answered*, run `.\qcamctl.exe regdump` and look
at sensor register `0x00` — `0x08` is HDCS-1000/1100, `0x10` is HDCS-1020, and
anything else means a Photobit PB-0100 or ST VV6410 that needs a sensor back
end writing against the `ISensor` interface.

**Do frames arrive?**

```powershell
.\qcamctl.exe stream -t 10
```
```
      8 frames   7.84 fps  luma 112  exp  61  gain  50  short 0  overrun 0  unknown 0
     16 frames   7.91 fps  luma 118  exp  58  gain  50  short 0  overrun 0  unknown 0
```

What to read here:

| Column | What it means |
| --- | --- |
| `fps` | Should settle near **7.9**. Much lower means packets are being dropped. |
| `luma` | Metered brightness. Should converge toward ~118. |
| `exp` / `gain` | Auto-exposure working. They should move, then settle. |
| `short` | Frames that ended early — bandwidth trouble. Should be 0 or near it. |
| `overrun` | Packet size mismatch. Should be 0. |
| `unknown` | **The important one.** Anything but 0 means the chunk framing is not what this driver expects. |

If `iso packets` is 0 in the summary, the transfer never started — free some
USB bandwidth (unplug other cameras and audio interfaces, which reserve
isochronous bandwidth whether or not they are streaming) or try
`--packet-size 600`, which trades frame rate for a smaller reservation.

**Do frames look right?**

```powershell
.\qcamctl.exe capture -n 3
```

Three BMPs in the current directory. Auto-exposure gets ~8 frames to settle
before the first is written.

If the picture is black or blown out, pin the controls manually to separate an
exposure problem from a plumbing problem:

```powershell
.\qcamctl.exe capture -n 1 --exposure 60 --gain 40 --no-awb
```

If the colours are wrong but the shapes are right, dump the mosaic straight off
the sensor and look at it yourself — this bypasses the entire colour pipeline:

```powershell
.\qcamctl.exe capture -n 1 -f raw
```

That writes a 360×296 8-bit Bayer file. Open it in GIMP as "Raw image data",
360×296, 8-bit greyscale; you should see the Bayer checkerboard.

Other flags worth knowing: `--malvar` for a better demosaic, `--size WxH` to
crop or scale, and `-v` on any command to log every single USB control
transfer — which is the fastest way to see exactly what reached the hardware
before things went wrong.

### 5. Install the service and the virtual camera

Only once step 4 produces good frames.

```powershell
.\scripts\install.ps1 -BinDir .\build\RelWithDebInfo
```

That copies the binaries to `C:\Program Files\qcam\`, registers the COM media
source and the virtual camera, then installs `qcamsvc` and starts it. The
service runs as a low-privilege `LOCAL SERVICE` identity, not LocalSystem.
From here on everything runs from Program Files, not from your build
directory, so rebuilding does nothing until you re-run the script.

The service **only opens the camera while an app is using it**. When an app
starts reading frames, the service opens the camera, which takes a second or
two, so the first frames are grey. Ten seconds after the last app stops, it
closes the camera again. Plugged in and idle, the camera is not streaming
anywhere.

While an app has the camera, the service owns it **exclusively**, so
`qcamctl probe`, `capture` and `stream` will report `Busy`. That is deliberate,
not a limitation: opening the device runs the sensor init sequence, and a
second process doing that to a live stream would corrupt it. Stop the service
(`Stop-Service qcamsvc`) when you want to drive the hardware directly again.

Only the Windows camera service may read the service's frames, so Windows'
camera privacy settings and in-use indicator cover this camera the same as
any other. To look at them yourself, use `attach` from an **elevated**
prompt. It reads the service's shared-memory ring the way the virtual camera
does, and asks the service to start the camera just as an app would:

```powershell
.\qcamctl.exe attach -t 5
```

**Working looks like:** open the Windows Camera app, and *"Logitech QuickCam
Express (qcam)"* is in the camera list. Then try Teams, Zoom or OBS.

To watch the service work, stop it and run it in the foreground:

```powershell
Stop-Service qcamsvc
& "C:\Program Files\qcam\qcamsvc.exe" --console -v
```

To undo everything: `.\scripts\uninstall.ps1`. It removes every piece,
including the driver and its certificate, and there are no Windows settings
to put back.

To set it up on another computer, see
[Moving to another computer](docs/installing.md#moving-to-another-computer).

### If it doesn't work

[`docs/troubleshooting.md`](docs/troubleshooting.md) walks the same four layers
with a failure table for each.

The one failure worth reporting in detail is **`unknown` chunks in step 4**.
The framing rules came from the documented STV06xx protocol, not from a capture
of your specific camera, so a nonzero count there is genuinely new information
about this hardware revision. Capture it with
[USBPcap](https://desowin.org/usbpcap/) while `qcamctl stream` runs, and look at
the isochronous IN payloads in Wireshark: each chunk should begin with a 4-byte
header of big-endian id then big-endian length.
[`docs/protocol.md`](docs/protocol.md) lists the ids this driver knows.
`src/core/framer.cpp` is where a new one goes, and `tests/test_framer.cpp`
shows how to write a test for it without the camera attached.

## Why the camera doesn't work today

It predates USB Video Class by about four years. UVC arrived in 2003 and is
what makes a modern webcam driverless; a 1999 camera speaks a vendor-specific
protocol that Windows has no idea about. The original Logitech driver was a
32-bit Windows 98/2000-era kernel driver and will not install — or load — on a
modern 64-bit Windows.

## How this works instead

```
   ┌──────────────────────────────────────────────────────────┐
   │  Teams / Zoom / OBS / Camera app                         │
   └───────────────────────▲──────────────────────────────────┘
                           │  Media Foundation / DirectShow bridge
   ┌───────────────────────┴──────────────────────────────────┐
   │  qcamvcam.dll — MF virtual camera source                 │
   │  loaded by the Windows Frame Server                      │
   └───────────────────────▲──────────────────────────────────┘
                           │  shared-memory frame ring (NV12)
   ┌───────────────────────┴──────────────────────────────────┐
   │  qcamsvc.exe — frame broker (Windows service)            │
   │  owns the device · STV0600 + HDCS-1000 init              │
   │  iso reassembly · demosaic · auto-exposure               │
   └───────────────────────▲──────────────────────────────────┘
                           │  WinUSB API (control + isochronous)
   ┌───────────────────────┴──────────────────────────────────┐
   │  WinUSB.sys — inbox, already Microsoft-signed            │
   │  bound by qcamusb.inf                                    │
   └───────────────────────▲──────────────────────────────────┘
                           │  USB 1.1 full speed
                    [ QuickCam Express ]
```

**There is no custom kernel-mode code.** The only driver binary involved is
`WinUSB.sys`, which ships with Windows. Everything specific to this camera —
the bridge protocol, the sensor register sequences, framing, demosaic,
exposure control — runs in user mode. That means no WHQL submission, no EV
certificate for a kernel driver, no possibility of bugchecking the machine,
and a debugging loop that is just a console tool.

This is only possible because two things landed in Windows after this camera
was discontinued: **user-mode isochronous transfers in WinUSB** (Windows 8.1)
and the **`MFCreateVirtualCamera` API** (Windows 11 build 22000), which lets a
user-mode media source appear to every app as a real camera.

## Components

| Path | What it is |
| --- | --- |
| `driver/qcamusb.inf` | Binds `046D:0840` (and the rest of the STV06xx family) to inbox WinUSB |
| `src/core/` | Portable protocol, framing, demosaic, auto-exposure. No Windows dependency |
| `src/win/` | WinUSB transport, device enumeration, shared-memory ring, vcam registration |
| `src/qcamsvc/` | The frame broker service |
| `src/qcamvcam/` | Media Foundation virtual camera source (COM in-proc server) |
| `src/qcamctl/` | Diagnostics and capture CLI |
| `tests/` | 96 unit tests, runnable with no hardware attached |

## State of the code

Honest summary, because it matters for what you do next.

**Verified by CI.** Every push builds and tests on Linux (gcc and clang) and
builds on Windows for both x64 and ARM64, with MSVC 14.51 (Visual Studio 18,
Windows SDK 10.0.26100). The 96 unit tests and the end-to-end
`qcamctl selftest` run on Linux *and* natively on Windows. The Linux job also
runs everything under AddressSanitizer and UndefinedBehaviorSanitizer.

That covers bridge register encoding, the I2C staging format, HDCS-1000
probe/init/window/exposure/gain sequences, the isochronous chunk framer,
demosaic, colour conversion, and the auto-exposure loop. `qcamctl selftest`
drives the whole stack against a mock transport and decodes a synthetic
frame.

**Compiles, but has never been executed.** The WinUSB transport, the service,
and the Media Foundation virtual camera build cleanly on MSVC, and that is
all that can be said for them: no part of this has been run against the
hardware, and `qcamvcam.dll` has never been loaded by the Frame Server. A
clean compile says the API usage is type-correct, not that the runtime
behaviour is right.

**Known to need checking on real hardware.** The register sequences come from
the documented STV06xx protocol rather than from a capture of *your* camera.
If frames arrive but look wrong, `qcamctl stream` will say whether the chunk
layer is being parsed correctly, and `qcamctl capture -f raw` gives you the
Bayer mosaic to inspect directly. See `docs/troubleshooting.md`.

**Windows 10.** Everything works except the system-wide camera:
`MFCreateVirtualCamera` is Windows 11 only. On Windows 10 the service and
`qcamctl` still capture; making the camera visible to apps there needs a
DirectShow source filter, which is not written yet.

## Documentation

- [`docs/hardware.md`](docs/hardware.md) — the camera, the chipset, how the label decodes
- [`docs/protocol.md`](docs/protocol.md) — the STV0600 wire protocol, in detail
- [`docs/architecture.md`](docs/architecture.md) — why the stack is shaped this way
- [`docs/building.md`](docs/building.md) — toolchain and build options
- [`docs/installing.md`](docs/installing.md) — installation, and the driver signing problem
- [`docs/troubleshooting.md`](docs/troubleshooting.md) — what to do when it doesn't work

## Licence

**GPL-2.0-or-later.** See [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).

This is not an arbitrary choice. The STV0600 register sequences and the chunk
framing were derived from the Linux `gspca/stv06xx` driver, which is
GPL-2.0-or-later; work derived from it inherits that licence. If you need this
code under different terms you would have to re-derive the protocol
independently, from USB captures of the hardware. `NOTICE.md` records exactly
what was derived and from where.
