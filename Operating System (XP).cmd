@echo off
rem Starts Operating System wearing the hand-made Windows XP theme (tests\designs\xp.xml),
rem on a copy of its disk: your own desktop and conversation are untouched.
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'File .*clipboard-bridge' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
start "" /b powershell -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0scripts\clipboard-bridge.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\os-win.ps1" -Facet "%~dp0tests\designs\xp.xml"
