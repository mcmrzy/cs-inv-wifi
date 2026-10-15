"""Package locally verified candidate artifacts; never deploys or flashes."""
from pathlib import Path
import hashlib,json,shutil,zipfile
root=Path(__file__).resolve().parents[1]
out=root/'release/CSERGY_Onboarding_20261014'
firmware=root/'release/esp32c3_v0.6.7_onboarding_dev_20261013'
for profile in ('device','factory','production'):
    manifest=json.loads((firmware/profile/'flash_manifest.json').read_text(encoding='utf-8'))
    assert manifest['version']=='0.6.7-onboarding-dev' and not manifest['hardware_acceptance']
    for name,digest in manifest['sha256'].items():
        assert hashlib.sha256((firmware/profile/name).read_bytes()).hexdigest()==digest
    shutil.copytree(firmware/profile,out/'firmware'/profile,dirs_exist_ok=True)
for src,dst in (
 ('release/CSERGY_FactoryTool_v1.2.0_Windows_x64.zip','CSERGY_FactoryTool_v1.2.0_Windows_x64.zip'),
 ('docs/research/2026-10-13_扫码引导实施与验收.md','使用与验收说明.md'),
 ('docs/ui_redesign/qr_onboarding/dark_zh.png','扫码界面_深色中文.png'),
 ('tools/flash_production.py','flash_production.py'),
 ('components/qrcodegen/UPSTREAM.md','third_party/qrcodegen_UPSTREAM.md'),
 ('tmp/v067_host_tests.log','verification/firmware_host_tests.log'),
 ('tmp/qr_app_tests.log','verification/app_tests.log'),
 ('tmp/qr_app_analyze.log','verification/app_analyze.log'),
 ('tmp/qr_app_build.log','verification/app_build.log'),
 ('tmp/qr_backend_tests.log','verification/backend_tests.log'),
 ('tmp/qr_backend_vet.log','verification/backend_vet.log'),
):
    target=out/dst;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/src,target)
apk=out/'CSERGY_App_v1.1.10_build38.apk'
assert apk.is_file()
metadata={'firmware':'0.6.7-onboarding-dev','tool':'1.2.0','app':'1.1.10+38',
 'app_signature_matches_existing':True,'hardware_acceptance':False,'server_deployed':False,
 'production_pin_configured':False,'cloud_binding_public_key_configured':False}
(out/'manifest.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
checks={str(p.relative_to(out)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest()
        for p in out.rglob('*') if p.is_file() and p.name!='SHA256SUMS.txt'}
(out/'SHA256SUMS.txt').write_text(''.join(f'{h}  {n}\n' for n,h in sorted(checks.items())),encoding='utf-8')
archive=out.with_suffix('.zip')
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    for p in out.rglob('*'):
        if p.is_file():z.write(p,p.relative_to(out.parent))
with zipfile.ZipFile(archive) as z:assert z.testzip() is None
digest=hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n',encoding='ascii')
print(json.dumps({'path':str(archive),'sha256':digest,'bytes':archive.stat().st_size}))
