# 查找 VSCode 扩展宿主日志中 esp-idf 相关的最新错误
$logs = Join-Path $env:APPDATA "Code\logs"
if (-not (Test-Path $logs)) { Write-Output "NO_LOGS_DIR"; exit 0 }
# 最新日期目录
$latest = Get-ChildItem $logs -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $latest) { Write-Output "NO_LOG_ENTRY"; exit 0 }
Write-Output ("LATEST_LOG_DIR: " + $latest.FullName)
# exthost 日志中的 esp-idf 片段
$exthost = Get-ChildItem $latest.FullName -Recurse -Filter "*.log" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "exthost" }
$found = $false
foreach ($f in $exthost) {
    $m = Select-String -Path $f.FullName -Pattern "esp-idf|Cannot read properties|replace" -ErrorAction SilentlyContinue | Select-Object -Last 30
    if ($m) {
        $found = $true
        Write-Output ("==== FILE: " + $f.FullName + " ====")
        $m | ForEach-Object { Write-Output $_.Line }
    }
}
if (-not $found) { Write-Output "NO_ESPIDF_ERROR_IN_EXTHOST" }
