from pathlib import Path
root=Path(__file__).resolve().parents[1]
text=(root/'main/direct/direct_poll.c').read_text(encoding='utf-8')
start=text.index('static bool uart_setup(')
brace=text.index('{',start);end=brace+1;depth=1
while depth:
    depth+=(text[end]=='{')-(text[end]=='}');end+=1
(root/'tmp/uart_setup_under_test.inc').write_text(text[start:end]+'\n',encoding='utf-8')
