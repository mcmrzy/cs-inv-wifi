"""Verify bundled firmware and release the standalone Windows workstation."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,zipfile
import pefile
import factory_gui_backend as backend
parser=argparse.ArgumentParser();parser.add_argument('--firmware-version',default='0.6.7-onboarding-dev');parser.add_argument('--host-log',default='v067_host_tests.log');args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
folder=root/'release'/f'CSERGY_FactoryTool_v{backend.VERSION}'
exe=folder/'CSERGY_FactoryTool.exe'
assert pefile.PE(str(exe)).OPTIONAL_HEADER.Subsystem==2,'must be a windowed executable'
for profile in ('device','factory','production'):
 path=folder/'firmware'/profile
 manifest=json.loads((path/'flash_manifest.json').read_text(encoding='utf-8'))
 assert manifest['profile']==profile and manifest['version']==args.firmware_version
 for name,digest in manifest['sha256'].items():
  assert hashlib.sha256((path/name).read_bytes()).hexdigest()==digest,(profile,name)
proof=folder/'验证记录';proof.mkdir(exist_ok=True)
tag='v'+backend.VERSION.replace('.','')
for name in (f'gui_{tag}_frozen.json',f'gui_{tag}_bad_hash.json',f'gui_{tag}_bad_bin.json',f'gui_{tag}_tests.log',args.host_log):
 shutil.copy2(root/'tmp'/name,proof/name)
smoke=json.loads((proof/f'gui_{tag}_frozen.json').read_text(encoding='utf-8'))
assert smoke['result']=='OK' and smoke['gui_version']==backend.VERSION and smoke['default_baud']=='2000000'
assert json.loads((proof/f'gui_{tag}_bad_hash.json').read_text(encoding='utf-8'))['message']=='hash mismatch app.bin'
assert '变化' in json.loads((proof/f'gui_{tag}_bad_bin.json').read_text(encoding='utf-8'))['message']
assert smoke.get('pairing_capability_checked') and smoke.get('secret_config_is_runtime_only')
# Bundling is an explicit whitelist (firmware, docs, proof and executable).
# Refuse common credential files and PEM private-key material before packaging.
for artifact in folder.rglob('*'):
 if not artifact.is_file():continue
 assert artifact.suffix.lower() not in ('.pem','.key','.pfx','.p12'), 'credential file in package'
 assert artifact.name not in ('.env','secrets.json','credentials.json'), 'credential file in package'
 raw=artifact.read_bytes()
 # TLS libraries contain PEM header constants; require actual encoded key
 # content after the header instead of rejecting the parser's literal string.
 assert not re.search(rb'-----BEGIN (?:EC |RSA )?PRIVATE KEY-----\r?\n[A-Za-z0-9+/=]{20,}',raw),'private key material in package'
 configured=os.environ.get('PRODUCT_SECRET','')
 if len(configured)>=8:assert configured.encode('utf-8') not in raw,'configured product secret in package'
(proof/'package_security.json').write_text(json.dumps({'result':'OK','runtime_secret_only':True,'private_key_files_present':False,'hardware_accessed':False},indent=2),encoding='utf-8')
checks={str(p.relative_to(folder)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.rglob('*') if p.is_file() and p.name!='SHA256SUMS.txt'}
(folder/'SHA256SUMS.txt').write_text(''.join(f'{h}  {name}\n' for name,h in sorted(checks.items())),encoding='utf-8')
archive=root/'release'/f'CSERGY_FactoryTool_v{backend.VERSION}_Windows_x64.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
 for p in folder.rglob('*'):
  if p.is_file():z.write(p,p.relative_to(folder.parent))
with zipfile.ZipFile(archive) as z:assert z.testzip() is None
digest=hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n',encoding='ascii')
print(json.dumps({'archive':str(archive),'bytes':archive.stat().st_size,'sha256':digest,'hardware_accessed':False},ensure_ascii=False))
