"""Physical USB factory tool. No hardware is accessed by import/tests.
Use: python factory_tool.py --port COM11 inspect
Requires pyserial only for physical commands, not codec/limits/report tests.
"""
import argparse, datetime, json, math, sqlite3, struct, time, zlib
from pathlib import Path
FIELDS=(('sn',32),('product',32),('model',32),('hw_system',16),('hw_controller',16),('hw_dsp',16),('hw_bms',16),('batch',32),('production_time',24),('calibration_version',32),('calibration_summary',64))
class FirmwareError(RuntimeError):
 def __init__(self,reply):
  self.reply=reply;self.code=reply.get('result','DEVICE_ERROR');super().__init__(str(reply))
def frame(command,request_id,payload):
 if len(payload)>1200: raise ValueError('payload too long')
 body=struct.pack('<BBIH',1,command,request_id,len(payload))+payload
 return b'\xc5F'+body+struct.pack('<I',zlib.crc32(body))
class Parser:
 def __init__(self): self.buffer=bytearray()
 def feed(self,data):
  self.buffer.extend(data);result=[]
  while True:
   start=self.buffer.find(b'\xc5F')
   if start<0:
    self.buffer[:]=self.buffer[-1:] if self.buffer.endswith(b'\xc5') else b'';break
   del self.buffer[:start]
   if len(self.buffer)<10:break
   version,command,rid,n=struct.unpack_from('<BBIH',self.buffer,2)
   if version!=1 or n>1200:del self.buffer[0];continue
   if len(self.buffer)<n+14:break
   packet=bytes(self.buffer[:n+14]);del self.buffer[:n+14]
   if zlib.crc32(packet[2:-4])==struct.unpack_from('<I',packet,n+10)[0]:result.append((command,rid,packet[10:-4]))
  return result

def record(profile,mac):
 d={**profile};d.setdefault('product','CSERGY');d.setdefault('model','CS-L10-6K2')
 parsed=datetime.datetime.strptime(d['production_time'],'%Y-%m-%dT%H:%M:%SZ')
 if parsed.year<2000:raise ValueError('production year')
 address=bytes.fromhex(mac.replace(':',''))
 if len(address)!=6 or address[0]&1 or not any(address):raise ValueError('MAC')
 result=bytearray(struct.pack('<IHHIBB',0x31544346,1,336,1,0,0)+address)
 for name,size in FIELDS:
  text=str(d.get(name,''));raw=text.encode('ascii')
  if len(raw)>=size or any(x<32 or x>126 for x in raw):raise ValueError(f'{name}: invalid or too long')
  if name in ('sn','product','model','hw_system','hw_controller','batch','production_time') and not raw:raise ValueError(f'{name}: required')
  if name=='sn' and not all(c.isalnum() or c in '-_.' for c in text):raise ValueError('SN characters')
  result.extend(raw+b'\0'*(size-len(raw)))
 if len(result)!=332:raise AssertionError(len(result))
 return bytes(result)+struct.pack('<I',zlib.crc32(result))

def evaluate(measured,reference,tolerance):
 if measured is None or reference is None or not tolerance:return {'status':'PENDING','reason':'missing sample/reference/threshold'}
 for n in (measured,reference):
  if not isinstance(n,(int,float)) or not math.isfinite(n):raise ValueError('nonfinite measurement')
 absolute=tolerance.get('absolute',0);relative=tolerance.get('relative_percent',0)
 if absolute<0 or relative<0 or not math.isfinite(absolute+relative):raise ValueError('invalid tolerance')
 error=abs(measured-reference);limit=absolute+abs(reference)*relative/100
 return {'status':'PASS' if error<=limit else 'FAIL','measured':measured,'reference':reference,'error':error,'limit':limit}
class Client:
 def __init__(self,port):
  import serial
  self.serial=serial.Serial(port,115200,timeout=.15);self.parser=Parser();self.request_id=0
 def request(self,command,payload=b'',timeout=8):
  self.request_id+=1;rid=self.request_id
  self.serial.write(frame(command,rid,payload));deadline=time.monotonic()+timeout
  while time.monotonic()<deadline:
   for cmd,i,data in self.parser.feed(self.serial.read(512)):
    if i==rid and cmd==command|0x80:
     result=json.loads(data)
     if result.get('result')!='OK':raise FirmwareError(result)
     return result
  raise TimeoutError(f'no valid reply id={rid}, command={command}')
 def close(self):self.serial.close()
def save(path,data):
 path=Path(path);path.parent.mkdir(parents=True,exist_ok=True);temp=path.with_suffix(path.suffix+'.tmp');temp.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8');temp.replace(path)
def verify(profile,snapshot):
 actual=snapshot.get('identity')
 if not actual:raise RuntimeError('no factory identity')
 for key,value in profile.items():
  if key in dict(FIELDS) and actual.get(key)!=value:raise RuntimeError(f'readback mismatch {key}: {actual.get(key)!r} != {value!r}')
 return actual

def reserve_identity(db,sn,mac):
 db.execute('CREATE TABLE IF NOT EXISTS devices(sn TEXT PRIMARY KEY,mac TEXT UNIQUE,status TEXT NOT NULL)')
 old=db.execute('SELECT mac FROM devices WHERE sn=?',(sn,)).fetchone()
 if old and old[0]!=mac:raise RuntimeError('SN reserved for another MAC')
 old=db.execute('SELECT sn FROM devices WHERE mac=?',(mac,)).fetchone()
 if old and old[0]!=sn:raise RuntimeError('MAC already reserved for another SN')
 db.execute('INSERT OR IGNORE INTO devices VALUES(?,?,?)',(sn,mac,'reserved'));db.commit()

def query_bms_identity(client,timeout=20):
 report=client.request(7,b'\1');deadline=time.monotonic()+timeout
 while report.get('pending') and time.monotonic()<deadline:
  time.sleep(.25);report=client.request(7,b'\0')
 if report.get('pending'):report['query_status']='PENDING_TIMEOUT_OR_OTA_PAUSE'
 return report

def build_test_report(report,limits=None,references=None,evidence=None):
 limits=limits or {};references=references or {};evidence=evidence or {}
 report=dict(report)
 measurements={**report.get('measurements',{}),**{f'bms_{k}':v for k,v in report.get('bms',{}).items()}}
 checks={'flash':{'status':'PASS' if report.get('flash_bytes')==4194304 else 'FAIL'},'dsp_link':{'status':'PASS' if report.get('dsp_online') else 'FAIL'},'bms_link':{'status':'PASS' if report.get('bms_online') else 'FAIL'},'keys':{'status':'PASS' if report.get('keys')==63 else 'PENDING'}}
 for name in ('display','buzzer'):
  state=evidence.get(name,'PENDING')
  if state not in ('PASS','FAIL','PENDING'):raise ValueError('invalid operator evidence')
  checks[name]={'status':state}
 for channel in set(references)|set(limits):checks[channel]=evaluate(measurements.get(channel),references.get(channel),limits.get(channel))
 checks['calibration_write']={'status':'PENDING','reason':report.get('calibration_write','not verified')}
 checks['power_test']={'status':'PENDING','reason':'requires calibrated instruments and approved load procedure'}
 report['checks']=checks;report['overall']='FAIL' if any(v['status']=='FAIL' for v in checks.values()) else 'PENDING' if any(v['status']=='PENDING' for v in checks.values()) else 'PASS'
 return report

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port');p.add_argument('--output',default='factory_report.json');p.add_argument('--registry',default='factory_sn.sqlite3')
 sub=p.add_subparsers(dest='command',required=True)
 sub.add_parser('inspect');sub.add_parser('export');sub.add_parser('lock');sub.add_parser('bms-identity')
 for name in ('provision','verify'):sub.add_parser(name).add_argument('profile')
 test=sub.add_parser('test');test.add_argument('--limits');test.add_argument('--references');test.add_argument('--interactive',action='store_true')
 args=p.parse_args()
 if not args.port:p.error('--port required; never guesses serial port')
 c=Client(args.port)
 try:
  snapshot=c.request(1)
  if args.command in ('inspect','export'):report=snapshot
  elif args.command=='bms-identity':report=query_bms_identity(c)
  elif args.command=='lock':
   identity=snapshot.get('identity',{})
   if any(identity.get(k,'UNSET') in ('','UNSET') for k in ('hw_system','hw_controller')):raise RuntimeError('hardware version is unknown; refuse release lock')
   if not identity.get('calibration_summary'):raise RuntimeError('no calibration/test record reference; refuse release lock')
   report=c.request(3)
  elif args.command in ('provision','verify'):
   profile=json.loads(Path(args.profile).read_text(encoding='utf-8-sig'))
   if args.command=='provision':
    payload=record(profile,snapshot['mac'])
    with sqlite3.connect(args.registry) as db:
     reserve_identity(db,profile['sn'],snapshot['mac'])
     report=c.request(2,payload)
     verify(profile,report)
     db.execute('UPDATE devices SET status=? WHERE sn=?',('written_waiting_reboot',profile['sn']));db.commit()
     report['next_step']='Power-cycle device, then verify profile. Written identity is not production acceptance.'
   else:
    verify(profile,snapshot);report=snapshot
    with sqlite3.connect(args.registry) as db:
     db.execute('CREATE TABLE IF NOT EXISTS devices(sn TEXT PRIMARY KEY,mac TEXT UNIQUE,status TEXT NOT NULL)')
     db.execute('UPDATE devices SET status=? WHERE sn=? AND mac=?',('readback_verified',profile['sn'],snapshot['mac']));db.commit()
  else:
   limits=json.loads(Path(args.limits).read_text(encoding='utf-8-sig')) if args.limits else {}
   references=json.loads(Path(args.references).read_text(encoding='utf-8-sig')) if args.references else {}
   c.request(6,b'\2');evidence={}
   if args.interactive:
    print('Check display patterns and buzzer; press each of the six keys.')
    evidence={name:('PASS' if input(name+' observed correctly? [yes/no]: ').strip().lower()=='yes' else 'FAIL') for name in ('display','buzzer')}
    input('Press every key, then Enter to capture: ')
   try:report=c.request(4)
   finally:c.request(6,b'\0')
   report=build_test_report(report,limits,references,evidence)
  report['captured_at']=datetime.datetime.now(datetime.timezone.utc).isoformat();save(args.output,report);print(json.dumps(report,ensure_ascii=False,indent=2))
 finally:c.close()
if __name__=='__main__':main()
