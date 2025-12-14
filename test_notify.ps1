try {
    Start-Sleep -Milliseconds 500
    $tcp = New-Object Net.Sockets.TcpClient("127.0.0.1", 4535)
    $stream = $tcp.GetStream()
    $reader = New-Object IO.StreamReader($stream)
    $writer = New-Object IO.StreamWriter($stream)
    $writer.AutoFlush = $true
    
    $writer.WriteLine("VER")
    $r = $reader.ReadLine()
    "VER: $r" | Out-File test_notify.txt
    
    $writer.WriteLine("STATUS")
    $r = $reader.ReadLine()
    "STATUS: $r" | Out-File test_notify.txt -Append
    
    $writer.WriteLine("START")
    $r = $reader.ReadLine()
    "START: $r" | Out-File test_notify.txt -Append
    
    $writer.WriteLine("STATUS")
    $r = $reader.ReadLine()
    "STATUS: $r" | Out-File test_notify.txt -Append
    
    $writer.WriteLine("STOP")
    $r = $reader.ReadLine()
    "STOP: $r" | Out-File test_notify.txt -Append
    
    $writer.WriteLine("QUIT")
    $r = $reader.ReadLine()
    "QUIT: $r" | Out-File test_notify.txt -Append
    
    $tcp.Close()
    "SUCCESS" | Out-File test_notify.txt -Append
} catch {
    "ERROR: $_" | Out-File test_notify.txt -Append
}
