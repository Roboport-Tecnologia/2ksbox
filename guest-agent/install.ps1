# 2ksbox's guest tools for Windows 11 (track M23, doc 24 section 4), run
# elevated by install.cmd from the drivers disc:
#   - every driver on the disc ($WinPEDriver$: NetKVM, viogpudo, vioser),
#     for a machine installed without it;
#   - the agent in C:\Program Files\2ksbox;
#   - two tasks at every user's logon: the agent with the user's elevated
#     token (vioser lets only Administrators open the clipboard's port),
#     and `2ksbox-agent --map` unelevated, which maps the host's shared
#     folder where Explorer sees it;
#   - both started now, so nothing waits for a reboot.
# Writes C:\2KSBOX\install.log. ASCII only (Windows PowerShell 5.1).
$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
New-Item -ItemType Directory -Force C:\2KSBOX | Out-Null
$log = 'C:\2KSBOX\install.log'
function say($s) { Add-Content -Path $log -Value $s; Write-Host $s }
say "2ksbox guest tools: installing from $here"

$drivers = Join-Path $root '$WinPEDriver$'
if (Test-Path -LiteralPath $drivers) {
  foreach ($l in (pnputil /add-driver "$drivers\*.inf" /subdirs /install 2>&1)) { if ("$l".Trim()) { say "  $("$l".Trim())" } }
}

$dst = Join-Path $env:ProgramFiles '2ksbox'
New-Item -ItemType Directory -Force $dst | Out-Null
Get-Process 2ksbox-agent -ErrorAction SilentlyContinue | Stop-Process -Force
Copy-Item (Join-Path $here '2ksbox-agent.exe') $dst -Force
$exe = Join-Path $dst '2ksbox-agent.exe'
say "agent: $exe"

$users = 'S-1-5-32-545'     # BUILTIN\Users, by SID (names are localized)
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries `
  -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName '2ksbox agent' -Force -Trigger $trigger -Settings $settings `
  -Action (New-ScheduledTaskAction -Execute $exe) `
  -Principal (New-ScheduledTaskPrincipal -GroupId $users -RunLevel Highest) | Out-Null
Register-ScheduledTask -TaskName '2ksbox shared folder' -Force -Trigger $trigger -Settings $settings `
  -Action (New-ScheduledTaskAction -Execute $exe -Argument '--map') `
  -Principal (New-ScheduledTaskPrincipal -GroupId $users -RunLevel Limited) | Out-Null
say "tasks: '2ksbox agent' (elevated) and '2ksbox shared folder', at every logon"

Start-ScheduledTask -TaskName '2ksbox agent'
Start-ScheduledTask -TaskName '2ksbox shared folder'
say "done: the clipboard is shared now; the shared folder, if the machine has one, gets a drive letter"
