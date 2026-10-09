# viogpudo-install.ps1: our viogpudo (track M24) on Windows 11, test
# signed. 64-bit Windows loads no kernel driver without a signature it
# trusts, and Microsoft's is only on upstream's builds, so this turns test
# mode on, makes a code-signing certificate of this machine's own, signs
# viogpudo.sys with it, writes the package's catalog and signs that,
# trusts the certificate, takes upstream's viogpudo out of the driver
# store (Windows ranks a Microsoft-signed driver above ours whatever the
# version) and installs ours on the virtio-gpu. Test mode and the driver
# start at the next boot. Secure Boot must be off (2ksbox's default: no
# keys enrolled).
#
# Run it elevated from the disc (build-viogpudo.sh iso); it picks the
# processor's folder and copies it to a work folder first (the disc is
# read-only):
#
#   powershell -ep bypass -f D:\viogpudo-install.ps1 [-VSyncHz <n>]    then restart
#
# -VSyncHz sets the vertical blank's rate (the service's Parameters key,
# read when the driver starts; 60 when unset, 0 for upstream's behaviour,
# the A/B without reinstalling). Upstream's driver comes back with
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

if ($VSyncHz -ge 0) {
    $key = "HKLM:\SYSTEM\CurrentControlSet\Services\VioGpuDod\Parameters"
    # New-Item -Force would empty a key that exists
    if (-not (Test-Path $key)) { New-Item $key | Out-Null }
    Set-ItemProperty $key -Name VSyncHz -Type DWord -Value $VSyncHz
    Write-Host "VSyncHz = $VSyncHz"
}
Write-Host "viogpudo-install: done; restart Windows (test mode and the driver start then)"
