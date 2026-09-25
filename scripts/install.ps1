#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Installs the qcam driver stack for the Logitech QuickCam Express.

.DESCRIPTION
    Five steps, in order:
      1. Copy the binaries into C:\Program Files\qcam.
      2. Sign qcamusb.inf with a single-use certificate (sign-driver.ps1) and
         install it, which binds the camera to the inbox WinUSB driver.
      3. Register qcamvcam.dll as the COM media source, and register the
         system-wide virtual camera.
      4. Install and start qcamsvc, which owns the device and publishes frames.
      5. Verify the camera is visible.

    Everything runs from the Program Files copy, never from the build
    directory. The service and the Windows Frame Server load these files, so
    they must live somewhere only administrators can change: a build tree in
    your Documents folder is writable by any program you run, and anything
    that swapped a file there would be running inside a Windows service.

    No test-signing mode and no Secure Boot changes are needed. Signing needs
    makecat.exe from the Windows SDK, which building the project already
    installs. See docs/installing.md.

.PARAMETER BinDir
    Directory holding qcamctl.exe, qcamsvc.exe, qcamvcam.dll and qcamusb.inf.
    Defaults to the directory this script lives in. The files are copied from
    here; the directory itself is not used after install.

.PARAMETER SkipDriver
    Skip the INF install (useful when re-running after only rebuilding).

.PARAMETER DriverOnly
    Only sign and install the WinUSB binding. Step 3 of the README bring-up:
    binds the camera so qcamctl can drive it, without installing the service
    that would take exclusive ownership of it.

.PARAMETER NoVirtualCamera
    Do not register the system-wide camera. qcamctl can still capture.

.EXAMPLE
    .\install.ps1 -BinDir ..\build\RelWithDebInfo
#>
[CmdletBinding()]
param(
    [string] $BinDir = $PSScriptRoot,
    [switch] $SkipDriver,
    [switch] $DriverOnly,
    [switch] $NoVirtualCamera
)

if ($DriverOnly -and $SkipDriver) { throw "-DriverOnly and -SkipDriver contradict each other" }

$ErrorActionPreference = 'Stop'

# Fixed rather than a parameter: the point is that it is admin-only, and
# Program Files already is.
$InstallDir = Join-Path $env:ProgramFiles 'qcam'

function Write-Step([string] $Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Assert-File([string] $Path, [string] $What) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$What not found at $Path. Build the project first, or pass -BinDir."
    }
}

$BinDir = (Resolve-Path -LiteralPath $BinDir).Path

Assert-File (Join-Path $BinDir 'qcamsvc.exe') 'qcamsvc.exe'
Assert-File (Join-Path $BinDir 'qcamctl.exe') 'qcamctl.exe'

# ---------------------------------------------------------------------------
Write-Step "Checking Windows version"
$build = [System.Environment]::OSVersion.Version.Build
Write-Host "  Windows build $build"
if ($build -lt 22000) {
    Write-Warning ("Virtual cameras need Windows 11 (build 22000+). " +
                   "The driver and qcamctl will work, but the camera will not " +
                   "appear in Teams/Zoom/OBS/Discord on this build.")
    $NoVirtualCamera = $true
}

# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

$files = @('qcamctl.exe', 'qcamsvc.exe')
if (-not $NoVirtualCamera) { $files += 'qcamvcam.dll' }
if ($DriverOnly) { $files = @() }

if ($files.Count) {
    Write-Step "Copying binaries to $InstallDir"
}

# A running service holds its exe open. Stop it before overwriting.
if ($files.Count -and (Get-Service -Name 'qcamsvc' -ErrorAction SilentlyContinue)) {
    Write-Host "  stopping the running qcamsvc"
    Stop-Service -Name 'qcamsvc' -Force
}

foreach ($name in $files) { Assert-File (Join-Path $BinDir $name) $name }

function Copy-Binaries {
    foreach ($name in $files) {
        $source = Join-Path $BinDir $name
        Copy-Item -LiteralPath $source -Destination $InstallDir -Force
        Write-Host "  $name"
    }
}

try {
    Copy-Binaries
} catch {
    # The Frame Server keeps qcamvcam.dll loaded while any app has the camera
    # open, or has recently. It is demand-started and comes back on its own.
    Write-Host "  a file is in use; stopping the Windows Frame Server and retrying"
    Stop-Service -Name 'FrameServer' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    Copy-Binaries
}

$svc  = Join-Path $InstallDir 'qcamsvc.exe'
$ctl  = Join-Path $InstallDir 'qcamctl.exe'
$vcam = Join-Path $InstallDir 'qcamvcam.dll'

# ---------------------------------------------------------------------------
if (-not $SkipDriver) {
    Write-Step "Signing the WinUSB binding (qcamusb.inf)"

    # Windows wants the driver package signed by a certificate it trusts.
    # sign-driver.ps1 makes a single-use certificate, signs, and destroys the
    # private key, so trusting that certificate cannot let anything else in.
    # No test-signing mode, and Secure Boot stays on. See docs/installing.md.
    #
    # Signed inside Program Files, which only administrators can write, so
    # nothing can swap the INF between it being hashed and being installed.
    $sourceInf = Join-Path $BinDir 'qcamusb.inf'
    Assert-File $sourceInf 'qcamusb.inf'
    $driverDir = Join-Path $InstallDir 'driver'
    $inf = Join-Path $driverDir 'qcamusb.inf'
    $cat = Join-Path $driverDir 'qcamusb.cat'
    $cer = Join-Path $driverDir 'qcamusb.cer'

    try {
        & (Join-Path $PSScriptRoot 'sign-driver.ps1') -InfPath $sourceInf -OutDir $driverDir
    } catch {
        # No Windows SDK on this machine. Fall back to a package signed
        # elsewhere, if one was brought along (see "Moving to another
        # computer" in docs/installing.md).
        $prebuilt = @('qcamusb.inf', 'qcamusb.cat', 'qcamusb.cer') |
            ForEach-Object { Join-Path $BinDir $_ }
        if (@($prebuilt | Where-Object { -not (Test-Path -LiteralPath $_) }).Count) {
            throw ("Could not sign the driver package here ($($_.Exception.Message)) " +
                   "and no pre-signed qcamusb.cat/qcamusb.cer was found in $BinDir.")
        }
        Write-Host "  no Windows SDK here; using the package signed on another machine"
        New-Item -ItemType Directory -Force -Path $driverDir | Out-Null
        Copy-Item -LiteralPath $prebuilt -Destination $driverDir -Force
    }

    # Only trust a certificate that is exactly what sign-driver.ps1 makes:
    # it signed this catalog, it can sign code and nothing else, and it can
    # never vouch for another certificate. Anything else is refused rather
    # than added to the machine's trusted roots.
    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($cer)
    $signature   = Get-AuthenticodeSignature -LiteralPath $cat
    $constraints = $certificate.Extensions |
        Where-Object { $_ -is [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension] }
    $usages      = $certificate.Extensions |
        Where-Object { $_ -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] }
    $ekus        = @($usages | ForEach-Object { $_.EnhancedKeyUsages } | ForEach-Object { $_.Value })
    if (-not $signature.SignerCertificate -or
        $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
        throw "qcamusb.cat is not signed by qcamusb.cer; refusing to trust it"
    }
    if (-not $constraints -or $constraints.CertificateAuthority) {
        throw "qcamusb.cer is (or could be) a certificate authority; refusing to trust it"
    }
    if ($ekus.Count -ne 1 -or $ekus[0] -ne '1.3.6.1.5.5.7.3.3') {
        throw "qcamusb.cer is not a code-signing-only certificate; refusing to trust it"
    }
    if ($certificate.Subject -notmatch 'qcam driver package') {
        throw "qcamusb.cer was not made by sign-driver.ps1; refusing to trust it"
    }

    # Root makes the signature valid; TrustedPublisher makes the install
    # silent instead of popping a "would you like to install" prompt.
    Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
    Write-Host "  trusted certificate $($certificate.Thumbprint)"

    Write-Step "Installing the WinUSB binding"
    # pnputil copies the package into the driver store.
    $output = & pnputil.exe /add-driver $inf /install 2>&1
    $output | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) {
        Write-Warning ("pnputil returned $LASTEXITCODE. The log that names the " +
                       "real reason is C:\Windows\INF\setupapi.dev.log; see " +
                       "docs/troubleshooting.md.")
    }
} else {
    Write-Step "Skipping driver install (-SkipDriver)"
}

if ($DriverOnly) {
    Write-Host ""
    Write-Host "Driver installed. Check it with: qcamctl list" -ForegroundColor Green
    return
}

# ---------------------------------------------------------------------------
if (-not $NoVirtualCamera) {
    Write-Step "Registering the virtual camera"
    # regsvr32 is a GUI-subsystem program: calling it with & neither waits for
    # it nor sets $LASTEXITCODE, so start it explicitly and read its exit code.
    $regsvr = Start-Process regsvr32.exe -ArgumentList '/s', "`"$vcam`"" -Wait -PassThru
    if ($regsvr.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($regsvr.ExitCode)" }
    Write-Host "  registered $vcam"

    # Done here, once, because it needs administrator rights and the service
    # deliberately does not have them. The registration persists across
    # reboots until uninstall.ps1 removes it.
    & $svc --register-vcam
    if ($LASTEXITCODE -ne 0) { throw "registering the virtual camera failed" }
}

# ---------------------------------------------------------------------------
Write-Step "Installing the frame broker service"
& $svc --install
if ($LASTEXITCODE -ne 0) { throw "qcamsvc --install failed" }
Write-Host "  runs as NT SERVICE\qcamsvc (LOCAL SERVICE), not LocalSystem"

Write-Host "  starting qcamsvc"
Start-Service -Name 'qcamsvc' -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$service = Get-Service -Name 'qcamsvc' -ErrorAction SilentlyContinue
if ($service) {
    Write-Host "  qcamsvc is $($service.Status)"
} else {
    Write-Warning "qcamsvc did not appear in the service list"
}

# ---------------------------------------------------------------------------
Write-Step "Looking for the camera"
& $ctl list
if ($LASTEXITCODE -ne 0) {
    Write-Warning ("No camera was found. Plug it in and re-run 'qcamctl list'. " +
                   "If it is plugged in, check Device Manager for a device with " +
                   "hardware id USB\VID_046D&PID_0840 and see docs/troubleshooting.md.")
} else {
    Write-Host ""
    Write-Host "Done. Installed to $InstallDir. Try:" -ForegroundColor Green
    Write-Host "  & '$ctl' attach -t 5     # read frames from the running service"
    if (-not $NoVirtualCamera) {
        Write-Host "  ...and open the Camera app; 'Logitech QuickCam Express (qcam)'"
        Write-Host "  should be in the camera list."
    }
}
