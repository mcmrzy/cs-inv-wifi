'use strict';
/*
 * algorithms/pv.js — 屋顶光伏发电模型
 *
 * PV 阵列标称按屋顶容量（这里取 ~1.35×逆变器额定 ≈ 8.4kWp），
 * 输出被逆变器 6.2kW 限流钳位。
 *
 * pv_power_w = eff_irrad × capacity_Wp × (1 - 0.004×(Tcell-25)) × inverter_eff
 *   eff_irrad = irradiance / 1000（STC 1000 W/m2）
 *   Tcell = Tamb + NOCT_spec 日照升（简化：Tcell = Tamb + irrad/800×25）
 */
function pvPower(irrWm2, capacityWp, ambTempC, maxOutW) {
  const eff = irrWm2 / 1000;
  if (eff <= 0) return 0;
  const tCell = ambTempC + Math.min(30, (irrWm2 / 800) * 25); // 光伏板升温
  const tempLoss = 1 - 0.004 * (tCell - 25);
  let p = capacityWp * eff * tempLoss;
  // 逆变器输入级效率（简化曲线）
  const invE = p <= 0 ? 0 : Math.min(1, 0.965 + 0.015 * Math.min(1, p / maxOutW));
  p *= invE;
  return Math.max(0, Math.min(maxOutW, p)); // 6.2kW clamp
}

/**
 * 创建 PV 模型（城市实例）
 * @param {number} ratedW 逆变器额定功率(6200)
 * @returns {{ step: (w)=>number, capacityWp:number }}
 */
function createPv(ratedW) {
  const capacityWp = Math.round(ratedW * 1.35 / 10) * 10; // ~8.4kWp
  return {
    capacityWp,
    step(weather) {
      return pvPower(weather.irradianceWm2, capacityWp, weather.tempC, ratedW);
    },
  };
}

module.exports = { pvPower, createPv };
