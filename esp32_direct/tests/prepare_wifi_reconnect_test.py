from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'main/wifi/wifi_sta.c').read_text(encoding='utf-8')
def function(signature):
    start=source.index(signature)
    brace=source.index('{',start);cursor=brace+1;depth=1
    while depth:
        depth+=(source[cursor]=='{')-(source[cursor]=='}');cursor+=1
    return source[start:cursor]
(root/'tmp/wifi_reconnect_under_test.inc').write_text('\n'.join(function(name) for name in (
    'static bool wifi_connect_if_needed(', 'static void wifi_remember_channel(',
    'static void wifi_event_handler(', 'bool wifi_sta_connect(', 'void wifi_sta_force_disconnect(')),encoding='utf-8')
