# Boots the OS in the Windows QEMU while moving the mouse over its window,
# the way a person does while waiting, and reports which boots got through.
param([int]$Runs = 3, [string]$Iso = "")
Add-Type @"
using System; using System.Runtime.InteropServices;
public class M { [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,UIntPtr e);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
"@
$root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $root "build\os-vm-serial.log"
$scratch = Join-Path $env:TEMP "os-trial-disk.img"
for ($i = 1; $i -le $Runs; $i++) {
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    if (Test-Path $scratch) { [IO.File]::Delete($scratch) }
    $a = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $root 'scripts\os-win.ps1'),'-Disk',$scratch)
    if ($Iso) { $a += @('-Iso', $Iso) }
    Start-Process powershell -ArgumentList $a
    $ok = $false
    for ($t = 0; $t -lt 60; $t++) {
        $q = Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($q -and $q.MainWindowHandle -ne 0) { [M]::SetForegroundWindow($q.MainWindowHandle) | Out-Null }
        for ($k = 0; $k -lt 20; $k++) {
            [M]::SetCursorPos(700 + ($k * 13 % 200), 450 + ($k * 7 % 120)) | Out-Null
            [M]::mouse_event(1, 5, 3, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 40
        }
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'shell: screen' -Quiet)) { $ok = $true; break }
    }
    $last = if (Test-Path $log) { (Get-Content $log | Where-Object { $_.Trim() } | Select-Object -Last 1) } else { "(no log)" }
    $resp = (Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Select-Object -First 1).Responding
    "run=$i ok=$ok seconds=$t responding=$resp last='$last'"
}
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
