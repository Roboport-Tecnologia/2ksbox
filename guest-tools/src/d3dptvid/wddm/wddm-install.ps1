# wddm-install.ps1: the WDDM display driver on 64-bit Windows 11, test
# signed (track M20). 64-bit Windows loads no unsigned kernel driver, so
# this turns test mode on, makes a code-signing certificate of this
# machine's own, signs d3dptkmd.sys and the user-mode drivers (Direct3D
# 9's d3dptumd.dll, Direct3D 10 and 11's d3dpt11.dll, and their 32-bit
# copies for 32-bit processes) with it, writes the package's catalog and
# signs that, trusts the certificate, and installs the package on
# d3dpt-vga. Test mode starts at the next boot, and so does the driver.
# Run it elevated from the folder with the driver's six files; it copies
# them to a work folder first (the disc is read-only). Secure Boot must be
# off (the default with no keys enrolled).
#
# DWM composes through Direct3D 11's driver (track M25 step 4: the 11.1
# interface, WDDM 1.2). An earlier install's D3D11Exclude, which kept it
# on Direct3D 9's driver, is removed; d3dpt11.dll still refuses a process
# HKLM\SOFTWARE\2ksbox's D3D11Exclude names when it is set by hand.
#
#   powershell -ep bypass -f wddm-install.ps1        then restart
#
# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = "Stop"
$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$work = Join-Path $env:SystemRoot "Temp\2ksbox-wddm"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
foreach ($f in "d3dptkmd.sys", "d3dptumd.dll", "d3dptumd32.dll", "d3dpt11.dll", "d3dpt11_32.dll", "d3dptkmd.inf") {
    Copy-Item (Join-Path $src $f) $work
    # a disc's files come over read-only, and signing rewrites them
    (Get-Item (Join-Path $work $f)).IsReadOnly = $false
}

# A version of this install's minute: the INF's DriverVer is the same for
# every build, and Windows keeps a device on the package it has over one
# of the same version, so a new build installed over an old one never ran
# (the Arm machine, track M25). Major and minor stay the INF's.
#
# Never older than the driver a d3dpt-vga device runs now: Windows ranks
# packages of one rank by date, then version, and the guest's clock is
# not the host's (a machine whose clock went back kept a package of the
# same day stamped later, and this install silently did nothing; The
# Witcher 3's machine, 2026-11-27). Past it by a minute when it is newer.
$inf = Join-Path $work "d3dptkmd.inf"
$now = Get-Date
$build = [int]($now.Date - [datetime]"2026-01-01").TotalDays
$rev = $now.Hour * 60 + $now.Minute
$mm = [regex]::Match((Get-Content $inf -Raw), '(?m)^DriverVer=[^,]*,(\d+)\.(\d+)\.')
$date = $now.Date
$ver = [version]"$($mm.Groups[1].Value).$($mm.Groups[2].Value).$build.$rev"
foreach ($d in Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceID LIKE 'PCI\\VEN_1234&DEV_3D00%'" -ErrorAction SilentlyContinue) {
    if (-not $d.DriverVersion) { continue }
    $iv = [version]$d.DriverVersion
    $idate = if ($d.DriverDate) { ([datetime]$d.DriverDate).Date } else { $date }
    if ($idate -gt $date) { $date = $idate }
    if ($iv -ge $ver) {
        $ver = if ($iv.Revision -lt 65534) { [version]"$($iv.Major).$($iv.Minor).$($iv.Build).$($iv.Revision + 1)" }
               else { [version]"$($iv.Major).$($iv.Minor).$($iv.Build + 1).0" }
    }
}
(Get-Content $inf) -replace '^DriverVer=[^,]*,\d+\.\d+\.\d+\.\d+', `
    "DriverVer=$($date.ToString('MM/dd/yyyy', [cultureinfo]::InvariantCulture)),$ver" | Set-Content $inf -Encoding ascii
Select-String -Path $inf -Pattern '^DriverVer=' | ForEach-Object { Write-Host "wddm-install: $($_.Line)" }

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
Sign (Join-Path $work "d3dptkmd.sys")
Sign (Join-Path $work "d3dptumd.dll")
Sign (Join-Path $work "d3dptumd32.dll")
Sign (Join-Path $work "d3dpt11.dll")
Sign (Join-Path $work "d3dpt11_32.dll")
New-FileCatalog -Path $work -CatalogFilePath (Join-Path $work "d3dptkmd.cat") -CatalogVersion 2.0 | Out-Null
Sign (Join-Path $work "d3dptkmd.cat")

# (no New-Item -Force on a key that is there: it empties the key)
if (-not (Test-Path "HKLM:\SOFTWARE\2ksbox")) { New-Item "HKLM:\SOFTWARE\2ksbox" | Out-Null }
Remove-ItemProperty "HKLM:\SOFTWARE\2ksbox" -Name D3D11Exclude -ErrorAction SilentlyContinue

pnputil /add-driver (Join-Path $work "d3dptkmd.inf") /install | Out-Host
Write-Host "wddm-install: done; restart Windows (test mode and the driver start then)"
