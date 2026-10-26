/*
 * arm_param.h — ARM 参数结构定义 (按 ARM 源码 CollectorParamAddr.h 精确移植)
 *
 * 布局说明 (2026-09-22 对齐 ARM App(10) 实际源码, 旧 App(8) 200B 混排布局废弃):
 *  - RunParamDef     174 字节 (0x03E8 起, 87 个 u16): ReadRunParam (Collector.c:368)
 *    已删除旧版全部 ×10/×100 放大, 全字段 = SysParam/Statistics 原值直传
 *  - OnlyReadDataDef 52 字节  (0x0190 起, 26 个 u16; Inverter_SN1..4 = 4×u32, 16B ASCII 自然序)
 *  - ReadWiteDataDef 4 字节   (0x00C8 起)
 *  - 控制参数        0x0000-0x0053 (84 字节, 42 个 u16)
 */
#ifndef ARM_PARAM_H
#define ARM_PARAM_H

#include <stdint.h>
#include <stddef.h>   /* offsetof (RunParamDef 布局静态断言) */

/* ==================== 控制参数索引 (CollectorParamAddr.h) ==================== */
#define INDEX_OUTPUT_PRIORITY         0x0000
#define INDEX_MAX_CHARGE_CURRENT_ALL  0x0001
#define INDEX_ACIN_VOLT_RANGE         0x0002
#define INDEX_BAT_CAPACITY            0x0003
#define INDEX_BATTERY_TYPE            0x0004
#define INDEX_OVERLOAD_RESTART        0x0005
#define INDEX_HIGH_TEMP_RESTART       0x0006
#define INDEX_OUTPUT_VOLTAGE          0x0007
#define INDEX_OUTPUT_FREQUENCY        0x0008
#define INDEX_MASTER_SLAVE            0x0009
#define INDEX_MAX_CHARGE_CITY_CURRENT 0x000A
#define INDEX_LOW_VOLT_RETURN_CITY    0x000B
#define INDEX_HIGH_VOLT_RETURN_BAT    0x000C
#define INDEX_BAT_CHARGE_PRIORITY     0x000D
#define INDEX_ALARM_CONTROL           0x000E
#define INDEX_BACKLIGHT_CTRL          0x000F
#define INDEX_POWER_SHUTDOWN_ALARM    0x0010
#define INDEX_OVERLOAD_USE_CITY_POWER 0x0011
#define INDEX_PARA_MAX_DISCHG_CURR    0x0012
#define INDEX_PARA_MAX_CHG_CURR       0x0013
#define INDEX_RECOVER_THRESHOLD_VOLT  0x0014
#define INDEX_SOLAR_POWER_BALANCE     0x0015
#define INDEX_AC_OUTPUT_MODE          0x0016
#define INDEX_LI_BAT_MATERIAL         0x0017
#define INDEX_CELL_SERIAL_LIFEPO4     0x0018
#define INDEX_CELL_SERIAL_LI_NMC      0x0019
#define INDEX_EQ_ENABLE               0x001A
#define INDEX_EQ_VOLTAGE_SET          0x001B
#define INDEX_EQ_TIME_SET             0x001C
#define INDEX_EQ_TIMEOUT_SET          0x001D
#define INDEX_EQ_INTERVAL_SET         0x001E
#define INDEX_EQ_ACTIVATE_SET         0x001F
#define INDEX_CHARGE_TIME_SET         0x0020
#define INDEX_CLOSE_CHARGE_TIME_SET   0x0021
#define INDEX_LVOLT_OPEN_GEN          0x0022
#define INDEX_HVOLT_CLOSE_GEN         0x0023
#define INDEX_SOC_BACK_UTL            0x0024
#define INDEX_SOC_BACK_BAT            0x0025
#define INDEX_SOC_BACK_GEN            0x0026
#define INDEX_SOC_CLOSE_GEN           0x0027
#define INDEX_SOCCUTOFF               0x0028
#define INDEX_BUZZER_EN               0x0029
#define INDEX_S_GRADE_SOC             0x002A
#define INDEX_A_GRADE_SOC             0x002B
#define INDEX_B_GRADE_SOC             0x002C
#define INDEX_MANUAL_SOCKET           0x002D
#define INDEX_CONTROL_SOCKET          0x002E
#define INDEX_RECOVER_THRESHOLD_SOC   0x002F
#define INDEX_LI_BAT_PROTOCOL_TYPE    0x0030
#define INDEX_GEN_RATE_WATT           0x0031
#define INDEX_GE_BALANCE_EN           0x0032
#define INDEX_GEN_WORK_START1_L       0x0033
#define INDEX_GEN_WORK_START1_H       0x0034
#define INDEX_GEN_WORK_END1_L         0x0035
#define INDEX_GEN_WORK_END1_H         0x0036
#define INDEX_GEN_WORK_START2_L       0x0037
#define INDEX_GEN_WORK_START2_H       0x0038
#define INDEX_GEN_WORK_END2_L         0x0039
#define INDEX_GEN_WORK_END2_H         0x003A
#define INDEX_UTI_OUTPUT_START1_L     0x003B
#define INDEX_UTI_OUTPUT_START1_H     0x003C
#define INDEX_UTI_OUTPUT_END1_L       0x003D
#define INDEX_UTI_OUTPUT_END1_H       0x003E
#define INDEX_UTI_OUTPUT_START2_L     0x003F
#define INDEX_UTI_OUTPUT_START2_H     0x0040
#define INDEX_UTI_OUTPUT_END2_L       0x0041
#define INDEX_UTI_OUTPUT_END2_H       0x0042
#define INDEX_VBAT_AGM                0x0043
#define INDEX_VBAT_FLOOD              0x0044
#define INDEX_VBAT_USER_PB            0x0045
#define INDEX_VBAT_AGM_FLOAT          0x0046
#define INDEX_VBAT_FLOOD_FLOAT        0x0047
#define INDEX_VBAT_LI_FLOAT           0x0048
#define INDEX_VBAT_USER_PB_FLOAT      0x0049
#define INDEX_VBAT_USER_LIFE_FLOAT    0x004A
#define INDEX_VBAT_USER_LINMC_FLOAT   0x004B
#define INDEX_VBAT_AGM_CUTOFF         0x004C
#define INDEX_VBAT_FLOOD_CUTOFF       0x004D
#define INDEX_VBAT_LI_CUTOFF          0x004E
#define INDEX_VBAT_USER_PB_CUTOFF     0x004F
#define INDEX_VBAT_USER_LIFE_CUTOFF   0x0050
#define INDEX_VBAT_USER_LINMC_CUTOFF  0x0051
#define INDEX_SOC_MAX_UTL_CHG         0x0052
#define INDEX_VMAX_UTL_CHG            0x0053

#define CONFIG_PARAM_MAX_INDEX (INDEX_VMAX_UTL_CHG + 1)   /* 84 个 u16 索引 */

/* ==================== 控制参数读取区 (READ_CTRLPARAM 0x01) ==================== */
/* ARM 端 ReadUserCtrlParam 校验 param1+param2 <= ConfigParamMaxIndex(84),
   故 0x01 最大读取 84 字节 = 42 个 u16 (索引 0x0000-0x0029), 即 V2.1 协议 9.2 表 42 键 */
#define CTRL_PARAM_START_INDEX 0x0000
#define CTRL_PARAM_SIZE        84      /* 可读取的最大字节数 */
#define CTRL_PARAM_COUNT       42      /* 42 个 u16 */

/* ==================== 运行参数 (RunParamDef, 174 字节, ARM App(10) 直传版) ====================
 * 依 ARM 源码 App(10) source/Collector/CollectorParamAddr.h RunParamDef (pack(1),
 * 87 个 u16) 精确移植, 2026-09-22。该版 ReadRunParam 删除了旧版全部 ×10/×100
 * 放大, 所有字段 = SysParam/Statistics 原值直传; 并在填充前 _memset(.,0,256),
 * 未赋值字段 (PvTemp/InvCurr/TransformerTemp/并机 socket) 恒 0, 不再是堆垃圾。
 * SysParam 原始量纲以 ARM 接收 DSP 的 UsartDsp.c 单位注释为权威 (App(8)/App(10)
 * 逐行相同; 例: VloadA/VgridA/Vpv "单位1V"、IgridA/IloadA/Ipv "单位0.01A"、
 * VbatA "单位0.1V"、LoadFreq "单位0.1Hz"、InvtTemperature "单位0.1°C");
 * 各字段注释: 字节偏移 + 权威单位 + 是否发布。
 * 换算与界限**全在服务端** (device-communication/internal/telemetry/heartbeat_v3.go)
 * —— 本固件只按字序原样转发, 不做任何换算。
 * "不发布" = 云端 57 值 heartbeat 契约暂无对应槽位。 */
#define RUN_PARAM_START_INDEX 0x03E8
#define RUN_PARAM_END_INDEX   0x044B
#define RUN_PARAM_SIZE        174     /* sizeof(RunParamDef), 87 个 u16 */
#define RUN_PARAM_WORDS       (RUN_PARAM_SIZE / 2)  /* 87: 原样转发的 u16 字数 */

#pragma pack(push, 1)
typedef struct {
    /* bit0:Standby bit1:Fault bit2:Charge bit3:Discharge bit4:PvCharge
       bit5:AcCharge bit6:GeCharge bit7:ACBypass bit8:ToLoad bit9:Pvinput
       bit10:AcInput bit11:GeInput */
    uint16_t SysStatus;       /*   0  位标志, 直传, 发布 */
    uint16_t Vpv1;            /*   2  1V (DSP 原值), 发布 ×10 → 0.1V */
    uint16_t Vpv2;            /*   4  1V, 发布 ×10 */
    uint16_t Ppv1;            /*   6  1W (云端无槽位, 不发布) */
    uint16_t Ppv2;            /*   8  1W (不发布) */
    uint16_t Ppv;             /*  10  1W PV 总功率, 发布 ×10 → 0.1W */
    uint16_t Buck1Curr;       /*  12  0.01A (Ipv1), 发布 ÷10 → 0.1A */
    uint16_t Buck2Curr;       /*  14  0.01A, 发布 ÷10 */
    int16_t  BoostTemp;       /*  16  1℃ ((s16)float LV_MOS 截断), 发布 ×10 → 0.1℃ */
    int16_t  InvertTemp;      /*  18  0.1℃ (InvtTemperature), 直传, 发布 */
    int16_t  OutputWatt;      /*  20  1W (Pload), 发布 ×10 → 0.1W (<0 钳 0) */
    int16_t  OutputVA;        /*  22  1VA (Sload), 发布 ×10 (<0 钳 0) */
    int16_t  OutputCurr;      /*  24  0.01A (IloadA), 发布 ÷10 → 0.1A (<0 钳 0) */
    int16_t  ACChrWatt;       /*  26  1W (=ACInWatt 同源), 发布 ×10 (<0 钳 0) */
    int16_t  ACChrVA;         /*  28  1VA (=ACInVA 同源), 发布 ×10 */
    uint16_t GridVolt;        /*  30  1V (VgridA), 发布 ×10 → 0.1V */
    uint16_t GridFreq;        /*  32  0.1Hz, 发布 ×10 → 0.01Hz */
    uint16_t ACOutputVolt;    /*  34  1V (VloadA, UsartDsp.c:290 "单位1V"), 发布 */
    uint16_t ACOutputFreq;    /*  36  0.1Hz (LoadFreq), 发布 ×10 → 0.01Hz */
    int16_t  ACInWatt;        /*  38  1W (Pgrid), 发布 ×10 → 0.1W (<0 钳 0) */
    int16_t  ACInVA;          /*  40  1VA (Sgrid), 发布 ×10 (<0 钳 0) */
    int16_t  ACDisChrWatt;    /*  42  1W (=ACInWatt 同源), 发布 ×10 */
    int16_t  ACDisChrVA;      /*  44  1VA (=ACInVA 同源), 发布 ×10 */
    int16_t  ACChrCurr;       /*  46  0.01A (IgridA, DSP Igrid_rms_DP×100), 发布 ÷10 → 0.1A (<0 钳 0) */
    uint16_t BatVolt;         /*  48  0.1V (VbatA), 发布 ×10 → 0.01V */
    uint16_t BatterySOC;      /*  50  1% (BatPercent, >100 ARM 置 0), 直传, 发布 */
    uint16_t BatDisChrWatt;   /*  52  1W (Pbat<0 取反), 发布 ×10 → 0.1W */
    uint16_t BatChrWatt;      /*  54  1W (Pbat>=0), 发布 ×10 */
    uint16_t BatChgCurr;      /*  56  0.1A (Ibat>=0), 直传, 发布 */
    uint16_t BatDischgCurr;   /*  58  0.1A (Ibat<0 取反), 直传, 发布 */
    uint16_t BatOverCharge;   /*  60  过充位 (warningCode2Bits.batOverCharged), 直传, 发布 */
    uint16_t BusVolt;         /*  62  0.1V (BusVoltage), 发布 ×10 → 0.01V */
    int16_t  Ibus;            /*  64  0.01A (Ibus); 云端契约无槽位, 不发布 */
    uint16_t PvTemp;          /*  66  ARM 未赋值 (恒 0), 不发布 */
    uint16_t InvCurr;         /*  68  ARM 未赋值 (恒 0), 不发布 */
    uint16_t TransformerTemp; /*  70  ARM 未赋值 (恒 0), 不发布 */
    uint16_t LoadPercent;     /*  72  0.1% (PloadPercent×10), 直传, 发布 */
    uint16_t GridPercent;     /*  74  0.1% (PgridPercent×10) (云端无槽位, 不发布) */
    uint16_t ParaChgCurr;     /*  76  ARM 固定 0, 直传, 发布 */
    uint32_t WorkTimeTotal;   /*  78  ARM 固定 0 (未实现), 直传, 发布 */
    uint16_t MpptFanSpeed;    /*  82  ARM 固定 0, 直传, 发布 */
    uint16_t InvFanSpeed;     /*  84  ARM 固定 0, 直传, 发布 */
    uint16_t PairedSocket;    /*  86  ARM 未赋值 (恒 0), 不发布 */
    uint16_t OnlineSocket;    /*  88  ARM 未赋值 (恒 0), 不发布 */
    uint16_t OnSocket;        /*  90  ARM 未赋值 (恒 0), 不发布 */
    uint64_t Warning;         /*  92  告警位: bit0~15 = warningCode1 (bit0 合并
                                     风扇告警), bit16+ 恒 0 (App(10) 已 memset);
                                     发布前仍 &0xFFFF 兜底 */
    uint16_t BmsWarning;      /* 100  BMS 告警 (warningCode2 & 0x1FFF), 直传, 发布 */
    uint32_t FaultValue;      /* 102  故障码 (errorCode), 直传, 发布 */
    /* 能量统计 14×u32 (0.1kWh 契约; today 组 ARM 内部 /360000、total 组原值,
       两侧换算不一致属 ARM 侧现状, ESP 透传), 全部直传发布 */
    uint32_t EGen_today;      /* 106 */
    uint32_t EGen_total;      /* 110 */
    uint32_t Epv_today;       /* 114 */
    uint32_t Epv_total;       /* 118 */
    uint32_t Eac_chrToday;    /* 122 */
    uint32_t Eac_chrTotal;    /* 126 */
    uint32_t Ebat_dischrToday;/* 130 */
    uint32_t Ebat_dischrTotal;/* 134 */
    uint32_t Ebat_chrToday;   /* 138 */
    uint32_t Ebat_chrTotal;   /* 142 */
    uint32_t Eac_dischrToday; /* 146 */
    uint32_t Eac_dischrTotal; /* 150 */
    uint32_t Eop_dischrToday; /* 154 */
    uint32_t Eop_dischrTotal; /* 158 */
    /* 逆变 A 相交流量块 (云端无槽位, 不发布; 量纲依 App(8) 注释保留) */
    uint16_t VinvtA;          /* 162  1V */
    int16_t  IinvtA;          /* 164  0.01A 带符号 */
    uint16_t InvtFreq;        /* 166  0.1Hz */
    int16_t  Pinvt;           /* 168  1W 带符号 */
    int16_t  Sinvt;           /* 170  1VA 带符号 */
    uint16_t InvtPercent;     /* 172  1% */
} RunParamDef;                /* 总 174 字节 */
#pragma pack(pop)

/* 布局静态断言: 编译期锁定关键字段偏移与总长, 防止再版式漂移 (需 <stddef.h>) */
_Static_assert(sizeof(RunParamDef) == RUN_PARAM_SIZE, "RunParamDef must be 174B");
_Static_assert(offsetof(RunParamDef, Ppv) == 10, "Ppv offset");
_Static_assert(offsetof(RunParamDef, BoostTemp) == 16, "BoostTemp offset");
_Static_assert(offsetof(RunParamDef, OutputWatt) == 20, "OutputWatt offset");
_Static_assert(offsetof(RunParamDef, GridVolt) == 30, "GridVolt offset");
_Static_assert(offsetof(RunParamDef, ACOutputVolt) == 34, "ACOutputVolt offset");
_Static_assert(offsetof(RunParamDef, BatVolt) == 48, "BatVolt offset");
_Static_assert(offsetof(RunParamDef, LoadPercent) == 72, "LoadPercent offset");
_Static_assert(offsetof(RunParamDef, Warning) == 92, "Warning offset");
_Static_assert(offsetof(RunParamDef, EGen_today) == 106, "EGen_today offset");
_Static_assert(offsetof(RunParamDef, VinvtA) == 162, "VinvtA offset");

/* ==================== 可读可写数据 (ReadWiteData_Def, 4 字节) ==================== */
#define READWITE_START_INDEX   0x00C8
#define READWITE_END_INDEX     0x00C9
#define INDEX_CTRLPARAM_ALTER_TIME_L 0x00C8
#define INDEX_CTRLPARAM_ALTER_TIME_H 0x00C9
#define INDEX_UTC_L             0x00CA
#define INDEX_UTC_H             0x00CB

#pragma pack(push, 1)
typedef struct {
    uint32_t CtrlParamAlterTime;
} ReadWiteDataDef;           /* 4 字节 */
#pragma pack(pop)

/* ==================== 只读数据 (OnlyReadData_Def, 52 字节) ==================== */
/* ARM 侧 pack(1): 5×u16(0..9) + Inverter_SN1..4 (4×u32, 偏移 10..25, 小端 ASCII
   自然序) + 13×u16 (偏移 26..51, 相对旧 44B 布局整体后移 8B)。
   全量读取 0x0190 起 52B (0x0190-0x01A9, 26 个 u16)。 */
#define ONLYREAD_START_INDEX 0x0190
#define ONLYREAD_END_INDEX   0x01A9
#define INDEX_ARM_FW_VER     0x0190
#define INDEX_DSP_FW_VER     0x0191
#define INDEX_INV_MODULE_L   0x0192
#define INDEX_INV_MODULE_H   0x0193
#define INDEX_HW_VER         0x0194
/* 4×u32 各占 2 个 u16 字: SN1@0x195, SN2@0x197, SN3@0x199, SN4@0x19B (起始字) */
#define INDEX_INV_SN1        0x0195
#define INDEX_INV_SN2        0x0197
#define INDEX_INV_SN3        0x0199
#define INDEX_INV_SN4        0x019B
#define INDEX_DEV_TYPE_CODE_L 0x019D
#define INDEX_DEV_TYPE_CODE_H 0x019E
#define INDEX_RATE_WATT      0x019F
#define INDEX_RATE_VA        0x01A0
#define INDEX_NOM_GRID_VOLT  0x01A1
#define INDEX_NOM_GRID_FREQ  0x01A2
#define INDEX_NOM_BAT_VOLT   0x01A3
#define INDEX_NOM_PV_CURR    0x01A4
#define INDEX_NOM_AC_CHG_CURR 0x01A5
#define INDEX_NOM_OP_VOLT    0x01A6
#define INDEX_NOM_OP_FREQ    0x01A7
#define INDEX_NOM_OP_POW     0x01A8
#define INDEX_BL_VERSION     0x01A9

#define ONLYREAD_DATA_SIZE   52      /* 26 个 u16 */

#pragma pack(push, 1)
typedef struct {
    uint16_t ARMFirmwareVersion;
    uint16_t DSPFirmwareVersion;
    uint16_t InverterModuleL;
    uint16_t InverterModuleH;
    uint16_t HardwareVersion;
    uint32_t Inverter_SN1;   /* 4 个 ASCII 字符, 小端: ch0 | ch1<<8 | ch2<<16 | ch3<<24 */
    uint32_t Inverter_SN2;
    uint32_t Inverter_SN3;
    uint32_t Inverter_SN4;
    uint16_t DeviceTypeCodeL;
    uint16_t DeviceTypeCodeH;
    uint16_t RateWatt;
    uint16_t RateVA;
    uint16_t NomGridVolt;
    uint16_t NomGridFreq;
    uint16_t NomBatVolt;
    uint16_t NomPvCurr;
    uint16_t NomAcChgCurr;
    uint16_t NomOpVolt;
    uint16_t NomOpFreq;
    uint16_t NomOpPow;
    uint16_t BLVersion;
} OnlyReadDataDef;           /* 52 字节 */
#pragma pack(pop)

_Static_assert(sizeof(OnlyReadDataDef) == 52, "OnlyReadDataDef must be 52B");

#endif /* ARM_PARAM_H */
