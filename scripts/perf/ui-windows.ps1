# Scroll and memory runs of the Windows app over a stress test library (kithcore/stress.h), for
# the performance workflow (.github/workflows/perf.yml) and for running by hand.
#
#   scripts\perf\ui-windows.ps1 -Exe <kithsong.exe> -Library <stress library> -Out <folder>
#       [-Label head] [-Runs 3] [-Start 1] [-Passes 5] [-Width 1280] [-Height 800] [-Visible]
#
# Each run is a fresh process with the profiler on (KITHSONG_PERF) and KITHSONG_PERF_RUN set, so
# the app runs the scroll test -Passes times over (KITHSONG_PERF_PASSES), saves the report as
# <Out>\<Label>-<n>.txt (n counting from -Start) and closes. The library is copied fresh for each
# run, as opening one can write to it, and the exe is copied out of the checkout, as a build
# inside one runs as a source run. LOCALAPPDATA points at a scratch folder whose settings.json names the library: the real
# %LOCALAPPDATA%\kithsong is never read.
#
# Nothing here takes the foreground or sends input: the app runs on a desktop of its own
# (CreateDesktop). That desktop is never composed, so DwmFlush doesn't wait for a refresh and
# frame times are the work each frame took. -Visible runs it on the normal desktop, behind the
# other windows, where DWM paces the frames: CI's runner has nobody at it, so it uses that.
# Compare runs taken the same way only.
param(
  [Parameter(Mandatory)] [string] $Exe,
  [Parameter(Mandatory)] [string] $Library,
  [Parameter(Mandatory)] [string] $Out,
  [string] $Label = 'head',
  [int] $Runs = 3,
  [int] $Start = 1,
  [int] $Passes = 5,
  [int] $Width = 1280,
  [int] $Height = 800,
  [int] $TimeoutSeconds = 600,
  [switch] $Visible
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "No $Exe." }
if (-not (Test-Path (Join-Path $Library 'kithsong-stress-library.txt'))) { throw "$Library isn't a stress test library (no kithsong-stress-library.txt)." }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class PerfRun {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct STARTUPINFO {
    public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }
  [StructLayout(LayoutKind.Sequential)]
  struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr mode, int flags, uint access, IntPtr sa);
  [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desk);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, int flags,
                                    IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

  static IntPtr desktop = IntPtr.Zero;
  static string desktopName;

  // The app inherits this process's environment, so LOCALAPPDATA and the KITHSONG_ variables
  // are set before this.
  public static int Start(string exe, string args, bool visible) {
    var si = new STARTUPINFO();
    si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
    si.dwFlags = 1;        // STARTF_USESHOWWINDOW
    si.wShowWindow = 4;    // SW_SHOWNOACTIVATE
    if (!visible) {
      if (desktop == IntPtr.Zero) {
        desktopName = "kithsong-perf-" + Guid.NewGuid().ToString("N").Substring(0, 8);
        desktop = CreateDesktopW(desktopName, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000 /* GENERIC_ALL */, IntPtr.Zero);
        if (desktop == IntPtr.Zero) throw new Exception("CreateDesktop failed: " + Marshal.GetLastWin32Error());
      }
      si.lpDesktop = desktopName;
    }
    var cmd = new StringBuilder("\"" + exe + "\" " + args);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, null, ref si, out pi))
      throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }

  public static void EndDesktop() {
    if (desktop != IntPtr.Zero) CloseDesktop(desktop);
    desktop = IntPtr.Zero;
  }
}
'@

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('kithsong-perf-' + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force $scratch, $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$Library = (Resolve-Path $Library).Path
$app = Join-Path $scratch 'kithsong.exe'
Copy-Item $Exe $app

$saved = @{}
foreach ($name in 'LOCALAPPDATA', 'KITHSONG_PERF', 'KITHSONG_PERF_RUN', 'KITHSONG_PERF_PASSES', 'KITHSONG_THEME') {
  $saved[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
  for ($run = $Start; $run -lt $Start + $Runs; $run++) {
    $local = Join-Path $scratch "run-$run"
    $config = Join-Path $local 'kithsong'
    $lib = Join-Path $local 'library'
    New-Item -ItemType Directory -Force $config | Out-Null
    Copy-Item -Recurse $Library $lib
    # No BOM, which WriteAllText leaves out.
    $settings = @{ library_dir = $lib; device_name = 'Perf runner'; firewall_dont_ask = $true } | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $config 'settings.json'), $settings)
    $report = Join-Path $Out "$Label-$run.txt"
    Remove-Item -Force $report -ErrorAction SilentlyContinue
    $env:LOCALAPPDATA = $local
    $env:KITHSONG_PERF = '1'
    $env:KITHSONG_PERF_RUN = $report
    $env:KITHSONG_PERF_PASSES = "$Passes"
    $env:KITHSONG_THEME = 'light'
    $began = Get-Date
    $procId = [PerfRun]::Start($app, "--window-size ${Width}x$Height", [bool]$Visible)
    try {
      while (Get-Process -Id $procId -ErrorAction SilentlyContinue) {
        if (((Get-Date) - $began).TotalSeconds -gt $TimeoutSeconds) { throw "$Label run $run took over $TimeoutSeconds s." }
        Start-Sleep -Milliseconds 250
      }
    } finally {
      Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path $report)) { throw "$Label run $run ended without a report." }
    Write-Host ("{0} run {1}: {2:N0} s" -f $Label, $run, ((Get-Date) - $began).TotalSeconds)
    # The library database can stay locked for a moment after exit.
    for ($i = 0; $i -lt 10 -and (Test-Path $local); $i++) {
      try { Remove-Item -Recurse -Force $local -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
  }
} finally {
  foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
  [PerfRun]::EndDesktop()
  for ($i = 0; $i -lt 10 -and (Test-Path $scratch); $i++) {
    try { Remove-Item -Recurse -Force $scratch -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
  }
}
