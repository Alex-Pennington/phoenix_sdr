#!/usr/bin/env pwsh
# Test UDP command interface to waterfall.exe

param(
    [string]$Command = "ENABLE_TELEM TICK",
    [string]$Host = "localhost",
    [int]$Port = 3006
)

$udpClient = New-Object System.Net.Sockets.UdpClient
try {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Command + "`n")
    $sent = $udpClient.Send($bytes, $bytes.Length, $Host, $Port)
    Write-Host "Sent command: $Command ($sent bytes)"
} finally {
    $udpClient.Close()
}
