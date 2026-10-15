# 检查 COM 串口状态
$ports = [System.IO.Ports.SerialPort]::GetPortNames()
Write-Output "Available ports: $($ports -join ', ')"
# 尝试打开 COM3 看是否被占用
try {
    $sp = New-Object System.IO.Ports.SerialPort("COM3", 115200, "None", 8, "One")
    $sp.ReadTimeout = 1000
    $sp.Open()
    Write-Output "COM3 opened OK"
    $sp.Close()
} catch {
    Write-Output "COM3 open failed: $($_.Exception.Message)"
}
