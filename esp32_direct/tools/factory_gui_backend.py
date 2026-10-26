"""GUI operations reuse the physical protocol and guarded wired flasher."""
import contextlib, datetime, json, re, sqlite3, sys, traceback
from pathlib import Path
import factory_tool as factory
import flash_production as flasher

VERSION='1.3.0'

MUTATIONS=('flash','flash-bin','provision','verify','lock','pairing','diagnostic-start','diagnostic-stop','test')

def provisioning_v2(snapshot):
 return snapshot.get('factory_api',0)>=3 and snapshot.get('identity_schema')==2 and snapshot.get('provisioning_identity_v2') is True

def require_mac(expected,actual=None):
 expected=str(expected or '').upper()
 if not re.fullmatch(r'(?:[0-9A-F]{2}:){5}[0-9A-F]{2}',expected):raise ValueError('请重新识别本台设备，缺少有效的预期 MAC')
 raw=bytes.fromhex(expected.replace(':',''))
 if raw[0]&1 or not any(raw):raise ValueError('预期 MAC 无效，请重新识别本台设备')
 if actual is not None and expected!=str(actual or '').upper():raise ValueError('设备 MAC 已改变，请重新识别并核对本台工单')
 return expected

def require_runtime_identity(snapshot):
 identity=snapshot.get('identity') or {}
 if not provisioning_v2(snapshot) or type(snapshot.get('runtime_generation')) is not int or 'identity_pending_reboot' not in snapshot:
  raise ValueError('当前固件缺少重启复核证据，请先更新固件并重新识别')
 if (snapshot.get('identity_pending_reboot') is not False or snapshot.get('runtime_sn')!=identity.get('sn') or
     type(identity.get('generation')) is not int or identity['generation']<1 or snapshot['runtime_generation']!=identity['generation']):
  raise ValueError('身份尚未重启生效，请断电重启后重新识别并复核')

def capabilities(snapshot):
 identity=snapshot.get('identity') or {}
 return {'diagnostics':bool(snapshot.get('diagnostics_enabled',snapshot.get('factory_write_enabled',False))),
         'identity_write':provisioning_v2(snapshot) and bool(snapshot.get('factory_write_enabled')) and not identity.get('locked',False),
         'pairing_write':bool(snapshot.get('pairing_identity_v1')) and bool(snapshot.get('factory_write_enabled')) and not identity.get('locked',False),
         'identity_verify':provisioning_v2(snapshot) and bool(identity),'identity_lock':bool(snapshot.get('factory_write_enabled')) and bool(identity) and not identity.get('locked',False)}

def friendly_error(error,operation):
 code=getattr(error,'code','')
 if code=='WRITE_DISABLED':
  return ('当前固件未开放诊断测试。请在“固件维护”页更新到同配置 v0.6.2，再重新识别设备。身份写入权限保持不变。'
          if operation.startswith('diagnostic') or operation=='test' else
          '当前固件只允许查询身份，不能写入或锁定 SN。量产写身份需 Factory 配置；Device 补写 PIN 使用已认证串口命令，不需要改成 Factory。')
 if code=='BUSY':return '设备正在执行 OTA，请等待升级完成后重试。'
 if isinstance(error,TimeoutError):return '设备未在规定时间内回复。请确认已上电、选择正确的 USB 串口，并关闭其他串口工具。'
 if code=='READBACK_MISMATCH':return '设备身份与工单不一致，请检查本台 SN 和字段。'
 return str(error)

def default_profile():
 return {**{name:'' for name,_ in factory.FIELDS},'sn':'','product':'CSERGY','model':'CS-L10-6K2','hw_system':'UNSET',
         'hw_controller':'UNSET','hw_dsp':'','hw_bms':'','batch':'',
         'production_time':datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
         'calibration_version':'','calibration_summary':''}

def worker_command(job):
 if getattr(sys,'frozen',False):return [sys.executable,'--worker',str(job)]
 return [sys.executable,str(Path(__file__).with_name('factory_gui.py')),'--worker',str(job)]

def load_json(path):
 return json.loads(Path(path).read_text(encoding='utf-8-sig')) if path else {}

def execute(job):
 port=job.get('port','').strip()
 if not port:raise ValueError('请选择实际串口')
 operation=job['operation']
 allowed=('flash','flash-bin','identify-rom','inspect','live','provision','verify','lock','pairing','bms-identity','diagnostic-start','diagnostic-stop','test')
 if operation not in allowed:raise ValueError('unknown operation')
 if operation in MUTATIONS:require_mac(job.get('expected_mac'))
 if operation=='identify-rom':
  import flash_identify
  return flash_identify.identify(port)
 if operation in ('flash','flash-bin'):
  # Frozen applications cannot run "python -m esptool". Use its packaged API
  # within this separate worker process; retain original hash/backup/readback flow.
  import esptool
  def command(p,*args,baud=flasher.DEFAULT_BAUD):
   esptool.main(flasher.esptool_arguments(p,*args,baud=baud))
  if operation=='flash-bin':
   import flash_single_bin
   def bin_command(p,*args,baud=flasher.DEFAULT_BAUD):
    # Keep the application stopped between backup and preservation readback;
    # normal app boot can legitimately update NVS while these checks run.
    esptool.main(flasher.esptool_arguments(p,'--after','no_reset',*args,baud=baud))
   return flash_single_bin.execute(job,bin_command)
  previous=flasher.command;argv=sys.argv
  try:
   flasher.command=command
   sys.argv=['flash_production','--port',port,'--release',job['release'],
             '--backup-dir',job['backup_dir'],'--baud',str(job.get('baud',flasher.DEFAULT_BAUD)),
             '--expected-mac',job['expected_mac']]
   if job.get('first_install'):sys.argv.append('--first-install')
   flasher.main()
  finally:flasher.command=previous;sys.argv=argv
  return {'result':'OK','operation':'flash','backup_dir':job['backup_dir'],'baud':job.get('baud',flasher.DEFAULT_BAUD),
          'next_step':'烧录分段已校验；请断电重启，再查询设备与复核身份。'}
 client=factory.Client(port)
 try:
  snapshot=client.request(1);caps=capabilities(snapshot)
  if operation in MUTATIONS:snapshot['mac']=require_mac(job['expected_mac'],snapshot.get('mac',''))
  if operation in ('diagnostic-start','diagnostic-stop','test') and not caps['diagnostics']:
   raise factory.FirmwareError({'result':'WRITE_DISABLED'})
  if operation in ('provision','lock') and not snapshot.get('factory_write_enabled'):
   raise factory.FirmwareError({'result':'WRITE_DISABLED'})
  if operation=='provision' and not provisioning_v2(snapshot):raise ValueError('当前固件不支持扩展工单，请更新 Factory 固件后重新识别')
  if operation=='provision' and (snapshot.get('identity') or {}).get('locked'):raise factory.FirmwareError({'result':'LOCKED'})
  if operation in ('diagnostic-start','diagnostic-stop'):
   response=client.request(6,b'\2' if operation=='diagnostic-start' else b'\0')
   return {**snapshot,**response,'diagnostic_active':operation=='diagnostic-start'}
  if operation=='live':return client.request(4)
  if operation=='test':
   try:report=client.request(4)
   finally:client.request(6,b'\0')
   return factory.build_test_report(report,load_json(job.get('limits')),
          load_json(job.get('references')),job.get('evidence'))
  if operation=='bms-identity':return factory.query_bms_identity(client)
  if operation=='inspect':return snapshot
  if operation=='pairing':
   if not caps['pairing_write']:raise factory.FirmwareError({'result':'WRITE_DISABLED'})
   import factory_pairing
   sn=(snapshot.get('identity') or {}).get('sn','')
   if sn!=snapshot.get('runtime_sn'):raise ValueError('身份SN尚未重启生效，请先重启设备')
   pin=factory_pairing.compute_pin(sn)
   result=client.request(8,factory_pairing.payload(sn,pin))
   report=client.request(1)
   require_mac(job['expected_mac'],report.get('mac',''))
   if not report.get('pairing_ready') or (report.get('identity') or {}).get('sn')!=sn or report.get('runtime_sn')!=sn:raise factory.FirmwareError({'result':'READBACK_MISMATCH'})
   return {**report, 'next_step':'扫码身份写入并读回成功。请在设备的手机连接页面扫描二维码。'}
  if operation=='lock':
   if provisioning_v2(snapshot):require_runtime_identity(snapshot)
   if snapshot.get('pairing_identity_v1') and not snapshot.get('pairing_ready'):raise ValueError('扫码 PIN 未就绪，不能锁定身份；请先写入并复核 PIN')
   identity=snapshot.get('identity') or {}
   if any(identity.get(k,'UNSET') in ('','UNSET') for k in ('hw_system','hw_controller')):
    raise ValueError('硬件版本未确定，不能锁定身份')
   if not identity.get('calibration_summary'):raise ValueError('没有校准/测试报告引用，不能锁定身份')
   return client.request(3)
  profile=job['profile']
  with contextlib.closing(sqlite3.connect(job['registry'])) as db:
   if operation=='provision':
    payload=factory.record(profile,snapshot['mac'],schema=2)
    factory.reserve_identity(db,profile['sn'],snapshot['mac'])
    report=client.request(2,payload);require_mac(job['expected_mac'],report.get('mac',''));factory.verify(profile,report)
    db.execute('UPDATE devices SET status=? WHERE sn=?',('written_waiting_reboot',profile['sn']));db.commit()
    report['next_step']='身份写入并读回成功。请断电重启后执行复核；这不等于量产验收通过。'
    return report
   require_runtime_identity(snapshot);factory.verify(profile,snapshot)
   factory.reserve_identity(db,profile['sn'],snapshot['mac'])
   db.execute('UPDATE devices SET status=? WHERE sn=? AND mac=?',('readback_verified',profile['sn'],snapshot['mac']));db.commit()
   return snapshot
 finally:client.close()

def worker_main(path):
 job=load_json(path)
 with open(job['log'],'w',encoding='utf-8',buffering=1) as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
  try:
   print('操作:',job['operation'],'串口:',job['port'],flush=True)
   report=execute(job)
   report['port']=job['port'];report['operation']=job['operation']
   report['captured_at']=datetime.datetime.now(datetime.timezone.utc).isoformat()
   factory.save(job['output'],report)
   print(json.dumps(report,ensure_ascii=False,indent=2),flush=True)
   return 0
  except BaseException as error:
   traceback.print_exc()
   factory.save(job['output'],{'result':'ERROR','message':friendly_error(error,job['operation']),
                             'code':getattr(error,'code','ERROR'),'operation':job['operation']})
   return 1
