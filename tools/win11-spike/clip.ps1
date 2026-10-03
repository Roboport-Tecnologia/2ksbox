# M23 step 5: the clipboard between the host and Windows 11, run elevated
# on the desktop (as main.ps1 under stub.ps1) by tools/clipboard-win11-test.sh.
# The REPORT disk carries vioser (virtio-win's virtio-serial driver), the
# agent and expect.txt, the text the host put on its clipboard. Appends
# W11-PROBE PASS / FAIL lines.
$r = (Get-Volume -FileSystemLabel REPORT | Select-Object -First 1).DriveLetter
function say($s) { Add-Content -Path "${r}:\w11.log" -Value "W11-PROBE $s" }
function check($name, $ok, $detail) { if ($ok) { say "PASS $name $detail" } else { say "FAIL $name $detail" } }
function waitclip($want, $secs) {
  $t = [Diagnostics.Stopwatch]::StartNew()
  $c = $null
  while ($t.Elapsed.TotalSeconds -lt $secs) {
    try { $c = Get-Clipboard -Raw } catch { }
    if ($c -eq $want) { return [math]::Round($t.Elapsed.TotalSeconds, 1) }
    Start-Sleep -Milliseconds 300
  }
  say "clipboard holds: $c"
  return -1
}
function setclip($v) {   # the clipboard may be open elsewhere a moment
  for ($i = 0; $i -lt 10; $i++) { try { Set-Clipboard -Value $v; return $true } catch { Start-Sleep -Milliseconds 200 } }
  return $false
}
say "start"
foreach ($l in (pnputil /add-driver "${r}:\vioser\vioser.inf" /install 2>&1)) { if ("$l".Trim()) { say "pnputil $("$l".Trim())" } }
Start-Sleep 3
$p = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*1AF4*' -or $_.InstanceId -like '*VIOSERIAL*' -or $_.FriendlyName -like '*Serial*' }
foreach ($d in $p) { say "dev $($d.Status) $($d.Class) $($d.FriendlyName) $($d.InstanceId)" }

# the agent, in the signed-in user's session (where its clipboard is),
# with the user's elevated token: vioser lets only SYSTEM and
# Administrators open the port (an unelevated agent gets error 5)
New-Item -ItemType Directory -Force C:\smbt | Out-Null
Copy-Item "${r}:\2ksbox-agent.exe" C:\smbt\2ksbox-agent.exe -Force
schtasks /create /f /tn agent /tr C:\smbt\2ksbox-agent.exe /sc once /st 23:59 /it /rl highest | Out-Null
schtasks /run /tn agent | Out-Null

$want = (Get-Content "${r}:\expect.txt" -Raw).Trim()
$s = waitclip $want 60
check host-to-guest ($s -ge 0) "after ${s} s"

$mark = "from-guest-" + (Get-Random)
if (setclip $mark) { say "guest-set $mark" } else { say "FAIL set-clipboard" }
# the host answers with its text again, plus "-again", once it has ours
$s = waitclip "$want-again" 60
check host-again ($s -ge 0) "after ${s} s"

if (Test-Path C:\2KSBOX\agent.log) { foreach ($l in Get-Content C:\2KSBOX\agent.log) { say "agent $l" } }
say "done"
