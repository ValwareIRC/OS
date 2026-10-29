# Boots the OS in the Windows QEMU a few times, with and without the
# clipboard bridge, and reports which boots reached user space.
param([int]$Runs = 3, [string]$Iso = "", [switch]$BridgeOnly)
# A scratch disk, so trials never touch the person's own the OS.
$scratch = Join-Path $env:TEMP "os-trial-disk.img"
$root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $root "build\os-vm-serial.log"
function Kill-All {
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
        Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'File .*clipboard-bridge' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    Start-Sleep 2
}
$modes = if ($BridgeOnly) { @($true) } else { @($true, $false) }
foreach ($bridge in $modes) {
    for ($i = 1; $i -le $Runs; $i++) {
        Kill-All
        if ($bridge) {
            Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile','-STA','-ExecutionPolicy','Bypass','-File',(Join-Path $root 'scripts\clipboard-bridge.ps1')
        }
        $a = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $root 'scripts\os-win.ps1'))
        if ($Iso) { $a += @('-Iso', $Iso) }
        Remove-Item $scratch -ErrorAction SilentlyContinue
        $a += @('-Disk', $scratch)
        Start-Process powershell -WindowStyle Minimized -ArgumentList $a
        $ok = $false
        for ($t = 0; $t -lt 90; $t++) {
            Start-Sleep 1
            if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'shell: screen' -Quiet)) { $ok = $true; break }
        }
        $last = if (Test-Path $log) { (Get-Content $log | Where-Object { $_ -match '\S' } | Select-Object -Last 1) } else { "(no log)" }
        $resp = (Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Select-Object -First 1).Responding
        "bridge=$bridge run=$i reached_choose=$ok seconds=$t responding=$resp last='$last'"
    }
}
Kill-All
