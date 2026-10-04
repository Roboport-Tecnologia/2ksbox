# M23: the share used by its path, as when \\10.0.2.4\host is typed into
# Explorer and the credential prompt answered: a connection with no drive
# letter, then Explorer on it, Notepad opening a file by its path, and
# Explorer on the server itself (the share list). In the signed-in user's
# unelevated session, through a one-shot task. smbserve's log (-v) times
# each request; the closing screenshot shows the result.
$r = (Get-Volume -FileSystemLabel REPORT | Select-Object -First 1).DriveLetter
function say($s) { Add-Content -Path "${r}:\w11.log" -Value "W11-PROBE $s" }
say "start"
New-Item -ItemType Directory -Force C:\smbt | Out-Null
Set-Content C:\smbt\unc.cmd -Encoding ASCII -Value @(
  'net use \\10.0.2.4\host /user:smb smb',
  'start explorer \\10.0.2.4\host',
  'timeout /t 15',
  'start notepad \\10.0.2.4\host\hello.txt',
  'timeout /t 40',
  'start explorer \\10.0.2.4')
schtasks /create /f /tn smbunc /tr C:\smbt\unc.cmd /sc once /st 23:59 /it | Out-Null
schtasks /run /tn smbunc | Out-Null
say "launched $LASTEXITCODE"
Start-Sleep 120
say "done"
