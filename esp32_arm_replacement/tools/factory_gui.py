"""CSERGY Windows workstation GUI. Serial actions run in isolated workers."""
import argparse, codecs, datetime, json, os, subprocess, sys
from pathlib import Path
import factory_gui_backend as backend

def application_dir():
 return Path(sys.executable).parent if getattr(sys,'frozen',False) else Path(__file__).resolve().parents[1]/'release'

FIELD_LABELS={'sn':'设备 SN','product':'品牌','model':'产品型号','hw_system':'整机硬件版本',
 'hw_controller':'控制板硬件版本','hw_dsp':'DSP 硬件版本','hw_bms':'BMS 硬件版本',
 'batch':'生产批次','production_time':'生产时间（UTC）','calibration_version':'校准版本',
 'calibration_summary':'校准 / 测试报告引用'}
STATUS={'PASS':'通过','FAIL':'失败','PENDING':'待验证'}

class App:
 def refresh_ports(self):
  from serial.tools import list_ports
  ports=list(list_ports.comports());self.ports['values']=[p.device for p in ports]
  if self.port.get() not in self.ports['values']:self.port.set('')
  self.port_hint.set('；'.join(f'{p.device} · {p.description}' for p in ports) or '未发现串口，请连接设备后刷新')

 def choose_directory(self,var):
  path=self.dialog.askdirectory();
  if path:var.set(path)
 def choose_json(self,var):
  path=self.dialog.askopenfilename(filetypes=[('JSON 文件','*.json')]);
  if path:var.set(path)
 def choose_release(self):
  self.choose_directory(self.release);self.describe_release()
 def select_bundled(self):
  folder=application_dir()/'firmware'/self.profile.get()
  if folder.is_dir():self.release.set(str(folder));self.describe_release()
 def describe_release(self):
  try:
   m=backend.load_json(Path(self.release.get())/'flash_manifest.json')
   self.firmware_info.set(f"型号：{m['model']}     版本：{m['version']}     配置：{m['profile']}")
  except Exception as error:self.firmware_info.set('固件目录不可用：'+str(error))
 def current_profile(self):return {k:v.get().strip() for k,v in self.fields.items()}
 def import_profile(self):
  path=self.dialog.askopenfilename(filetypes=[('JSON 工单','*.json')])
  if not path:return
  try:
   profile=backend.load_json(path)
   for k,v in self.fields.items():v.set(profile.get(k,''))
  except Exception as error:self.message.showerror('工单读取失败',str(error))
 def save_profile(self):
  path=self.dialog.asksaveasfilename(defaultextension='.json',filetypes=[('JSON 工单','*.json')])
  if path:
   try:backend.factory.save(path,self.current_profile())
   except Exception as error:self.message.showerror('工单保存失败',str(error))
 def load_snapshot(self):
  identity=(self.snapshot or {}).get('identity')
  if not identity:self.message.showinfo('尚无身份','请先查询已写入 factory 身份的设备。');return
  for k,v in self.fields.items():v.set(identity.get(k,''))
 def open_data(self):
  try:
   folder=Path(self.data.get()).resolve();folder.mkdir(parents=True,exist_ok=True);os.startfile(folder)
  except Exception as error:self.message.showerror('目录打开失败',str(error))
 def export_report(self):
  if not self.last_report:self.message.showinfo('尚无结果','请先查询设备或采集报告。');return
  path=self.dialog.asksaveasfilename(defaultextension='.json',filetypes=[('JSON 报告','*.json')])
  if path:
   try:backend.factory.save(path,self.last_report)
   except Exception as error:self.message.showerror('报告保存失败',str(error))

 def flash(self):
  self.describe_release()
  self.run('flash')

 def run(self,operation):
  if self.process:return
  if not self.port.get():self.message.showerror('未选择串口','请选择烧录电脑上的实际串口。');return
  if operation in ('diagnostic-start','provision','verify','lock','pairing'):
   caps=backend.capabilities(self.snapshot) if self.snapshot and self.peer_port==self.port.get() else {}
   required={'diagnostic-start':'diagnostics','provision':'identity_write','verify':'identity_verify','lock':'identity_lock','pairing':'pairing_write'}[operation]
   if not caps.get(required):
    self.message.showinfo('功能未开放','请先识别本台设备。诊断需同配置 v0.6.2；身份写入需未锁定的 Factory 配置。');return
  if self.diag_active and self.port.get()!=self.diag_port:
   self.message.showerror('测试尚未停止',f'请先选择 {self.diag_port} 停止测试，再切换设备。');return
  if operation=='test' and not self.diag_active:
   self.message.showerror('尚未开始测试','请先启动测试，按下六个按键并观察屏幕、蜂鸣器。');return
  if operation in ('flash','flash-bin') and self.diag_active:
   self.message.showerror('测试尚未停止','请先停止测试，再烧录固件。');return
  try:
   data=Path(self.data.get()).resolve();data.mkdir(parents=True,exist_ok=True)
   stamp=datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f');folder=data/'reports';folder.mkdir(exist_ok=True)
   job={'operation':operation,'port':self.port.get(),'output':str(folder/f'{stamp}_{operation}.json'),
        'log':str(folder/f'{stamp}_{operation}.log'),'registry':str(data/'factory_sn.sqlite3')}
   if operation in ('provision','verify'):
    job['profile']=self.current_profile()
    # Only validates form syntax here; actual wire record uses device MAC.
    backend.factory.record(job['profile'],'02:00:00:00:00:01')
   if operation=='provision' and not self.message.askyesno('写入身份',f"串口：{job['port']}\nSN：{job['profile']['sn']}\n型号：{job['profile']['model']}\n\n将写入并读回。继续？"):return
   if operation=='lock' and not self.message.askyesno('锁定身份','请确认实际硬件版本和校准/测试报告已完成审核。锁定后不能通过普通设置改写身份。继续？'):return
   if operation=='flash':
    release=Path(self.release.get()).resolve();m=backend.load_json(release/'flash_manifest.json')
    job.update(release=str(release),backup_dir=str(data/'backups'),first_install=self.first_install.get(),baud=int(self.baud.get()))
    extra='\n首次迁移：旧 cache 区会在备份后清除。' if job['first_install'] else '\n常规更新：保留 factory 身份和用户 NVS。'
    if m.get('profile')=='device':extra+='\n此镜像固定 SN H1ZZX0013900001P，仅用于该单机。'
    if not self.message.askyesno('备份并烧录',f"串口：{job['port']}\n传输波特率：{job['baud']}\n版本：{m.get('version')}\n配置：{m.get('profile')}\n目录：{release}{extra}\n\n开始备份、烧录和校验？"):return
   if operation=='flash-bin':
    import flash_single_bin
    kind='merged' if self.bin_type.get()=='完整 4MB 合并固件' else 'app'
    info=flash_single_bin.describe(self.bin_file.get(),kind)
    job.update(bin_file=str(Path(self.bin_file.get()).resolve()),bin_kind=kind,bin_sha256=info['sha256'],baud=int(self.baud.get()),backup_dir=str(data/'backups'),layout=str(application_dir()/'firmware'/'device'))
    if not self.message.askyesno('确认单个 BIN 烧录',f"串口：{job['port']}\n文件：{job['bin_file']}\n类型：{self.bin_type.get()}\n地址：{hex(info['offset'])}\n大小：{info['bytes']} 字节\n波特率：{job['baud']}\nSHA256：{info['sha256']}\n\n{info['scope']}\n\n先整片备份，再写入并校验。继续？"):return
   if operation=='test':
    reverse={v:k for k,v in STATUS.items()}
    job.update(evidence={k:reverse[v.get()] for k,v in self.evidence.items()},limits=self.limits.get(),references=self.references.get())
    backend.load_json(job['limits']);backend.load_json(job['references'])
   path=folder/f'{stamp}_{operation}.job.json';backend.factory.save(path,job)
   kwargs={'creationflags':subprocess.CREATE_NO_WINDOW} if os.name=='nt' else {}
   self.process=subprocess.Popen(backend.worker_command(path),**kwargs);self.job=job
   self.log_offset=0;self.decoder=codecs.getincrementaldecoder('utf-8')(errors='replace')
   labels={'inspect':'识别设备','live':'读取实时数据','flash':'备份并更新固件','flash-bin':'备份并烧录 BIN','diagnostic-start':'启动诊断','diagnostic-stop':'结束诊断','test':'保存检测报告','provision':'写入身份','verify':'复核身份','lock':'锁定身份','pairing':'写入扫码身份','bms-identity':'读取 BMS 身份'}
   self.status.set('正在执行：'+labels[operation]+' · '+job['port']);self.progress.start()
   for b in self.actions:b.configure(state='disabled')
   self.append_log('\n──── '+operation+' / '+job['port']+' ────\n')
  except Exception as error:self.message.showerror('操作未启动',str(error))

 def append_log(self,text):
  self.log.configure(state='normal');self.log.insert('end',text);self.log.see('end');self.log.configure(state='disabled')
 def poll(self):
  if self.process:
   try:
    path=Path(self.job['log'])
    if path.exists():
     with path.open('rb') as f:f.seek(self.log_offset);raw=f.read();self.log_offset=f.tell()
     if raw:self.append_log(self.decoder.decode(raw))
    code=self.process.poll()
    if code is not None:
     report=backend.load_json(self.job['output']) if Path(self.job['output']).exists() else {'result':'ERROR','message':'工作进程未返回结果，请查看日志'}
     self.last_report=report;self.progress.stop()
     okay=code==0 and report.get('result','OK')=='OK'
     self.status.set(('完成' if okay else '失败')+' · '+self.job['operation']+' · 报告已保存在数据目录')
     self.device_text.configure(state='normal');self.device_text.delete('1.0','end');self.device_text.insert('end',json.dumps(report,ensure_ascii=False,indent=2));self.device_text.configure(state='disabled')
     if okay and self.job['operation'] in ('inspect','provision','verify','lock','pairing'):self.snapshot=report
     if okay and self.job['operation']=='diagnostic-start':
      self.diag_active=True;self.diag_port=self.job['port']
      for var in self.evidence.values():var.set('待验证')
      for i,b in enumerate(self.key_labels):b.configure(text=KEY_NAMES[i]+'  待按',text_color='#1267b9')
     if okay and self.job['operation'] in ('diagnostic-stop','test'):self.diag_active=False
     if 'checks' in report:
      self.test_tree.delete(*self.test_tree.get_children())
      for name,value in report['checks'].items():self.test_tree.insert('', 'end',values=(name,STATUS.get(value['status'],value['status']),json.dumps(value,ensure_ascii=False)))
      self.status.set('测试结论：'+STATUS.get(report['overall'],report['overall'])+' · 报告已保存')
     if okay:self.render_report(report,self.job['operation'],self.job['port'])
     if not okay:
      if self.job['operation']=='live':self.diag_active=False
      self.message.showerror('操作失败',report.get('message','请查看日志'))
     self.process=None
     self.progress.set(0);self.update_gates()
   except Exception as error:
    self.append_log('\n日志/结果读取失败：'+str(error)+'\n')
    if self.process.poll() is not None:
     self.process=None;self.progress.stop();self.status.set('读取结果失败，请查看数据目录日志')
     self.update_gates()
  import time
  if self.diag_active and not self.process and time.monotonic()>self.next_live:
   self.next_live=time.monotonic()+2;self.run('live')
  self.root.after(150,self.poll)

 def close(self):
  if self.process:self.message.showinfo('操作进行中','请等待当前操作结束，避免中断烧录或身份写入。');return
  if self.diag_active:
   choice=self.message.askyesnocancel('测试尚未停止','是：停止设备测试。\n否：设备已断电，直接退出。\n取消：返回工具。')
   if choice:
    self.port.set(self.diag_port)
    self.run('diagnostic-stop');return
   if choice is None:return
  self.root.destroy()

from factory_gui_modern import make_app, KEY_NAMES
App=make_app(App,FIELD_LABELS)

def self_test(output):
 import tkinter as tk, serial, esptool
 import customtkinter as ct
 ct.set_appearance_mode('light');root=ct.CTk();root.withdraw();app=App(root,Path(output).parent/'test_data',scan=False);root.update_idletasks()
 assert len(app.tabs)==4 and app.fields['sn'].get()=='' and not app.port.get()
 assert app.fields['model'].get()=='CS-L10-6K2'
 assert app.baud.get()=='2000000'
 assert not app.bin_file.get() and app.bin_type.get()=='应用固件 app.bin'
 calls=[];app.message.showerror=lambda *args:calls.append(args);app.run('inspect')
 assert calls and app.process is None
 app.port.set('TEST');app.render_report({'firmware':'0.6.1','factory_write_enabled':False},'inspect','TEST')
 assert not backend.capabilities(app.snapshot)['diagnostics']
 assert all(b.cget('state')=='disabled' for b,g in app.gates.items() if g=='diagnostics')
 app.render_report({'firmware':'0.6.2','diagnostics_enabled':True,'factory_write_enabled':False,'runtime_sn':'H1ZZX0013900001P'},'inspect','TEST')
 assert backend.capabilities(app.snapshot)['diagnostics'] and not backend.capabilities(app.snapshot)['identity_write']
 assert app.stats['sn'].get()=='H1ZZX0013900001P'
 assert all(b.cget('state')=='normal' for b,g in app.gates.items() if g=='diagnostics')
 app.render_report({'firmware':'0.6.7-onboarding-dev','pairing_identity_v1':True,'pairing_ready':False,'factory_write_enabled':True,'identity':{'sn':'H1ZZX00139000000'},'runtime_sn':'H1ZZX00139000000'},'inspect','TEST')
 assert backend.capabilities(app.snapshot)['pairing_write']
 assert all(b.cget('state')=='normal' for b,g in app.gates.items() if g=='pairing_write')
 app.render_report({'firmware':'0.6.3','factory_write_enabled':True,'identity':{'sn':'H1ZZX00139000000'}},'inspect','TEST')
 assert not backend.capabilities(app.snapshot)['pairing_write']
 assert all(b.cget('state')=='disabled' for b,g in app.gates.items() if g=='pairing_write')
 import factory_pairing,os
 previous=os.environ.pop('PRODUCT_SECRET',None)
 try:
  try:factory_pairing.compute_pin('H1ZZX00139000000')
  except ValueError:pass
  else:raise AssertionError('Missing runtime secret must reject PIN generation')
 finally:
  if previous is not None:os.environ['PRODUCT_SECRET']=previous
 root.destroy()
 stubfiles=list(Path(esptool.__file__).parent.rglob('*esp32c3*.json'))
 assert stubfiles,'missing ESP32-C3 flasher stub resources'
 backend.factory.save(output,{'result':'OK','gui_version':backend.VERSION,'default_baud':'2000000','gui_tabs':4,'serial':serial.VERSION,
  'esptool':esptool.__version__,'esp32c3_stubs':len(stubfiles),'sn_default':'empty',
  'pairing_capability_checked':True,'secret_config_is_runtime_only':True,'hardware_accessed':False,'missing_port_rejected':True,'frozen':bool(getattr(sys,'frozen',False))})

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--worker');parser.add_argument('--self-test');parser.add_argument('--data-dir')
 args=parser.parse_args()
 if args.worker:return backend.worker_main(args.worker)
 if args.self_test:
  try:self_test(args.self_test);return 0
  except BaseException as error:
   import traceback
   backend.factory.save(args.self_test,{'result':'ERROR','message':str(error),'traceback':traceback.format_exc()});return 1
 import tkinter as tk
 import customtkinter as ct
 ct.set_appearance_mode('light');root=ct.CTk();App(root,args.data_dir or application_dir()/'工位数据');root.mainloop();return 0

if __name__=='__main__':sys.exit(main())
