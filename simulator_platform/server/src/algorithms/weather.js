'use strict';
/*
 * algorithms/weather.js — 城市天气/太阳模型（驱动源）
 *
 * 数据来源（双模式）：
 *  1) 真实天气（weather-provider，Open-Meteo）：
 *     - 辐照直接取实测 shortwave_radiation W/m²（已含云量影响）
 *     - 日出日落 daily.sunrise/sunset 定义白天区间
 *     - 云量 / 温度 / 湿度取实测
 *  2) 内置回退（无网络 / 缓存过期）：
 *     - 太阳高度角（本地时）推晴空辐照，云量马尔可夫折减
 *
 * 输出：{ tempC, humidity, cloudCover, irradianceWm2, isDay, fromReal }
 */

const SOLAR_CONSTANT = 1367;

function dayOfYear(now) {
  const start = Date.UTC(now.getUTCFullYear(), 0, 0);
  return Math.floor((Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()) - start) / 86400000);
}
function solarDeclination(doy) {
  return 23.44 * Math.sin((2 * Math.PI / 365) * (doy - 81));
}

/** 内置回退：按本地太阳时（tz 偏移）算晴空辐照 */
function geometryIrradiance(latDeg, now, tzOffset, cloudCover, irradScale) {
  const localH = (now.getUTCHours() + now.getUTCMinutes() / 60 + (tzOffset || 8) + 24) % 24;
  const doy = dayOfYear(now);
  const decRad = (solarDeclination(doy) * Math.PI) / 180;
  const ha = ((localH - 12) * 15 * Math.PI) / 180;
  const lat = (latDeg * Math.PI) / 180;
  const sinAlt = Math.sin(lat) * Math.sin(decRad) + Math.cos(lat) * Math.cos(decRad) * Math.cos(ha);
  if (sinAlt <= 0) return 0;
  const alt = (Math.asin(Math.min(1, Math.max(-1, sinAlt))) * 180) / Math.PI;
  const altC = Math.max(alt, 1.0);
  const am = Math.max(1.0, 1.0 / (sinAlt + 0.50572 * Math.pow(6.07995 + altC, -1.6364)));
  let ghi = SOLAR_CONSTANT * 0.7 * Math.pow(0.72, am) * sinAlt * (irradScale || 1);
  ghi *= 1 - (cloudCover || 0) * 0.92;
  return Math.max(0, Math.min(1100, ghi));
}

/** 判断本地时刻是否在日出-日落之间（真实 daily） */
function isDayFromSun(now, real) {
  if (!real || !real.sunrise || !real.sunset) return null; // 未知
  try {
    const rise = new Date(real.sunrise).getTime();
    const set = new Date(real.sunset).getTime();
    const t = now.getTime();
    return t >= rise && t <= set;
  } catch { return null; }
}

/** 云量马尔可夫过程（仅离线回退用） */
class CloudModel {
  constructor(climate) { this.climate = climate; this.state = Math.random() < climate.cloudBase ? 'cloudy' : 'clear'; this.remainMin = this._next(); this.cloudCover = climate.cloudBase; }
  _next() { return this.state === 'clear' ? 10 + Math.random() * 80 : 5 + Math.random() * 35; }
  step() {
    this.remainMin -= 1;
    if (this.remainMin <= 0) {
      const r = Math.random();
      if (this.state === 'clear') this.state = r < this.climate.rainP ? 'cloudy' : 'clear';
      else this.state = r < this.climate.rainP ? 'rain' : 'clear';
      this.remainMin = this._next();
    }
    const b = this.climate.cloudBase;
    if (this.state === 'clear') this.cloudCover = b * (0.2 + Math.random() * 0.2);
    else if (this.state === 'cloudy') this.cloudCover = 0.55 + Math.random() * 0.35;
    else this.cloudCover = 0.92 + Math.random() * 0.08;
    return this.cloudCover;
  }
  get current() { return this.cloudCover; }
}

function createWeatherFor(city, provider) {
  const climate = city.climate || {};
  const tz = city.tzOffset || 8;
  const cloud = new CloudModel(climate);
  return {
    step() { return cloud.step(); },
    /** 采样天气。real 来自真实天气 provider（可为 null） */
    sample(now, real) {
      // 云量 / 温度 / 湿度：真实优先，离线回退
      const cc = (real && typeof real.cloudCover === 'number') ? real.cloudCover : cloud.current;
      const tempC = (real && typeof real.tempC === 'number') ? real.tempC
                      : climate.tempC + Math.sin(((now.getUTCHours() + tz - 8) / 24) * 2 * Math.PI) * 6 - cc * 5;
      const hum = (real && typeof real.humidity === 'number') ? real.humidity
                      : Math.min(100, climate.humidity + (cc - climate.cloudBase) * 40);

      // 是否白天
      let isDay = (real && typeof real.isDay === 'boolean') ? real.isDay : null;
      if (isDay === null) isDay = isDayFromSun(now, real);
      if (isDay === null) isDay = geometryIrradiance(city.lat, now, tz, cc, climate.irradScale) > 5;

      // 辐照：真实实测优先；数据源已现实测（含云量）——若非白天钳 0
      let irrad;
      if (real && typeof real.irradianceWm2 === 'number' && (real.source === 'open-meteo')) {
        irrad = isDay ? real.irradianceWm2 : 0;
      } else {
        irrad = geometryIrradiance(city.lat, now, tz, cc, climate.irradScale);
      }

      return {
        tempC, humidity: hum, cloudCover: cc, isDay,
        irradianceWm2: Math.max(0, Math.min(1100, irrad)),
        fromReal: !!(real && typeof real.tempC === 'number'),
      };
    },
  };
}

module.exports = { createWeatherFor, geometryIrradiance, solarDeclination, dayOfYear, SOLAR_CONSTANT };
