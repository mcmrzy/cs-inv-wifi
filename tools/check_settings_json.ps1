# 验证 .vscode/settings.json JSON 格式与 idf.flashBaudRate 配置
try {
    $j = Get-Content "d:\CS_INV_WIFI\.vscode\settings.json" -Raw | ConvertFrom-Json
    $rate = $j.'idf.flashBaudRate'
    $port = $j.'idf.portWin'
    $type = $j.'idf.flashType'
    Write-Output "JSON_OK flashBaudRate=$rate port=$port flashType=$type"
} catch {
    Write-Output ("JSON_ERROR: " + $_.Exception.Message)
}
