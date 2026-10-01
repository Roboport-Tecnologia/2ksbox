# M20 step 1: runs once at the first logon (autounattend.xml). Reports what
# Windows sees on COM1, which tools/win11-spike.py timestamps, then shuts
# down so the script knows the install finished.
$port = New-Object System.IO.Ports.SerialPort COM1, 115200
$port.Open()
function say($s) { $port.WriteLine("W11-INFO $s") }

$v = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
say "build=$($v.CurrentBuild).$($v.UBR) version=$($v.DisplayVersion) edition=$($v.EditionID)"
say "firmware=$env:firmware_type"
try { $t = Get-Tpm; say "tpm present=$($t.TpmPresent) ready=$($t.TpmReady) enabled=$($t.TpmEnabled)" } catch { say "tpm error=$_" }
try {
  $w = Get-CimInstance -Namespace root/cimv2/security/microsofttpm -ClassName Win32_Tpm
  say "tpm spec=$($w.SpecVersion) manufacturer=$($w.ManufacturerIdTxt) version=$($w.ManufacturerVersion)"
} catch { say "tpm-wmi error=$_" }
try { say "secureboot=$(Confirm-SecureBootUEFI)" } catch { say "secureboot error=$_" }
say "cpu=$((Get-CimInstance Win32_Processor).Name) cores=$((Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors)"
say "memory=$([math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1))GB"
foreach ($d in Get-CimInstance Win32_PnPEntity | Where-Object { $_.ConfigManagerErrorCode -ne 0 }) {
  say "device-problem code=$($d.ConfigManagerErrorCode) name=$($d.Name) id=$($d.DeviceID)"
}

# Every later logon says when the desktop came up. Explorer runs the Run
# key once it has started, so the line is "the desktop is there".
$logon = 'powershell -NoProfile -WindowStyle Hidden -Command "$p=New-Object System.IO.Ports.SerialPort COM1,115200;$p.Open();$p.WriteLine(''W11-DESKTOP'');$p.Close()"'
Set-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name W11Spike -Value $logon

# A full shutdown each time: fast startup would make the next boot a
# resume and the boot timing meaningless.
powercfg /h off
say "done"
$port.Close()
shutdown /s /t 5
