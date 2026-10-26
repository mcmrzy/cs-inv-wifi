"""Extract the real OTA boot-confirmation function for a fake ESP/NVS boundary."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
for name in ('sdkconfig.defaults','sdkconfig.device','sdkconfig.factory','sdkconfig.production'):
 config=(root/name).read_text(encoding='utf-8')
 stack=re.search(r'^CONFIG_ESP_MAIN_TASK_STACK_SIZE=(\d+)$',config,re.M)
 assert stack and int(stack[1])>=12288, f'{name}: boot confirmation needs a 12KB startup stack'
source=(root/'main/ota/ota_service.c').read_text(encoding='utf-8')
start=source.index('static void boot_health_check(')
brace=source.index('{',start);depth=1;end=brace+1
while depth:
 depth+=(source[end]=='{')-(source[end]=='}');end+=1
boot=source[start:end]
start=source.index('bool ota_service_start_boot_health_check(')
brace=source.index('{',start);depth=1;end=brace+1
while depth:
 depth+=(source[end]=='{')-(source[end]=='}');end+=1
(root/'tmp/ota_boot_under_test.inc').write_text(boot+'\n'+source[start:end],encoding='utf-8')
