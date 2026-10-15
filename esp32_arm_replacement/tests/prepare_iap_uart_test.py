"""Extract the actual UART sender for whole-frame and bounded-wait regression."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'main/direct/direct_poll.c').read_text(encoding='utf-8')
start=source.index('static bool iap_send(')
brace=source.index('{',start);depth=1;end=brace+1
while depth:
 depth+=(source[end]=='{')-(source[end]=='}');end+=1
(root/'tmp/iap_uart_under_test.inc').write_text(source[start:end],encoding='utf-8')
