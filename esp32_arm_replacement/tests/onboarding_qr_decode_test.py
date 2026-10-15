"""Independent decoder acceptance. Install zxing-cpp in the host test runtime."""
from pathlib import Path
import sys
if Path('tmp/qr_decode_python').exists():
    sys.path.insert(0,str(Path('tmp/qr_decode_python').resolve()))
import zxingcpp
from PIL import Image
expected='csinv://bind?sn=H1ZZX0013900001P&pin=001234'
paths=list(Path('tmp').glob('onboarding_scene_*.ppm'))
assert len(paths)==4, 'Generate all four actual scene captures first'
for path in [Path('tmp/onboarding_qr.pgm'),*paths]:
    results=zxingcpp.read_barcodes(Image.open(path))
    assert len(results)==1 and results[0].text==expected,(path,results)
print('onboarding QR independent ZXing decode: PASS (raw + 4 actual scene images)')
