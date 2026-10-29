# Checks "Claude on this PC" end to end without any window: starts the
# bridge, boots the image hidden with the Claude mind handed over, waits for
# the companion to speak, then boots again plainly to see the saved mind
# came back. Uses a copy of a disk, never the person's own.
param([Parameter(Mandatory)][string]$Iso, [Parameter(Mandatory)][string]$Disk, [string]$Model = "haiku")
$root = Split-Path -Parent $PSScriptRoot
$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$node = (Get-Command node).Source
$blog = Join-Path $root "build\claude-test-bridge.log"
$bridge = Start-Process $node -ArgumentList @((Join-Path $PSScriptRoot "claude-bridge.mjs"), "--port", "4570") -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $blog -RedirectStandardError (Join-Path $root "build\claude-test-bridge.err")
$mind = Join-Path $env:TEMP "os-claude-test-mind.json"
[IO.File]::WriteAllText($mind, '{"provider":"claude-pc","base_url":"http://10.0.2.2:4570/v1","api_key":"","model":"' + $Model + '"}')
function Boot([string]$log, [string[]]$extra, [int]$seconds, [string]$until, [int]$linger) {
    if (Test-Path $log) { [IO.File]::Delete($log) }
    $a = @("-M", "q35,vmport=on", "-m", "4G", "-smp", "4", "-accel", "whpx,kernel-irqchip=on", "-cpu", "Skylake-Client-noTSX-IBRS",
           "-vga", "std", "-display", "none", "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", ("file:" + ($log -replace "\\", "/")),
           "-drive", ("id=disk0,file=" + ($Disk -replace "\\", "/") + ",format=raw,if=none"), "-device", "ide-hd,drive=disk0,bus=ide.0",
           "-cdrom", (Resolve-Path $Iso).Path) + $extra
    $p = Start-Process $qemu -ArgumentList $a -PassThru -WindowStyle Hidden
    for ($t = 0; $t -lt $seconds; $t++) {
        Start-Sleep 1
        if ($until -and (Test-Path $log) -and (Select-String -Path $log -Pattern $until -Quiet)) { Start-Sleep $linger; break }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep 2
}
try {
    $l1 = Join-Path $root "build\claude-test-1.log"
    Boot $l1 @("-fw_cfg", "name=opt/os/mind.json,file=$mind") 150 "shell: screen desktop" 45
    "--- boot with Claude:"
    Select-String -Path $l1 -Pattern '^shell: mind|claude|companion|mind ' | Select-Object -First 8 | ForEach-Object { $_.Line }
    "--- bridge:"
    Get-Content $blog
    $l2 = Join-Path $root "build\claude-test-2.log"
    Boot $l2 @() 60 "shell: screen desktop" 3
    "--- plain boot afterwards:"
    Select-String -Path $l2 -Pattern '^shell: mind|mind ' | Select-Object -First 3 | ForEach-Object { $_.Line }
} finally {
    Stop-Process -Id $bridge.Id -Force -ErrorAction SilentlyContinue
}
