# CS-L10-6K2 工厂工具（开发版）

## 镜像配置

- Device：当前SN H1ZZX0013900001P固定，USB只读。不用于量产。
- Factory：factory身份逐台写入，物理USB允许写入/测试；未写身份时只启动显示、DSP/BMS采集与USB，不启动网络。
- Production：factory身份逐台读取，物理USB只读。未配置身份不联网。仍是待实机验收的软件版本，不等同正式量产放行。

构建：`./build.ps1 -Profile Device|Factory|Production`。保留原sdkconfig，生成对应sdkconfig.*；当前build目录最后一个配置对应最后构建目标。

## 首烧 / 有线维护

Python需要pyserial、esptool。工具总是显式指定串口，示例COM11只是占位。设备不在本机，不会自动烧录。

```
python tools/flash_production.py --port COM11 --release release/.../factory --first-install
```

首烧迁移修改cache边界；先备份整片4MB，再清除旧SPIFFS区域，写bootloader/分区/otadata/app。Wi-Fi和用户NVS保存区不擦；旧cache数据不会自动迁移。已是新分区的设备可省略first-install，factory区域前后读回必须相同。不同分区且未显式首烧时停止；修复不默认擦身份。

直接全量4MB在0x000000写入会覆盖NVS及factory身份。全量镜像只适合明确首烧/重新生产，不能当保留身份的维修流程。app-only OTA不会更新分区表；旧版本必须先有线迁移。

## 写入与复核

复制factory_profile.example.json后逐台填写SN。产品型号CS-L10-6K2。未知硬件版本填UNSET，不能出厂锁定。生产时间UTC，格式YYYY-MM-DDTHH:MM:SSZ；MAC从设备实际读取，不手填。

全局参数放在子命令前：

```
python tools/factory_tool.py --port COM11 --output inspect.json inspect
python tools/factory_tool.py --port COM11 --registry 工位sn.sqlite3 --output write.json provision 工单.json
```

写后断电重启，再执行：

```
python tools/factory_tool.py --port COM11 --output verify.json verify 工单.json
python tools/factory_tool.py --port COM11 --output test.json test --interactive
python tools/factory_tool.py --port COM11 --output test.json test --interactive --limits limits.json --references references.json
```

limits是通道字典，如 `{"battery_v":{"absolute":0.2,"relative_percent":0}}`；references如 `{"battery_v":48.0}`。这里数值仅举工具格式，不代表本型号精度要求；没有标准时不要套用例子。检测值来自设备实时快照；测量精度需要外部仪器真实参考值。

显示图案、蜂鸣器由操作员确认，六键必须分别按下（keys=3F）；通信离线FAIL，参考/阈值/真实校准/带载测试缺失PENDING，PENDING不能当PASS。当前工具不写DSP/BMS采样校准系数；报告明确记录缺项。

工位SQLite注册表拒绝同SN不同MAC和同MAC不同SN。多工位必须共用登记服务或各自预分配号码，本地工具不能保证跨离线工位全局唯一。写入返回成功不代表断电验证；verify记录是读回证据，操作员必须按工艺完成断电重启。

完成真实硬件版本及校准/测试报告引用后才允许 `lock`；factory固件和工具都检查前置条件。锁定不代表工具已经检验报告引用的真实性；生产系统须审核报告。factory锁持久保存，普通用户恢复设置不清除。更换为Production程序时使用保留factory的分段烧录流程。

## 升级

v0.6.1在镜像内部增加产品/配置描述。OTA只接受同类Device/Factory/Production镜像；无描述的旧版或其他配置会被拒绝，不能靠修改manifest把固定SN镜像装进量产设备。跨配置更换及回退旧版使用保留factory的有线流程。此检查防止配置误用，不替代发行签名。

新镜像确认要求主循环、显示首帧与任务、DSP/BMS UART任务、USB服务及身份存储的软件健康。外部BMS不回复不会仅因此回滚。检查失败向SDK请求回滚；不存在可回滚镜像时保持失败及USB恢复，不把镜像标为有效。软件健康不代表实际显示或电力验收。

## 可选 BMS 身份读取

```
python tools/factory_tool.py --port COM11 --output bms_identity.json bms-identity
```

显式触发8项查询，约十余秒；OTA静默窗口会暂停查询，超时报告PENDING。正常开机不查询。返回真实软件、硬件、SN、产品文本、CRC及valid_mask，不填猜测的版本。无响应/部分响应不视为升级授权，也不等于普通BMS采集故障。协议与实际APP支持限制见docs/TARGET_PROTOCOL_EVIDENCE.md。

BMS双点电压/充放电电流校准封帧与严格ACK已实现并主机验证，运行写入和参数读回尚未启用；ACK存储成功不代表校准精度通过。

ESP网络和蓝牙升级页读取实际服务状态；下载100%仍需校验/启动确认。Production OTA manifest增加model与hardware（与factory的model、hw_controller完全一致），并必须包含sha256；未知硬件/不匹配目标会拒绝，手机/云端需同步这些字段。Device保持原客户端兼容。

签名发行、逐设备MQTT凭据与后台联调尚未完成，不宣称当前方案已实现强制签名或逐台云鉴权。

DSP/BMS直连核心见main/direct/direct_iap.c；BLE/MQTT统一走OTA服务的原始文件暂存及唯一UART owner。兼容入口沿用原ARM Boot事务，不要求原协议没有的额外版本/停机读取。BMS沿用原ARM全零匹配字段；Boot回显不能当作实际硬件身份。成功只代表原Boot事务完成，消息明确版本未读回；待实机升级/恢复验收。严格策略核心仍保留给提供真实验证接口的调用方。
