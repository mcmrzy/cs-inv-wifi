from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / 'main/ota/ota_service.c').read_text(encoding='utf-8')
def extract(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
text = '\n'.join(extract(s) for s in (
    'bool ota_service_admin_begin(', 'void ota_service_admin_end(',
    'static OtaResult check_available('))
(root / 'tmp/admin_ota_under_test.inc').write_text(text, encoding='utf-8')
