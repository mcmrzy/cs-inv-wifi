'use strict';
/*
 * algorithms/load.js — 家庭负载曲线
 *
 * 以本地时刻 + 星期为准，构建早/晚双峰负载（0.2–8kW），叠加城市/季节系数与噪声。
 * 使场景页负载变化"像真的家庭用电"。
 */
function loadPower(nowLocal, isWeekend, scale = 1.0) {
  const h = nowLocal.getHours() + nowLocal.getMinutes() / 60;
  // 基础曲线（kW）：夜间低，早/晚高峰
  const base =
    0.25 + // 待机
    1.1 * gauss(h, 7.5, 1.2) +   // 早高峰
    0.8 * gauss(h, 12.5, 1.5) +  // 午间
    2.4 * gauss(h, 19.5, 1.8) +  // 晚高峰（最大）
    0.6 * gauss(h, 22.5, 1.2);   // 睡前
  // 周末变动
  const factor = isWeekend ? 1.15 : 1.0;
  const noise = 1 + (Math.random() - 0.5) * 0.18; // ±9%
  let w = base * scale * factor * noise * 1000;      // -> W
  return Math.max(100, Math.round(w));
}

function gauss(x, mu, sigma) {
  const z = (x - mu) / sigma;
  return Math.exp(-0.5 * z * z);
}

function createLoad(cityClimate) {
  return {
    step(nowLocal, isWeekend) {
      // 城市温度高则空调负载略高
      const acFactor = 1 + Math.max(0, (cityClimate.tempC - 18) / 30) * 0.3;
      return loadPower(nowLocal, isWeekend, acFactor);
    },
  };
}

module.exports = { loadPower, createLoad, gauss };
