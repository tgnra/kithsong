# Store screenshots of the Windows app, from the demo library (kithcore/demo.h): each page in
# light and dark, as PNGs of the window's frame at a fixed size.
#
#   scripts\store-screenshots\windows.ps1 [-Exe <kithsong.exe>] [-Out <folder>]
#       [-Width 1600] [-Height 1000] [-Shots home,album,...] [-Visible]
#
# Shots: home, albums, album, artist and search, the library's pages; import, the import page
# part-way through a made-up import; review, the same with the review of the album that needs
# a decision open (the dialog drawn over the window where it shows); and sync, a made-up phone's
# page part-way through a sync with a selection as Opus. Each has a track paused part-way, as
# the store listing does: Copperline's first at 0:21 in light, Nightjar Season's at 0:08 in
# dark (--demo-playing). See kithcore/demo.h.
#
# The exe defaults to the release preset's build (cmake --preset windows-x64-release). It's
# copied out of the checkout first, as a build inside one runs as a source run, with its own
# settings folder and temp library. Each shot is a fresh run: LOCALAPPDATA points at a scratch
# folder whose settings.json names the device and turns off the firewall prompt, and
# --demo-library makes the library there. The real %LOCALAPPDATA%\kithsong is never read.
#
# Nothing here takes the foreground or sends input. The app runs on a desktop of its own
# (CreateDesktop), where it can't come in front of anything, and PrintWindow with
# PW_RENDERFULLCONTENT draws it there; -Visible runs it on the normal desktop instead, behind
# the other windows, which the script falls back to by itself if a shot comes out blank (a
# runner without an interactive session might). The capture is of the frame DWM draws, so the
# invisible resize borders are cut off, and it's flat: no Mica, square corners.
param(
  [string] $Exe,
  [string] $Out = 'out\windows',
  [int] $Width = 1600,
  [int] $Height = 1000,
  [string[]] $Shots = @('home', 'albums', 'album', 'artist', 'search', 'import', 'review', 'sync'),
  [string] $DeviceName = 'Living Room PC',
  [switch] $Visible
)
$ErrorActionPreference = 'Stop'
$Shots = $Shots -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } # -File passes "a,b" as one
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Exe) { $Exe = Join-Path $repo 'build\windows-x64-release\desktop\windows\kithsong.exe' }
if (-not (Test-Path $Exe)) { throw "No $Exe. Build it first: cmake --preset windows-x64-release; cmake --build --preset windows-x64-release --target kithsong" }

# What each shot opens (--page).
$pages = @{
  home   = 'home'
  albums = 'albums'
  album  = 'album:Copperline'
  artist = 'artist:Ivy Sunderholt'
  search = 'search:night'
  import = 'import'
  review = 'import-review'
  sync   = 'sync'
}
# The track paused in the now-playing strip, by theme (--demo-playing).
$playing = @{
  light = 'Copperline#1@0:21'
  dark  = 'Nightjar Season#1@0:08'
}

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public static class Shot {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct STARTUPINFO {
    public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }
  [StructLayout(LayoutKind.Sequential)]
  struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  delegate bool EnumProc(IntPtr wnd, IntPtr lp);

  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr mode, int flags, uint access, IntPtr sa);
  [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desk);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desk, EnumProc fn, IntPtr lp);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc fn, IntPtr lp);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr wnd, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr wnd, out int pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr wnd);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr wnd, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr wnd, IntPtr dc, uint flags);
  [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr wnd, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr wnd, IntPtr after, int x, int y, int w, int h, uint flags);
  [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr wnd, uint cmd);
  [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr wnd, int attr, out RECT r, int size);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, int flags,
                                    IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

  static IntPtr desktop = IntPtr.Zero;
  public static string DesktopName;

  // A desktop of its own, where the app can't take the foreground. The app inherits the
  // environment of this process, so LOCALAPPDATA and KITHSONG_THEME are set before this.
  public static int Start(string exe, string args, bool visible) {
    var si = new STARTUPINFO();
    si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
    si.dwFlags = 1;        // STARTF_USESHOWWINDOW
    si.wShowWindow = 4;    // SW_SHOWNOACTIVATE
    if (!visible) {
      if (desktop == IntPtr.Zero) {
        DesktopName = "kithsong-shots-" + Guid.NewGuid().ToString("N").Substring(0, 8);
        desktop = CreateDesktopW(DesktopName, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000 /* GENERIC_ALL */, IntPtr.Zero);
        if (desktop == IntPtr.Zero) throw new Exception("CreateDesktop failed: " + Marshal.GetLastWin32Error());
      }
      si.lpDesktop = DesktopName;
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

  // The app's main window: by class and process, never the first window it has.
  public static IntPtr Find(int pid, bool visible) {
    IntPtr found = IntPtr.Zero;
    EnumProc fn = (w, lp) => {
      var cls = new StringBuilder(32);
      int owner;
      GetWindowThreadProcessId(w, out owner);
      if (owner == pid && GetClassNameW(w, cls, 32) > 0 && cls.ToString() == "kithsong" && IsWindowVisible(w)) {
        found = w;
        return false;
      }
      return true;
    };
    if (visible) EnumWindows(fn, IntPtr.Zero); else EnumDesktopWindows(desktop, fn, IntPtr.Zero);
    GC.KeepAlive(fn);
    return found;
  }

  // A dialog the main window owns (the import review), or zero.
  public static IntPtr FindOwned(int pid, IntPtr owner, bool visible) {
    IntPtr found = IntPtr.Zero;
    EnumProc fn = (w, lp) => {
      var cls = new StringBuilder(32);
      int pidOf;
      GetWindowThreadProcessId(w, out pidOf);
      if (pidOf == pid && IsWindowVisible(w) && GetWindow(w, 4 /* GW_OWNER */) == owner &&
          GetClassNameW(w, cls, 32) > 0 && cls.ToString() == "#32770") {
        found = w;
        return false;
      }
      return true;
    };
    if (visible) EnumWindows(fn, IntPtr.Zero); else EnumDesktopWindows(desktop, fn, IntPtr.Zero);
    GC.KeepAlive(fn);
    return found;
  }

  static RECT Frame(IntPtr wnd) {
    RECT r;
    if (DwmGetWindowAttribute(wnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, out r, 16) != 0) GetWindowRect(wnd, out r);
    return r;
  }

  // The dialog drawn over the window's capture where it shows, as far as it's inside.
  public static void DrawOver(Bitmap main, IntPtr mainWnd, IntPtr dlg) {
    RECT m = Frame(mainWnd), d = Frame(dlg);
    using (var over = Capture(dlg))
    using (var g = Graphics.FromImage(main)) g.DrawImage(over, d.Left - m.Left, d.Top - m.Top, over.Width, over.Height);
  }

  public static void SendToBack(IntPtr wnd) {
    SetWindowPos(wnd, new IntPtr(1) /* HWND_BOTTOM */, 0, 0, 0, 0, 0x0013 /* NOSIZE|NOMOVE|NOACTIVATE */);
  }

  // The window's frame as DWM draws it, without the invisible resize borders.
  public static Bitmap Capture(IntPtr wnd) {
    RECT outer, frame;
    if (!GetWindowRect(wnd, out outer)) throw new Exception("The window has gone.");
    if (DwmGetWindowAttribute(wnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, out frame, 16) != 0) frame = outer;
    int w = outer.Right - outer.Left, h = outer.Bottom - outer.Top;
    using (var whole = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
      using (var g = Graphics.FromImage(whole)) {
        IntPtr dc = g.GetHdc();
        bool ok = PrintWindow(wnd, dc, 2 /* PW_RENDERFULLCONTENT */);
        g.ReleaseHdc(dc);
        if (!ok) throw new Exception("PrintWindow failed.");
      }
      var crop = new Rectangle(frame.Left - outer.Left, frame.Top - outer.Top, frame.Right - frame.Left, frame.Bottom - frame.Top);
      crop.Intersect(new Rectangle(0, 0, w, h));
      var bmp = new Bitmap(crop.Width, crop.Height, PixelFormat.Format24bppRgb);
      using (var g = Graphics.FromImage(bmp)) g.DrawImage(whole, new Rectangle(0, 0, crop.Width, crop.Height), crop, GraphicsUnit.Pixel);
      return bmp;
    }
  }

  public static string Hash(Bitmap bmp) {
    var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
    try {
      var bytes = new byte[data.Stride * bmp.Height];
      Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
      using (var sha = System.Security.Cryptography.SHA1.Create()) return Convert.ToBase64String(sha.ComputeHash(bytes));
    } finally { bmp.UnlockBits(data); }
  }

  // Whether every pixel is the same, as a capture of nothing drawn is.
  public static bool Blank(Bitmap bmp) {
    Color first = bmp.GetPixel(0, 0);
    for (int y = 0; y < bmp.Height; y += 7)
      for (int x = 0; x < bmp.Width; x += 7)
        if (bmp.GetPixel(x, y) != first) return false;
    return true;
  }

  public static void Close(IntPtr wnd) { PostMessageW(wnd, 0x0010 /* WM_CLOSE */, IntPtr.Zero, IntPtr.Zero); }
}
'@

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('kithsong-shots-' + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force $scratch, $Out | Out-Null
$Out = (Resolve-Path $Out).Path
# Out of the checkout, so it runs as an installed copy would.
$app = Join-Path $scratch 'kithsong.exe'
Copy-Item $Exe $app

$savedLocal = $env:LOCALAPPDATA
$savedTheme = $env:KITHSONG_THEME
$failed = @()
try {
  foreach ($theme in 'light', 'dark') {
    foreach ($shot in $Shots) {
      if (-not $pages.ContainsKey($shot)) { throw "Unknown shot $shot; one of $($pages.Keys -join ', ')." }
      # A desktop of its own can draw nothing where there's no interactive session (a CI
      # runner may be one): then the shot is taken again, and the rest too, on the normal one.
      for ($attempt = 0; $attempt -lt 2; $attempt++) {
        $local = Join-Path $scratch "$shot-$theme-$attempt"
        $config = Join-Path $local 'kithsong'
        New-Item -ItemType Directory -Force $config | Out-Null
        # No BOM, which WriteAllText leaves out. No library_dir: --demo-library sets it.
        $settings = @{ device_name = $DeviceName; firewall_dont_ask = $true } | ConvertTo-Json
        [IO.File]::WriteAllText((Join-Path $config 'settings.json'), $settings)
        $env:LOCALAPPDATA = $local
        $env:KITHSONG_THEME = $theme
        $procId = [Shot]::Start($app, "--demo-library --window-size ${Width}x$Height --page `"$($pages[$shot])`" --demo-playing `"$($playing[$theme])`"", [bool]$Visible)
        $blank = $true
        try {
          $wnd = [IntPtr]::Zero
          for ($i = 0; $i -lt 300 -and $wnd -eq [IntPtr]::Zero; $i++) {
            Start-Sleep -Milliseconds 100
            if (-not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { throw "Kithsong exited before its window showed ($shot, $theme)." }
            $wnd = [Shot]::Find($procId, [bool]$Visible)
          }
          if ($wnd -eq [IntPtr]::Zero) {
            if ($Visible) { throw "No window after 30 s ($shot, $theme)." }
          } else {
            if ($Visible) { [Shot]::SendToBack($wnd) }
            # Settled once two captures a second apart are the same: covers load, the page opens.
            Start-Sleep -Seconds 2
            $last = $null
            $bmp = $null
            $dlgSeen = $false
            for ($i = 0; $i -lt 15; $i++) {
              if ($bmp) { $bmp.Dispose() }
              $bmp = [Shot]::Capture($wnd)
              if ($shot -eq 'review') {
                # Not settled until the dialog is up.
                $dlg = [Shot]::FindOwned($procId, $wnd, [bool]$Visible)
                if ($dlg -eq [IntPtr]::Zero) { $last = $null; $dlgSeen = $false; Start-Sleep -Seconds 1; continue }
                # Its controls come into the capture over a few seconds (demo_review_redraw).
                if (-not $dlgSeen) { $dlgSeen = $true; Start-Sleep -Seconds 5; $bmp.Dispose(); $bmp = [Shot]::Capture($wnd) }
                [Shot]::DrawOver($bmp, $wnd, $dlg)
              }
              $hash = [Shot]::Hash($bmp)
              if ($hash -eq $last -and -not [Shot]::Blank($bmp)) { break }
              $last = $hash
              Start-Sleep -Seconds 1
            }
            $blank = [Shot]::Blank($bmp)
            if (-not $blank) {
              $file = Join-Path $Out "windows-$shot-$theme.png"
              $bmp.Save($file, [Drawing.Imaging.ImageFormat]::Png)
              Write-Host "$file ($($bmp.Width)x$($bmp.Height))"
            }
            $bmp.Dispose()
            [Shot]::Close($wnd)
            for ($i = 0; $i -lt 100 -and (Get-Process -Id $procId -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
          }
        } finally {
          Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
        }
        if (-not $blank) { break }
        if ($Visible) {
          $failed += "$shot-$theme"
          break
        }
        Write-Warning "Nothing drawn on a desktop of its own ($shot, $theme): going on with -Visible."
        $Visible = [switch]$true
      }
    }
  }
} finally {
  $env:LOCALAPPDATA = $savedLocal
  $env:KITHSONG_THEME = $savedTheme
  [Shot]::EndDesktop()
  # The library database can stay locked for a moment after exit.
  for ($i = 0; $i -lt 10 -and (Test-Path $scratch); $i++) {
    try { Remove-Item -Recurse -Force $scratch -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
  }
}
if ($failed) { throw "Blank captures: $($failed -join ', ')." }
