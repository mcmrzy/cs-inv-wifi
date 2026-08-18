'use strict';
/*
 * weather-provider.js — 真实天气源（双模式）
 *
 * 默认使用 Open-Meteo（免费、无需 API key）按经纬度拉取：
 *  current:  shortwave_radiation(实测辐照 W/m²), is_day, cloud_cover(%),
 *            temperature_2m(°C), relative_humidity_2m(%)
 *  daily:    sunrise, sunset（当天日出日落，定义白天区间）
 *
 * 以 缓存 1 小时 的低频拉取（daily 当天有效），避免频繁打 API。
 * 无网络 / 失败时返回 null，由 weather.c 用内置太阳公式 + 气候基线回退续算。
 */
const https = require('https');

const API_BASE = 'https://api.open-meteo.com/v1/forecast';
const CACHE_TTL_MS = 60 * 60 * 1000; // 1h

class WeatherProvider {
  constructor({ ttlMs = CACHE_TTL_MS } = {}) {
    this.ttl = ttlMs;
    this.cache = new Map(); // "lat,lng" -> {ts, data}
    this.lastError = null;
  }

  _key(lat, lng) { return `${lat},${lng}`; }

  getCached(lat, lng) {
    const k = this._key(lat, lng);
    const e = this.cache.get(k);
    if (!e) return null;
    if (Date.now() - e.ts > this.ttl) { this.cache.delete(k); return null; }
    return e.data;
  }

  /** 拉取并缓存（失败返回 null，不影响调用方） */
  async fetch(lat, lng) {
    const url = `${API_BASE}?latitude=${lat}&longitude=${lng}` +
      `&current=cloud_cover,temperature_2m,relative_humidity_2m,shortwave_radiation,is_day` +
      `&daily=sunrise,sunset&timezone=auto&forecast_days=2`;
    try {
      const data = await this._httpGet(url);
      const cur = data && data.current;
      const dl = data && data.daily;
      if (!cur) throw new Error('no current');
      const out = {
        cloudCover: (cur.cloud_cover / 100),   // 0..1
        tempC: cur.temperature_2m,
        humidity: cur.relative_humidity_2m,
        irradianceWm2: cur.shortwave_radiation ?? 0,  // 实测辐照
        isDay: cur.is_day === 1,
        sunrise: dl && dl.sunrise && dl.sunrise[0] ? dl.sunrise[0] : null,
        sunset: dl && dl.sunset && dl.sunset[0] ? dl.sunset[0] : null,
        utcOffset: (data.utc_offset_seconds || 28800) / 3600, // 时区（中 8）
        source: 'open-meteo',
        ts: Date.now(),
      };
      this.cache.set(this._key(lat, lng), { ts: Date.now(), data: out });
      return out;
    } catch (e) {
      this.lastError = e.message;
      return null;
    }
  }

  /** 异步获取：命中缓存直接用，否则拉一次（仅被认领城市按需触发） */
  async ensure(lat, lng) {
    const c = this.getCached(lat, lng);
    if (c) return c;
    await this.fetch(lat, lng);
    return this.getCached(lat, lng);
  }

  _httpGet(url) {
    return new Promise((resolve, reject) => {
      https.get(url, (res) => {
        let body = '';
        res.on('data', (d) => (body += d));
        res.on('end', () => {
          if (res.statusCode !== 200) return reject(new Error('HTTP ' + res.statusCode));
          try { resolve(JSON.parse(body)); } catch (e) { reject(e); }
        });
      }).on('error', reject);
    });
  }
}

module.exports = { WeatherProvider };
