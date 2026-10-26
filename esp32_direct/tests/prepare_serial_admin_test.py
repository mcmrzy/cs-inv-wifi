from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'main/factory/factory_service.c').read_text(encoding='utf-8')
start=s.index('for(int i=0;i<n;i++){');brace=s.index('{',start);depth=1;end=brace+1
while depth:
 depth+=(s[end]=='{')-(s[end]=='}');end+=1
(root/'tmp/serial_mux_under_test.inc').write_text(s[start:end],encoding='utf-8')
