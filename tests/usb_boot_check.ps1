# Boots a USB stick's files (a folder or drive, e.g. D:\) the way a PC with
# UEFI firmware would: read-only, as USB storage, headless. Says whether the
# kernel started and the desktop came up.
#   powershell -File tests\usb_boot_check.ps1 -Drive D:/
param([string]$Drive = "D:/", [int]$Limit = 180)
$root = Split-Path -Parent $PSScriptRoot
$qemu = "C:\Program Files\qemu"
# a copy of the UEFI firmware where the path has no spaces (QEMU's arguments are joined unquoted)
$fwSrc = Get-ChildItem (Join-Path $qemu "share") -Filter "edk2-x86_64-code.fd" | Select-Object -First 1
$fw = Get-Item (Join-Path $root "build\edk2-x86_64-code.fd") -ErrorAction SilentlyContinue
if (-not $fw) { Copy-Item $fwSrc.FullName (Join-Path $root "build"); $fw = Get-Item (Join-Path $root "build\edk2-x86_64-code.fd") }
$log = Join-Path $root "build\usb-boot.log"
if (Test-Path $log) { Clear-Content $log }
$qargs = @("-M", "q35", "-m", "6G", "-smp", "4", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg",
           "-cpu", "Skylake-Client-noTSX-IBRS", "-display", "none", "-nic", "none",
           "-drive", ("if=pflash,format=raw,readonly=on,file=" + ($fw.FullName -replace '\\', '/')),
           "-drive", "if=none,id=usb0,format=raw,readonly=on,file=fat:$Drive",
           "-device", "qemu-xhci", "-device", "usb-storage,drive=usb0",
           "-serial", ("file:" + ($log -replace '\\', '/')))
$p = Start-Process (Join-Path $qemu "qemu-system-x86_64.exe") -ArgumentList $qargs -PassThru -WindowStyle Hidden `
        -RedirectStandardError (Join-Path $root "build\usb-boot.err")
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Limit -and -not $p.HasExited) {
    Start-Sleep -Seconds 3
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern "shell: screen" -Quiet)) { break }
}
$s = [int]((Get-Date) - $t0).TotalSeconds
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
"after $s s:"
Get-Content $log -ErrorAction SilentlyContinue | Select-String "kernel starting|modules:|storage|screen " |
    Select-Object -First 6 | ForEach-Object { $_.Line }
Get-Content (Join-Path $root "build\usb-boot.err") -ErrorAction SilentlyContinue | Select-Object -First 3
