# Shows the log of the OS running on another computer on this network, as it is written.
#
# On that computer, start the OS with "netlog" on its kernel command line: on the USB stick, open
# boot/limine/limine.conf and add this line under "/Operating System" (with the four spaces):
#     cmdline: netlog
# ("cmdline: netlog verbose" also shows the log on its screen while it starts.)
# Then run this here:  powershell -ExecutionPolicy Bypass -File scripts\netlog.ps1
# Windows may ask whether PowerShell may receive on private networks: allow it. The log is also kept in
# netlog.txt beside this script.

$port = 6666
$keep = Join-Path $PSScriptRoot "netlog.txt"
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Client.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket, [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
$udp.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, $port)))
$from = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
Write-Host "Waiting for the OS's log on port $port (Ctrl+C to stop)..."
try {
    while ($true) {
        $bytes = $udp.Receive([ref]$from)
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        Write-Host -NoNewline $text
        Add-Content -Path $keep -Value $text -NoNewline
    }
} finally {
    $udp.Close()
}
