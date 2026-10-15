from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'main/direct/direct_panel_runtime.inc').read_text(encoding='utf-8')
def function(name):
    start=source.index('static ',source.index(name)-35)
    brace=source.index('{',source.index(name));end=brace+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
(root/'tmp/onboarding_panel_under_test.inc').write_text(
    function('panel_onboarding_inputs')+'\n'+function('panel_onboarding_close'),encoding='utf-8')
