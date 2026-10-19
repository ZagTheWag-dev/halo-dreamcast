# Records a video of a disc image running in Flycast: the window's picture by
# PrintWindow (the window alone, never the screen), at a constant frame rate
# (a frame captured late repeats the last one), piped to ffmpeg, and
# Flycast's sound (SDL's "disk" driver, as dc_flycast_test.ps1 -audio) muxed
# in, cut to start where the picture does. Then Flycast is closed.
#
#   powershell -ExecutionPolicy Bypass -File tools\dc_flycast_record.ps1 -disc <cdi> -out <mp4> [-seconds 60] [-fps 30] [-skip 0]
#
# -skip: seconds after Flycast starts before recording begins.
param([string]$disc, [string]$out, [int]$seconds = 60, [int]$fps = 30, [double]$skip = 0, [int]$width = 960, [int]$height = 720,
      [string]$ffmpeg = "C:\Tools\ffmpeg\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe")
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Text; using System.Diagnostics; using System.Drawing; using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class Rec {
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
 // returns "frames captured/frames written"
 public static string Record(IntPtr h, int seconds, int fps, string ffmpeg, string video) {
  RECT r; GetClientRect(h, out r); int w = r.R & ~1, hh = r.B & ~1;
  var psi = new ProcessStartInfo(ffmpeg, String.Format("-y -loglevel error -f rawvideo -pix_fmt bgra -s {0}x{1} -r {2} -i - -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p \"{3}\"", w, hh, fps, video));
  psi.UseShellExecute = false; psi.RedirectStandardInput = true; psi.CreateNoWindow = true;
  var p = Process.Start(psi); var input = p.StandardInput.BaseStream;
  var bmp = new Bitmap(r.R, r.B, PixelFormat.Format32bppArgb); var g = Graphics.FromImage(bmp);
  byte[] frame = new byte[w * hh * 4]; int total = seconds * fps, written = 0, captured = 0;
  var clock = Stopwatch.StartNew();
  while (written < total) {
   IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); captured++;
   var data = bmp.LockBits(new Rectangle(0, 0, w, hh), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
   for (int y = 0; y < hh; y++) Marshal.Copy(data.Scan0 + y * data.Stride, frame, y * w * 4, w * 4);
   bmp.UnlockBits(data);
   // every frame slot up to now gets this picture (repeats when behind)
   long due = (long)(clock.Elapsed.TotalSeconds * fps) + 1;
   if (due > total) due = total;
   do { input.Write(frame, 0, frame.Length); written++; } while (written < due);
   long next = (long)written * 1000 / fps - (long)clock.Elapsed.TotalMilliseconds;
   if (next > 0) System.Threading.Thread.Sleep((int)next);
  }
  input.Close(); p.WaitForExit();
  return captured + "/" + written;
 }
}
"@
$raw = [IO.Path]::ChangeExtension($out, ".raw"); $video = [IO.Path]::ChangeExtension($out, ".video.mp4")
Remove-Item $raw, $video -ErrorAction SilentlyContinue
$env:SDL_AUDIODRIVER = "disk"; $env:SDL_DISKAUDIOFILE = $raw
$p = Start-Process -FilePath "C:\Tools\flycast\flycast.exe" -ArgumentList "-config audio:backend=sdl2 -config window:maximized=no -config window:width=$width -config window:height=$height `"$disc`"" -PassThru
$clock = [Diagnostics.Stopwatch]::StartNew()
$h = [IntPtr]::Zero
while ($h -eq [IntPtr]::Zero -and $clock.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 100; $h = [Rec]::Game() }
if ($skip -gt $clock.Elapsed.TotalSeconds) { Start-Sleep -Milliseconds ([int](($skip - $clock.Elapsed.TotalSeconds) * 1000)) }
# the sound file's bytes so far = where the picture starts in it
Start-Sleep -Milliseconds 200
$offset = (Get-Item $raw -ErrorAction SilentlyContinue).Length
$stats = [Rec]::Record($h, $seconds, $fps, $ffmpeg, $video)
Get-Process flycast -ErrorAction SilentlyContinue | Stop-Process -Force
$start = if ($offset) { $offset / (44100 * 4) } else { 0 }
& $ffmpeg -y -loglevel error -i $video -f s16le -ar 44100 -ac 2 -ss $start -i $raw -map 0:v -map 1:a -c:v copy -c:a aac -b:a 192k -shortest $out
"frames $stats, sound from $([math]::Round($start, 2)) s"
