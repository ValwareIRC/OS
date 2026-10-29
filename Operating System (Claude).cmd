@echo off
rem Starts Operating System thinking with Claude, through this PC's own Claude
rem Code sign-in (no API key). Claude can only talk: every tool is turned off.
rem Your saved mind (GLM) is kept as it was and comes back on a plain start.
rem Another model: edit "-Model sonnet" below (opus, sonnet, haiku or fable).
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'File .*clipboard-bridge' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
start "" /b powershell -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0scripts\clipboard-bridge.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\os-win.ps1" -Claude -Model sonnet
