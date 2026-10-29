# Boots the image in the Windows QEMU (WHPX, no window) while QEMU itself is
# fed mouse movement over QMP, and reports whether the boot got through.
# Never touches the person's own mouse or desktop.
param([Parameter(Mandatory)][string]$Iso, [int]$Runs = 3, [switch]$Abs, [string]$Irqchip = "off", [string]$Mem = "4G")
$root = Split-Path -Parent $PSScriptRoot
$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$log = Join-Path $root "build\mousetest-serial.log"
$scratch = Join-Path $env:TEMP "os-mousetest-disk.img"
for ($i = 1; $i -le $Runs; $i++) {
    if (Test-Path $log) { [IO.File]::Delete($log) }
    if (Test-Path $scratch) { [IO.File]::Delete($scratch) }
    $fs = [IO.File]::Create($scratch); $fs.SetLength(256MB); $fs.Close()
    $a = @("-M", "q35,vmport=on", "-m", $Mem, "-smp", "4",
           "-accel", "whpx,kernel-irqchip=$Irqchip", "-accel", "tcg,thread=multi", "-cpu", "Skylake-Client-noTSX-IBRS",
           "-vga", "std", "-display", "none", "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", ("file:" + ($log -replace "\\", "/")), "-qmp", "tcp:127.0.0.1:4466,server=on,wait=off",
           "-drive", ("id=disk0,file=" + ($scratch -replace "\\", "/") + ",format=raw,if=none"), "-device", "ide-hd,drive=disk0,bus=ide.0",
           "-cdrom", (Resolve-Path $Iso).Path)
    $p = Start-Process $qemu -ArgumentList $a -PassThru -WindowStyle Hidden
    Start-Sleep 1
    $c = $null
    for ($t = 0; $t -lt 20 -and -not $c; $t++) { try { $c = New-Object Net.Sockets.TcpClient("127.0.0.1", 4466) } catch { Start-Sleep -Milliseconds 250 } }
    $s = $c.GetStream(); $w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
    $w.WriteLine('{"execute":"qmp_capabilities"}')
    $ok = $false; $sw = [Diagnostics.Stopwatch]::StartNew(); $k = 0
    while ($sw.Elapsed.TotalSeconds -lt 60) {
        $dx = if ($k % 2) { 7 } else { -7 }; $k++
        $ev = if ($Abs) { '{"type":"abs","data":{"axis":"x","value":' + (12000 + 40 * $dx * ($k % 50)) + '}},{"type":"abs","data":{"axis":"y","value":' + (15000 + 90 * ($k % 40)) + '}}' } else { '{"type":"rel","data":{"axis":"x","value":' + $dx + '}},{"type":"rel","data":{"axis":"y","value":3}}' }
        try { $w.WriteLine('{"execute":"input-send-event","arguments":{"events":[' + $ev + ']}}') } catch { break }
        while ($s.DataAvailable) { [void]$s.ReadByte() }
        Start-Sleep -Milliseconds 20
        if ($k % 25 -eq 0 -and (Test-Path $log) -and (Select-String -Path $log -Pattern 'shell: screen' -Quiet)) { $ok = $true; break }
    }
    $last = if (Test-Path $log) { (Get-Content $log | Where-Object { $_.Trim() } | Select-Object -Last 1) } else { "(no log)" }
    $c.Close(); Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep 1
    "run=$i ok=$ok seconds=$([int]$sw.Elapsed.TotalSeconds) last='$last'"
}
