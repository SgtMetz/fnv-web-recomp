# Save a PNG of the relinked game's main window (PrintWindow, so it works when the window is covered).
# usage: powershell -File tools/screenshot.ps1 [-Out out/shot.png]
param([string]$Out = "E:\Games\Claude fnv\out\shot.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class FnvWin {
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(uint pid) {
    IntPtr best = IntPtr.Zero; int area = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r); int a = (r.R - r.L) * (r.B - r.T);
        var c = new StringBuilder(64); GetClassName(h, c, 64);
        if (a > area && c.ToString() != "ConsoleWindowClass" && c.ToString() != "PseudoConsoleWindow") { area = a; best = h; } }
      return true; }, IntPtr.Zero);
    return best; }
}
"@
$p = Get-Process FalloutNV_relinked -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no process"; exit 1 }
$h = [FnvWin]::Find([uint32]$p.Id)
if ($h -eq [IntPtr]::Zero) { "no window"; exit 1 }
$r = New-Object FnvWin+RECT
[FnvWin]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[FnvWin]::PrintWindow($h, $dc, 2) | Out-Null
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out ($w x $hh)"
