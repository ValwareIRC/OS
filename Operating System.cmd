@echo off
rem Starts Operating System in a window on this PC.
rem Native Windows QEMU when installed (C:\Program Files\qemu), else QEMU in WSL.
rem Ctrl+V inside it pastes this PC's clipboard (scripts\clipboard-bridge.ps1).
rem To start already connected to an AI, copy os.conf.example to os.conf
rem and fill it in.
rem One clipboard bridge at a time: stop any left over from an earlier start.
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'File .*clipboard-bridge' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
start "" /b powershell -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0scripts\clipboard-bridge.ps1"
if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\os-win.ps1"
) else (
    wsl -d Ubuntu --cd "%~dp0." -- bash -lc "bash scripts/os-vm.sh"
)
