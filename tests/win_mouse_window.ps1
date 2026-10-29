# Like tests\win_mouse_boot.mjs, but with QEMU's real SDL window (as
# scripts\os-win.ps1 runs it): mouse-move messages are posted straight to that
# window during boot (PostMessage; the person's real cursor never moves).
# Reports how far the boot got and whether QEMU stayed alive and responding.
#   powershell -File tests\win_mouse_window.ps1 [-Iso path] [-Display sdl] [-Seconds 120] [-NoMouse]
param([string]$Iso = "", [string]$Display = "sdl", [int]$Seconds = 120, [switch]$NoMouse)
$root = Split-Path -Parent $PSScriptRoot
if (-not $Iso) { $Iso = Join-Path $root "build\os.iso" }
$log = Join-Path $root "build\win-mouse-window.log"
Remove-Item $log -ErrorAction SilentlyContinue
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
}
"@
$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$args = @("-name", "OS-mouse-test", "-M", "q35,vmport=on", "-m", "8G", "-smp", "4",
          "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg,thread=multi", "-cpu", "Skylake-Client-noTSX-IBRS",
          "-vga", "std", "-display", $Display, "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
          "-serial", ("file:" + ($log -replace "\\", "/")), "-cdrom", $Iso)
$p = Start-Process $qemu -ArgumentList $args -PassThru
$t0 = Get-Date; $hwnd = [IntPtr]::Zero; $sent = 0; $hungSeen = 0; $inputAt = $null; $okAt = $null; $x = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    $p.Refresh()
    if ($hwnd -eq [IntPtr]::Zero) { $hwnd = $p.MainWindowHandle }
    if ($hwnd -ne [IntPtr]::Zero -and -not $NoMouse) {
        for ($k = 0; $k -lt 50; $k++) {
            $x = ($x + 7) % 600
            $lp = [IntPtr](($x + 100) -bor ((200 + ($x % 300)) -shl 16))
            [void][W]::PostMessage($hwnd, 0x200, [IntPtr]::Zero, $lp)   # WM_MOUSEMOVE
            $sent++
        }
        if ([W]::IsHungAppWindow($hwnd)) { $hungSeen++ }
    }
    if (Test-Path $log) {
        $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
        $s = [int]((Get-Date) - $t0).TotalSeconds
        if (-not $inputAt -and $text -match "input: ") { $inputAt = $s }
        if (-not $okAt -and $text -match "OS BOOT OK") { $okAt = $s; break }
    }
    Start-Sleep -Milliseconds 20
}
$last = if (Test-Path $log) { (Get-Content $log -Tail 1) } else { "" }
$state = if ($p.HasExited) { "EXITED (code $($p.ExitCode))" } else { "running" }
Write-Host ("display {0}, mouse {1}: {2} moves posted; input set up at {3}; boot ok at {4}; seen hung {5} times; qemu {6}; last line: {7}" -f `
    $Display, -not $NoMouse, $sent, ($(if ($inputAt) { "$inputAt s" } else { "NEVER" })), ($(if ($okAt) { "$okAt s" } else { "NEVER" })), $hungSeen, $state, $last)
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
