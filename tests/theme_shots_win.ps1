# Photographs designs on their desktops with Windows QEMU (Hyper-V), headless:
# for each design, boots straight to its desktop with no disk and saves
# build/themes/<name>.png. Closes only the VMs it started.
#   powershell -File tests\theme_shots_win.ps1 [-Designs a.xml,b.xml] [-Iso build\os.iso]
param([string[]]$Designs = @(), [string]$Iso = "", [int]$Settle = 12, [switch]$WithMind)
$root = Split-Path -Parent $PSScriptRoot
if (-not $Iso) { $Iso = Join-Path $root "build\os.iso" }
if (-not $Designs.Count) { $Designs = Get-ChildItem (Join-Path $root "userland\rootfs\usr\share\os\themes\*.xml") | ForEach-Object FullName }
$out = Join-Path $root "build\themes"
New-Item -ItemType Directory -Force $out | Out-Null
$mind = Join-Path $out "mind.json"
# -WithMind: the model that comes with the system (a companion view then shows a
# conversation, not an error); otherwise none, so no greeting covers the desktop
$json = if ($WithMind) { '{"provider":"builtin","base_url":"http://127.0.0.1:8080/v1","api_key":"","model":"os"}' }
        else { '{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}' }
[IO.File]::WriteAllText($mind, $json)
$port = 4466

function Qmp([System.IO.StreamWriter]$w, [System.IO.StreamReader]$r, [string]$json) {
    $w.WriteLine($json); $w.Flush()
    while ($true) { $l = $r.ReadLine(); if ($l -eq $null -or $l -match '"return"|"error"') { return $l } }
}

foreach ($d in $Designs) {
    $name = [IO.Path]::GetFileNameWithoutExtension($d)
    $log = Join-Path $out "$name.log"
    if (Test-Path $log) { Clear-Content $log }
    $qargs = @("-M", "q35,vmport=on", "-m", "6G", "-smp", "4", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg",
               "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none", "-nic", "none",
               "-fw_cfg", ("name=opt/os/mind.json,file=" + ($mind -replace '\\', '/')),
               "-fw_cfg", ("name=opt/os/facet.xml,file=" + ($d -replace '\\', '/')),
               "-qmp", "tcp:127.0.0.1:$port,server,nowait",
               "-serial", ("file:" + ($log -replace '\\', '/')), "-cdrom", ($Iso -replace '\\', '/'))
    $p = Start-Process "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList $qargs -PassThru -WindowStyle Hidden
    $t0 = Get-Date; $up = $false
    while (((Get-Date) - $t0).TotalSeconds -lt 180 -and -not $p.HasExited) {
        Start-Sleep -Seconds 2
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern "shell: screen desktop" -Quiet)) { $up = $true; break }
    }
    if ($up) {
        Start-Sleep -Seconds $Settle
        $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $port)
        $s = $c.GetStream(); $r = New-Object System.IO.StreamReader($s); $w = New-Object System.IO.StreamWriter($s)
        [void]$r.ReadLine()
        [void](Qmp $w $r '{"execute":"qmp_capabilities"}')
        $png = (Join-Path $out "$name.png") -replace '\\', '/'
        $res = Qmp $w $r ('{"execute":"screendump","arguments":{"filename":"' + $png + '","format":"png"}}')
        [void](Qmp $w $r '{"execute":"quit"}')
        $c.Close()
        "${name}: " + $(if ($res -match '"return"') { "saved" } else { "screendump failed: $res" })
    } else { "${name}: the desktop never came up" }
    if (-not $p.HasExited) { Start-Sleep -Seconds 2; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
}
