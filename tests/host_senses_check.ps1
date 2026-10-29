# Starts scripts\host-senses.mjs on a spare port, prints what it senses, and
# stops it again. Read-only: it never changes a light.
#   powershell -ExecutionPolicy Bypass -File tests\host_senses_check.ps1 [-Port 4591] [-Wait 12]
param([int]$Port = 4591, [int]$Wait = 12)
$root = Split-Path -Parent $PSScriptRoot
$node = (Get-Command node -ErrorAction SilentlyContinue).Source
if (-not $node) { Write-Host "Node.js is needed."; exit 1 }
if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    Write-Host "Port $Port is taken; pass -Port."; exit 1
}
$log = Join-Path $env:TEMP "host-senses-check.log"
$err = Join-Path $env:TEMP "host-senses-check.err"
$p = Start-Process $node -WindowStyle Hidden -PassThru -ArgumentList @((Join-Path $root "scripts\host-senses.mjs"), "--port", $Port) `
        -RedirectStandardOutput $log -RedirectStandardError $err
$base = "http://127.0.0.1:$Port"
$failed = $false
try {
    $ok = $false
    for ($i = 0; $i -lt 40 -and -not $ok; $i++) {
        Start-Sleep -Milliseconds 250
        try { $ok = (Invoke-RestMethod "$base/health" -TimeoutSec 2).ok } catch {}
    }
    if (-not $ok) { Write-Host "FAIL: /health never answered"; $failed = $true; return }
    Write-Host "health: ok"
    # The helper needs a moment to reach iCUE (and the slower vendor SDKs).
    $deadline = (Get-Date).AddSeconds($Wait)
    do { Start-Sleep -Seconds 1; $s = Invoke-RestMethod "$base/senses" -TimeoutSec 2 } while ((Get-Date) -lt $deadline)

    $c = $s.host.cpu
    Write-Host ("time:    {0}" -f $s.time)
    Write-Host ("host:    {0}" -f $s.host.name)
    Write-Host ("cpu:     {0}; load {1}%; clock {2} MHz; temperature {3}" -f $c.model, $c.load, $c.clock_mhz, $(if ($null -ne $c.temp_c) { "$($c.temp_c) C" } else { "unknown" }))
    foreach ($g in $s.host.gpu) {
        Write-Host ("gpu:     {0}; load {1}%; {2} C; fan {3}%; {4} W; VRAM {5}/{6} MB" -f $g.name, $g.load, $g.temp_c, $g.fan_pct, $g.power_w, $g.vram_used_mb, $g.vram_total_mb)
    }
    Write-Host ("memory:  {0} / {1} MB" -f $s.host.memory.used_mb, $s.host.memory.total_mb)
    foreach ($f in $s.host.fans) { Write-Host ("fan:     {0}: {1} rpm, {2}%" -f $f.name, $f.rpm, $f.pct) }
    if (-not $s.lights.Count) { Write-Host "lights:  none found" }
    foreach ($l in $s.lights) {
        Write-Host ("light:   {0} [{1}, {2}] {3} LEDs, colour {4}{5}, samples {6}{7}" -f $l.id, $l.kind, $l.vendor, $l.leds, $l.color,
            $(if ($l.effect) { " ($($l.effect))" } else { "" }), (@($l.colors) -join " "), $(if ($l.controllable) { "" } else { " (read only)" }))
    }
    foreach ($m in $s.missing) { Write-Host "missing: $m" }

    # Shape checks the guest relies on.
    foreach ($k in "time", "host", "lights", "missing") { if ($null -eq $s.PSObject.Properties[$k]) { Write-Host "FAIL: no $k"; $failed = $true } }
    if ($null -eq $c.load) { Write-Host "FAIL: no CPU load"; $failed = $true }
    try { Invoke-RestMethod "$base/lights" -Method Post -ContentType "application/json" -Body '{"target":"all","color":"red"}' | Out-Null
          Write-Host "FAIL: a bad colour was accepted"; $failed = $true } catch { Write-Host "bad input: refused as it should be" }
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # Its helper follows by itself once its input closes.
    Start-Sleep -Seconds 2
    Write-Host "stopped (log: $log)"
}
if ($failed) { exit 1 }
