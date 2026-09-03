/*
 * arm_param.h — ARM 参数结构定义 (按 ARM 源码 CollectorParamAddr.h 精确移植)
 *
 * 布局说明:
 *  - RunParamDef     176 字节 (0x03E8 起, 88 个 u16), 以 ARM 源码为准
 *  - OnlyReadDataDef 52 字节  (0x0190 起, 26 个 u16; Inverter_SN1..4 = 4×u32, 16B ASCII 自然序)
 *  - ReadWiteDataDef 4 字节   (0x00C8 起)
 *  - 控制参数        0x0000-0x0053 (84 字节, 42 个 u16)
 */
#ifndef ARM_PARAM_H
#define ARM_PARAM_H

#include <stdint.h>

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

/* ==================== 运行参数 (RunParamDef, 176 字节) ==================== */
#define RUN_PARAM_START_INDEX 0x03E8
#define RUN_PARAM_END_INDEX   0x0442
#define RUN_PARAM_SIZE        176     /* 88 个 u16 */

#pragma pack(push, 1)
typedef struct {
    /* bit0:Standby bit1:Fault bit2:Charge bit3:Discharge bit4:PvCharge
       bit5:AcCharge bit6:GeCharge bit7:ACBypass bit8:ToLoad bit9:Pvinput
       bit10:AcInput bit11:GeInput */
    uint16_t SysStatus;      /* 0   */
    uint16_t Vpv1;           /* 2   0.1V */
    uint16_t Vpv2;           /* 4   0.1V */
    uint32_t Ppv;            /* 6   0.1W */
    uint16_t Buck1Curr;      /* 10  0.1A (ARM 固定 0) */
    uint16_t Buck2Curr;      /* 12  0.1A (ARM 固定 0) */
    uint16_t BoostTemp;      /* 14  0.1C */
    uint16_t InvertTemp;     /* 16  0.1C */
    uint32_t OutputWatt;     /* 18  0.1W 负载有功 */
    uint32_t OutputVA;       /* 22  0.1VA 负载视在 */
    uint16_t OutputCurr;     /* 26  0.1A 负载电流 */
    uint32_t ACChrWatt;      /* 28  0.1W 交流充电功率 */
    uint32_t ACChrVA;        /* 32  0.1VA */
    uint16_t GridVolt;       /* 36  0.1V 交流输入电压 */
    uint16_t GridFreq;       /* 38  0.01Hz */
    uint16_t ACOutputVolt;   /* 40  0.1V */
    uint16_t ACOutputFreq;   /* 42  0.01Hz */
    uint32_t ACInWatt;       /* 44  0.1W (与 ACChrWatt 同值) */
    uint32_t ACInVA;         /* 48  0.1VA (与 ACChrVA 同值) */
    uint32_t ACDisChrWatt;   /* 52  0.1W (与 ACChrWatt 同值) */
    uint32_t ACDisChrVA;     /* 56  0.1VA (与 ACChrVA 同值) */
    uint16_t ACChrCurr;      /* 60  0.1A 交流充电电流 */
    uint16_t BatVolt;        /* 62  0.01V */
    uint16_t BatterySOC;     /* 64  1% */
    uint32_t BatDisChrWatt;  /* 66  0.1W (ARM 与 BatChrWatt 同值 Pbat) */
    uint32_t BatChrWatt;     /* 70  0.1W */
    uint16_t BatChgCurr;     /* 74  0.1A */
    uint16_t BatDischgCurr;  /* 76  0.1A */
    uint16_t BatOverCharge;  /* 78  过充标志 */
    uint16_t BusVolt;        /* 80  0.01V */
    uint16_t PvTemp;         /* 82  0.1C (ARM 未赋值) */
    uint16_t InvCurr;        /* 84  0.1A (ARM 固定 0) */
    uint16_t TransformerTemp;/* 86  0.1C (ARM 未赋值) */
    uint16_t LoadPercent;    /* 88  0.1% */
    uint16_t ParaChgCurr;    /* 90  1A (ARM 固定 0) */
    uint32_t WorkTimeTotal;  /* 92  s (ARM 固定 0) */
    uint16_t MpptFanSpeed;   /* 96  % (ARM 固定 0) */
    uint16_t InvFanSpeed;    /* 98  % (ARM 固定 0) */
    uint16_t PairedSocket;   /* 100 从机功能 (ARM 未赋值) */
    uint16_t OnlineSocket;   /* 102 */
    uint16_t OnSocket;       /* 104 */
    uint64_t Warning;        /* 106 告警位 */
    uint16_t BmsWarning;     /* 114 BMS 告警 */
    uint32_t FaultValue;     /* 116 故障码 */
    /* 能量统计 0.1kWh */
    uint32_t EGen_today;     /* 120 */
    uint32_t EGen_total;     /* 124 */
    uint32_t Epv_today;      /* 128 */
    uint32_t Epv_total;      /* 132 */
    uint32_t Eac_chrToday;   /* 136 */
    uint32_t Eac_chrTotal;   /* 140 */
    uint32_t Ebat_dischrToday;/* 144 */
    uint32_t Ebat_dischrTotal;/* 148 */
    uint32_t Ebat_chrToday;  /* 152 */
    uint32_t Ebat_chrTotal;  /* 156 */
    uint32_t Eac_dischrToday;/* 160 */
    uint32_t Eac_dischrTotal;/* 164 */
    uint32_t Eop_dischrToday;/* 168 */
    uint32_t Eop_dischrTotal;/* 172 */
} RunParamDef;               /* 总 176 字节 */
#pragma pack(pop)

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
