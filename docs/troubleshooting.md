# Troubleshooting

The stack has four layers, and almost every problem is "which layer stopped".
`qcamctl` exists to answer that in order. Work down this list; each step
assumes the one before it passed.

## 1. Is the device bound to WinUSB?

```powershell
qcamctl list
```

**Nothing listed.** Open Device Manager and look for the camera.

| What you see | What it means |
| --- | --- |
| Nothing anywhere, even with *View → Show hidden devices* | Not enumerating. Try another port, preferably one directly on the machine rather than through a hub. The camera draws 100 mA; an unpowered hub with other devices on it can starve it. |
| **Other devices → Unknown USB Device**, hardware id `USB\VID_046D&PID_0840` | Enumerating fine, but no driver is bound. The INF did not install — see below. |
| Under **Universal Serial Bus devices** as *"Logitech QuickCam Express (qcam)"* | Bound correctly. The problem is elsewhere; go to step 2. |
| A different PID than `0840` | You have a different QuickCam variant. `qcamctl list` prints the table of known ids. If the PID is one of the listed ones, the INF should have bound it; if the sensor is not HDCS, probing will say so at step 2. |

**The INF will not install.** Almost always signing. Check:

```powershell
pnputil /enum-drivers | Select-String -Context 3 qcam
Get-Content C:\Windows\INF\setupapi.dev.log -Tail 80
```

`setupapi.dev.log` names the actual reason. A signature complaint usually
means the INF was installed from somewhere without its catalog — install the
signed copy in `C:\Program Files\qcam\driver\`, or re-run `install.ps1`, which
re-signs it. See `docs/installing.md`. Test-signing mode is not needed and
will not help.

## 2. Does the sensor answer?

```powershell
qcamctl probe
```

Expected:

```
camera opened.
  device      : 046d:0840  Logitech QuickCam Express (qcam)
  sensor      : Agilent HDCS-1000/1100
  native      : 360x296 Bayer GRBG
  output      : 360x296 NV12
  nominal fps : 7.91
```

| Failure | Cause |
| --- | --- |
| `Busy` | Something else has the device. Usually `qcamsvc` — `Stop-Service qcamsvc` and retry. WinUSB access is exclusive. |
| `NoDevice` after "no supported sensor answered" | The bridge is talking but the sensor is not one this driver implements. Run `qcamctl regdump` and look at sensor register `0x00`: `0x08` is HDCS-1000/1100, `0x10` is HDCS-1020, anything else is a Photobit PB-0100 or ST VV6410, which need a sensor back end writing. |
| `Io` on every transfer | The control endpoint is not responding. Replug, try another port. If it persists, the camera may genuinely be dead — these are 25 years old and the cable strain relief is usually the first thing to fail. |
| `NoDevice` immediately | It was unplugged between `list` and `probe`. |

## 3. Do frames arrive?

```powershell
qcamctl stream -t 10
```

The counters tell you where it stopped.

**`iso packets` is 0.** The isochronous transfer never started. Either the
alternate setting did not take, or the host controller refused the bandwidth
reservation. Free some bus bandwidth — unplug other USB devices, especially
audio interfaces and other cameras, which reserve isochronous bandwidth
whether or not they are streaming. Then:

```powershell
qcamctl stream --packet-size 600 -t 10
```

A smaller packet needs less reserved bandwidth. It works out to a lower frame
rate, which is the trade.

**Packets arrive but `chunks` is 0, or nearly all chunks are unknown.** The
chunk layer is not what the driver expects. This is the interesting failure:
it means the bridge is producing a framing this driver does not parse. Capture
the traffic and look at it:

- Install [USBPcap](https://desowin.org/usbpcap/) and capture while running
  `qcamctl stream`.
- Open it in Wireshark and look at the isochronous IN payloads.
- Each chunk should start with a 4-byte header: two bytes of big-endian id,
  two of big-endian length. `docs/protocol.md` lists the ids.

If the ids are different, that is genuinely new information about this
hardware revision. `src/core/framer.cpp` is where it goes, and
`tests/test_framer.cpp` shows how to write a test for it without the camera.

**Frames arrive but `short` is high.** The camera is producing less data per
frame than the geometry expects. Usually bandwidth: the host is dropping
packets. Same fix as above — free bus bandwidth or lower the packet size.

**`overrun` is high.** More data per frame than expected, which means the
packet size written to the ASIC does not match what the endpoint actually
granted. The driver reads the negotiated size back and should handle this;
if it is happening, `qcamctl stream -v` will show both numbers.

## 4. Do frames look right?

```powershell
qcamctl capture -n 3
```

Three BMPs in the current directory.

**Black or nearly black.** Auto-exposure has not converged, or the sensor is
not actually running. `qcamctl stream` shows the `luma`, `exp` and `gain`
columns — if exposure and gain are climbing to their limits and luma stays at
zero, no light is reaching the sensor, or the sensor is halted. Check the
`CONFIG` register in `qcamctl regdump`: bit 3 must be set for continuous
capture, or the sensor stops after one frame.

**Washed out / pure white.** Too much exposure. Try manual:

```powershell
qcamctl capture -n 1 --exposure 20 --gain 20
```

**Recognisable but the colours are wrong.** The Bayer phase is probably right
(this is well established for the STV0600) but check by capturing the raw
mosaic:

```powershell
qcamctl capture -n 1 -f raw
```

That writes the 360×296 8-bit Bayer mosaic. Open it in a raw viewer, or in
GIMP as "Raw image data", 360×296, 8-bit greyscale. You should see the
characteristic Bayer checkerboard. If the image is recognisable but colour
assignment is wrong, the phase is off by a pixel — the enum in
`include/qcam/types.h` has all four, and `Decoder::Configure` is where it is
set.

**A green or magenta cast that does not settle.** Auto white balance chasing
something. `--no-awb` disables it, which is the right first test.

**Diagonal tearing or skew.** The frame is being assembled with a byte offset,
which means the chunk layer is losing or gaining bytes. Check the `short` and
`overrun` counters in `qcamctl stream`.

## 5. Does it appear in applications?

```powershell
Get-Service qcamsvc
qcamctl attach -t 5
```

`attach` reads from the service's shared-memory ring, which is exactly what the
virtual camera does, and asks the service to open the camera the same way an
app does. Run it from an **elevated** prompt: apart from administrators, only
the Windows Frame Server may read frames. If `attach` sees frames but
applications do not, the problem is the Media Foundation side.

| Symptom | Cause |
| --- | --- |
| `attach` says the ring is not present | The service is not running, or it is running but could not open the camera. `qcamsvc --console -v` shows why. |
| `attach` fails with `Busy` | The prompt is not elevated. |
| The camera light never comes on / the service log says it is waiting | Expected when nothing is reading: the camera only streams while an app, or `attach`, asks for frames. |
| The camera is not in any app's list | The COM server is not registered, or the DLL moved. Re-run `regsvr32 qcamvcam.dll` from where it now lives. Check `HKLM\Software\Classes\CLSID\{9BB2B860-94A0-4B47-ADF7-7F3FBCA2FB6E}\InprocServer32`. |
| The camera is listed but shows a grey picture | The virtual camera is running and the ring has no frames. That grey frame is deliberate — it keeps conferencing apps from erroring out at open time. Check the service. |
| The camera is listed but fails to open | The Frame Server could not load the DLL. Put it somewhere readable by `LOCAL SERVICE`; a per-user directory will not work. |
| It worked, then stopped after a rebuild | The registration records the DLL's path. Re-register. |
| Windows 10 | Virtual cameras need Windows 11 build 22000. See `docs/installing.md`. |

To watch the Frame Server load the source, use DebugView from Sysinternals with
*Capture Global Win32* enabled — the DLL logs through `OutputDebugString`.

## Reading the logs

| Component | Where |
| --- | --- |
| `qcamctl` | stderr; `-v` for everything including per-transfer tracing |
| `qcamsvc` as a service | `OutputDebugString` — use DebugView with *Capture Global Win32* |
| `qcamsvc --console` | stderr |
| `qcamvcam.dll` | `OutputDebugString`, from inside the Frame Server process |

`-v` on `qcamctl` logs every control transfer, which is the fastest way to see
exactly what reached the hardware before it went wrong.

## When the camera is just broken

These are 25-year-old consumer devices. Before spending long on software:

- The cable strain relief at the camera end fails first; wiggle it while
  watching `qcamctl list` in a loop.
- The CMOS sensor degrades. Heavy hot-pixel noise or a strong colour cast that
  no amount of correction fixes is usually the sensor, not the driver.
- Check it enumerates at all: `pnputil /enum-devices /connected` should list
  something at `VID_046D` even with no driver bound. If nothing appears on any
  port, no driver will help.
