"""Bundle the locally verified provisioning firmware and workstation.

Never flashes hardware, never deploys a server, and never asserts device or
server acceptance. Every copied firmware file is re-hashed against its
manifest before it enters the archive.
"""
from pathlib import Path
import hashlib,json,shutil,zipfile

root=Path(__file__).resolve().parents[1]
firmware=root/'release/esp32c3_v0.6.34_provisioning_dev_20261016'
tool=root/'release/CSERGY_FactoryTool_v1.3.0_Windows_x64.zip'
out=root/'release/CSERGY_Provisioning_20261016'

version=json.loads((firmware/'factory/flash_manifest.json').read_text(encoding='utf-8'))['version']
assert version=='0.6.34-provisioning-dev',version
for profile in ('device','factory','production'):
    manifest=json.loads((firmware/profile/'flash_manifest.json').read_text(encoding='utf-8'))
    assert manifest['profile']==profile and manifest['version']==version
    assert manifest['model']=='CS-L10-6K2'
    assert manifest['identity_schema']==2 and manifest['factory_api']==3
    assert manifest['hardware_acceptance'] is False,'build must not claim hardware acceptance'
    for name,digest in manifest['sha256'].items():
        assert hashlib.sha256((firmware/profile/name).read_bytes()).hexdigest()==digest,(profile,name)
    shutil.copytree(firmware/profile,out/'firmware'/profile,dirs_exist_ok=True)

for src,dst in (
 (tool,'CSERGY_FactoryTool_v1.3.0_Windows_x64.zip'),
 ('docs/DEVICE_PROVISIONING_V0634.md','设备配置与INFO说明.md'),
 ('tools/README_GUI.md','工位工具使用说明.md'),
 ('tools/factory_profile.example.json','工单示例.json'),
 ('tools/flash_production.py','tools/flash_production.py'),
 ('tmp/final_host_tests.log','verification/firmware_host_tests.log'),
 ('tmp/gui_v130_tests.log','verification/workstation_host_tests.log'),
 ('tmp/gui_v130_frozen.json','verification/frozen_self_test.json'),
 ('tmp/gui_v130_bad_hash.json','verification/guard_bad_hash.json'),
 ('tmp/gui_v130_bad_bin.json','verification/guard_bad_bin.json'),
 ('tmp/gui_v130_missing_mac.json','verification/guard_missing_mac.json'),
):
    source=root/src
    assert source.is_file(),source
    target=out/dst;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)

metadata={
 'firmware':version,'workstation':'1.3.0','model':'CS-L10-6K2',
 'identity_schema':2,'legacy_identity_schema_readable':1,
 'info_identity_reported':True,'info_envelope_max_bytes':4096,
 'hardware_acceptance':False,'physical_flash_verified':False,
 'server_info_fields_deployed':False,
}
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
print(json.dumps({'path':str(archive),'sha256':digest,'bytes':archive.stat().st_size,
                  'hardware_accessed':False},ensure_ascii=False))
