'use strict';
/*
 * algorithms/grid.js — 并网/离网能量流与系统快照组装
 *
 * 汇总 pv/load/battery => 生成 heartbeat 上报体，结构与固件 telemetry.c
 * 的 57 值数组对齐（sys/pv/ac/chr/bat/eng/fan/diag/sock），
 * 使模拟器在 MQTT 上看起来与真机上报一致。
 */
const RATED_W = 6200;

/** 计算 SysStatus 位域（对齐 arm_param.h：bit0 Standby..bit12 AcInput）
 *  bit2 Charge, bit3 Discharge, bit4 PvCharge, bit5 AcCharge
 *  bit8 ToLoad, bit9 Pvinput, bit10 AcInput
 */
function sysStatusBits(st) {
  let b = 0;
  if (st.pvW > 20) b |= 0x0200;                       // bit9 Pvinput
  if (st.loadW > 30) b |= 0x0100;                     // bit8 ToLoad
  if (st.batChargeW > 5) b |= 0x0004;                 // bit2 Charge
  if (st.batDischargeW > 5) b |= 0x0008;              // bit3 Discharge
  if (st.pvW > 0 && st.batChargeW > 0) b |= 0x0010;   // bit4 PvCharge
  if (st.gridImportW > 5) b |= 0x0400;                // bit10 AcInput
  if (st.pvW > 0 && st.loadW > 0) b |= 0x0001;        // bit0 Standby?（占位）
  if (b === 0) b = 0x0001; // Standby
  return b;
}

/**
 * 生成一次完整快照
 * @param {object} st { pvW,loadW,chargeW,dischargeW,soc,gridImportW,gridExportW, tempC, weather, tSec }
 * @param {object} eng 能量累计结构（原地累加）
 * @returns  {sys:[]...} 对齐固件结构的上报体
 */
function buildSnapshot(st, eng, ratedW = RATED_W) {
  const oc = st.soc;
  const invOut = Math.min(ratedW, st.loadW); // 逆变器输出近似负载

  // 能量累计（kWh, 0.1kWh 缩放 -> 这里用浮点 kWh）
  const dtH = 1 / 60;
  if (eng) {
    const gen = (st.pvW * dtH) / 1000;                                   // kWh
    const consumption = ((st.loadW) * dtH) / 1000;
    eng.EGen_today += gen;
    eng.Epv_today += gen;
    eng.loadToday += consumption;
  }

  return {
    sn: st.sn,
    t: st.tSec,
    sys: [sysStatusBits(st), Math.round(st.pvW / 10), // FaultValue 占位, BmsWarning
          st.fault ?? 0, st.bmsWarning ?? 0,
          Math.round(38 + (st.tempC - 20) * 0.3) /*InvertTemp 0.1°C*/, 0,
          Math.round(st.busV ?? 410 * 100), Math.round(st.loadPercent ?? (st.loadW / ratedW) * 100),
          0],
    pv: [Math.round(st.pvVolt ?? (st.pvW > 0 ? 3600 : 0)), 0,
         Math.round(st.pv2V ?? (st.pvW > 0 ? 3580 : 0)), 0, Math.round(st.pvW * 10)],
    ac: [Math.round(2205 + (Math.random() * 4 - 2)), Math.round(5000),
         Math.round(invOut * 10), Math.round(invOut * 10),
         Math.round(st.curr ?? st.loadW / 230 * 10),
         Math.round((st.gridImportW > 0 ? 2205 : 0)), Math.round(5000),
         Math.round(st.gridImportW * 10), Math.round(st.gridImportW * 10),
         Math.round(st.gridExportW * 10), Math.round(st.gridExportW * 10)],
    chr: [Math.round(st.batChargeW * 10), Math.round(st.batChargeW * 10), Math.round(st.batCurr ?? st.batChargeW / 48 * 10)],
    bat: [Math.round(st.batVolt ?? 48 * 100), Math.round(st.soc * 100),
          st.batDischargeW > 0 ? -Math.round(st.batDischargeW / st.batVolt * 10)
                               : Math.round(st.batChargeW / st.batVolt * 10),
          Math.round(st.batDischargeW * 10), Math.round(st.batChargeW * 10)],
    eng: (eng
      ? [Math.round((eng.EGen_today) * 10), Math.round((eng.EGen_total) * 10),
         Math.round((eng.Epv_today) * 10), Math.round((eng.Epv_total) * 10),
         Math.round((eng.Eac_chrToday) * 10), Math.round((eng.Eac_chrTotal) * 10),
         Math.round(eng.loadToday * 10), Math.round(eng.loadTotal * 10),
         0, 0, 0, 0, 0, 0]
      : [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    fan: [Math.round(st.pvW > 100 ? 40 + Math.random() * 20 : 0), Math.round((st.loadW / ratedW) * 80)],
    diag: [0, 0, Math.round(st.tempC * 10)],
    sock: [1, 1, 1],
  };
}

module.exports = { buildSnapshot, sysStatusBits, RATED_W };
