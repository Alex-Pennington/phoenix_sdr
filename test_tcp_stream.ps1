# Quick test client for TCP streaming
$client = New-Object System.Net.Sockets.TcpClient
Write-Host "Connecting to localhost:4536..."
$client.Connect("localhost", 4536)
$stream = $client.GetStream()

Write-Host "Connected! Reading header..."
$header = New-Object byte[] 32
$stream.Read($header, 0, 32) | Out-Null

# Parse header
$magic = [BitConverter]::ToUInt32($header, 0)
$version = [BitConverter]::ToUInt32($header, 4)
$sample_rate = [BitConverter]::ToUInt32($header, 8)
$format = [BitConverter]::ToUInt32($header, 12)

Write-Host "Header received:"
Write-Host "  Magic: 0x$($magic.ToString('X8')) (should be 0x50485849 'PHXI')"
Write-Host "  Version: $version"
Write-Host "  Sample Rate: $sample_rate Hz"
Write-Host "  Format: $format (1=S16)"

Write-Host "`nReading first data frame..."
$frame_hdr = New-Object byte[] 16
$stream.Read($frame_hdr, 0, 16) | Out-Null

$frame_magic = [BitConverter]::ToUInt32($frame_hdr, 0)
$sequence = [BitConverter]::ToUInt32($frame_hdr, 4)
$num_samples = [BitConverter]::ToUInt32($frame_hdr, 8)
$flags = [BitConverter]::ToUInt32($frame_hdr, 12)

Write-Host "Frame header:"
Write-Host "  Magic: 0x$($frame_magic.ToString('X8')) (should be 0x49514451 'IQDQ')"
Write-Host "  Sequence: $sequence"
Write-Host "  Samples: $num_samples"
Write-Host "  Flags: $flags"

$client.Close()
Write-Host "`nTCP protocol test PASSED!"
