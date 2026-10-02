# M20 step 4: run elevated on the desktop by tools/win11-spike.py PROBE=1
# (typed into Win+R from the REPORT disk). Appends what Windows sees of
# the machine's devices to the REPORT disk's w11.log.
$r = (Get-Volume -FileSystemLabel REPORT | Select-Object -First 1).DriveLetter
function say($s) { Add-Content -Path "${r}:\w11.log" -Value "W11-PROBE $s" }
try { $t = Get-Tpm; say "tpm present=$($t.TpmPresent) ready=$($t.TpmReady) enabled=$($t.TpmEnabled) owned=$($t.TpmOwned)" } catch { say "tpm error=$_" }
foreach ($l in (tpmtool getdeviceinformation 2>&1)) { if ("$l".Trim()) { say "tpmtool $("$l".Trim())" } }
try {
  $w = Get-CimInstance -Namespace root/cimv2/security/microsofttpm -ClassName Win32_Tpm
  say "tpm-wmi activated=$(($w | Invoke-CimMethod -MethodName IsActivated).IsActivated) enabled=$(($w | Invoke-CimMethod -MethodName IsEnabled).IsEnabled) owned=$(($w | Invoke-CimMethod -MethodName IsOwned).IsOwned)"
} catch { say "tpm-wmi error=$_" }
try { $e = (Get-TpmEndorsementKeyInfo -Hash sha256).PublicKeyHash; say "tpm ek=$e" } catch { say "tpm-ek error=$_" }
foreach ($c in 'SCSIAdapter','HDC','DiskDrive','CDROM','Display','MEDIA','AudioEndpoint','Net','USB','Keyboard','Mouse','HIDClass','SecurityDevices','Ports','System') {
  foreach ($d in Get-PnpDevice -PresentOnly -Class $c -ErrorAction SilentlyContinue) {
    say "dev class=$c status=$($d.Status) name=$($d.FriendlyName) id=$($d.InstanceId)"
  }
}
foreach ($a in Get-NetAdapter) { say "adapter $($a.Name) | $($a.InterfaceDescription) | $($a.Status) | $($a.LinkSpeed)" }
foreach ($i in Get-NetIPAddress -AddressFamily IPv4) { say "ip $($i.InterfaceAlias) $($i.IPAddress)" }
foreach ($l in (pnputil /enum-drivers 2>&1 | Select-String -Pattern 'Original Name|Provider Name' )) { say "store $("$l".Trim())" }
foreach ($v in Get-Volume) { say "vol $($v.DriveLetter) label=$($v.FileSystemLabel) type=$($v.DriveType) fs=$($v.FileSystem)" }
foreach ($a in Get-CimInstance Win32_SoundDevice) { say "sound $($a.Name) status=$($a.Status)" }
foreach ($g in Get-CimInstance Win32_VideoController) { say "video $($g.Name) $($g.CurrentHorizontalResolution)x$($g.CurrentVerticalResolution)" }
say "done"
