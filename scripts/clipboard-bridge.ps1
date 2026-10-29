# Sends this PC's clipboard to a running the OS VM whenever it changes, so
# Ctrl+V inside the OS pastes it. the OS's second serial port (COM2) listens
# on 127.0.0.1:4555; each clipboard text goes as 0x02 + UTF-8 text + 0x03.
# Started (hidden) by Operating System.cmd; exits on its own once the VM is gone.
param([int]$Port = 4555)
Add-Type -AssemblyName System.Windows.Forms
$last = $null
$client = $null
$noVm = 0
$beat = 0
while ($true) {
    # Gone for good once no the OS VM has been running for half a minute.
    if (Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue) { $noVm = 0 } else { $noVm++ }
    if ($noVm -gt 60) { exit 0 }

    if (-not $client) {
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $client.Connect("127.0.0.1", $Port)
            $last = $null      # send the current clipboard to a fresh VM
        } catch {
            $client = $null
            Start-Sleep -Milliseconds 500
            continue
        }
    }
    try {
        $text = [System.Windows.Forms.Clipboard]::GetText()
    } catch { $text = "" }
    try {
        $stream = $client.GetStream()
        if ($text -and $text -ne $last) {
            if ($text.Length -gt 4000) { $text = $text.Substring(0, 4000) }
            $bytes = [byte[]](2) + [System.Text.Encoding]::UTF8.GetBytes($text) + [byte[]](3)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            $last = $text
        } elseif ((++$beat % 3) -eq 0) {
            # A byte outside any frame (the OS ignores it): writing to a VM that
            # has gone fails, so a dead connection is noticed and replaced.
            $stream.Write([byte[]](0), 0, 1)
        }
    } catch {
        try { $client.Close() } catch {}
        $client = $null
    }
    Start-Sleep -Milliseconds 400
}
