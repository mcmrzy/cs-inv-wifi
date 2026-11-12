param([string]$Port='',
      [string]$Python='C:/Users/29538/.espressif/python_env/idf5.5_py3.13_env/Scripts/python.exe')
$ErrorActionPreference='Stop'
if(-not (Test-Path -LiteralPath $Python)){throw 'ESP-IDF Python missing; use your existing USB serial terminal instead.'}
if(-not $Port){
    & $Python -m serial.tools.list_ports -v
    Write-Host 'Run: ./console.ps1 -Port COM5 (replace COM5 with the ESP USB port)'
    exit
}
Write-Host 'USB terminal 115200; inverter baud is set by the firmware baud command.'
Write-Host 'Type help then Enter. Use stop to cancel a test. Ctrl+] closes this terminal.'
& $Python -m serial.tools.miniterm $Port 115200 --eol CRLF --raw
if($LASTEXITCODE -ne 0){throw 'USB serial terminal exited with an error'}
