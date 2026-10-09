# viogpudo-install.ps1: our viogpudo (track M24) on Windows 11, test
# signed. 64-bit Windows loads no kernel driver without a signature it
# trusts, and Microsoft's is only on upstream's builds, so this turns test
# mode on, makes a code-signing certificate of this machine's own, signs
# viogpudo.sys with it, writes the package's catalog and signs that,
# trusts the certificate, takes upstream's viogpudo out of the driver
# store (Windows ranks a Microsoft-signed driver above ours whatever the
# version), installs ours on the virtio-gpu, and installs the resolution
# service built with it (vgpusrv, viogpuap) so the desktop follows the
# window. Test mode and the driver start at the next boot. Secure Boot must be off (2ksbox's default: no
# keys enrolled).
#
# Run it elevated from the disc (build-viogpudo.sh iso); it picks the
# processor's folder and copies it to a work folder first (the disc is
# read-only):
#
#   powershell -ep bypass -f D:\viogpudo-install.ps1 [-VSyncHz <n>]    then restart
#
# -VSyncHz sets the vertical blank's rate (the service's Parameters key,
# read when the driver starts; 0 for upstream's behaviour, the A/B without
# reinstalling). Without it the value is removed and the rate is the one
# the host gives in the device's EDID (patch 02; 60 when it gives none). Upstream's driver comes back with
# `pnputil /add-driver <drivers disc>:\$WinPEDriver$\viogpudo\viogpudo.inf
# /install` after `pnputil /delete-driver` of ours.
#
# SPDX-License-Identifier: GPL-2.0-or-later
param([int]$VSyncHz = -1)
$ErrorActionPreference = "Stop"

$arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
$src = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) $arch
$work = Join-Path $env:SystemRoot "Temp\2ksbox-viogpudo"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
foreach ($f in "viogpudo.sys", "viogpudo.inf") {
    Copy-Item (Join-Path $src $f) $work
    # a disc's files come over read-only, and signing rewrites them
    (Get-Item (Join-Path $work $f)).IsReadOnly = $false
}

bcdedit /set testsigning on | Out-Host
if ($LASTEXITCODE) { throw "bcdedit refused test mode (Secure Boot on?)" }

$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq "CN=2ksbox test signing" | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=2ksbox test signing" `
        -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(20)
}
foreach ($store in "Root", "TrustedPublisher") {
    $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, "LocalMachine")
    $s.Open("ReadWrite"); $s.Add($cert); $s.Close()
}

function Sign($path) {
    $r = Set-AuthenticodeSignature -FilePath $path -Certificate $cert -HashAlgorithm SHA256
    if ($r.Status -ne "Valid") { throw "signing $path`: $($r.StatusMessage)" }
}
Sign (Join-Path $work "viogpudo.sys")
New-FileCatalog -Path $work -CatalogFilePath (Join-Path $work "viogpudo.cat") -CatalogVersion 2.0 | Out-Null
Sign (Join-Path $work "viogpudo.cat")

# every viogpudo in the store, upstream's and an earlier one of ours
Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like "*\viogpudo.inf" } | ForEach-Object {
    Write-Host "removing $($_.Driver) ($($_.ProviderName) $($_.Version))"
    pnputil /delete-driver $_.Driver /uninstall /force | Out-Host
}

pnputil /add-driver (Join-Path $work "viogpudo.inf") /install | Out-Host
# 3010: installed, a restart needed
if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) { throw "pnputil could not install the driver ($LASTEXITCODE)" }

# The resolution service (vgpusrv starts viogpuap in the console session,
# which applies the window's size to the desktop; without it the desktop
# takes that size only at boot), from this build, in place of any other
# copy: as guest-agent/install.ps1 does with upstream's.
$gdst = Join-Path $env:ProgramFiles "2ksbox\viogpu"
$srv = Join-Path $gdst "vgpusrv.exe"
New-Item -ItemType Directory -Force $gdst | Out-Null
$had = Get-Service vgpusrv -ErrorAction SilentlyContinue
if ($had) { Stop-Service vgpusrv -Force -ErrorAction SilentlyContinue }
Get-Process viogpuap -ErrorAction SilentlyContinue | Stop-Process -Force
foreach ($f in "vgpusrv.exe", "viogpuap.exe") { Copy-Item (Join-Path $src $f) $gdst -Force }
if ($had) {
    # one installed before, maybe elsewhere: pointed at this copy
    sc.exe config vgpusrv binPath= "`"$srv`"" | Out-Host
    Start-Service vgpusrv
} else {
    & $srv -i | Out-Host
}
Write-Host "resolution service: $((Get-Service vgpusrv -ErrorAction SilentlyContinue).Status)"

$key = "HKLM:\SYSTEM\CurrentControlSet\Services\VioGpuDod\Parameters"
if ($VSyncHz -ge 0) {
    # New-Item -Force would empty a key that exists
    if (-not (Test-Path $key)) { New-Item $key | Out-Null }
    Set-ItemProperty $key -Name VSyncHz -Type DWord -Value $VSyncHz
    Write-Host "VSyncHz = $VSyncHz"
} else {
    # none: the rate the host gives in the EDID (patch 02), not an earlier
    # install's A/B
    Remove-ItemProperty $key -Name VSyncHz -ErrorAction SilentlyContinue
    Write-Host "VSyncHz: the EDID's rate"
}
Write-Host "viogpudo-install: done; restart Windows (test mode and the driver start then)"
