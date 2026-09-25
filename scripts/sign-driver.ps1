<#
.SYNOPSIS
    Signs qcamusb.inf with a single-use local certificate, so it installs with
    Secure Boot on and without test-signing mode.

.DESCRIPTION
    qcamusb.inf contains no code: the only driver it loads is WinUSB.sys, which
    Microsoft has already signed. What Windows still wants is a signature on
    the *package* (the .cat catalog listing the INF's hash), from a
    certificate the machine trusts. That is a user-mode trust decision, not
    kernel code signing, so test-signing mode is not needed for it. This is
    the same approach Zadig/libwdi use for WinUSB bindings.

    This script:
      1. Builds a catalog for the INF with makecat.exe (Windows SDK).
      2. Creates a self-signed, code-signing-only, non-CA certificate.
      3. Signs the catalog with it.
      4. Exports the public certificate as qcamusb.cer.
      5. Deletes the certificate and its private key.

    Step 5 is the important one. The certificate becomes trusted machine-wide
    when install.ps1 imports it, and it is safe to trust only because the key
    no longer exists: nothing else can ever be signed with it.

    Needs no administrator rights. Writes qcamusb.inf, qcamusb.cat and
    qcamusb.cer into OutDir.

.PARAMETER InfPath
    The INF to sign.

.PARAMETER OutDir
    Where to write the signed package. install.ps1 uses a directory only
    administrators can write to, so nothing can swap the INF after it is hashed.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $InfPath,
    [Parameter(Mandatory)] [string] $OutDir
)

$ErrorActionPreference = 'Stop'

# Also matched by install.ps1 and uninstall.ps1 to find these certificates.
$Subject = 'CN=qcam driver package - local key destroyed'

function Find-SdkTool([string] $Name) {
    $arch = switch ($env:PROCESSOR_ARCHITECTURE) {
        'AMD64' { 'x64' }
        'ARM64' { 'arm64' }
        default { 'x86' }
    }
    $root = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $found = Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object {
            foreach ($a in @($arch, 'x86')) {
                $candidate = Join-Path $_.FullName "$a\$Name"
                if (Test-Path -LiteralPath $candidate) { $candidate }
            }
        } | Select-Object -First 1
    if (-not $found) {
        throw ("$Name not found. It ships with the Windows SDK, which building " +
               "this project already needs (Visual Studio installer: 'Windows 11 SDK').")
    }
    return $found
}

$InfPath = (Resolve-Path -LiteralPath $InfPath).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

$inf = Join-Path $OutDir 'qcamusb.inf'
$cat = Join-Path $OutDir 'qcamusb.cat'
$cer = Join-Path $OutDir 'qcamusb.cer'
$cdf = Join-Path $OutDir 'qcamusb.cdf'

if ($InfPath -ne $inf) { Copy-Item -LiteralPath $InfPath -Destination $inf -Force }
Remove-Item -LiteralPath $cat, $cer -Force -ErrorAction SilentlyContinue

# --- 1. Catalog -------------------------------------------------------------
# OSAttr says which Windows versions the package is for; 2:10.0 covers
# Windows 10 and 11. <HASH> tags the member by its hash, which is how PnP
# looks the INF up.
$makecat = Find-SdkTool 'makecat.exe'
@"
[CatalogHeader]
Name=qcamusb.cat
ResultDir=$OutDir
PublicVersion=0x0000001
EncodingType=0x00010001
CatalogVersion=2
HashAlgorithms=SHA256
CATATTR1=0x10010001:OSAttr:2:10.0

[CatalogFiles]
<HASH>qcamusb.inf=$inf
<HASH>qcamusb.infATTR1=0x10010001:OSAttr:2:10.0
"@ | Set-Content -LiteralPath $cdf -Encoding ASCII

$output = & $makecat -v $cdf 2>&1
Remove-Item -LiteralPath $cdf -Force
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $cat)) {
    $output | ForEach-Object { Write-Host "  $_" }
    throw "makecat failed with exit code $LASTEXITCODE"
}

# --- 2-5. Sign with a certificate that stops existing afterwards ------------
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
    -FriendlyName 'qcam driver package' `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 `
    -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(20) `
    -TextExtension @('2.5.29.19={critical}{text}CA=false')
try {
    $result = Set-AuthenticodeSignature -LiteralPath $cat -Certificate $cert `
        -HashAlgorithm SHA256
    # Not trusted yet, so UnknownError is the expected status here; anything
    # without a signer certificate means the signing itself failed.
    if (-not $result.SignerCertificate -or
        $result.SignerCertificate.Thumbprint -ne $cert.Thumbprint) {
        throw "signing the catalog failed: $($result.StatusMessage)"
    }
    Export-Certificate -Cert $cert -FilePath $cer -Type CERT | Out-Null
} finally {
    Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
}

Write-Host "  signed $cat"
Write-Host "  certificate $($cert.Thumbprint) (private key destroyed)"
