# M23 step 2: the shared folder from Windows 11, run elevated on the
# desktop by tools/win11-spike.py PROBE=1 PROBE_PS1=<this> SMB=<socket>
# with smbserve listening there (user smb, password smb, share host).
# Appends W11-PROBE lines to the REPORT disk's w11.log.
$r = (Get-Volume -FileSystemLabel REPORT | Select-Object -First 1).DriveLetter
function say($s) { Add-Content -Path "${r}:\w11.log" -Value "W11-PROBE $s" }
foreach ($l in (net use Z: \\10.0.2.4\host /user:smb smb 2>&1)) { if ("$l".Trim()) { say "net-use $("$l".Trim())" } }
try { foreach ($c in Get-SmbConnection) { say "conn server=$($c.ServerName) share=$($c.ShareName) dialect=$($c.Dialect) signed=$($c.Signed) encrypted=$($c.Encrypted) user=$($c.UserName)" } } catch { say "conn error=$_" }
try { foreach ($f in Get-ChildItem -Force -Recurse Z:\) { say "ls $($f.FullName) $($f.Length)" } } catch { say "ls error=$_" }
try { say "read $(Get-Content Z:\hello.txt)" } catch { say "read error=$_" }
try { Set-Content -Path Z:\from-guest.txt -Value "written by Windows 11"; say "write ok" } catch { say "write error=$_" }
try {
  Copy-Item C:\Windows\System32\notepad.exe Z:\notepad.exe
  $a = (Get-FileHash C:\Windows\System32\notepad.exe).Hash; $b = (Get-FileHash Z:\notepad.exe).Hash
  say "copy match=$($a -eq $b) $a"
} catch { say "copy error=$_" }
try { New-Item -ItemType Directory Z:\gdir | Out-Null; Rename-Item Z:\gdir gdir2; Remove-Item Z:\gdir2; say "dir ok" } catch { say "dir error=$_" }
try { Remove-Item Z:\notepad.exe; say "delete ok" } catch { say "delete error=$_" }
foreach ($l in (net use 2>&1)) { if ("$l".Trim()) { say "net-use-list $("$l".Trim())" } }
say "done"
