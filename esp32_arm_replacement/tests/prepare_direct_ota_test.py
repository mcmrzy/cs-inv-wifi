"""Test the real downstream OTA service against fake file/UART boundaries."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'main/ota/ota_service.c').read_text(encoding='utf-8')
start=source.index('static bool direct_stage_read(')
end=source.index('static int http_stream_read(',start)
(root/'tmp/direct_ota_under_test.inc').write_text(source[start:end],encoding='utf-8')
