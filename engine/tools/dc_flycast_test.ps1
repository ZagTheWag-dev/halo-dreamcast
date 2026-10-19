# Boots a disc image in Flycast, saves captures of its picture (PrintWindow on
# the window titled "Flycast - ...": the window alone, never the screen) and
# the game's log (Flycast's "Flycast Serial Output" console, read through
# AttachConsole), then closes Flycast. Needs emu.cfg's
# Debug.SerialConsoleEnabled = yes.
#
#   powershell -ExecutionPolicy Bypass -File tools\dc_flycast_test.ps1 -disc <cdi> -log <txt> -shot <png> [-wait 40] [-shots 1] [-every 10]
#
# With -shots N the captures are <shot>-0.png ... <shot>-(N-1).png.
#
# With -audio <raw> Flycast's sound goes to that file instead of the speakers
# (its SDL2 backend with SDL's "disk" driver: 16-bit stereo at 44100 Hz, no
# header; tools/dc_audio.py measures it). Without it the run is silent too:
# Flycast's "null" backend, so a test never plays out loud.
param([string]$disc, [string]$log, [string]$shot, [int]$wait = 40, [int]$shots = 1, [int]$every = 10, [string]$audio = "")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class T5 {
 [DllImport("kernel32.dll")] public static extern bool FreeConsole();
 [DllImport("kernel32.dll")] public static extern bool AttachConsole(uint pid);
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
 [StructLayout(LayoutKind.Sequential)] public struct COORD { public short X, Y; }
 [StructLayout(LayoutKind.Sequential)] public struct SMALL_RECT { public short L, T, R, B; }
 [StructLayout(LayoutKind.Sequential)] public struct INFO { public COORD Size, Cursor; public short Attr; public SMALL_RECT Win; public COORD Max; }
 [DllImport("kernel32.dll")] public static extern bool GetConsoleScreenBufferInfo(IntPtr h, out INFO i);
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool ReadConsoleOutputCharacter(IntPtr h, StringBuilder b, uint n, COORD c, out uint r);
 public struct RECT { public int L, T, R, B; }
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 public static IntPtr Game() { IntPtr found = IntPtr.Zero;
  EnumWindows((h, l) => { var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
   if (IsWindowVisible(h) && sb.ToString().StartsWith("Flycast - ")) { found = h; return false; } return true; }, IntPtr.Zero);
  return found; }
 public static string Dump(uint pid) {
  FreeConsole(); if (!AttachConsole(pid)) return "";
  IntPtr h = CreateFile("CONOUT$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
  INFO i; if (!GetConsoleScreenBufferInfo(h, out i)) { FreeConsole(); return ""; }
  var sb = new StringBuilder();
  for (short y = 0; y <= i.Cursor.Y; y++) { var line = new StringBuilder(i.Size.X); uint r; COORD c; c.X = 0; c.Y = y;
   ReadConsoleOutputCharacter(h, line, (uint)i.Size.X, c, out r); sb.AppendLine(line.ToString(0, (int)r).TrimEnd()); }
  FreeConsole(); return sb.ToString(); }
}
"@
if ($audio) {
 $env:SDL_AUDIODRIVER = "disk"; $env:SDL_DISKAUDIOFILE = $audio
 $backend = "sdl2"
} else { $backend = "null" }
$p = Start-Process -FilePath "C:\Tools\flycast\flycast.exe" -ArgumentList "-config audio:backend=$backend `"$disc`"" -PassThru
Start-Sleep -Seconds $wait
$h = [T5]::Game()
for ($n = 0; $n -lt $shots; $n++) {
 if ($n -gt 0) { Start-Sleep -Seconds $every }
 if ($shot -and $h -ne [IntPtr]::Zero) {
  $r = New-Object T5+RECT; [T5]::GetClientRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R), ($r.B)
  $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
  [T5]::PrintWindow($h, $dc, 3) | Out-Null   # client area + full DX content, the window alone
  $g.ReleaseHdc($dc)
  $name = if ($shots -gt 1) { $shot -replace '\.png$', "-$n.png" } else { $shot }
  $bmp.Save($name) } }
$text = ""
foreach ($q in (Get-Process flycast -ErrorAction SilentlyContinue)) { $t = [T5]::Dump([uint32]$q.Id); if ($t.Length -gt $text.Length) { $text = $t } }
if ($log) { [IO.File]::WriteAllText($log, $text) }
Get-Process flycast -ErrorAction SilentlyContinue | Stop-Process -Force
"done $($text.Length) chars"
