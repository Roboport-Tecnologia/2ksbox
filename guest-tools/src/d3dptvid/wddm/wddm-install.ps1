# wddm-install.ps1: the WDDM display driver on 64-bit Windows 11, test
# signed (track M20). 64-bit Windows loads no unsigned kernel driver, so
# this turns test mode on, makes a code-signing certificate of this
# machine's own, signs d3dptkmd.sys and d3dptumd.dll with it, writes the
# package's catalog and signs that, trusts the certificate, and installs
# the package on d3dpt-vga. Test mode starts at the next boot, and so
# does the driver. Run it elevated from the folder with the driver's
# three files; it copies them to a work folder first (the disc is
# read-only). Secure Boot must be off (the default with no keys enrolled).
#
#   powershell -ep bypass -f wddm-install.ps1        then restart
#
# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = "Stop"
$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$work = Join-Path $env:SystemRoot "Temp\2ksbox-wddm"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
foreach ($f in "d3dptkmd.sys", "d3dptumd.dll", "d3dptkmd.inf") {
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
Sign (Join-Path $work "d3dptkmd.sys")
Sign (Join-Path $work "d3dptumd.dll")
New-FileCatalog -Path $work -CatalogFilePath (Join-Path $work "d3dptkmd.cat") -CatalogVersion 2.0 | Out-Null
Sign (Join-Path $work "d3dptkmd.cat")

pnputil /add-driver (Join-Path $work "d3dptkmd.inf") /install | Out-Host
Write-Host "wddm-install: done; restart Windows (test mode and the driver start then)"
