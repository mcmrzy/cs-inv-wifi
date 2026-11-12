"""Build the native UI translation table; no runtime allocation is needed."""
from pathlib import Path
import json
root=Path(__file__).resolve().parents[1]
pairs=[line.split('|',1) for line in (root/'tools/native_locale_terms.txt').read_text(encoding='utf-8').splitlines() if line]
assert len({pair[0] for pair in pairs})==len(pairs),'Duplicate translation key'
pairs.sort(key=lambda pair:len(pair[0].encode('utf-8')),reverse=True)
text='/* Generated from tools/native_locale_terms.txt; longest source match wins. */\n'
offsets=[]
for i,lang in enumerate(('zh','en')):
    at=0;column=[]
    text+=f'static const char {lang}_text[]=\n'
    for pair in pairs:
        column.append(at)
        text+=json.dumps(pair[i],ensure_ascii=False)[:-1]+'\\0"\n'
        at+=len(pair[i].encode('utf-8'))+1
    assert at<65536
    offsets.append(column)
    text+=';\n'
text+='static const translation_t terms[]={\n'
text+='\n'.join(f'{{{zh},{en}}},' for zh,en in zip(*offsets))+'\n};\n'
(root/'main/panel_locale_terms.inc').write_text(text,encoding='utf-8')
print(f'Generated {len(pairs)} native UI translation terms')
