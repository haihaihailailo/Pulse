param([string]$Out, [string]$Proc = 'Pulse', [int]$ClickX = -1, [int]$ClickY = -1)
# 截取指定进程的主窗口（含周围 40px）；可选：先在窗口内的物理像素坐标处点击一次
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class PW2 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern void mouse_event(int f, int x, int y, int d, int e);
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
}
"@
[PW2]::SetProcessDPIAware() | Out-Null
$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { "window not found"; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object PW2+RECT
[PW2]::GetWindowRect($h, [ref]$r) | Out-Null
if ($ClickX -ge 0) {
  $old = New-Object PW2+POINT; [PW2]::GetCursorPos([ref]$old) | Out-Null
  [PW2]::SetCursorPos($r.L + $ClickX, $r.T + $ClickY) | Out-Null
  Start-Sleep -Milliseconds 80
  [PW2]::mouse_event(2, 0, 0, 0, 0); [PW2]::mouse_event(4, 0, 0, 0, 0)
  Start-Sleep -Milliseconds 80
  [PW2]::SetCursorPos($old.X, $old.Y) | Out-Null
  Start-Sleep -Milliseconds 2500
  [PW2]::GetWindowRect($h, [ref]$r) | Out-Null
}
"rect $($r.L),$($r.T),$($r.R),$($r.B)  size $($r.R - $r.L)x$($r.B - $r.T)"
$pad = 40; $x = $r.L - $pad; $y = [Math]::Max(0, $r.T - $pad); $w = $r.R - $r.L + 2 * $pad; $hh = $r.B - $r.T + 2 * $pad
$bmp = New-Object System.Drawing.Bitmap $w, $hh
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($x, $y, 0, 0, $bmp.Size)
$bmp.Save($Out)
