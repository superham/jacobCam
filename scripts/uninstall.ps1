#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Removes the qcam driver stack.

.DESCRIPTION
    Unwinds install.ps1: stops and deletes the service, removes the registered
    virtual camera, unregisters the COM server, deletes C:\Program Files\qcam,
    removes the driver package, and removes the signing certificate from the
    machine's trusted stores. Afterwards nothing of qcam is left, and there is
    no Windows setting to put back: install.ps1 never changes any.

.PARAMETER BinDir
    Where the installed binaries are. Defaults to C:\Program Files\qcam, which
    is where install.ps1 puts them. Pass a build directory only to remove an
    install made by an older version of the script, which ran from there.

.PARAMETER KeepDriver
    Leave the WinUSB binding and its certificate in place. Useful when you are
    about to reinstall a rebuilt binary with install.ps1 -SkipDriver.
#>
[CmdletBinding()]
param(
    [string] $BinDir = (Join-Path $env:ProgramFiles 'qcam'),
    [switch] $KeepDriver
)

$ErrorActionPreference = 'Continue'

function Write-Step([string] $Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

$defaultDir = Join-Path $env:ProgramFiles 'qcam'
if (Test-Path -LiteralPath $BinDir) {
    $BinDir = (Resolve-Path -LiteralPath $BinDir).Path
}
$svc  = Join-Path $BinDir 'qcamsvc.exe'
$vcam = Join-Path $BinDir 'qcamvcam.dll'

Write-Step "Stopping the service"
Stop-Service -Name 'qcamsvc' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Step "Removing the registered virtual camera"
if (Test-Path -LiteralPath $svc) {
    & $svc --remove-vcam
} else {
    Write-Warning "qcamsvc.exe not found in $BinDir; skipping virtual camera removal"
}

Write-Step "Unregistering the COM media source"
if (Test-Path -LiteralPath $vcam) {
    & regsvr32.exe /u /s $vcam
    Write-Host "  unregistered $vcam"
} else {
    Write-Warning "qcamvcam.dll not found; skipping"
}

Write-Step "Removing the service"
if (Test-Path -LiteralPath $svc) { & $svc --uninstall }

# Only ever delete the directory install.ps1 created, never a build tree
# that was passed in with -BinDir.
if ($BinDir -eq $defaultDir -and (Test-Path -LiteralPath $defaultDir)) {
    Write-Step "Deleting $defaultDir"
    # The Frame Server may still have qcamvcam.dll loaded.
    Stop-Service -Name 'FrameServer' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    Remove-Item -LiteralPath $defaultDir -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $defaultDir) {
        Write-Warning "Could not delete $defaultDir; a file is still in use. Reboot and delete it."
    }
}

if (-not $KeepDriver) {
    Write-Step "Removing the driver package"
    # Find our package by its provider name, then delete it. (Not named
    # $matches: that is PowerShell's automatic variable, and every -match
    # below would overwrite it.)
    $packages = & pnputil.exe /enum-drivers
    $current  = $null
    $found    = @()
    foreach ($line in $packages) {
        if ($line -match '^Published Name\s*:\s*(\S+)') { $current = $Matches[1] }
        if ($line -match 'qcam' -and $current) {
            $found += $current
            $current = $null
        }
    }
    if ($found.Count -eq 0) {
        Write-Host "  no qcam driver package found in the driver store"
    }
    foreach ($package in ($found | Select-Object -Unique)) {
        Write-Host "  deleting $package"
        & pnputil.exe /delete-driver $package /uninstall /force
    }

    Write-Step "Removing the signing certificate"
    # Every reinstall makes a new one, so there may be several.
    $stores = 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher'
    $certs  = Get-ChildItem -Path $stores |
        Where-Object { $_.Subject -like '*qcam driver package*' }
    if (-not $certs) { Write-Host "  none found" }
    foreach ($cert in $certs) {
        Write-Host "  $($cert.Thumbprint) from $(Split-Path -Leaf $cert.PSParentPath)"
        Remove-Item -LiteralPath $cert.PSPath -Force
    }
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
