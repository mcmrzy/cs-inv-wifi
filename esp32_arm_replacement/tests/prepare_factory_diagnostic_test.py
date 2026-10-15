from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'main/factory/factory_service.c').read_text(encoding='utf-8')
start=source.index('else if(request.command==6)')
brace=source.index('{',start);depth=1;end=brace+1
while depth:
 depth+=(source[end]=='{')-(source[end]=='}');end+=1
(root/'tmp/factory_diagnostic_under_test.inc').write_text(source[brace+1:end-1],encoding='utf-8')
panel=(root/'main/direct/direct_panel_runtime.inc').read_text(encoding='utf-8')
start=panel.index('if(factory_testing){panel_beep();')
end=panel.index('\n',start)
(root/'tmp/factory_key_guard_under_test.inc').write_text(panel[start:end],encoding='utf-8')
start=source.index('if(atomic_load(&display_test)&&tick-')
end=source.index('\n',start)
(root/'tmp/factory_diagnostic_lease_under_test.inc').write_text(source[start:end],encoding='utf-8')
