# Runs the OS in a native Windows QEMU window (no WSL graphics involved).
# Uses Windows Hypervisor Platform when it is turned on, else plain emulation.
# os.conf (see os.conf.example) hands the OS its AI settings at boot;
# COM2 on 127.0.0.1:4555 carries the clipboard (scripts\clipboard-bridge.ps1).
# The person's disk (/dev/sda inside: what the OS remembers across reboots)
# is os-disk.img next to the image, or in %LOCALAPPDATA%\Operating System
# when that folder is not writable; -Disk picks another file. (A disk or
# settings file still under the system's old working name is taken over.)
# -Facet tries a desktop document (a theme) on a copy of the disk, so the
# person's own desktop is left as it was.
# -Claude thinks with Claude through this PC's Claude Code sign-in (no key):
# scripts\claude-bridge.mjs answers the VM at 10.0.2.2:4570 for this boot,
# and the mind saved on the disk is kept aside, untouched, for plain boots.
# -Display picks QEMU's window: gtk (default; the SDL window was closed by
# Windows when the cursor moved over it while booting) or sdl. QEMU's own
# errors and exit code go to buildos-vm-qemu.log.
# -Memory sets the VM's memory (e.g. 12G); by default it takes 8 GB, or a
# quarter of this PC's when that is more (everything inside lives in memory:
# the system, the bundled apps such as Prism, and what they use).
# -Fullscreen is as close to it running on this PC as a VM gets: the whole
# screen at its own resolution (no scaling) and
# QEMU ahead of other programs for the processor. Ctrl+Alt+F leaves and
# re-enters full screen; Ctrl+Alt+G lets go of the mouse.
param([string]$Iso = "", [string]$Disk = "", [string]$Facet = "", [switch]$Claude, [string]$Model = "sonnet", [string]$Display = "gtk",
      [string]$Memory = "", [switch]$Fullscreen)
$root = Split-Path -Parent $PSScriptRoot
# The newest finished image in dist\ (not the build in progress), else the build.
if (-not $Iso) {
    $done = Get-ChildItem (Join-Path $root "dist\*") -Include "Operating-System-*.iso","Lumen-*.iso" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $Iso = if ($done) { $done.FullName } else { Join-Path $root "build\os.iso" }
}
# Enough memory for the image (an offline-mind build carries a big model).
$mem = $Memory
if (-not $mem) {
    $pcGB = [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    $gb = [math]::Max(8, [math]::Min(16, [math]::Floor($pcGB / 4)))
    $mem = "$($gb)G"
}
Write-Host "Starting $Iso with $mem of memory"
$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
if (-not (Test-Path $qemu)) { Write-Host "QEMU for Windows not found at $qemu"; exit 1 }
if (-not (Test-Path $Iso)) { Write-Host "No image at $Iso. Build it first with make."; exit 1 }

# 32 GiB, sparse: NTFS must be told before the file grows, then it only
# takes the space the OS actually writes.
function New-OSDisk([string]$path) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [IO.File]::WriteAllBytes($path, [byte[]]@())
    & fsutil sparse setflag $path | Out-Null
    $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    try { $fs.SetLength(32GB) } finally { $fs.Close() }
    Write-Host "Created an empty disk at $path"
}

if (-not $Disk) {
    $isoDir = Split-Path -Parent (Resolve-Path $Iso).Path
    $candidates = @((Join-Path $isoDir "os-disk.img"), (Join-Path $env:LOCALAPPDATA "Operating System\os-disk.img"))
    # The disk from before the rename becomes this one (same contents).
    $old = @((Join-Path $isoDir "lumen-disk.img"), (Join-Path $env:LOCALAPPDATA "Lumen\lumen-disk.img"))
    for ($i = 0; $i -lt 2; $i++) {
        if ((Test-Path $old[$i]) -and -not (Test-Path $candidates[$i])) {
            $dir = Split-Path -Parent $candidates[$i]
            if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
            Move-Item $old[$i] $candidates[$i]
            Write-Host "Your disk now goes by $($candidates[$i])"
        }
    }
    $Disk = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Disk) {
        foreach ($c in $candidates) {
            try { New-OSDisk $c; $Disk = $c; break } catch { Write-Host "Cannot create $c, trying elsewhere" }
        }
    }
} elseif (-not (Test-Path $Disk)) {
    New-OSDisk $Disk
}
# Room for apps: past its first 320 MiB (the saved settings and home) the disk
# holds a file system for apps from Flathub. A disk made smaller grows to
# 32 GiB; the file is sparse, so only what the OS writes takes space here.
if ($Disk -and (Test-Path $Disk) -and ((Get-Item $Disk).Length -lt 32GB)) {
    & fsutil sparse setflag $Disk | Out-Null
    $fs = [IO.File]::Open($Disk, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    try { $fs.SetLength(32GB) } finally { $fs.Close() }
    Write-Host "Your disk now has room for apps (32 GiB, sparse)"
}

$facetArgs = @()
$snapshot = $false
if ($Facet) {
    if (-not (Test-Path $Facet)) { Write-Host "No design at $Facet"; exit 1 }
    # The disk in snapshot mode: QEMU keeps this session's writes in a
    # throwaway file, so nothing is copied and the disk itself never changes.
    $snapshot = $true
    $facetArgs = @("-fw_cfg", ("name=opt/os/facet.xml,file=" + ((Resolve-Path $Facet).Path -replace ",", ",,")))
    Write-Host "Trying $Facet; your own desktop is not changed (this session's changes are thrown away)"
}

# kernel-irqchip=on: Hyper-V keeps the guest's timers and interrupts itself,
# so the system keeps running while this window is moved or resized (Windows
# holds QEMU's display thread, and with it QEMU's lock, for the whole drag).
# zoom-to-fit: the picture is scaled to the window, never the window to the
# picture. Without it QEMU sizes its window (in full screen, its drawing area)
# from the picture, so a resolution change made the window taller than the
# screen: off the bottom in a window, stretched and cut off in full screen.
$cpus = 4
$gtk = "gtk,show-menubar=off,zoom-to-fit=on"
# How much room the system has, in this screen's real pixels: the screen
# itself in full screen, else what a window fits in (the work area less the
# title bar and borders); it draws at that size or the largest of its own that fits.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Name DpiAware -Namespace OS -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[OS.DpiAware]::SetProcessDPIAware() | Out-Null
$screen = [System.Windows.Forms.Screen]::PrimaryScreen
$size = Join-Path $env:TEMP "os-display.txt"
if ($Fullscreen) {
    # gl=on: the graphics card draws the window. QEMU's own full-screen drawing
    # on Windows made the whole VM 2-3x slower (measured with
    # tests\win_display_time.ps1: init at 55 s instead of 21 s).
    $gtk += ",full-screen=on,gl=on"
    [IO.File]::WriteAllText($size, "$($screen.Bounds.Width)x$($screen.Bounds.Height)")
    Write-Host "Full screen at $($screen.Bounds.Width)x$($screen.Bounds.Height) with $cpus processors (Ctrl+Alt+F leaves full screen, Ctrl+Alt+G frees the mouse)"
} else {
    $mw = $screen.WorkingArea.Width - 16
    $mh = $screen.WorkingArea.Height - 48
    [IO.File]::WriteAllText($size, "max $($mw)x$($mh)")
}
$facetArgs += @("-fw_cfg", "name=opt/os/display,file=$size")
# The last session's console is kept (what happened before a restart).
$serialLog = Join-Path $root "build\os-vm-serial.log"
if (Test-Path $serialLog) { Copy-Item $serialLog (Join-Path $root "build\os-vm-serial.previous.log") -Force }
$args = @("-name", "Operating System", "-M", "q35,vmport=on", "-m", $mem, "-smp", "$cpus",
          "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg,thread=multi",
          "-cpu", "Skylake-Client-noTSX-IBRS",
          "-vga", "std", "-display", $(if ($Display -eq "gtk") { $gtk } else { $Display }),
          "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
          "-audiodev", "dsound,id=snd0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0",
          "-serial", ("file:" + ((Join-Path $root "build\os-vm-serial.log") -replace "\\", "/")),
          "-serial", "tcp:127.0.0.1:4555,server=on,wait=off",
          "-cdrom", $Iso)
if ($Disk) {
    # QEMU option syntax: forward slashes are fine, commas must be doubled.
    $qdisk = ($Disk -replace "\\", "/") -replace ",", ",,"
    $snap = if ($snapshot) { ",snapshot=on" } else { "" }
    $args += @("-drive", "id=disk0,file=$qdisk,format=raw,if=none$snap", "-device", "ide-hd,drive=disk0,bus=ide.0")
} else {
    Write-Host "No disk: nothing will be remembered anything after this session."
}

$conf = Join-Path $root "os.conf"
if (-not (Test-Path $conf) -and (Test-Path (Join-Path $root "lumen.conf"))) { $conf = Join-Path $root "lumen.conf" }
if ((Test-Path $conf) -and -not $Claude) {
    $kv = @{}
    foreach ($line in Get-Content $conf) {
        if ($line -match '^\s*([a-z]+)=(.*)$') { $kv[$Matches[1]] = $Matches[2].Trim() -replace '["\\]', '' }
    }
    $mind = Join-Path $env:TEMP "os-mind.json"
    $json = '{"provider":"' + $kv["provider"] + '","base_url":"' + $kv["url"] + '","api_key":"' + $kv["key"] + '","model":"' + $kv["model"] + '"}'
    [IO.File]::WriteAllText($mind, $json)
    $args += @("-fw_cfg", "name=opt/os/mind.json,file=$mind")
    Write-Host "Using AI settings from os.conf ($($kv["provider"]))"
}
if ($Claude) {
    $node = Get-Command node -ErrorAction SilentlyContinue
    $claudeExe = Join-Path $env:USERPROFILE ".local\bin\claude.exe"
    if (-not $node -or -not (Test-Path $claudeExe)) {
        Write-Host "Claude on this PC needs Node.js and Claude Code (claude.exe) installed and signed in."; exit 1
    }
    $listening = Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort 4570 -State Listen -ErrorAction SilentlyContinue
    if (-not $listening) {
        Start-Process $node.Source -WindowStyle Hidden -ArgumentList @((Join-Path $PSScriptRoot "claude-bridge.mjs"), "--exit-with-vm") `
            -RedirectStandardOutput (Join-Path $root "build\claude-bridge.log") -RedirectStandardError (Join-Path $root "build\claude-bridge.err")
    }
    $mind = Join-Path $env:TEMP "os-claude-mind.json"
    [IO.File]::WriteAllText($mind, '{"provider":"claude-pc","base_url":"http://10.0.2.2:4570/v1","api_key":"","model":"' + $Model + '"}')
    $facetArgs += @("-fw_cfg", "name=opt/os/mind.json,file=$mind")
    Write-Host "Thinking with Claude ($Model) through this PC's Claude Code sign-in"
}
# The PC's senses (CPU, GPU, fans, RGB lights) for the VM at 10.0.2.2:4571,
# unless they already run (e.g. elevated, from scripts\host-senses\enable-admin.ps1).
$senseNode = Get-Command node -ErrorAction SilentlyContinue
if ($senseNode -and -not (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort 4571 -State Listen -ErrorAction SilentlyContinue)) {
    Start-Process $senseNode.Source -WindowStyle Hidden -ArgumentList @((Join-Path $PSScriptRoot "host-senses.mjs"), "--exit-with-vm") `
        -RedirectStandardOutput (Join-Path $root "build\host-senses.log") -RedirectStandardError (Join-Path $root "build\host-senses.err")
}
# QEMU's own errors and how it ended, for when the window goes away by itself.
$qlog = Join-Path $root "build\os-vm-qemu.log"
"$(Get-Date -Format s) starting $Iso (display $Display)" | Out-File -Encoding utf8 $qlog
if ($Fullscreen) {
    # ahead of other programs once it is up (above normal, not high: the PC's
    # own sound and input keep their place)
    Start-Job -ScriptBlock {
        for ($i = 0; $i -lt 30; $i++) {
            Start-Sleep -Seconds 1
            $q = Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue
            if ($q) { $q | ForEach-Object { $_.PriorityClass = "AboveNormal" }; break }
        }
    } | Out-Null
}
& $qemu @args @facetArgs 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 -Append $qlog
"$(Get-Date -Format s) QEMU ended, exit code $LASTEXITCODE" | Out-File -Encoding utf8 -Append $qlog

