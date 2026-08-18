'use strict';
/*
 * algorithms/battery.js — 电池储能模型
 *
 * 48V 电池组，额定容量可配（默认 200Ah ≈ 9.6kWh）。
 * 策略：光伏富余 => 充电（优先自用）；光伏不足 => 放电补足负载；
 *       过充/过放保护；充放效率。
 *
 * 时间积分更新 SOC（SOC 变化 = (charge - discharge)Δt / capacity 单位换算）。
 */
class Battery {
  constructor({ nominalWh = 9600, soc0 = 0.6, chgEff = 0.95, dischgEff = 0.95,
                minSoc = 0.15, maxSoc = 0.95 } = {}) {
    this.nominalWh = nominalWh;
    this.soc = soc0;
    this.chgEff = chgEff;
    this.dischgEff = dischgEff;
    this.minSoc = minSoc;
    this.maxSoc = maxSoc;
    this.maxPowerW = nominalWh * 0.5; // 0.5C 限制
  }

  /**
   * @param {number} pvW 光伏功率
   * @param {number} loadW 负载
   * @param {number} dtS 步长秒
   * @returns {{chargeW,dischargeW,soc,gridImportW,gridExportW,selfUseW}}
   */
  step(pvW, loadW, dtS) {
    let chargeW = 0, dischargeW = 0, gridImportW = 0, gridExportW = 0;
    const surplus = pvW - loadW; // 光伏富余(+) 或缺口(-)

    if (surplus >= 0) {
      // 富余：当 SOC<max 优先充电池；剩余上网
      const chgSpaceW = Math.min(this.surplusToW(), this.maxPowerW);
      chargeW = Math.min(surplus, chgSpaceW);
      const exportW = surplus - chargeW;
      gridExportW = exportW;
    } else {
      // 缺口：电池放电补足（到下限），仍缺则电网买电
      const need = -surplus;
      dischargeW = Math.min(need, this.deficitToW(), this.maxPowerW * this.dischgEff);
      gridImportW = need - dischargeW;
    }

    // SOC 更新（能量积分）
    const netWh = (chargeW * this.chgEff - dischargeW / this.dischgEff) * (dtS / 3600);
    this.soc = Math.max(this.minSoc, Math.min(this.maxSoc, this.soc + netWh / this.nominalWh));

    return {
      chargeW, dischargeW, soc: this.soc,
      gridImportW, gridExportW,
      voltageV: 48 + (this.soc - 0.5) * 4,
    };
  }

  surplusToW() { return (this.maxSoc - this.soc) * this.nominalWh / 1; } // 满充所需 Wh（1h 内可充）
  deficitToW() { return (this.soc - this.minSoc) * this.nominalWh / 1; }
}

function createBattery(opts) { return new Battery(opts); }

module.exports = { Battery, createBattery };
