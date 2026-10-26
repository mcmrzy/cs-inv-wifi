from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / 'main/telemetry/telemetry.c').read_text(encoding='utf-8')
def function(name):
    start = source.index('void ' + name + '(')
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor]
(root / 'tmp/telemetry_fresh_under_test.inc').write_text(
    function('telemetry_on_mqtt_connected') + '\n' + function('telemetry_request_info') + '\n' + function('telemetry_tick'), encoding='utf-8')
