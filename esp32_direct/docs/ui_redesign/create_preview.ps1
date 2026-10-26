$src='C:/Users/29538/Documents/xwechat_files/wxid_6n4stl4q5dzq22_9518/msg/file/2026-09/App(10)/App/source'
$lattice=Get-Content -LiteralPath "$src/GUI/LatticeDiagram.c" -Raw
function Icon([string]$name,[int]$x,[int]$y){
 $body=[regex]::Match($lattice,'GUI_CHARINFO\s+'+$name+'\s*=\s*\{([^}]+)\}', 'Singleline').Groups[1].Value
 $w=[Convert]::ToInt32([regex]::Match($body,'XSize\s*=\s*0[Xx]([0-9A-Fa-f]+)').Groups[1].Value,16)
 $h=[Convert]::ToInt32([regex]::Match($body,'XDist\s*=\s*0[Xx]([0-9A-Fa-f]+)').Groups[1].Value,16)
 $stride=[int][regex]::Match($body,'BytesPerLine\s*=\s*(\d+)').Groups[1].Value
 $dataName=[regex]::Match($body,'pData\s*=\s*(\w+)').Groups[1].Value
 $dataBody=[regex]::Match($lattice,'const unsigned char\s+'+$dataName+'\[\d+\]\s*=\s*\{(.*?)\};','Singleline').Groups[1].Value
 $dataBody=[regex]::Replace($dataBody,'/\*.*?\*/','','Singleline')
 $bytes=@([regex]::Matches($dataBody,'0[Xx]([0-9A-Fa-f]+)') | ForEach-Object {[Convert]::ToInt32($_.Groups[1].Value,16)})
 if($bytes.Count -ne $stride*$h){throw "Invalid source bitmap $name"}
 $path=[Text.StringBuilder]::new()
 for($row=0;$row -lt $h;$row++){for($col=0;$col -lt $w;$col++){if($bytes[$row*$stride+[int][Math]::Floor($col/8)] -band (128 -shr ($col%8))){[void]$path.Append("M$($x+$col) $($y+$row)h1v1h-1z")}}}
 return '<path fill="#454b50" d="'+$path.ToString()+'"/>'
}
$icons=(Icon 'PowerGridIcon' 16 35)+(Icon 'SolarIcon' 134 21)+(Icon 'GeneratorIcon' 14 136)+(Icon 'InverterIcon' 140 90)+(Icon 'HouseIcon' 262 95)
$svg=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'preview_template.svg') -Raw
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'arm-vs-dark.svg'),$svg.Replace('__ICONS__',$icons),[Text.UTF8Encoding]::new($false))
'Created source reconstruction and dark design preview'

