#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 CS-I10-6k2 系统参数数据字段 Excel 文件 - 完整版"""

from openpyxl import Workbook
from openpyxl.styles import Font, Alignment, Border, Side, PatternFill

wb = Workbook()
header_font = Font(bold=True, size=11, color="FFFFFF")
header_fill = PatternFill(start_color="4472C4", end_color="4472C4", fill_type="solid")
thin_border = Border(left=Side(style='thin'), right=Side(style='thin'), top=Side(style='thin'), bottom=Side(style='thin'))
wrap = Alignment(wrap_text=True, vertical='top')

def write_header(ws, row, headers):
    for col, h in enumerate(headers, 1):
        c = ws.cell(row=row, column=col, value=h)
        c.font = header_font; c.fill = header_fill; c.border = thin_border
        c.alignment = Alignment(horizontal='center', vertical='center')

def write_row(ws, row, data):
    for col, v in enumerate(data, 1):
        c = ws.cell(row=row, column=col, value=v)
        c.border = thin_border; c.alignment = wrap

ws = wb.active
ws.title = "全部数据字段"
write_header(ws, 1, ["分类", "主题", "参数", "Key", "类型", "单位", "范围/枚举", "说明", "上报频率"])

all_fields = [
    # 设备信息
    ["设备信息", "info", "序列号", "sn", "string", "-", "-", "16位设备序列号", "连接时"],
    ["设备信息", "info", "产品型号", "model", "string", "-", "CS-I10-6k2", "产品型号", "连接时"],
    ["设备信息", "info", "制造商", "manufacturer", "string", "-", "辰烁科技", "制造商", "连接时"],
    ["设备信息", "info", "ARM固件", "firmware_arm", "string", "-", "-", "逆变器主控固件版本", "连接时"],
    ["设备信息", "info", "ESP固件", "firmware_esp", "string", "-", "-", "WiFi模块固件版本", "连接时"],
    ["设备信息", "info", "DSP固件", "firmware_dsp", "string", "-", "-", "数字信号处理器固件版本", "连接时"],
    ["设备信息", "info", "BMS固件", "firmware_bms", "string", "-", "-", "电池管理系统固件版本", "连接时"],
    ["设备信息", "info", "逆变器类型", "type", "string", "-", "离网逆变器", "逆变器类型", "连接时"],
    ["设备信息", "info", "额定功率", "rated_power", "int", "W", "6200", "额定功率", "连接时"],
    ["设备信息", "info", "额定电压", "rated_voltage", "int", "V", "220", "额定电压", "连接时"],
    ["设备信息", "info", "额定频率", "rated_freq", "float", "Hz", "50.0", "额定频率", "连接时"],
    ["设备信息", "info", "电池标称电压", "battery_voltage", "float", "V", "51.2", "48V LiFePO4", "连接时"],
    ["设备信息", "info", "电池类型", "battery_type", "string", "-", "LiFePO4/NCM/LeadAcid", "电池类型", "连接时"],
    ["设备信息", "info", "电芯串联数", "cell_count", "int", "个", "16", "16S串联", "连接时"],
    ["设备信息", "info", "并机模式", "parallel_mode", "string", "-", "standalone/single_phase/three_phase", "并机模式", "连接时"],
    ["设备信息", "info", "并机角色", "parallel_role", "string", "-", "standalone/master/slave", "并机角色", "连接时"],
    ["设备信息", "info", "三相相位", "parallel_phase", "string/null", "-", "L1/L2/L3/null", "仅三相模式", "连接时"],
    # 交流输出
    ["交流输出", "data/ac", "输出电压", "voltage", "float", "V", "200~250", "单相AC输出", "5s"],
    ["交流输出", "data/ac", "输出电流", "current", "float", "A", "0~28.2", "实时输出电流", "5s"],
    ["交流输出", "data/ac", "有功功率", "power", "float", "W", "0~6200", "实际有功输出", "5s"],
    ["交流输出", "data/ac", "视在功率", "apparent", "float", "VA", "0~6200", "V×I", "5s"],
    ["交流输出", "data/ac", "输出频率", "frequency", "float", "Hz", "49.5~50.5", "离网模式自生成", "5s"],
    ["交流输出", "data/ac", "功率因数", "pf", "float", "-", "0~1", "必须≤1", "5s"],
    ["交流输出", "data/ac", "负载率", "load_percent", "float", "%", "0~100", "当前负载/6200W", "5s"],
    ["交流输出", "data/ac", "电压THD", "thd_v", "float", "%", "0~5", "输出波形质量", "5s"],
    # 电池BMS
    ["电池BMS", "data/battery", "SOC", "soc", "float", "%", "0~100", "剩余电量", "5s"],
    ["电池BMS", "data/battery", "SOH", "soh", "float", "%", "0~100", "电池健康状态", "5s"],
    ["电池BMS", "data/battery", "总电压", "voltage", "float", "V", "40~60", "电池组端电压", "5s"],
    ["电池BMS", "data/battery", "电流", "current", "float", "A", "-120~120", "正=充电，负=放电", "5s"],
    ["电池BMS", "data/battery", "功率", "power", "float", "W", "-", "正=充电，负=放电", "5s"],
    ["电池BMS", "data/battery", "剩余容量", "capacity_remain", "float", "Ah", "-", "当前可用容量", "5s"],
    ["电池BMS", "data/battery", "额定容量", "capacity_total", "float", "Ah", "100", "标称容量", "5s"],
    ["电池BMS", "data/battery", "循环次数", "cycle_count", "int", "次", "-", "累计充放电循环", "5s"],
    ["电池BMS", "data/battery", "最高温度", "temp_max", "float", "℃", "-20~85", "电芯最高温度", "5s"],
    ["电池BMS", "data/battery", "最低温度", "temp_min", "float", "℃", "-20~85", "电芯最低温度", "5s"],
    ["电池BMS", "data/battery", "单体最高电压", "cell_volt_max", "float", "V", "-", "-", "5s"],
    ["电池BMS", "data/battery", "单体最低电压", "cell_volt_min", "float", "V", "-", "-", "5s"],
    ["电池BMS", "data/battery", "电芯压差", "cell_volt_diff", "float", "V", "-", "最大值-最小值", "5s"],
    ["电池BMS", "data/battery", "充放电状态", "charge_state", "int", "-", "0/1/2", "0=静置/1=充电/2=放电", "5s"],
    ["电池BMS", "data/battery", "保护状态", "protect_status", "int", "-", "位掩码", "见保护状态位定义", "5s"],
    ["电池BMS", "data/battery", "BMS故障码", "bms_fault_code", "int", "-", "-", "0=正常，非零=故障", "5s"],
    ["电池BMS", "data/battery", "最大充电电流", "max_chg_current", "float", "A", "-", "BMS允许的最大充电电流", "5s"],
    ["电池BMS", "data/battery", "最大放电电流", "max_dischg_current", "float", "A", "-", "BMS允许的最大放电电流", "5s"],
    ["电池BMS", "data/battery", "充电目标电压", "charge_volt_ref", "float", "V", "-", "BMS充电截止目标电压", "5s"],
    ["电池BMS", "data/battery", "放电截止电压", "dischg_cut_volt", "float", "V", "-", "BMS放电保护截止电压", "5s"],
    ["电池BMS", "data/battery", "电池组温度", "temp_battery", "float", "℃", "-20~60", "电池组平均温度", "5s"],
    # 光伏MPPT
    ["光伏MPPT", "data/pv", "PV1电压", "pv1_voltage", "float", "V", "60~150", "第一路光伏电压", "5s"],
    ["光伏MPPT", "data/pv", "PV1电流", "pv1_current", "float", "A", "0~30", "第一路光伏电流", "5s"],
    ["光伏MPPT", "data/pv", "PV1功率", "pv1_power", "float", "W", "0~2500", "第一路光伏功率", "5s"],
    ["光伏MPPT", "data/pv", "PV2电压", "pv2_voltage", "float", "V", "60~150", "第二路光伏电压", "5s"],
    ["光伏MPPT", "data/pv", "PV2电流", "pv2_current", "float", "A", "0~30", "第二路光伏电流", "5s"],
    ["光伏MPPT", "data/pv", "PV2功率", "pv2_power", "float", "W", "0~2500", "第二路光伏功率", "5s"],
    ["光伏MPPT", "data/pv", "PV总功率", "pv_power_total", "float", "W", "0~5000", "两路PV总功率", "5s"],
    ["光伏MPPT", "data/pv", "MPPT状态", "mppt_state", "string", "-", "tracking/standby/fault", "MPPT工作状态", "5s"],
    # 系统状态
    ["系统状态", "data/status", "运行状态", "state", "int", "-", "0/2/3/4", "0=待机/2=离网/3=故障/4=维护", "5s"],
    ["系统状态", "data/status", "故障代码", "fault_code", "int", "-", "-", "0=正常，非零=故障编码", "5s"],
    ["系统状态", "data/status", "告警代码", "alarm_code", "int", "-", "-", "0=无告警，非零=告警编码", "5s"],
    ["系统状态", "data/status", "逆变器温度", "temp_inv", "float", "℃", "-20~85", "逆变器内部温度", "5s"],
    ["系统状态", "data/status", "散热器1温度", "temp_mos", "float", "℃", "-20~85", "功率模块散热器1温度", "5s"],
    ["系统状态", "data/status", "散热器2温度", "temp_radiator2", "float", "℃", "-20~100", "功率模块散热器2温度", "5s"],
    ["系统状态", "data/status", "散热器3温度", "temp_radiator3", "float", "℃", "-20~100", "功率模块散热器3温度", "5s"],
    ["系统状态", "data/status", "直流母线电压", "dc_bus_voltage", "float", "V", "0~450", "直流母线电压", "5s"],
    ["系统状态", "data/status", "直流母线正端电压", "v_bus_p", "float", "V", "0~450", "直流母线正极端电压", "5s"],
    ["系统状态", "data/status", "累计运行时长", "runtime_hours", "int", "h", "-", "累计逆变运行小时", "5s"],
    ["系统状态", "data/status", "风扇转速", "fan_speed", "int", "%", "0~100", "0=停转", "5s"],
    ["系统状态", "data/status", "转换效率", "efficiency", "float", "%", "0~100", "DC→AC实时效率", "5s"],
    # 能量统计
    ["能量统计", "data/energy", "当日发电量", "daily_pv", "float", "kWh", "-", "当日光伏发电量", "60s"],
    ["能量统计", "data/energy", "累计发电量", "total_pv", "float", "kWh", "-", "累计光伏发电量", "60s"],
    ["能量统计", "data/energy", "当日充电量", "daily_charge", "float", "kWh", "-", "当日充入电池电量", "60s"],
    ["能量统计", "data/energy", "累计充电量", "total_charge", "float", "kWh", "-", "累计充电量", "60s"],
    ["能量统计", "data/energy", "当日放电量", "daily_discharge", "float", "kWh", "-", "当日电池放电量", "60s"],
    ["能量统计", "data/energy", "累计放电量", "total_discharge", "float", "kWh", "-", "累计放电量", "60s"],
    ["能量统计", "data/energy", "当日用电量", "daily_load", "float", "kWh", "-", "当日负载消耗", "60s"],
    ["能量统计", "data/energy", "累计用电量", "total_load", "float", "kWh", "-", "累计负载消耗", "60s"],
    # 电芯详情（补充 charge_ah_total / discharge_ah_total）
    ["电芯详情", "data/cells", "电芯数量", "cell_count", "int", "个", "16", "16S串联", "30s"],
    ["电芯详情", "data/cells", "电芯电压数组", "voltages", "float[]", "V", "-", "每节电芯电压(16个值)", "30s"],
    ["电芯详情", "data/cells", "电芯温度数组", "temps", "float[]", "℃", "-", "每节电芯温度(16个值)", "30s"],
    ["电芯详情", "data/cells", "充电累计Ah", "charge_ah_total", "float", "Ah", "-", "总充电安时", "30s"],
    ["电芯详情", "data/cells", "放电累计Ah", "discharge_ah_total", "float", "Ah", "-", "总放电安时", "30s"],
    # 告警事件
    ["告警事件", "data/alarm", "故障码", "code", "int", "-", "-", "故障编码", "事件触发"],
    ["告警事件", "data/alarm", "级别", "level", "string", "-", "warning/fault", "告警级别", "事件触发"],
    ["告警事件", "data/alarm", "描述", "message", "string", "-", "-", "中文故障描述", "事件触发"],
    ["告警事件", "data/alarm", "时间戳", "timestamp", "int", "s", "-", "Unix时间戳", "事件触发"],
    # OTA升级
    ["OTA升级", "ota/status", "目标固件", "target", "string", "-", "esp/arm/dsp/bms", "升级目标", "按需"],
    ["OTA升级", "ota/status", "升级状态", "state", "string", "-", "idle/downloading/flashing/verifying/success/failed", "当前状态", "按需"],
    ["OTA升级", "ota/status", "进度", "progress", "int", "%", "0~100", "升级进度", "按需"],
    ["OTA升级", "ota/status", "消息", "message", "string", "-", "-", "状态描述", "按需"],
    ["OTA升级", "ota/status", "任务ID", "task_id", "string", "-", "-", "OTA任务标识", "按需"],
    ["OTA升级", "ota/status", "下载速度", "speed_kbps", "int", "KB/s", "-", "固件下载速度", "按需"],
    # 并机状态
    ["并机状态", "data/parallel", "并机模式", "mode", "string", "-", "single_phase/three_phase", "并机模式", "5s"],
    ["并机状态", "data/parallel", "并机数量", "count", "int", "台", "1~8", "当前并机台数", "5s"],
    ["并机状态", "data/parallel", "总额定功率", "total_rated_power", "int", "W", "-", "所有并机额定功率之和", "5s"],
    ["并机状态", "data/parallel", "总有功功率", "total_active_power", "float", "W", "-", "所有并机实际输出之和", "5s"],
    ["并机状态", "data/parallel", "同步状态", "sync_state", "string", "-", "synced/syncing/fault", "同步状态", "5s"],
    ["并机状态", "data/parallel", "机器列表", "machines", "array", "-", "-", "各机器状态", "5s"],
    # 三相系统
    ["三相系统", "data/three_phase", "L1电压", "voltage_l1", "float", "V", "200~250", "L1相电压", "5s"],
    ["三相系统", "data/three_phase", "L2电压", "voltage_l2", "float", "V", "200~250", "L2相电压", "5s"],
    ["三相系统", "data/three_phase", "L3电压", "voltage_l3", "float", "V", "200~250", "L3相电压", "5s"],
    ["三相系统", "data/three_phase", "L1电流", "current_l1", "float", "A", "0~28.2", "L1相电流", "5s"],
    ["三相系统", "data/three_phase", "L2电流", "current_l2", "float", "A", "0~28.2", "L2相电流", "5s"],
    ["三相系统", "data/three_phase", "L3电流", "current_l3", "float", "A", "0~28.2", "L3相电流", "5s"],
    ["三相系统", "data/three_phase", "L1功率", "power_l1", "float", "W", "0~6200", "L1相有功功率", "5s"],
    ["三相系统", "data/three_phase", "L2功率", "power_l2", "float", "W", "0~6200", "L2相有功功率", "5s"],
    ["三相系统", "data/three_phase", "L3功率", "power_l3", "float", "W", "0~6200", "L3相有功功率", "5s"],
    ["三相系统", "data/three_phase", "总功率", "power_total", "float", "W", "0~18600", "三相总功率", "5s"],
    ["三相系统", "data/three_phase", "线电压", "voltage_ll", "float", "V", "-", "线电压(≈√3×相电压)", "5s"],
    ["三相系统", "data/three_phase", "频率", "frequency", "float", "Hz", "49.5~50.5", "系统频率(主机同步)", "5s"],
    ["三相系统", "data/three_phase", "电压不平衡度", "unbalance_v", "float", "%", "-", "三相电压不平衡度", "5s"],
    ["三相系统", "data/three_phase", "电流不平衡度", "unbalance_i", "float", "%", "-", "三相电流不平衡度", "5s"],
    # ESP32状态
    ["ESP32状态", "status", "在线状态", "online", "bool", "-", "true/false", "设备在线状态", "60s"],
    ["ESP32状态", "status", "WiFi信号", "rssi", "int", "dBm", "-", "WiFi信号强度", "60s"],
    ["ESP32状态", "status", "IP地址", "ip", "string", "-", "-", "设备本地IP地址", "60s"],
]

for i, row in enumerate(all_fields, 2):
    write_row(ws, i, row)
for i, w in enumerate([12, 20, 16, 20, 12, 8, 25, 30, 12], 1):
    ws.column_dimensions[ws.cell(row=1, column=i).column_letter].width = w
ws.freeze_panes = "A2"

# 控制命令
ws2 = wb.create_sheet("控制命令")
write_header(ws2, 1, ["分类", "命令", "Payload", "说明"])
cmds = [
    ["逆变器", "ac_on", '""', "交流输出开启"],
    ["逆变器", "ac_off", '""', "交流输出关闭"],
    ["逆变器", "set_power_limit", '{"value":6200}', "有功功率上限(W)"],
    ["逆变器", "set_charge_limit", '{"value":6200}', "最大充电功率(W)"],
    ["逆变器", "set_discharge_limit", '{"value":6200}', "最大放电功率(W)"],
    ["逆变器", "set_soc_low", '{"value":200}', "放电截止SOC(0.1%=20%)"],
    ["逆变器", "set_soc_high", '{"value":950}', "充电截止SOC(0.1%=95%)"],
    ["逆变器", "force_charge", '{"value":1}', "强制充电:0=关,1=开"],
    ["逆变器", "force_discharge", '{"value":1}', "强制放电:0=关,1=开"],
    ["逆变器", "restart", '""', "故障复位"],
    ["逆变器", "query", '""', "立即上报全量数据"],
    ["BMS", "bms/charge_enable", '{"value":1}', "充放电使能:0=禁止,1=允许"],
    ["BMS", "bms/charge_current", '{"value":500}', "最大充电电流(0.1A,500=50A)"],
    ["BMS", "bms/discharge_current", '{"value":1000}', "最大放电电流(0.1A,1000=100A)"],
    ["BMS", "bms/charge_volt", '{"value":584}', "充电截止电压(0.1V,584=58.4V)"],
    ["BMS", "bms/discharge_volt", '{"value":430}', "放电截止电压(0.1V,430=43.0V)"],
    ["OTA", "ota/esp", '""', "触发ESP固件升级"],
    ["OTA", "ota/arm", '""', "触发ARM固件升级"],
    ["OTA", "ota/dsp", '""', "触发DSP固件升级（经ARM中转）"],
    ["OTA", "ota/bms", '""', "触发BMS固件升级（经ARM中转）"],
    ["OTA", "ota/query", '""', "查询OTA状态"],
    ["并机", "parallel_enable", '{"mode":"single_phase"}', "使能并机,设置模式"],
    ["并机", "parallel_set_master", '""', "设置本机为主机"],
    ["并机", "parallel_set_slave", '{"id":1}', "设置本机为从机"],
    ["并机", "parallel_set_phase", '{"phase":"L2"}', "设置三相相位"],
    ["并机", "parallel_disable", '""', "退出并机"],
    ["并机", "parallel_sync_start", '""', "同步启动所有并机"],
    ["并机", "parallel_sync_stop", '""', "同步停止所有并机"],
]
for i, row in enumerate(cmds, 2):
    write_row(ws2, i, row)
for i, w in enumerate([10, 24, 22, 35], 1):
    ws2.column_dimensions[ws2.cell(row=1, column=i).column_letter].width = w

# 命令回复格式
ws3 = wb.create_sheet("命令回复格式")
write_header(ws3, 1, ["字段", "类型", "说明"])
for i, row in enumerate([["result", "string", "ok=成功/error=失败"], ["cmd", "string", "原始命令topic"], ["message", "string", "执行结果描述"], ["timestamp", "int", "Unix时间戳"]], 2):
    write_row(ws3, i, row)
for i, w in enumerate([14, 12, 30], 1):
    ws3.column_dimensions[ws3.cell(row=1, column=i).column_letter].width = w

# MQTT主题
ws4 = wb.create_sheet("MQTT主题")
write_header(ws4, 1, ["方向", "主题", "QoS", "频率", "说明"])
topics = [
    ["上行", "cs_inv/{sn}/status", 1, "60s", "在线状态+RSSI+IP"],
    ["上行", "cs_inv/{sn}/info", 1, "连接时", "设备信息"],
    ["上行", "cs_inv/{sn}/data/ac", 0, "5s", "交流输出"],
    ["上行", "cs_inv/{sn}/data/battery", 0, "5s", "电池状态"],
    ["上行", "cs_inv/{sn}/data/pv", 0, "5s", "光伏MPPT"],
    ["上行", "cs_inv/{sn}/data/status", 0, "5s", "系统状态"],
    ["上行", "cs_inv/{sn}/data/energy", 0, "60s", "能量统计"],
    ["上行", "cs_inv/{sn}/data/cells", 0, "30s", "电芯详情"],
    ["上行", "cs_inv/{sn}/data/alarm", 1, "事件", "告警/故障"],
    ["上行", "cs_inv/{sn}/data/parallel", 0, "5s", "并机状态(仅主机)"],
    ["上行", "cs_inv/{sn}/data/three_phase", 0, "5s", "三相系统(仅三相主机)"],
    ["上行", "cs_inv/{sn}/ota/status", 1, "按需", "OTA升级状态(esp/arm/dsp/bms)"],
    ["上行", "cs_inv/{sn}/cmd/response", 1, "按需", "命令回复"],
    ["下行", "cs_inv/{sn}/cmd", 1, "-", "控制命令"],
    ["下行", "cs_inv/{sn}/ota/cmd", 1, "-", "OTA命令(支持esp/arm/dsp/bms)"],
]
for i, row in enumerate(topics, 2):
    write_row(ws4, i, row)
for i, w in enumerate([8, 35, 8, 10, 25], 1):
    ws4.column_dimensions[ws4.cell(row=1, column=i).column_letter].width = w

# 故障码
ws5 = wb.create_sheet("PCS故障码")
write_header(ws5, 1, ["代码", "名称", "说明", "恢复条件"])
for i, row in enumerate([[1,"母线过压","直流母线电压超限","电压恢复后手动复位"],[2,"母线欠压","直流母线电压过低","电压恢复正常"],[3,"输出过压","AC输出电压>250V","电压恢复正常"],[4,"输出欠压","AC输出电压<200V","电压恢复正常"],[5,"过频","输出频率>50.5Hz","频率恢复正常"],[6,"欠频","输出频率<49.5Hz","频率恢复正常"],[7,"孤岛保护","检测到孤岛效应","电网恢复后自动重连"],[8,"过温保护","温度>85℃","温度降至75℃以下"],[9,"PV过压","光伏输入电压>150V","电压恢复正常"],[10,"输出过载","功率>6200W","负载降低后手动复位"],[11,"绝缘阻抗低","绝缘阻抗<500kΩ","排除漏电后复位"],[12,"输出短路","AC输出短路","排除短路后复位"],[16,"电池反接","电池极性接反","纠正接线后复位"],[17,"BMS通信丢失","与BMS通信中断","通信恢复"]], 2):
    write_row(ws5, i, row)
for i, w in enumerate([8, 14, 25, 25], 1):
    ws5.column_dimensions[ws5.cell(row=1, column=i).column_letter].width = w

ws6 = wb.create_sheet("BMS故障码")
write_header(ws6, 1, ["代码", "名称", "说明"])
for i, row in enumerate([["0x0101","单体过压","单体电压超过上限"],["0x0102","单体欠压","单体电压低于下限"],["0x0201","充电过流","充电电流超过限制"],["0x0202","放电过流","放电电流超过限制"],["0x0301","充电过温","充电时温度过高"],["0x0302","放电过温","放电时温度过高"]], 2):
    write_row(ws6, i, row)
for i, w in enumerate([10, 12, 25], 1):
    ws6.column_dimensions[ws6.cell(row=1, column=i).column_letter].width = w

ws7 = wb.create_sheet("保护状态位")
write_header(ws7, 1, ["位", "名称", "说明"])
for i, row in enumerate([["Bit0","过压保护","电池总电压超过上限"],["Bit1","欠压保护","电池总电压低于下限"],["Bit2","充电过流","充电电流超过限制"],["Bit3","放电过流","放电电流超过限制"],["Bit4","过温保护","电芯温度过高"],["Bit5","低温保护","电芯温度过低"],["Bit6","短路保护","检测到短路"],["Bit7","反接保护","电池极性接反"]], 2):
    write_row(ws7, i, row)
for i, w in enumerate([8, 14, 25], 1):
    ws7.column_dimensions[ws7.cell(row=1, column=i).column_letter].width = w

# 产品规格
ws8 = wb.create_sheet("产品规格")
write_header(ws8, 1, ["参数", "值"])
for i, row in enumerate([["型号","CS-I10-6k2"],["额定功率","6200W"],["输出电压","220V 单相"],["输出频率","50Hz"],["电池电压","48V (51.2V LiFePO4)"],["电池容量","100Ah / 16S"],["MPPT路数","2路"],["PV最大电压","150V"],["通信接口","WiFi(ESP32) + RS485(BMS)"],["工作温度","-20℃ ~ +55℃"]], 2):
    write_row(ws8, i, row)
for i, w in enumerate([16, 30], 1):
    ws8.column_dimensions[ws8.cell(row=1, column=i).column_letter].width = w

output = r"c:\Users\29538\Downloads\cs-inv-wifi-main\cs-inv-wifi-main\docs\CS-I10-6k2_系统参数数据字段规范_v3.xlsx"
wb.save(output)
print(f"已生成: {output}")
print(f"总计 {len(all_fields)} 个数据字段 + {len(cmds)} 个控制命令")
