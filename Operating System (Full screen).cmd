@echo off
rem Starts Operating System over the whole screen, at the screen's own
rem resolution, with more of this PC's processors, thinking with Claude.
rem Ctrl+Alt+F leaves (and re-enters) full screen; Ctrl+Alt+G frees the mouse.
rem Closing other heavy programs first gives it more room.
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'File .*clipboard-bridge' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
start "" /b powershell -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0scripts\clipboard-bridge.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\os-win.ps1" -Claude -Model sonnet -Fullscreen
