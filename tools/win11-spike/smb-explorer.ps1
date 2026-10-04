# M23 step 3: the shared folder under Explorer's copy engine and real
# use, run elevated on the desktop by tools/smb-win11-test.sh (through
# tools/win11-spike.py PROBE=1). The host put Z:\hostsrc and its hashes
# (Z:\hostsrc.sha256) on the share. Appends W11-PROBE PASS / FAIL lines
# to the REPORT disk's w11.log. Run as main.ps1 under stub.ps1. ASCII only: Windows PowerShell 5.1 reads a
# script without a BOM as ANSI, so non-ASCII names are built from codes.
$r = (Get-Volume -FileSystemLabel REPORT | Select-Object -First 1).DriveLetter
function say($s) { Add-Content -Path "${r}:\w11.log" -Value "W11-PROBE $s" }
function check($name, $ok, $detail) { if ($ok) { say "PASS $name $detail" } else { say "FAIL $name $detail" } }
$ErrorActionPreference = 'Stop'
say "start"

function hashes($root) {
  $h = @{}
  foreach ($f in Get-ChildItem -LiteralPath $root -Recurse -File -Force) {
    $h[$f.FullName.Substring($root.Length).TrimStart('\')] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
  }
  $h
}
function diff($a, $b) {   # names whose hashes differ or are missing on one side
  $bad = @()
  foreach ($k in $a.Keys) { if ($b[$k] -ne $a[$k]) { $bad += $k } }
  foreach ($k in $b.Keys) { if (-not $a.ContainsKey($k)) { $bad += $k } }
  $bad
}
function tally($root) {
  $f = @(Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue)
  "{0}/{1}" -f $f.Count, ($f | Measure-Object -Sum Length).Sum
}
# Explorer's copy engine (IFileOperation underneath, as a drag in Explorer):
# no UI, yes to all, no confirmations. It returns at once, so wait until
# the destination's count and size match and stay put.
function shellcopy($from, $toDir, $want, $secs) {
  $sh = New-Object -ComObject Shell.Application
  $sh.NameSpace($toDir).CopyHere($from, 4 + 16 + 512 + 1024)
  $leaf = Split-Path $from -Leaf
  $dst = Join-Path $toDir $leaf
  $t = [Diagnostics.Stopwatch]::StartNew()
  while ($t.Elapsed.TotalSeconds -lt $secs) {
    Start-Sleep -Milliseconds 500
    if ((tally $dst) -eq $want) { Start-Sleep 2; if ((tally $dst) -eq $want) { break } }
  }
  [math]::Round($t.Elapsed.TotalSeconds, 1)
}

$u1 = "caf$([char]0xe9) $([char]0x65e5)$([char]0x672c)$([char]0x8a9e).txt"   # cafe-acute, three CJK
$local = 'C:\smbt'
if (Test-Path $local) { Remove-Item -Recurse -Force $local }
New-Item -ItemType Directory $local | Out-Null

try {
  net use Z: \\10.0.2.4\host /user:smb smb 2>&1 | Out-Null
  check map (Test-Path Z:\hostsrc) ""
} catch { say "FAIL map $_" }

# 1. The host's tree down, through Explorer's engine.
try {
  $want = @{}
  foreach ($l in Get-Content -LiteralPath Z:\hostsrc.sha256 -Encoding UTF8) {
    if ($l) { $p = $l.Split("`t"); $want[$p[1]] = $p[0] }
  }
  $n = @(Get-ChildItem -LiteralPath Z:\hostsrc\many -File).Count
  check listing ($n -eq 3000) "many=$n"
  $tal = tally Z:\hostsrc
  $s = shellcopy Z:\hostsrc $local $tal 600
  $bad = diff $want (hashes "$local\hostsrc")
  check down ($bad.Count -eq 0) "files=$($want.Count) $tal in ${s}s bad=$($bad -join ',')"
} catch { say "FAIL down $_" }

# 2. A guest tree up: Unicode, a long name, an empty and a read-only file,
# a date the copy must keep.
try {
  $g = "$local\gsrc"
  New-Item -ItemType Directory "$g\a\b\c" | Out-Null
  $rnd = New-Object Random 23
  for ($i = 0; $i -lt 200; $i++) {
    $buf = New-Object byte[] ($rnd.Next(0, 300000))
    $rnd.NextBytes($buf)
    $dir = @($g, "$g\a", "$g\a\b", "$g\a\b\c")[$i % 4]
    [IO.File]::WriteAllBytes("$dir\f$i.bin", $buf)
  }
  Set-Content -LiteralPath "$g\$u1" -Value 'unicode'
  Set-Content -LiteralPath ("$g\" + ('L' * 180) + '.txt') -Value 'long'
  New-Item -ItemType File "$g\empty.txt" | Out-Null
  Set-Content "$g\ro.txt" -Value 'read only'
  Set-Content "$g\dated.txt" -Value 'dated'
  (Get-Item "$g\dated.txt").LastWriteTime = Get-Date '2005-05-05 05:05:05'
  Set-ItemProperty "$g\ro.txt" -Name IsReadOnly -Value $true
  $mine = hashes $g
  $s = shellcopy $g 'Z:\' (tally $g) 600
  $bad = diff $mine (hashes Z:\gsrc)
  check up ($bad.Count -eq 0) "files=$($mine.Count) $(tally $g) in ${s}s bad=$($bad -join ',')"
  $d = (Get-Item Z:\gsrc\dated.txt).LastWriteTime
  check mtime ($d -eq (Get-Date '2005-05-05 05:05:05')) "$d"
  check readonly ((Get-Item Z:\gsrc\ro.txt).IsReadOnly) ""
  check unicode (Test-Path -LiteralPath "Z:\gsrc\$u1") ""
  ($mine.GetEnumerator() | ForEach-Object { "$($_.Value)`t$($_.Key)" }) | Set-Content -Encoding UTF8 Z:\gsrc.sha256
} catch { say "FAIL up $_" }

# 3. A large file both ways, timed.
try {
  $big = "$local\big.bin"
  $fs = [IO.File]::Create($big)
  $blk = New-Object byte[] (1MB)
  (New-Object Random 5).NextBytes($blk)
  for ($i = 0; $i -lt 512; $i++) { $blk[0] = $i % 256; $blk[1] = [math]::Floor($i / 256); $fs.Write($blk, 0, $blk.Length) }
  $fs.Close()
  $h = (Get-FileHash $big).Hash
  $t = [Diagnostics.Stopwatch]::StartNew(); Copy-Item $big Z:\big.bin; $up = $t.Elapsed.TotalSeconds
  $t = [Diagnostics.Stopwatch]::StartNew(); Copy-Item Z:\big.bin "$local\big2.bin"; $down = $t.Elapsed.TotalSeconds
  $ok = ((Get-FileHash Z:\big.bin).Hash -eq $h) -and ((Get-FileHash "$local\big2.bin").Hash -eq $h)
  check big $ok ("512 MB up {0:N1} s ({1:N0} MB/s), down {2:N1} s ({3:N0} MB/s)" -f $up, (512 / $up), $down, (512 / $down))
  Remove-Item Z:\big.bin
  check big-delete (-not (Test-Path Z:\big.bin)) ""
} catch { say "FAIL big $_" }

# 4. Editing in place: append, truncate, overwrite, and the
# write-a-temp-then-ReplaceFile save many editors do.
try {
  Set-Content Z:\edit.txt -Value 'one' -NoNewline
  Add-Content Z:\edit.txt -Value 'two' -NoNewline
  check append ((Get-Content Z:\edit.txt -Raw) -eq 'onetwo') ""
  $f = [IO.File]::Open('Z:\edit.txt', 'Open', 'ReadWrite'); $f.SetLength(3); $f.Close()
  check truncate ((Get-Content Z:\edit.txt -Raw) -eq 'one') ""
  Set-Content Z:\edit.new -Value 'saved' -NoNewline
  [IO.File]::Replace('Z:\edit.new', 'Z:\edit.txt', 'Z:\edit.bak')
  check replace (((Get-Content Z:\edit.txt -Raw) -eq 'saved') -and ((Get-Content Z:\edit.bak -Raw) -eq 'one') -and -not (Test-Path Z:\edit.new)) ""
  Copy-Item -Force "$local\gsrc\dated.txt" Z:\edit.txt
  check overwrite ((Get-Content Z:\edit.txt -Raw).Trim() -eq 'dated') ""
  Rename-Item Z:\edit.txt EDIT.TXT
  check case-rename ((Get-ChildItem Z:\ -Filter edit.txt).Name -ceq 'EDIT.TXT') "$((Get-ChildItem Z:\ -Filter edit.txt).Name)"
  Remove-Item Z:\EDIT.TXT, Z:\edit.bak
} catch { say "FAIL edit $_" }

# 5. The drive itself.
try {
  $v = [IO.DriveInfo]'Z:'
  check space (($v.TotalSize -gt 0) -and ($v.AvailableFreeSpace -gt 0)) ("total {0:N0} GB free {1:N0} GB fs {2}" -f ($v.TotalSize / 1GB), ($v.AvailableFreeSpace / 1GB), $v.DriveFormat)
  $c = Get-SmbConnection -ServerName 10.0.2.4 | Where-Object ShareName -eq host
  check connection ($c.Signed) "dialect=$($c.Dialect) signed=$($c.Signed)"
} catch { say "FAIL drive $_" }

# 5b. Caching (leases): the same 512 MB hashed twice through the share,
# and two caches that must not go stale: after another open writes the
# file, and after the host rewrites it (the harness, every 3 s).
try {
  Copy-Item "$local\big.bin" Z:\cache.bin
  $t = [Diagnostics.Stopwatch]::StartNew(); $h1 = (Get-FileHash Z:\cache.bin).Hash; $a = $t.Elapsed.TotalSeconds
  $t = [Diagnostics.Stopwatch]::StartNew(); $h2 = (Get-FileHash Z:\cache.bin).Hash; $b = $t.Elapsed.TotalSeconds
  $h0 = (Get-FileHash "$local\big.bin").Hash
  check hash-twice (($h1 -eq $h0) -and ($h2 -eq $h0)) ("512 MB: first {0:N1} s, second {1:N1} s" -f $a, $b)
  Remove-Item Z:\cache.bin
} catch { say "FAIL hash-twice $_" }
try {
  Set-Content Z:\coh.txt -Value 'one' -NoNewline
  $fs = [IO.File]::Open('Z:\coh.txt', 'Open', 'Read', 'ReadWrite, Delete')
  $sr = New-Object IO.StreamReader($fs)
  $v1 = $sr.ReadToEnd()
  [IO.File]::WriteAllText('Z:\coh.txt', 'two')
  Start-Sleep -Milliseconds 500
  $fs.Seek(0, 'Begin') | Out-Null; $sr.DiscardBufferedData(); $v2 = $sr.ReadToEnd()
  $fs.Close()
  check coherent-write (($v1 -eq 'one') -and ($v2 -eq 'two')) "$v1 -> $v2"
  Remove-Item Z:\coh.txt
} catch { say "FAIL coherent-write $_" }
try {
  $fs = [IO.File]::Open('Z:\live.txt', 'Open', 'Read', 'ReadWrite, Delete')
  $sr = New-Object IO.StreamReader($fs)
  $v1 = $sr.ReadToEnd().Trim()
  Start-Sleep 5
  $fs.Seek(0, 'Begin') | Out-Null; $sr.DiscardBufferedData(); $v2 = $sr.ReadToEnd().Trim()
  $fs.Close()
  check coherent-host ($v1 -ne $v2) "$v1 -> $v2"
} catch { say "FAIL coherent-host $_" }

# 6. Change notification: the harness drops a file into hostwatch every
# few seconds on the host; .NET's watcher (CHANGE_NOTIFY underneath) must
# report it as Created (the server's change records, not "enumerate
# again", which the watcher raises only as an Error event).
try {
  $fw = New-Object IO.FileSystemWatcher 'Z:\hostwatch'
  $fw.IncludeSubdirectories = $false
  $fw.EnableRaisingEvents = $true
  $t = [Diagnostics.Stopwatch]::StartNew()
  $res = $fw.WaitForChanged([IO.WatcherChangeTypes]::All, 20000)
  $fw.Dispose()
  check notify ((-not $res.TimedOut) -and $res.ChangeType -eq 'Created' -and $res.Name -like 'tick-*') ("{0} {1} after {2:N1} s" -f $res.ChangeType, $res.Name, $t.Elapsed.TotalSeconds)
} catch { say "FAIL notify $_" }

# 7. Explorer's own window on the share, for the closing screenshot. The
# signed-in user's unelevated session has no Z: (net use above mapped it
# for this elevated one), so a one-shot task in that session maps it too
# and opens Explorer there.
try {
  $cmd = 'cmd /c net use Y: \\10.0.2.4\host /user:smb smb & start explorer Y:\gsrc'
  schtasks /create /f /tn smbshow /tr $cmd /sc once /st 23:59 /it | Out-Null
  schtasks /run /tn smbshow | Out-Null
  check explorer ($LASTEXITCODE -eq 0) ""
} catch { say "FAIL explorer $_" }
say "done"
