# One-time step: lets the companion read the CPU temperature (AMD Ryzen
# Master SDK) and the MSI motherboard's lights (Mystic Light). Both only
# answer an elevated program.
#
# Run it once from an administrator PowerShell:
#   powershell -ExecutionPolicy Bypass -File scripts\host-senses\enable-admin.ps1
# It copies the senses service to "C:\Program Files\Operating System Senses"
# (where only administrators can change it, so nothing unelevated can swap
# what runs elevated) and registers a scheduled task, "Operating System
# Senses", that starts it at every logon with highest privileges and no
# window. After that no further prompts appear. The launcher finds it on
# port 4571 and uses it.
#
# Re-run it after changing the service, so the installed copy is updated.
#   -Remove   takes the task and the installed copy away again.
param([switch]$Remove, [string]$User = "$env:USERDOMAIN\$env:USERNAME")
$ErrorActionPreference = "Stop"
$name = "Operating System Senses"
$dest = Join-Path $env:ProgramFiles $name
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host "This needs an administrator PowerShell (Start, type PowerShell, right-click, Run as administrator)."
    Write-Host "Nothing was changed."
    exit 1
}

# Whatever already answers on 4571 (an unelevated copy) makes way.
function Stop-Senses {
    foreach ($c in Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort 4571 -State Listen -ErrorAction SilentlyContinue) {
        $p = Get-CimInstance Win32_Process -Filter "ProcessId=$($c.OwningProcess)"
        if ($p -and $p.CommandLine -match 'host-senses\.mjs') { Stop-Process -Id $p.ProcessId -Force; Start-Sleep -Seconds 2 }
    }
}

if ($Remove) {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
    Stop-Senses
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Write-Host "Removed the task and $dest."
    exit 0
}

$scripts = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $scripts
$node = (Get-Command node -ErrorAction SilentlyContinue).Source
if (-not $node) { Write-Host "Node.js is needed."; exit 1 }
if (-not $node.StartsWith($env:ProgramFiles, [StringComparison]::OrdinalIgnoreCase)) {
    Write-Host "Warning: $node is outside Program Files, so an unelevated program could change what runs elevated."
}

# Build the helper now if it isn't built yet (the installed copy can't build).
$exe = Join-Path $PSScriptRoot "bin\OperatingSystemSenses.exe"
$src = Join-Path $PSScriptRoot "SensesHelper.cs"
if (-not (Test-Path $exe) -or (Get-Item $src).LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
    New-Item -ItemType Directory -Force (Split-Path $exe) | Out-Null
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" -nologo -optimize -platform:x64 -target:exe "-out:$exe" `
        -r:System.Web.Extensions.dll -r:Microsoft.CSharp.dll -r:System.Core.dll $src
    if (-not (Test-Path $exe)) { Write-Host "Could not build the helper."; exit 1 }
}

Stop-Senses
Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $dest "host-senses\bin") | Out-Null
Copy-Item (Join-Path $scripts "host-senses.mjs") $dest -Force
Copy-Item (Join-Path $PSScriptRoot "iCUESDK.x64_2019.dll") (Join-Path $dest "host-senses") -Force
Copy-Item $exe (Join-Path $dest "host-senses\bin") -Force

$log = Join-Path $root "build\host-senses.log"
# conhost --headless: a console program with no window at all.
$action = New-ScheduledTaskAction -Execute "$env:WINDIR\System32\conhost.exe" `
    -Argument ("--headless `"{0}`" `"{1}`" --log `"{2}`"" -f $node, (Join-Path $dest "host-senses.mjs"), $log) -WorkingDirectory $dest
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $User
$principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -Hidden `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force `
    -Description "The companion's senses (CPU temperature, fans, RGB lights) on 127.0.0.1:4571. From scripts\host-senses\enable-admin.ps1." | Out-Null
Start-ScheduledTask -TaskName $name

$ok = $false
for ($i = 0; $i -lt 40 -and -not $ok; $i++) {
    Start-Sleep -Milliseconds 500
    try { $ok = (Invoke-RestMethod "http://127.0.0.1:4571/health" -TimeoutSec 2).ok } catch {}
}
if ($ok) { Write-Host "Done: the senses run elevated now and at every logon (task `"$name`")." }
else { Write-Host "The task is registered but the service did not answer yet; see $log." }
