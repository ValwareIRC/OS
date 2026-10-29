# A real computer's own drive must never be written (Windows QEMU, fast):
# boots the newest image with a disk that looks like someone's Windows drive
# (tests/foreign_disk_check.py makes one), waits for the desktop and a save,
# and checks the file is byte for byte the same. Headless; closes only its VM.
#   powershell -File tests\foreign_disk_win.ps1 -Disk build\foreign-disk\foreign.img
param([string]$Disk, [int]$Limit = 240, [string]$Iso = "")
$root = Split-Path -Parent $PSScriptRoot
if (-not $Iso) { $Iso = Join-Path $root "build\os.iso" }
$Disk = (Resolve-Path $Disk).Path
$log = Join-Path $root "build\foreign-disk\win-serial.log"
Remove-Item $log -ErrorAction SilentlyContinue
$before = (Get-FileHash $Disk -Algorithm SHA256).Hash
$qargs = @("-name", "os-foreign-disk-test", "-M", "q35,vmport=on", "-m", "6G", "-smp", "4",
           "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg,thread=multi", "-cpu", "Skylake-Client-noTSX-IBRS",
           "-vga", "std", "-display", "none", "-nic", "none",
           "-drive", "id=disk0,file=$($Disk -replace '\\','/'),format=raw,if=none", "-device", "ide-hd,drive=disk0,bus=ide.0",
           "-boot", "d", "-serial", ("file:" + ($log -replace "\\", "/")), "-cdrom", $Iso)
$p = Start-Process "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList $qargs -PassThru -WindowStyle Hidden `
        -RedirectStandardError (Join-Path $root "build\foreign-disk\win-qemu.err")
$t0 = Get-Date
$desk = $null
while (((Get-Date) - $t0).TotalSeconds -lt $Limit -and -not $p.HasExited) {
    Start-Sleep -Seconds 2
    $text = if (Test-Path $log) { Get-Content $log -Raw -ErrorAction SilentlyContinue } else { "" }
    if (-not $desk -and $text -and $text.Contains("shell: screen ")) { $desk = Get-Date }
    if ($desk -and ((Get-Date) - $desk).TotalSeconds -gt 40) { break } # long enough for a save
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
Get-Content $log -ErrorAction SilentlyContinue | Select-String "storage:|os-data|store|screen " | Select-Object -First 12 | ForEach-Object { $_.Line }
$after = (Get-FileHash $Disk -Algorithm SHA256).Hash
if (-not $desk) { "FAIL: the desktop never came up" }
elseif ($before -eq $after) { "PASS: the disk is untouched" } else { "FAIL: the disk was written" }
