# release.ps1 — ESP32-C3 L10 生产固件构建 + OTA manifest 生成
#
# 交互模式:  .\release.ps1
# 命令行模式: .\release.ps1 -Version 1.3.0 -OtaUrl "https://xxx.com/fw" -Target esp

param(
    [string]$Version,
    [string]$OtaUrl,
    [string]$Target,
    [switch]$SkipBuild,
    [switch]$NoGitTag
)

$ErrorActionPreference = "Stop"

# ══════════════════════════════════════════════
#  读取当前版本 (默认值)
# ══════════════════════════════════════════════
$CurrentVer = (Get-Content "version.txt" -Raw).Trim()

# ══════════════════════════════════════════════
#  交互式输入
# ══════════════════════════════════════════════
Write-Host ""
Write-Host "╔══════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║   CS-L10-6K2 固件发布工具               ║" -ForegroundColor Cyan
Write-Host "╚══════════════════════════════════════════╝" -ForegroundColor Cyan
Write-Host ""

# 版本号
if (-not $Version) {
    $input = Read-Host "版本号 (当前: $CurrentVer, 回车沿用)"
    $Version = if ($input) { $input } else { $CurrentVer }
}
# 补齐 v 前缀 (用户输入 1.3.0 或 v1.3.0 都行)
$Version = $Version -replace '^v', ''

Write-Host "  版本: $Version" -ForegroundColor White

# OTA 下载地址
if (-not $OtaUrl) {
    $input = Read-Host "OTA 下载基地址 (如 https://ota.example.com/fw, 回车跳过生成 URL)"
    $OtaUrl = $input
}
if ($OtaUrl) {
    $OtaUrl = $OtaUrl.TrimEnd('/')
    Write-Host "  OTA URL: $OtaUrl" -ForegroundColor White
} else {
    Write-Host "  OTA URL: (跳过, manifest 中 url 为空)" -ForegroundColor DarkGray
}

# 升级目标
if (-not $Target) {
    $input = Read-Host "升级目标 [esp/arm/both] (默认 esp)"
    $Target = if ($input) { $input } else { "esp" }
}
$Target = $Target.ToLower()
if ($Target -notin @("esp", "arm", "both")) {
    Write-Error "无效目标: $Target (应为 esp/arm/both)"; exit 1
}
Write-Host "  目标: $Target" -ForegroundColor White

# 确认
Write-Host ""
$confirm = Read-Host "确认开始构建? [Y/n]"
if ($confirm -eq 'n' -or $confirm -eq 'N') {
    Write-Host "已取消" -ForegroundColor Yellow; exit 0
}

# ══════════════════════════════════════════════
#  编译
# ══════════════════════════════════════════════
Write-Host ""
Write-Host "--- 编译 ---" -ForegroundColor Cyan

# 先更新 version.txt (CMake 会读取)
$Version | Out-File -NoNewline -Encoding ascii "version.txt"

if (-not $SkipBuild) {
    # 定位 ESP-IDF (如果 idf.py 不在 PATH 中, 直接用 Python 环境调用)
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        $IdfPy = "D:\.espressif\v5.5.4\esp-idf\tools\idf.py"
        $IdfPython = "C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
        if (-not (Test-Path $IdfPy)) { Write-Error "找不到 idf.py: $IdfPy"; exit 1 }
        if (-not (Test-Path $IdfPython)) { Write-Error "找不到 ESP-IDF Python: $IdfPython"; exit 1 }
        $env:IDF_PATH = "D:\.espressif\v5.5.4\esp-idf"
        Write-Host "  ESP-IDF: $env:IDF_PATH" -ForegroundColor DarkGray
        & $IdfPython $IdfPy build
    } else {
        idf.py build
    }
    if ($LASTEXITCODE -ne 0) { Write-Error "编译失败"; exit 1 }
    Write-Host "  编译完成" -ForegroundColor Green
} else {
    Write-Host "  跳过编译 (-SkipBuild)" -ForegroundColor DarkGray
}

# ══════════════════════════════════════════════
#  收集产物
# ══════════════════════════════════════════════
$FwName = "cs_inv_l10_v$Version"
$BuildDir = "build"
$ReleaseDir = "release\v$Version"

$AppBin = "$BuildDir\cs_inv_l10_c3.bin"
if (-not (Test-Path $AppBin)) { Write-Error "找不到固件: $AppBin"; exit 1 }

New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null

Copy-Item $AppBin "$ReleaseDir\${FwName}.bin"
Copy-Item "$BuildDir\bootloader\bootloader.bin" "$ReleaseDir\bootloader.bin" -ErrorAction SilentlyContinue
Copy-Item "$BuildDir\partitions.bin" "$ReleaseDir\partitions.bin" -ErrorAction SilentlyContinue

$Hash = (Get-FileHash "$ReleaseDir\${FwName}.bin" -Algorithm SHA256).Hash.ToLower()
$Size = (Get-Item "$ReleaseDir\${FwName}.bin").Length

Write-Host "  固件: ${FwName}.bin ($Size bytes)" -ForegroundColor White
Write-Host "  SHA256: $Hash" -ForegroundColor White

# ══════════════════════════════════════════════
#  生成 OTA manifest
# ══════════════════════════════════════════════
Write-Host ""
Write-Host "--- OTA Manifest ---" -ForegroundColor Cyan

$Timestamp = [int][double]::Parse((Get-Date -UFormat %s))

# 根据 target 生成不同的 manifest
function Build-Manifest($t) {
    $fwFile = if ($t -eq "arm") { "${FwName}_arm.bin" } else { "${FwName}.bin" }
    return @{
        v               = 1
        action          = "start"
        target          = $t
        version         = $Version
        task_id         = "release-$Version-$t"
        url             = if ($OtaUrl) { "$OtaUrl/$fwFile" } else { "" }
        sha256          = $Hash
        size            = $Size
        security_version = 0
        timestamp       = $Timestamp
    }
}

$Manifests = @()
if ($Target -eq "esp" -or $Target -eq "both") {
    $Manifests += @{ target = "esp"; data = Build-Manifest "esp" }
}
if ($Target -eq "arm" -or $Target -eq "both") {
    $Manifests += @{ target = "arm"; data = Build-Manifest "arm" }
}

foreach ($m in $Manifests) {
    $json = $m.data | ConvertTo-Json -Depth 3
    $file = "$ReleaseDir\${FwName}_manifest_$($m.target).json"
    $json | Out-File -Encoding utf8 $file
    Write-Host "  $($m.target): $file" -ForegroundColor White
    Write-Host $json -ForegroundColor DarkGray
    Write-Host ""
}

# ══════════════════════════════════════════════
#  Git tag
# ══════════════════════════════════════════════
if (-not $NoGitTag) {
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $tag = "v$Version"
        $exists = git tag -l $tag
        if ($exists) {
            Write-Host "  Git tag $tag 已存在, 跳过" -ForegroundColor DarkGray
        } else {
            $doTag = Read-Host "创建 git tag '$tag'? [Y/n]"
            if ($doTag -ne 'n' -and $doTag -ne 'N') {
                git tag $tag
                Write-Host "  已创建 tag: $tag" -ForegroundColor Green
                Write-Host "  推送: git push origin $tag" -ForegroundColor DarkGray
            }
        }
    } else {
        Write-Host "  Git 未安装或不在 PATH, 跳过 tag" -ForegroundColor DarkGray
    }
}

# ══════════════════════════════════════════════
#  汇总
# ══════════════════════════════════════════════
Write-Host ""
Write-Host "╔══════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "║   发布 v$Version 完成                    " -ForegroundColor Green
Write-Host "╚══════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
Write-Host "  产物目录:  $ReleaseDir\" -ForegroundColor White
Write-Host "  固件文件:  ${FwName}.bin" -ForegroundColor White
Write-Host "  大小:      $Size bytes ($([math]::Round($Size/1024, 1)) KB)" -ForegroundColor White
Write-Host ""
Write-Host "下一步:" -ForegroundColor Yellow
Write-Host "  1. 把 ${FwName}.bin 上传到 OTA 服务器" -ForegroundColor White
Write-Host "  2. 通过 MQTT 下发 manifest JSON 到目标设备" -ForegroundColor White
if (-not $NoGitTag) {
    Write-Host "  3. git push origin v$Version (推送 tag)" -ForegroundColor White
}
