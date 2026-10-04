# The script tools/win11-spike.py launches when PROBE_PS1 names another:
# that one is copied beside this as main.ps1 and run from here, so a
# script Defender or PowerShell refuses reports why (a W11-PROBE line)
# instead of the shell's "Windows cannot access the specified device"
# box with nothing in the log. M23.
$r = (Get-Volume -FileSystemLabel REPORT | Select-Object -First 1).DriveLetter
try { & "${r}:\main.ps1" } catch { Add-Content -Path "${r}:\w11.log" -Value "W11-PROBE FAIL main.ps1: $_"; Add-Content -Path "${r}:\w11.log" -Value "W11-PROBE done" }
