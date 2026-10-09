# viogpudo-install.ps1: our viogpudo (track M24) on Windows 11, test
# signed. 64-bit Windows loads no kernel driver without a signature it
# trusts, and Microsoft's is only on upstream's builds, so this turns test
# mode on, makes a code-signing certificate of this machine's own, signs
# viogpudo.sys with it, writes the package's catalog and signs that,
# trusts the certificate, takes upstream's viogpudo out of the driver
# store (Windows ranks a Microsoft-signed driver above ours whatever the
# version), installs ours on the virtio-gpu, and installs the resolution
# service built with it (vgpusrv, viogpuap) so the desktop follows the
# window. The driver starts at the next boot. Secure Boot must be off (2ksbox's default: no
# keys enrolled).
#
# Nothing is removed until Windows will take ours: test mode must already
# be on in the running kernel (its code integrity options), so a first run
# on a machine without it only turns it on and asks for a restart and a
# second run. Smart App Control keeps test signing off even then (Windows
# 11 on Arm had it evaluating; track M24): the script names it and stops.
# If pnputil still refuses ours, upstream's is put back from the drivers
# disc, so the virtio-gpu is never left without a driver (a black window).
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

# The running kernel's code integrity options (SYSTEM_CODEINTEGRITY_
# INFORMATION, class 103): CODEINTEGRITY_OPTION_TESTSIGN is 0x2. The BCD
# says what the next boot asks for; this says what Windows enforces now.
Add-Type 'using System; using System.Runtime.InteropServices;
public static class CodeIntegrity {
    [DllImport("ntdll.dll")]
    static extern int NtQuerySystemInformation(int c, int[] b, int l, out int r);
    public static int Options() {
        int[] b = { 8, 0 }; int r;
        return NtQuerySystemInformation(103, b, 8, out r) == 0 ? b[1] : -1;
    }
}'
$ci = [CodeIntegrity]::Options()
if ($ci -lt 0 -or -not ($ci -band 2)) {
    $sac = (Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
    if ($sac -eq 1 -or $sac -eq 2) {
        Write-Host ("Smart App Control is {0}, and it keeps test-signed drivers out even in test mode." -f $(if ($sac -eq 1) { "on" } else { "evaluating" }))
        Write-Host "Turn it off (Windows Security > App & browser control > Smart App Control > Off;"
        Write-Host "it cannot be turned on again without reinstalling Windows), restart, and run this again."
    } else {
        Write-Host "Test mode is set for the next boot. Restart Windows and run this again."
    }
    Write-Host "viogpudo-install: nothing installed; the current display driver is untouched"
    exit 1
}

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
if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) {
    $code = $LASTEXITCODE
    # never a virtio-gpu without a driver: upstream's back from the drivers disc
    $up = Get-Volume | Where-Object DriveLetter | ForEach-Object {
        "$($_.DriveLetter):\`$WinPEDriver`$\viogpudo\viogpudo.inf" } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($up) {
        Write-Host "putting upstream's viogpudo back from $up"
        pnputil /add-driver $up /install | Out-Host
    } else {
        Write-Host "no drivers disc in a drive: the virtio-gpu has no driver until one is installed"
    }
    throw "pnputil could not install our driver ($code; C:\Windows\INF\setupapi.dev.log says why)"
}

# The resolution service (vgpusrv starts viogpuap in the console session,
# which applies the window's size to the desktop; without it the desktop
# takes that size only at boot), from this build, in place of any other
# copy: as guest-agent/install.ps1 does with upstream's.
$gdst = Join-Path $env:ProgramFiles "2ksbox\viogpu"
$srv = Join-Path $gdst "vgpusrv.exe"
New-Item -ItemType Directory -Force $gdst | Out-Null
$had = Get-Service vgpusrv -ErrorAction SilentlyContinue
if ($had) { Stop-Service vgpusrv -Force -ErrorAction SilentlyContinue }
# Stop-Service returns before the service's process has gone, and the
# helper it started holds its file too: both gone before the copy
Get-Process vgpusrv, viogpuap -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process vgpusrv, viogpuap -ErrorAction SilentlyContinue | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
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
