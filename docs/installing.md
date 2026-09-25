# Installing

Five pieces go in, in this order:

1. The binaries are copied to `C:\Program Files\qcam\`.
2. `qcamusb.inf` — binds the camera to the inbox WinUSB driver.
3. `qcamvcam.dll` — registered as a COM server.
4. The virtual camera registration (`qcamsvc --register-vcam`), done once.
5. `qcamsvc.exe` — installed and started as a service.

`scripts\install.ps1` does all five. Run it from an elevated PowerShell:

```powershell
.\scripts\install.ps1 -BinDir .\build\RelWithDebInfo
```

`-BinDir` is only where the files are copied *from*. Nothing runs from the
build directory after install, and that is deliberate: the service and the
Frame Server load these files, and a build tree under your user profile is
writable by any program you run. Program Files is writable only by
administrators.

No step here changes a Windows security setting: no test-signing mode, and
Secure Boot stays on.

## Driver signing

`qcamusb.inf` contains no code, but Windows still requires the **driver
package** to be signed by a certificate the machine trusts before it will
install it. `install.ps1` handles this with `scripts\sign-driver.ps1`, the
same way Zadig does for WinUSB bindings:

1. `makecat.exe` (Windows SDK) builds `qcamusb.cat`, which records the INF's
   hash.
2. A fresh self-signed certificate is created. It can sign code and nothing
   else, and it cannot act as a certificate authority.
3. The catalog is signed with it and the public half is exported as
   `qcamusb.cer`.
4. **The certificate and its private key are deleted.**
5. `install.ps1` adds `qcamusb.cer` to the machine's `Trusted Root
   Certification Authorities` and `Trusted Publishers`, then installs the
   package.

Trusting a self-made certificate is normally a real risk: anyone holding its
key could sign software your machine would then trust. Step 4 is what makes it
safe here — by the time the certificate is trusted, the key no longer exists
anywhere, so the only thing it will ever vouch for is this one catalog.
`install.ps1` also refuses to trust any `.cer` that did not sign the catalog,
that is a CA, or that is not code-signing-only.

### Why test signing is not needed

Test-signing mode relaxes **kernel** code signing, the rule about which
`.sys` files may load. This package loads no `.sys` of its own: WinUSB.sys
ships with Windows and Microsoft signed it. The package signature is a
separate, user-mode check, and a locally trusted certificate satisfies it.

### Distribution: attestation signing

To give this to other people without asking them to trust a local certificate,
the package needs attestation signing through the Microsoft **Partner Center**
hardware dashboard. That needs:

- A Partner Center hardware account.
- An **EV code-signing certificate** (a hardware token from a CA; this is the
  expensive part, typically a few hundred dollars a year).
- Uploading the package as a `.cab`; Microsoft signs it and returns it.

Attestation signing is available for INF-only packages like this one and does
**not** require WHQL testing — which is one of the practical benefits of the
no-kernel-code design.

## Installing the driver package manually

`install.ps1` leaves the signed package in `C:\Program Files\qcam\driver\`.
To reinstall just the binding from there:

```powershell
pnputil /add-driver "C:\Program Files\qcam\driver\qcamusb.inf" /install
```

Installing `driver\qcamusb.inf` straight from the repository will fail: it has
no catalog next to it, so Windows treats it as unsigned.

If the camera is already plugged in and sitting as an unknown device, force a
rescan or unplug and replug it. Confirm with:

```powershell
pnputil /enum-devices /class USBDevice
```

It should appear as *"Logitech QuickCam Express (qcam)"* under **Universal
Serial Bus devices** in Device Manager. If it is still under **Other devices**
with a yellow mark, the INF did not take — see `docs/troubleshooting.md`.

## Registering the virtual camera

```powershell
regsvr32 "C:\Program Files\qcam\qcamvcam.dll"
& "C:\Program Files\qcam\qcamsvc.exe" --register-vcam
```

`regsvr32` writes the COM registration under
`HKLM\Software\Classes\CLSID\{9BB2B860-94A0-4B47-ADF7-7F3FBCA2FB6E}`. The DLL
must stay where it is — the registration records its path.

**The DLL is loaded by the Windows Frame Server, not by the application.** Put
it somewhere readable by `LOCAL SERVICE` and writable only by administrators.
A per-user directory will not work, and a user-writable one is a privilege
escalation waiting to happen. `C:\Program Files\qcam\` is the right place.

`--register-vcam` needs administrator rights. It registers a System-lifetime
camera that persists across reboots until `--remove-vcam`, so the camera
appears in application pickers even before the hardware is plugged in — it
produces blank frames until then.

## Installing the service

```powershell
& "C:\Program Files\qcam\qcamsvc.exe" --install
Start-Service qcamsvc
```

It starts automatically and runs as `NT SERVICE\qcamsvc` — a per-service
identity under the `LOCAL SERVICE` account, with every privilege except
`SeCreateGlobalPrivilege` stripped from its token. It does not run as
LocalSystem and has no administrator rights, which is why the virtual camera
is registered by the installer instead of by the service.

### Who can see the frames

The frame ring is a live camera feed in shared memory. Windows' camera
privacy settings and in-use indicator are enforced at the Frame Server, so
anything that could read the ring directly would get around both. The ring's
DACL therefore allows:

| Who | Access |
| --- | --- |
| `NT SERVICE\qcamsvc` | write — the service |
| `NT SERVICE\FrameServer` | read — hosts `qcamvcam.dll` |
| SYSTEM, Administrators (elevated) | full — for `qcamctl attach` |

Both services run as `LOCAL SERVICE`, which is why the grants name each
service's own SID: granting the account would let the Frame Server write
frames, and let every other `LOCAL SERVICE` process watch the camera.

### Streaming on demand

The service opens the camera only while something is reading frames. Each
time the virtual camera or `qcamctl attach` opens the ring or waits for a
frame, it signals the service's demand event
(`Global\qcam.demand.4EA75BBB`, same DACL). On the first signal the service
opens the camera and starts publishing. When no signal has arrived for ten
seconds, it closes the camera again. An app therefore sees a second or two of
grey frames at start while the sensor initialises.

To run it in the foreground instead, which is how you debug it:

```powershell
Stop-Service qcamsvc
& "C:\Program Files\qcam\qcamsvc.exe" --console -v
```

Useful options:

```
--size WxH        published frame size (default: sensor native, 360x296)
--vcam            console mode: register a temporary camera until Ctrl-C
--always-on       console mode: stream whenever the camera is plugged in
--name "TEXT"     friendly name shown in app camera pickers
```

If you change `--size`, the virtual camera adopts the live format when it is
instantiated. Restart the Frame Server (or reboot) after changing it, or
applications that already have the source loaded will keep the old format.

## Verifying

```powershell
qcamctl list               # is the device bound to WinUSB?
qcamctl probe              # does the sensor answer?
qcamctl capture -n 3       # do frames arrive and decode?
qcamctl stream -t 10       # frame rate and error counters
```

Then open the Camera app. *"Logitech QuickCam Express (qcam)"* should be in the
camera list.

## Uninstalling

```powershell
.\scripts\uninstall.ps1
```

That is the whole of it. It removes the service, the virtual camera, the COM
registration, `C:\Program Files\qcam\`, the driver package and the signing
certificate. The installer never changes a Windows setting, so there is
nothing else to put back.

Pass `-KeepDriver` to leave the WinUSB binding and its certificate in place,
which is what you want when reinstalling a rebuilt binary with
`install.ps1 -SkipDriver`.

An install made by an older version of the script ran straight from the build
directory; remove that one with `-BinDir .\build\RelWithDebInfo`. If that
older install also had you turn on test signing, turn it back off with
`bcdedit /set testsigning off` and re-enable Secure Boot; nothing needs it now.

## Moving to another computer

Nothing is tied to the machine it was built on. To set up a second PC:

1. **Copy the files, or build there.** Put `qcamctl.exe`, `qcamsvc.exe`,
   `qcamvcam.dll` and `qcamusb.inf` from the build directory in one folder,
   and bring the `scripts\` folder too. Match the architecture: an x64 build
   for an Intel or AMD PC, ARM64 for a Snapdragon one. CI also builds both on
   every push; the files are under the run's *Artifacts* on GitHub.
2. **Install the Visual C++ runtime** if the machine has never had it: the
   binaries link the MSVC runtime dynamically. Get `vc_redist.x64.exe` (or
   `.arm64`) from Microsoft.
3. **Run the installer**, elevated:
   `.\scripts\install.ps1 -BinDir <folder>`.

Signing happens on the new machine with its own single-use certificate, which
needs `makecat.exe` from the Windows SDK. If that machine has no SDK, copy
`qcamusb.inf`, `qcamusb.cat` and `qcamusb.cer` from
`C:\Program Files\qcam\driver\` on a machine where it is installed into the
folder from step 1; the installer uses them when it cannot sign locally. That
certificate's key was destroyed when it was made, so it is as safe to trust on
the second machine as on the first.
## Windows 10

The driver, the service and `qcamctl` all work. The system-wide camera does
not: `MFCreateVirtualCamera` is Windows 11 build 22000 and later.
`install.ps1` detects this and skips that step.

Making the camera visible to applications on Windows 10 needs a DirectShow
source filter, which is not implemented. The frame ring is the integration
point for one — it is a documented shared-memory format, and a DirectShow
filter would read it exactly the way `qcamvcam` does.
