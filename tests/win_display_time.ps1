# Times a boot of the image under Windows QEMU with a given -display, with
# no disk and its own serial log: how long until the kernel's first line,
# init, and the desktop. Closes only the VM it started.
#   powershell -File tests\win_display_time.ps1 -Name A -Display "gtk,show-menubar=off,zoom-to-fit=on,full-screen=on"
param([string]$Name = "A", [string]$Display = "gtk,show-menubar=off,zoom-to-fit=on", [int]$Limit = 150,
      [string]$Size = "", [int]$Cpus = 4, [int]$Shot = 0)
$root = Split-Path -Parent $PSScriptRoot
$iso = Get-ChildItem (Join-Path $root "dist\Operating-System-*.iso") | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$log = Join-Path $root "build\wintime-$Name.log"
Remove-Item $log -ErrorAction SilentlyContinue
$extra = @()
if ($Size) {
    $f = Join-Path $env:TEMP "os-display-test.txt"
    [IO.File]::WriteAllText($f, $Size)
    $extra = @("-fw_cfg", "name=opt/os/display,file=$f")
}
$qargs = @("-name", "os-display-test-$Name", "-M", "q35,vmport=on", "-m", "8G", "-smp", "$Cpus",
           "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg,thread=multi", "-cpu", "Skylake-Client-noTSX-IBRS",
           "-vga", "std", "-display", $Display, "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", ("file:" + ($log -replace "\\", "/")), "-cdrom", $iso.FullName) + $extra
$t0 = Get-Date
$p = Start-Process "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList $qargs -PassThru `
        -RedirectStandardError (Join-Path $root "build\wintime-$Name.err")
$marks = [ordered]@{ "kernel" = "kernel starting"; "init" = "user space is alive"; "shell" = "shell: screen " }
$seen = @{}
while (((Get-Date) - $t0).TotalSeconds -lt $Limit -and $seen.Count -lt $marks.Count -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 500
    $text = if (Test-Path $log) { Get-Content $log -Raw -ErrorAction SilentlyContinue } else { "" }
    foreach ($k in $marks.Keys) {
        if (-not $seen.ContainsKey($k) -and $text -and $text.Contains($marks[$k])) { $seen[$k] = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
    }
}
if ($Shot -and -not $p.HasExited) { # what is on the screen now, before closing
    Start-Sleep -Seconds $Shot
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save((Join-Path $root "build\wintime-$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
$out = "$Name [$Display] " + (($marks.Keys | ForEach-Object { "$($_)=" + $(if ($seen.ContainsKey($_)) { "$($seen[$_])s" } else { "never" }) }) -join " ")
$out
