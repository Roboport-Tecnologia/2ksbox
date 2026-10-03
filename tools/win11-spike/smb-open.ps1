# M23: files on the share opened in the signed-in user's own apps, as by
# hand (Notepad, Paint), through a one-shot task in that user's
# unelevated session that maps the share itself. smbserve's log (-v)
# times each request; the closing screenshot shows the apps.
$r = (Get-Volume -FileSystemLabel REPORT | Select-Object -First 1).DriveLetter
function say($s) { Add-Content -Path "${r}:\w11.log" -Value "W11-PROBE $s" }
say "start"
# a batch file, since schtasks /tr loses quotes PowerShell 5.1 passes it
New-Item -ItemType Directory -Force C:\smbt | Out-Null
Set-Content C:\smbt\open.cmd -Encoding ASCII -Value @(
  'net use Y: \\10.0.2.4\host /user:smb smb',
  'start notepad Y:\hello.txt',
  'timeout /t 20',
  'start mspaint "Y:\test\New Bitmap image.bmp"')
schtasks /create /f /tn smbopen /tr C:\smbt\open.cmd /sc once /st 23:59 /it | Out-Null
schtasks /run /tn smbopen | Out-Null
say "launched $LASTEXITCODE"
Start-Sleep 60
say "done"
