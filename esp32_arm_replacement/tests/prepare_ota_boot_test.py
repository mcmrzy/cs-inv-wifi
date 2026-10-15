"""Extract the real OTA boot-confirmation function for a fake ESP/NVS boundary."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'main/ota/ota_service.c').read_text(encoding='utf-8')
start=source.index('static void boot_health_task(')
brace=source.index('{',start);depth=1;end=brace+1
while depth:
 depth+=(source[end]=='{')-(source[end]=='}');end+=1
(root/'tmp/ota_boot_under_test.inc').write_text(source[start:end],encoding='utf-8')
