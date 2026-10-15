"""Extract the actual panel init functions for the host hardware boundary fake.

Production splash renderer is compiled unchanged; ESP hardware calls are faked.
This checks temporal safety (no home before splash, no scanout before a complete
clear), rather than testing a separately recreated initialization sequence.
"""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'main/direct/direct_panel.c').read_text(encoding='utf-8')
def extract(name):
    start=s.index(name+'(')
    start=s.rfind('\n',0,start)+1
    brace=s.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
(root/'tmp/boot_init_under_test.inc').write_text(extract('panel_register_init')+'\n'+extract('direct_panel_init')+'\n',encoding='utf-8')
