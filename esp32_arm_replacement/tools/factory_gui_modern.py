"""Capability-driven workspace; reuses the isolated serial controller."""
import json
import customtkinter as ct
import factory_gui_backend as backend

KEY_NAMES=('返回','确认','上','左','下','右')

def make_app(base,field_labels):
 class ModernApp(base):
  def __init__(self,root,data_dir=None,scan=True):
   import tkinter as tk
   from tkinter import ttk,filedialog,messagebox
   from pathlib import Path
   self.tk=tk;self.ttk=ttk;self.dialog=filedialog;self.message=messagebox;self.root=root
   self.process=None;self.job=None;self.snapshot=None;self.last_report=None;self.log_offset=0
   self.actions=[];self.gates={};self.peer_port='';self.diag_active=False;self.diag_port='';self.next_live=0
   root.title('CSERGY · 设备服务工作台');root.geometry('1220x870');root.minsize(1120,800)
   root.configure(fg_color='#eef4fc');root.protocol('WM_DELETE_WINDOW',self.close)
   self.data=tk.StringVar(value=str(data_dir or Path(__file__).parent/'工位数据'))
   side=ct.CTkFrame(root,width=200,corner_radius=0,fg_color='#082a4e');side.pack(side='left',fill='y');side.pack_propagate(False)
   ct.CTkLabel(side,text='CSERGY',font=('Arial',29,'bold'),text_color='white').pack(anchor='w',padx=22,pady=(32,0))
   ct.CTkLabel(side,text='辰烁科技 / 设备服务',text_color='#9bcfff').pack(anchor='w',padx=22,pady=(6,32))
   body=ct.CTkFrame(root,fg_color='transparent');body.pack(side='left',fill='both',expand=True,padx=24,pady=22)
   self.title=ct.CTkLabel(body,text='设备概览',font=('Microsoft YaHei UI',26,'bold'),text_color='#123d6b');self.title.pack(anchor='w')
   ct.CTkLabel(body,text='CS-L10-6K2 · 查询、维护与出厂检测',text_color='#32618d').pack(anchor='w',pady=(4,18))
   bar=self.card(body);bar.pack(fill='x',pady=(0,16));self.port=tk.StringVar()
   self.ports=ct.CTkComboBox(bar,variable=self.port,values=[],state='readonly',width=140,command=lambda _:self.update_gates());self.ports.pack(side='left',padx=16,pady=16)
   self.button(bar,'刷新串口',self.refresh_ports).pack(side='left',padx=(0,10))
   self.button(bar,'识别设备',lambda:self.run('inspect'),True).pack(side='left')
   self.port_hint=tk.StringVar(value='先选择设备 USB 串口');ct.CTkLabel(bar,textvariable=self.port_hint,text_color='#32618d',wraplength=280).pack(side='left',padx=14)
   content=ct.CTkFrame(body,fg_color='transparent');content.pack(fill='both',expand=True)
   self.tabs={};self.nav={}
   for key,label in [('device','设备概览'),('flash','固件维护'),('identity','身份工单'),('test','检测工位')]:
    self.tabs[key]=ct.CTkScrollableFrame(content,fg_color='transparent')
    b=ct.CTkButton(side,text=label,height=45,anchor='w',font=('Microsoft YaHei UI',15),fg_color='transparent',hover_color='#164577',command=lambda k=key,l=label:self.show_page(k,l));b.pack(fill='x',padx=14,pady=4);self.nav[key]=b
   ct.CTkLabel(side,text='工作台 v'+backend.VERSION,text_color='#9bcfff').pack(side='bottom',pady=18)
   self.button(side,'打开报告与备份',self.open_data).pack(side='bottom',padx=14,pady=6)
   self.button(side,'更改数据目录',lambda:self.choose_directory(self.data)).pack(side='bottom',padx=14,pady=6)
   self.build_device();self.build_flash();self.build_identity();self.build_test()
   self.status=tk.StringVar(value='就绪 · 识别设备后显示可用功能');ct.CTkLabel(body,textvariable=self.status,text_color='#124777',wraplength=880).pack(anchor='w',pady=8)
   self.progress=ct.CTkProgressBar(body,mode='indeterminate',height=4);self.progress.pack(fill='x');self.progress.set(0)
   self.log_panel=ct.CTkFrame(body,fg_color='#0b2949');self.log=ct.CTkTextbox(self.log_panel,height=125,font=('Consolas',11),fg_color='#0b2949',text_color='#cbe6ff',state='disabled');self.log.pack(fill='x')
   ct.CTkButton(body,text='查看 / 收起技术日志',fg_color='transparent',text_color='#1267b9',height=26,command=self.toggle_log).pack(anchor='w',pady=4)
   self.device_text=ct.CTkTextbox(self.tabs['device'],state='disabled')
   self.show_page('device','设备概览');self.update_gates()
   if scan:self.refresh_ports()
   root.after(150,self.poll)
  def card(self,parent):return ct.CTkFrame(parent,fg_color='white',corner_radius=16,border_width=1,border_color='#dce9f8')
  def button(self,parent,text,command,accent=False,gate=None):
   b=ct.CTkButton(parent,text=text,command=command,height=36,font=('Microsoft YaHei UI',13),corner_radius=9,fg_color='#126ee5' if accent else '#e4f0ff',text_color='white' if accent else '#164577',hover_color='#0a59c1' if accent else '#c9e2ff');self.actions.append(b)
   if gate:self.gates[b]=gate
   return b
  def note(self,parent,text):ct.CTkLabel(parent,text=text,wraplength=850,justify='left',anchor='w',text_color='#32618d',font=('Microsoft YaHei UI',13)).pack(fill='x',padx=10,pady=(8,12))
  def show_page(self,key,label):
   for k,p in self.tabs.items():p.pack_forget();self.nav[k].configure(fg_color='#126ee5' if k==key else 'transparent')
   self.tabs[key].pack(fill='both',expand=True);self.title.configure(text=label)
  def toggle_log(self):
   if self.log_panel.winfo_manager():self.log_panel.pack_forget()
   else:self.log_panel.pack(fill='x',pady=4)
  def build_device(self):
   f=self.tabs['device'];grid=ct.CTkFrame(f,fg_color='transparent');grid.pack(fill='x');grid.columnconfigure((0,1),weight=1);self.stats={}
   for i,(key,label) in enumerate([('sn','设备序列号'),('firmware','运行固件'),('diagnostics','诊断能力'),('identity','身份权限')]):
    c=self.card(grid);c.grid(row=i//2,column=i%2,sticky='ew',padx=5,pady=6)
    ct.CTkLabel(c,text=label,text_color='#32618d').pack(anchor='w',padx=20,pady=(17,6))
    v=self.tk.StringVar(value='等待识别');self.stats[key]=v;ct.CTkLabel(c,textvariable=v,text_color='#123d6b',font=('Microsoft YaHei UI',19,'bold'),wraplength=380,justify='left').pack(anchor='w',padx=20,pady=(0,20))
   self.cap_hint=self.tk.StringVar(value='连接设备 → 识别设备 → 选择可用功能。更换串口后需要重新识别。')
   ct.CTkLabel(f,textvariable=self.cap_hint,wraplength=860,justify='left',text_color='#1267b9',font=('Microsoft YaHei UI',14)).pack(fill='x',pady=18)
   row=ct.CTkFrame(f,fg_color='transparent');row.pack(fill='x')
   for label,op in [('刷新实时测量','live'),('读取 BMS 身份','bms-identity')]:self.button(row,label,lambda o=op:self.run(o),op=='live').pack(side='left',padx=5)
   self.button(row,'导出结果',self.export_report).pack(side='left',padx=5)
   self.measure=self.tk.StringVar(value='尚未读取测量数据');ct.CTkLabel(f,textvariable=self.measure,text_color='#123d6b',justify='left',font=('Microsoft YaHei UI',15),wraplength=860).pack(anchor='w',padx=5,pady=22)
  def build_flash(self):
   f=self.tabs['flash'];self.note(f,'01  选择配置     →     02  整片备份     →     03  分段写入并校验')
   c=self.card(f);c.pack(fill='x');self.note(c,'当前单机使用 Device。新版诊断无需切换到 Factory。')
   self.profile=self.tk.StringVar(value='device');ct.CTkComboBox(c,variable=self.profile,values=['device','factory','production'],state='readonly',command=lambda _:self.select_bundled()).pack(anchor='w',padx=18,pady=8)
   self.release=self.tk.StringVar();ct.CTkEntry(c,textvariable=self.release,height=36).pack(fill='x',padx=18,pady=8)
   self.button(c,'选择其他固件目录',self.choose_release).pack(anchor='w',padx=18,pady=8)
   self.firmware_info=self.tk.StringVar();ct.CTkLabel(c,textvariable=self.firmware_info,text_color='#1267b9',wraplength=820).pack(anchor='w',padx=18,pady=12)
   speed=ct.CTkFrame(c,fg_color='transparent');speed.pack(fill='x',padx=18,pady=(0,16))
   ct.CTkLabel(speed,text='传输波特率',text_color='#32618d').pack(side='left',padx=(0,12))
   self.baud=self.tk.StringVar(value=str(backend.flasher.DEFAULT_BAUD))
   ct.CTkComboBox(speed,variable=self.baud,values=[str(v) for v in reversed(backend.flasher.BAUD_RATES)],state='readonly',width=140).pack(side='left')
   ct.CTkLabel(speed,text='默认 2M · 备份、写入与校验均使用所选速率',text_color='#1267b9').pack(side='left',padx=14)
   self.first_install=self.tk.BooleanVar(value=False);ct.CTkCheckBox(f,text='首次迁移旧分区（仅用于分区不匹配的旧设备）',variable=self.first_install,text_color='#164577').pack(anchor='w',pady=20)
   self.note(f,'常规更新保留身份区与用户设置。Device 默认 SN 为 H1ZZX0013900001P，管理员可修改；Factory 用于逐台写身份，Production 用于读取量产身份。')
   self.button(f,'备份并更新固件',self.flash,True).pack(anchor='w',pady=12);self.select_bundled()
   bin_card=self.card(f);bin_card.pack(fill='x',pady=16)
   self.note(bin_card,'或者选择单个 .bin 文件 · 支持应用固件和完整 4MB 合并固件')
   self.bin_file=self.tk.StringVar();ct.CTkEntry(bin_card,textvariable=self.bin_file,height=36).pack(fill='x',padx=18,pady=6)
   row=ct.CTkFrame(bin_card,fg_color='transparent');row.pack(fill='x',padx=18,pady=8)
   self.button(row,'选择 BIN 文件',self.choose_bin).pack(side='left',padx=(0,12))
   self.bin_type=self.tk.StringVar(value='应用固件 app.bin')
   ct.CTkComboBox(row,variable=self.bin_type,values=['应用固件 app.bin','完整 4MB 合并固件'],state='readonly',width=220,command=lambda _:self.describe_bin()).pack(side='left')
   self.bin_info=self.tk.StringVar(value='应用：0x20000，保留设置和身份；完整4MB：0x0，覆盖整片。')
   ct.CTkLabel(bin_card,textvariable=self.bin_info,wraplength=820,justify='left',text_color='#1267b9').pack(anchor='w',padx=18,pady=8)
   self.button(bin_card,'备份并烧录所选 BIN',lambda:self.run('flash-bin'),True).pack(anchor='w',padx=18,pady=(6,18))
  def choose_bin(self):
   from pathlib import Path
   path=self.dialog.askopenfilename(filetypes=[('固件 BIN','*.bin')])
   if path:
    self.bin_file.set(path);self.bin_type.set('完整 4MB 合并固件' if Path(path).stat().st_size==0x400000 else '应用固件 app.bin');self.describe_bin()
  def describe_bin(self):
   import flash_single_bin
   try:
    info=flash_single_bin.describe(self.bin_file.get(),'merged' if self.bin_type.get()=='完整 4MB 合并固件' else 'app')
    self.bin_info.set(f"ESP32-C3 · 地址 {hex(info['offset'])} · {info['bytes']} 字节\n{info['scope']}")
   except Exception as error:self.bin_info.set('文件不可用：'+str(error))
  def build_identity(self):
   f=self.tabs['identity'];self.note(f,'逐台身份工单 · 查询与写入是不同权限。未确定的硬件版本不能锁定出厂身份。')
   self.fields={};form=self.card(f);form.pack(fill='x');form.columnconfigure((0,1),weight=1)
   for i,(name,_) in enumerate(backend.factory.FIELDS):
    cell=ct.CTkFrame(form,fg_color='transparent');cell.grid(row=i//2,column=i%2,sticky='ew',padx=16,pady=8)
    ct.CTkLabel(cell,text=field_labels[name],text_color='#32618d',anchor='w').pack(fill='x')
    v=self.tk.StringVar(value=backend.default_profile()[name]);self.fields[name]=v;ct.CTkEntry(cell,textvariable=v,height=34).pack(fill='x')
   row=ct.CTkFrame(f,fg_color='transparent');row.pack(fill='x',pady=16)
   for text,action,gate in [('导入工单',self.import_profile,None),('保存工单',self.save_profile,None),('填入已读身份',self.load_snapshot,None),('写入并读回',lambda:self.run('provision'),'identity_write'),('重启后复核',lambda:self.run('verify'),'identity_verify'),('写入扫码PIN',lambda:self.run('pairing'),'pairing_write'),('锁定身份',lambda:self.run('lock'),'identity_lock')]:self.button(row,text,action,text=='写入并读回',gate).pack(side='left',padx=3)
   self.note(f,'本工位保存 SN / MAC 登记，拒绝重复绑定。写入后请断电重启并复核；只读配置的写入按钮自动禁用。')
  def build_test(self):
   f=self.tabs['test'];self.note(f,'01  启动诊断     →     02  按键、观察屏幕与蜂鸣器     →     03  保存报告')
   row=ct.CTkFrame(f,fg_color='transparent');row.pack(fill='x',pady=8)
   self.button(row,'启动屏幕 / 按键 / 蜂鸣器',lambda:self.run('diagnostic-start'),True,'diagnostics').pack(side='left',padx=5)
   self.button(row,'结束诊断',lambda:self.run('diagnostic-stop'),False,'diagnostic_stop').pack(side='left',padx=5)
   self.key_labels=[];keys=self.card(f);keys.pack(fill='x',pady=12)
   for i in range(6):
    b=ct.CTkLabel(keys,text=KEY_NAMES[i]+'  待按',text_color='#1267b9',font=('Microsoft YaHei UI',13,'bold'));b.pack(side='left',expand=True,pady=20);self.key_labels.append(b)
   self.evidence={};row=ct.CTkFrame(f,fg_color='transparent');row.pack(fill='x',pady=8)
   for name,label in [('display','屏幕观察'),('buzzer','蜂鸣器观察')]:
    ct.CTkLabel(row,text=label,text_color='#164577').pack(side='left',padx=8);v=self.tk.StringVar(value='待验证');self.evidence[name]=v;ct.CTkComboBox(row,variable=v,values=['待验证','通过','失败'],state='readonly',width=110).pack(side='left',padx=8)
   self.limits=self.tk.StringVar();self.references=self.tk.StringVar()
   for label,var in [('误差标准 JSON',self.limits),('仪器参考值 JSON',self.references)]:
    row=ct.CTkFrame(f,fg_color='transparent');row.pack(fill='x',pady=5);ct.CTkLabel(row,text=label,width=120,text_color='#32618d').pack(side='left');ct.CTkEntry(row,textvariable=var).pack(side='left',fill='x',expand=True,padx=8);self.button(row,'选择',lambda v=var:self.choose_json(v)).pack(side='left')
   self.note(f,'未提供误差阈值、参考值或带载证据的项目保持“待验证”。不自动写入未知 DSP / BMS 校准系数。')
   self.button(f,'保存报告并结束诊断',lambda:self.run('test'),True,'test').pack(anchor='w',pady=8)
   style=self.ttk.Style();style.theme_use('clam');style.configure('Treeview',font=('Microsoft YaHei UI',11),rowheight=34);style.configure('Treeview.Heading',font=('Microsoft YaHei UI',11,'bold'),background='#dcecff')
   self.test_tree=self.ttk.Treeview(f,columns=('item','status','detail'),show='headings',height=5)
   for name,label,width in [('item','项目',170),('status','结论',80),('detail','测量 / 说明',550)]:self.test_tree.heading(name,text=label);self.test_tree.column(name,width=width)
   self.test_tree.pack(fill='x',pady=8)
  def refresh_ports(self):
   from serial.tools import list_ports
   ports=list(list_ports.comports());values=[p.device for p in ports];self.ports.configure(values=values)
   if self.port.get() not in values:self.port.set('')
   self.port_hint.set('；'.join(f'{p.device} · {p.description}' for p in ports) or '未发现串口，请连接后刷新');self.update_gates()
  def update_gates(self):
   known=self.snapshot and self.peer_port==self.port.get();caps=backend.capabilities(self.snapshot) if known else {}
   caps.update(diagnostic_stop=bool(known and self.diag_active),test=bool(known and caps.get('diagnostics') and self.diag_active))
   for b in self.actions:b.configure(state='disabled' if self.process or (b in self.gates and not caps.get(self.gates[b])) else 'normal')
  def render_report(self,report,operation,port):
   if 'firmware' in report:
    self.snapshot=report;self.peer_port=port;caps=backend.capabilities(report)
    self.stats['sn'].set(report.get('runtime_sn') or (report.get('identity') or {}).get('sn') or ('尚未写入生产身份' if 'runtime_sn' in report else '旧固件未提供运行 SN'))
    self.stats['firmware'].set(report['firmware']+' / '+report.get('firmware_profile',''))
    self.stats['diagnostics'].set('可用 · 屏幕 / 按键 / 蜂鸣器' if caps['diagnostics'] else '需更新同配置固件')
    self.stats['identity'].set('允许写入' if caps['identity_write'] else '受保护 · 只读')
    self.cap_hint.set('设备已识别，进入检测工位开始测试。身份权限与诊断权限独立。' if caps['diagnostics'] else '旧固件未开放诊断：更新同配置 v0.6.2 后重新识别。无需为测试改用 Factory。')
   if 'keys' in report:
    for i,b in enumerate(self.key_labels):b.configure(text=KEY_NAMES[i]+'  '+('已按' if int(report['keys'])&(1<<i) else '待按'),text_color='#00875a' if int(report['keys'])&(1<<i) else '#1267b9')
   if 'diagnostic_active' in report:self.diag_active=bool(report['diagnostic_active'])
   if 'dsp_online' in report:
    lines=['DSP：'+('在线' if report['dsp_online'] else '未收到数据')+'    BMS：'+('在线' if report.get('bms_online') else '未收到数据')]
    labels={'battery_v':'电池电压 V','output_v':'输出电压 V','grid_v':'电网电压 V','output_w':'输出功率 W','inverter_c':'逆变温度 ℃'}
    lines += [labels[k]+'：'+str(v) for k,v in report.get('measurements',{}).items() if k in labels]
    labels={'voltage_v':'BMS电压 V','current_a':'BMS电流 A','soc':'电量 %'}
    lines += [labels[k]+'：'+str(v) for k,v in report.get('bms',{}).items() if k in labels]
    self.measure.set('\n'.join(lines))
   if operation=='bms-identity':
    labels={'software':'BMS软件版本','hardware':'BMS硬件版本','compile_time':'编译时间','module_sn':'模组SN','pcb_sn':'PCB SN','product':'产品','cpuid':'芯片标识'}
    lines=[label+'：'+str(report[k]) for k,label in labels.items() if k in report]
    state=report.get('capability','')
    if state!='IDENTITY_READ_ONLY':lines.append('查询未完整：'+{'PARTIAL':'仅返回部分字段','UNSUPPORTED_OR_NO_REPLY':'无回复或协议不支持','QUERYING':'查询中','NOT_QUERIED':'尚未查询完成'}.get(state,state))
    self.measure.set('\n'.join(lines) or 'BMS未返回身份字段')
   if report.get('next_step'):self.cap_hint.set(report['next_step'])
   if operation in ('flash','flash-bin'):self.snapshot=None;self.peer_port='';self.cap_hint.set('固件已校验。请断电重启，再识别设备。')
   self.update_gates()
 return ModernApp
