"""Validated ESP32-C3 app / full-flash file flashing, with pre-write backup."""
import hashlib,io,json,struct,datetime
from pathlib import Path
import flash_production as transport

def validate_image(data,application=False):
 from esptool.bin_image import ESP32C3FirmwareImage
 if len(data)<36 or data[0]!=0xe9 or struct.unpack_from('<H',data,12)[0]!=5:
  raise ValueError('不是可识别的 ESP32-C3 固件镜像')
 if application and data[32:36]!=b'\x32\x54\xcd\xab':
  raise ValueError('此文件不是应用固件，不能按 app.bin 写入')
 image=ESP32C3FirmwareImage(io.BytesIO(data));image.verify()
 if image.checksum!=image.calculate_checksum():raise ValueError('固件镜像校验和错误')
 if image.append_digest and image.stored_digest!=image.calc_digest:raise ValueError('固件镜像 SHA256 错误')

def partitions(raw):
 result=[];checksum=False
 for pos in range(0,len(raw)-31,32):
  entry=raw[pos:pos+32];magic=entry[:2]
  if magic==b'\xeb\xeb':
   if hashlib.md5(raw[:pos]).digest()!=entry[16:32]:raise ValueError('分区表 MD5 错误')
   checksum=True;break
  if magic==b'\xff\xff':break
  if magic!=b'\xaa\x50':raise ValueError('分区表格式错误')
  _,kind,subtype,offset,size,label,flags=struct.unpack('<HBBII16sI',entry)
  if not size or offset<0x9000 or offset+size>0x400000 or offset%0x1000 or size%0x1000:
   raise ValueError('分区范围不合法')
  if any(offset<r['offset']+r['size'] and r['offset']<offset+size for r in result):raise ValueError('分区重叠')
  result.append(dict(type=kind,subtype=subtype,offset=offset,size=size))
 if not checksum or not result:raise ValueError('缺少有效的分区表校验')
 return result

def describe(path,kind):
 data=Path(path).read_bytes()
 if kind=='merged':
  if len(data)!=0x400000:raise ValueError('完整合并固件必须为 4MB；其他长度需使用固件目录方式')
  validate_image(data)
  entries=partitions(data[0x8000:0x9000]);apps=[e for e in entries if e['type']==0]
  bootable=False
  for entry in apps:
   chunk=data[entry['offset']:entry['offset']+entry['size']]
   if chunk[:4]==b'\xff'*4:continue
   validate_image(chunk,True)
   if entry['subtype'] in (0,16):bootable=True
  if not bootable:raise ValueError('合并文件未包含 factory 或 ota_0 应用镜像')
  offset=0;scope='覆盖整片 Flash，包括 SN 身份区、网络设置和缓存。已有 SN 可能被清除或替换。'
 elif kind=='app':
  if len(data)>0x1a0000:raise ValueError('应用固件超过本项目 ota_0 分区容量')
  validate_image(data,True)
  offset=0x20000;scope='写入 ota_0，并重置 OTA 启动选择；保留用户 NVS 和 factory 身份区。需设备采用本项目分区。'
  if data[288:292]==b'CSIV' and data[294]==1:scope+=' 此 Device 镜像默认 SN H1ZZX0013900001P，已有管理员持久 SN 优先。'
 else:raise ValueError('请选择应用固件或完整 4MB 合并固件')
 return dict(kind=kind,offset=offset,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),scope=scope)

def execute(job,command):
 info=describe(job['bin_file'],job['bin_kind'])
 if info['sha256']!=job['bin_sha256']:raise RuntimeError('所选文件在确认后发生变化，请重新选择')
 baud=int(job.get('baud',transport.DEFAULT_BAUD))
 if baud not in transport.BAUD_RATES:raise ValueError('unsupported flashing baud rate')
 expected=None
 if info['kind']=='app':
  layout=Path(job['layout']);manifest=json.loads((layout/'flash_manifest.json').read_text())
  expected=(layout/'partition-table.bin').read_bytes()
  if hashlib.sha256(expected).hexdigest()!=manifest['sha256']['partition-table.bin']:raise RuntimeError('分区模板校验失败')
 stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
 backup=Path(job['backup_dir'])/(job['port']+'_'+stamp);backup.mkdir(parents=True,exist_ok=False)
 # Write the checked snapshot, not a path that could be replaced during backup.
 data=Path(job['bin_file']).read_bytes()
 if hashlib.sha256(data).hexdigest()!=info['sha256']:raise RuntimeError('固件文件发生变化')
 staged=backup/'selected_firmware.bin';staged.write_bytes(data)
 print(f"BIN {info['kind']} offset={hex(info['offset'])} bytes={info['bytes']} baud={baud}",flush=True)
 command(job['port'],'read_flash','0','0x400000',backup/'before_4MB.bin',baud=baud)
 full=(backup/'before_4MB.bin').read_bytes()
 if len(full)!=0x400000:raise RuntimeError('整片备份不完整，停止烧录')
 pairs=[hex(info['offset']),staged]
 if expected is not None:
  if full[0x8000:0x8000+len(expected)]!=expected:raise RuntimeError('设备分区不匹配，备份已保存；请使用完整固件目录迁移')
  ota=backup/'ota_data_initial.bin';ota.write_bytes(b'\xff'*8192);pairs+=['0x10000',ota]
 command(job['port'],'write_flash','--flash_size','4MB',*pairs,baud=baud)
 command(job['port'],'verify_flash',*pairs,baud=baud)
 if expected is not None:
  command(job['port'],'read_flash','0x9000','0x6000',backup/'nvs_after.bin',baud=baud)
  command(job['port'],'read_flash','0x3f8000','0x8000',backup/'factory_after.bin',baud=baud)
  if (backup/'nvs_after.bin').read_bytes()!=full[0x9000:0xf000] or (backup/'factory_after.bin').read_bytes()!=full[0x3f8000:]:raise RuntimeError('用户设置或身份区发生意外变化')
 report={'result':'OK','operation':'flash-bin','backup_dir':str(backup),'baud':baud,**info,'next_step':'固件写入及校验完成。请断电重启，再识别设备；备份已保存。'}
 (backup/'flash_result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
 return report
