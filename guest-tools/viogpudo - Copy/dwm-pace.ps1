# dwm-pace.ps1 [out] [seconds]: DWM's pace (track M24), in the interactive
# session (DWM is not in SYSTEM's; a scheduled task as the logged-on user
# runs it). It lists the screens, animates a window so DWM has a frame to
# compose at every refresh, and writes to <out> (C:KSBOX\dwm-pace.txt):
# - DwmGetCompositionTimingInfo's refresh rate and period (16.667 ms on a
#   60 Hz blank, 15.625 ms on dxgkrnl's simulated one), then a line a
#   second of its counters, which Windows 11 advances only once a second
#   whatever DWM does, so they are not the measure;
# - the measure: 300 DwmFlush calls, each returning at DWM's next
#   composition, their intervals' mean, median, percentiles and a
#   histogram by millisecond.
# Only the primary screen is DWM's clock: on an x64 machine with the
# standard VGA beside the virtio-gpu, make the virtio screen the only one
# first (DisplaySwitch /internal there).
#
# SPDX-License-Identifier: GPL-2.0-or-later
param([string]$Out = "C:\2KSBOX\dwm-pace.txt", [int]$Seconds = 10)
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class DwmPace {
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct UNSIGNED_RATIO { public uint num; public uint den; }
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct DWM_TIMING_INFO {
        public uint cbSize;
        public UNSIGNED_RATIO rateRefresh;
        public ulong qpcRefreshPeriod;
        public UNSIGNED_RATIO rateCompose;
        public ulong qpcVBlank;
        public ulong cRefresh;
        public uint cDXRefresh;
        public ulong qpcCompose;
        public ulong cFrame;
        public uint cDXPresent;
        public ulong cRefreshFrame;
        public ulong cFrameSubmitted;
        public uint cDXPresentSubmitted;
        public ulong cFrameConfirmed;
        public uint cDXPresentConfirmed;
        public ulong cRefreshConfirmed;
        public uint cDXRefreshConfirmed;
        public ulong cFramesLate;
        public uint cFramesOutstanding;
        public ulong cFrameDisplayed;
        public ulong qpcFrameDisplayed;
        public ulong cRefreshFrameDisplayed;
        public ulong cFrameComplete;
        public ulong qpcFrameComplete;
        public ulong cFramePending;
        public ulong qpcFramePending;
        public ulong cFramesDisplayed;
        public ulong cFramesComplete;
        public ulong cFramesPending;
        public ulong cFramesAvailable;
        public ulong cFramesDropped;
        public ulong cFramesMissed;
        public ulong cRefreshNextDisplayed;
        public ulong cRefreshNextPresented;
        public ulong cRefreshesDisplayed;
        public ulong cRefreshesPresented;
        public ulong cRefreshStarted;
        public ulong cPixelsReceived;
        public ulong cPixelsDrawn;
        public ulong cBuffersEmpty;
    }
    [DllImport("dwmapi.dll")]
    public static extern int DwmFlush();
    // n DwmFlush calls (each returns at DWM's next composition): the
    // intervals between returns, in ms
    public static double[] Flushes(int n) {
        double[] r = new double[n];
        long f = System.Diagnostics.Stopwatch.Frequency;
        DwmFlush();
        long t = System.Diagnostics.Stopwatch.GetTimestamp();
        for (int i = 0; i < n; i++) {
            DwmFlush();
            long u = System.Diagnostics.Stopwatch.GetTimestamp();
            r[i] = (u - t) * 1000.0 / f; t = u;
        }
        return r;
    }
    [DllImport("dwmapi.dll")]
    public static extern int DwmGetCompositionTimingInfo(IntPtr hwnd, ref DWM_TIMING_INFO info);
    public static DWM_TIMING_INFO Get() {
        DWM_TIMING_INFO t = new DWM_TIMING_INFO();
        t.cbSize = (uint)Marshal.SizeOf(typeof(DWM_TIMING_INFO));
        int hr = DwmGetCompositionTimingInfo(IntPtr.Zero, ref t);
        if (hr != 0) throw new Exception("DwmGetCompositionTimingInfo: 0x" + hr.ToString("x8"));
        return t;
    }
}
"@
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$lines = @()
foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
    $lines += "screen {0} primary={1} bounds={2}" -f $s.DeviceName, $s.Primary, $s.Bounds
}
# A window that repaints on every timer tick, so DWM has a frame to
# compose at every refresh it can.
$form = New-Object System.Windows.Forms.Form
$form.Text = "dwm-pace"; $form.TopMost = $true
$form.StartPosition = "Manual"; $form.Location = New-Object System.Drawing.Point(100, 100)
$form.Size = New-Object System.Drawing.Size(400, 300)
$script:n = 0
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1
$timer.Add_Tick({ $script:n++; $form.BackColor = [System.Drawing.Color]::FromArgb(($script:n * 7) % 256, ($script:n * 3) % 256, 128) })
$form.Show(); $timer.Start()
$freq = [Diagnostics.Stopwatch]::Frequency
$prev = [DwmPace]::Get()
$t0 = [Diagnostics.Stopwatch]::GetTimestamp()
$lines += "rateRefresh {0}/{1}  qpcRefreshPeriod {2:N3} ms  rateCompose {3}/{4}" -f `
    $prev.rateRefresh.num, $prev.rateRefresh.den, ($prev.qpcRefreshPeriod * 1000.0 / $freq), `
    $prev.rateCompose.num, $prev.rateCompose.den
for ($i = 0; $i -lt $Seconds; $i++) {
    $until = [Diagnostics.Stopwatch]::GetTimestamp() + $freq
    while ([Diagnostics.Stopwatch]::GetTimestamp() -lt $until) {
        [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 1
    }
    $cur = [DwmPace]::Get()
    $t1 = [Diagnostics.Stopwatch]::GetTimestamp()
    $dt = ($t1 - $t0) / [double]$freq
    $dr = $cur.cRefresh - $prev.cRefresh
    $df = $cur.cFrame - $prev.cFrame
    $vb = if ($dr) { ($cur.qpcVBlank - $prev.qpcVBlank) * 1000.0 / $freq / $dr } else { 0 }
    $lines += "{0,5:N2} s  refreshes/s {1,6:N1}  frames/s {2,6:N1}  vblank interval {3,7:N3} ms" -f `
        $dt, ($dr / $dt), ($df / $dt), $vb
    $prev = $cur; $t0 = $t1
}
# DwmFlush's pace, the window still animating between batches
$all = @()
for ($b = 0; $b -lt 6; $b++) {
    [System.Windows.Forms.Application]::DoEvents()
    $all += [DwmPace]::Flushes(50)
}
$s = $all | Sort-Object
$lines += "DwmFlush intervals ({0}): mean {1:N3} ms  median {2:N3}  p5 {3:N3}  p95 {4:N3}  min {5:N3}  max {6:N3}" -f `
    $all.Count, ($all | Measure-Object -Average).Average, $s[[int]($s.Count / 2)], `
    $s[[int]($s.Count * 0.05)], $s[[int]($s.Count * 0.95)], $s[0], $s[-1]
$hist = $all | Group-Object { [math]::Round($_, 0) } | Sort-Object { [double]$_.Name }
$lines += "  by ms: " + (($hist | ForEach-Object { "$($_.Name):$($_.Count)" }) -join "  ")
$timer.Stop(); $form.Close()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$lines | Set-Content -Encoding ascii $Out
