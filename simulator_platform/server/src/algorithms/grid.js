'use strict';
/*
 * algorithms/grid.js — 并网/离网能量流与系统快照组装
 *
 * 汇总 pv/load/battery => 生成 heartbeat 上报体，结构与固件 telemetry.c
 * 及服务器解析器 V2.1 的 57 值数组完全对齐：
 *   sys[11]/pv[5]/ac[11]/chr[3]/bat[5]/eng[14]/fan[2]/diag[3]/sock[3]
 * 字段顺序与缩放严格按 docs/CS-L10-6K2_MQTT_上报协议设计_V2.1.md §6.2。
 */
const RATED_W = 6200;

/** 计算 SysStatus 位域（对齐 arm_param.h：bit0 Standby..bit12 AcInput）
 *  bit0 Standby, bit1 Fault, bit2 Charge, bit3 Discharge, bit4 PvCharge,
 *  bit5 AcCharge, bit7 AcBypass, bit8 ToLoad, bit9 Pvinput, bit10 AcInput
 */
function sysStatusBits(st) {
  let b = 0;
  if (!st.powerOn && st.powerOn !== undefined) return 0x0001; // Standby
  if (st.pvW > 20) b |= 0x0200;                       // bit9 Pvinput
  if (st.loadW > 30) b |= 0x0100;                     // bit8 ToLoad
  if (st.batChargeW > 5) b |= 0x0004;                 // bit2 Charge
  if (st.batDischargeW > 5) b |= 0x0008;              // bit3 Discharge
  if (st.pvW > 0 && st.batChargeW > 0) b |= 0x0010;   // bit4 PvCharge
  if (st.acChargeW > 5) b |= 0x0020;                  // bit5 AcCharge
  if (st.gridExportW > 5) b |= 0x0080;                // bit7 AcBypass
  if (st.gridImportW > 5) b |= 0x0400;                // bit10 AcInput
  if (b === 0) b = 0x0001;                            // Standby
  return b;
}

/**
 * 生成一次完整快照（57 值，对齐服务器解析器 V2.1）
 * @param {object} st { pvW,loadW,batChargeW,batDischargeW,soc,gridImportW,gridExportW,
 *                      tempC,invTemp,boostTemp,transTemp,pvTemp,busV,loadPercent,
 *                      batVolt,invCurr,acChargeW,workTimeTotal, tSec, powerOn }
 * @param {object} eng 能量累计结构（原地累加）
 * @returns  {sys:[]...} 对齐协议 V2.1 的上报体
 */
function buildSnapshot(st, eng, ratedW = RATED_W) {
  const invOut = Math.min(ratedW, st.loadW); // 逆变器输出近似负载
  const batV = st.batVolt || 48;
  const busV = st.busV || 410;

  // 能量累计（kWh 浮点，组包时 ×10 到 0.1kWh）
  const dtH = 1 / 60;
  if (eng) {
    const gen = (st.pvW * dtH) / 1000;
    eng.EGen_today += gen;
    eng.Epv_today += gen;
    eng.loadToday += (st.loadW * dtH) / 1000;
  }

  // 电池电流：充电为正，放电为负（协议 bat[2]）
  const batCurr = st.batDischargeW > 0
    ? -(st.batDischargeW / batV) * 10
    : (st.batChargeW / batV) * 10;

  return {
    sn: st.sn,
    t: st.tSec,
    sys: [
      sysStatusBits(st),          // [0] sys_status
      st.faultCode ?? 0,          // [1] fault_code
      st.warning ?? 0,            // [2] warning
      st.bmsWarning ?? 0,         // [3] bms_warning
      Math.round((st.invTemp ?? 45) * 10),          // [4] inverter_temperature 0.1°C
      Math.round((st.boostTemp ?? 40) * 10),        // [5] boost_temperature 0.1°C
      st.transformerTemp != null ? Math.round(st.transformerTemp * 10) : null, // [6] transformer_temperature
      st.pvTemp != null ? Math.round(st.pvTemp * 10) : null,       // [7] pv_temperature
      Math.round(busV * 100),     // [8] dc_bus_voltage 0.01V
      Math.round((st.loadPercent ?? (st.loadW / ratedW) * 100) * 10), // [9] load_percent 0.1%
      st.batOverCharge ? 1 : 0,   // [10] battery_overcharge
    ],
    pv: [
      Math.round((st.pv1V ?? (st.pvW > 0 ? 360 : 0)) * 10), // [0] pv1_voltage 0.1V
      Math.round((st.buck1Curr ?? 0) * 10),                  // [1] buck1_current 0.1A
      Math.round((st.pv2V ?? (st.pvW > 0 ? 358 : 0)) * 10),  // [2] pv2_voltage 0.1V
      Math.round((st.buck2Curr ?? 0) * 10),                  // [3] buck2_current 0.1A
      Math.round(st.pvW * 10),                               // [4] pv_total_power 0.1W
    ],
    ac: [
      Math.round((st.acOutV ?? 230) * 10),        // [0] ac_output_voltage 0.1V
      Math.round((st.acOutFreq ?? 50) * 100),     // [1] ac_output_frequency 0.01Hz
      Math.round(invOut * 10),                    // [2] output_power 0.1W
      Math.round(invOut * 10),                    // [3] output_apparent_power 0.1VA
      Math.round((st.invCurr ?? invOut / 230) * 10), // [4] output_current 0.1A
      Math.round((st.gridVolt ?? (st.gridImportW > 0 ? 220.5 : 0)) * 10), // [5] grid_voltage
      Math.round((st.gridFreq ?? 50) * 100),      // [6] grid_frequency 0.01Hz
      Math.round(st.gridImportW * 10),            // [7] ac_input_power 0.1W
      Math.round(st.gridImportW * 10),            // [8] ac_input_apparent_power
      Math.round(st.gridExportW * 10),            // [9] ac_bypass_power 0.1W
      Math.round(st.gridExportW * 10),            // [10] ac_bypass_apparent_power
    ],
    chr: [
      Math.round((st.acChargeW ?? 0) * 10),   // [0] ac_charge_power 0.1W
      Math.round((st.acChargeW ?? 0) * 10),   // [1] ac_charge_apparent_power
      Math.round((st.acChgCurr ?? 0) * 10),   // [2] ac_charge_current 0.1A
    ],
    bat: [
      Math.round(batV * 100),          // [0] battery_voltage 0.01V
      Math.round((st.soc || 0) * 100), // [1] battery_soc 1%
      Math.round(batCurr),             // [2] battery_current 0.1A（充+放-）
      Math.round(st.batChargeW * 10),  // [3] battery_charge_power 0.1W
      Math.round(st.batDischargeW * 10), // [4] battery_discharge_power
    ],
    eng: (eng
      ? [
          Math.round((eng.EGen_today) * 10),        // [0] gen_energy_daily
          Math.round((eng.EGen_total) * 10),        // [1] gen_energy_total
          Math.round((eng.Epv_today) * 10),         // [2] daily_pv_energy
          Math.round((eng.Epv_total) * 10),         // [3] total_pv_energy
          Math.round((eng.Eac_chrToday) * 10),      // [4] ac_charge_energy_daily
          Math.round((eng.Eac_chrTotal) * 10),      // [5] ac_charge_energy_total
          Math.round((eng.Ebat_dischrToday) * 10),  // [6] daily_discharge_energy
          Math.round((eng.Ebat_dischrTotal) * 10),  // [7] total_discharge_energy
          Math.round((eng.Ebat_chrToday) * 10),     // [8] daily_charge_energy
          Math.round((eng.Ebat_chrTotal) * 10),     // [9] total_charge_energy
          Math.round((eng.Eac_dischrToday) * 10),   // [10] ac_bypass_energy_daily
          Math.round((eng.Eac_dischrTotal) * 10),   // [11] ac_bypass_energy_total
          Math.round((eng.Eop_dischrToday || 0) * 10), // [12] output_energy_daily
          Math.round((eng.Eop_dischrTotal || 0) * 10), // [13] output_energy_total
        ]
      : [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    fan: [
      Math.round(st.pvW > 100 ? 40 + Math.random() * 20 : 0), // [0] mppt_fan_speed %
      Math.round(invOut > 100 ? 30 + (invOut / ratedW) * 50 : 0), // [1] inv_fan_speed %
    ],
    diag: [
      Math.round((st.invCurr ?? invOut / 230) * 10), // [0] inv_current 0.1A
      Math.round((st.parallelChgCurr ?? 0)),         // [1] parallel_charge_current 1A
      Math.round(st.workTimeTotal ?? 0),             // [2] work_time_total s
    ],
    sock: [
      1, // [0] paired_socket（模拟单机占位）
      1, // [1] online_socket
      1, // [2] on_socket
    ],
  };
}

module.exports = { buildSnapshot, sysStatusBits, RATED_W };