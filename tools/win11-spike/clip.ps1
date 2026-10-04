# M23: the guest tools as a user installs them, and what they share, run
# elevated on the desktop (as main.ps1 under stub.ps1) by
# tools/clipboard-win11-test.sh. The drivers disc's 2ksbox\install.ps1
# installs vioser and the agent's two logon tasks; then the clipboard
# both ways (expect.txt on the REPORT disk is the host's text) and the
# player's shared folder: mapped by the agent's --map, and read here.
# Appends W11-PROBE PASS / FAIL lines.
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

# the guest tools, from the drivers disc, as install.cmd runs them
$d = (Get-Volume -FileSystemLabel 2KSBOX_DRIVERS -ErrorAction SilentlyContinue | Select-Object -First 1).DriveLetter
if (-not $d) { say "FAIL no drivers disc"; say "done"; return }
& "${d}:\2ksbox\install.ps1" *>&1 | ForEach-Object { if ("$_".Trim()) { say "install $("$_".Trim())" } }
$t1 = Get-ScheduledTask -TaskName '2ksbox agent' -ErrorAction SilentlyContinue
$t2 = Get-ScheduledTask -TaskName '2ksbox shared folder' -ErrorAction SilentlyContinue
check tasks ($t1 -and $t2) ""

$want = (Get-Content "${r}:\expect.txt" -Raw).Trim()
$s = waitclip $want 60
check host-to-guest ($s -ge 0) "after ${s} s"
$mark = "from-guest-" + (Get-Random)
if (setclip $mark) { say "guest-set $mark" } else { say "FAIL set-clipboard" }
$s = waitclip "$want-again" 60
check host-again ($s -ge 0) "after ${s} s"

# the shared folder: the agent's --map put it on a letter in the user's
# (unelevated) session, which this elevated one does not see; its log says
$t = [Diagnostics.Stopwatch]::StartNew()
$m = $null
while ($t.Elapsed.TotalSeconds -lt 90 -and -not $m) {
  if (Test-Path C:\2KSBOX\agent.log) { $m = Select-String -Path C:\2KSBOX\agent.log -Pattern 'shared folder on|shared folder already|mapping the shared folder failed|no shared folder' | Select-Object -Last 1 }
  if (-not $m) { Start-Sleep 2 }
}
check share-mapped ("$m" -match 'shared folder on|already connected') "$($m.Line)"
net use \\10.0.2.4\host /user:2ksbox 2ksbox 2>&1 | Out-Null
$h = $null
try { $h = Get-Content \\10.0.2.4\host\hello.txt -Raw } catch { }
check share-read ("$h".Trim() -eq 'hello from the host') "$("$h".Trim())"

if (Test-Path C:\2KSBOX\agent.log) { foreach ($l in Get-Content C:\2KSBOX\agent.log) { say "agent $l" } }
say "done"
