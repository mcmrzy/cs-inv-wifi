'use strict';
/*
 * geo.js — 中国地级市/地区坐标 + 时区 + 气候基线
 *
 * 数据来自 tools/fetch_cities.py（阿里 DataV GeoAtlas，离线拉取）
 * 存于   geo-cities.json（337 个省级/市级行政区坐标）。
 * 这里负责：加载城市表 + 补气候基线；供地图数据源与模拟引擎使用。
 */
const fs = require('fs');
const path = require('path');

// 气候基线要素（粗粒度，供内置模拟用）
const CLIMATE_PRESETS = {
  north:     { tempC: 11, humidity: 55, rainP: 0.18, cloudBase: 0.42, irradScale: 1.12 },
  northeast: { tempC:  6, humidity: 58, rainP: 0.20, cloudBase: 0.45, irradScale: 1.05 },
  northwest: { tempC:  9, humidity: 40, rainP: 0.10, cloudBase: 0.30, irradScale: 1.20 },
  central:   { tempC: 16, humidity: 68, rainP: 0.32, cloudBase: 0.55, irradScale: 0.92 },
  east:      { tempC: 16, humidity: 70, rainP: 0.34, cloudBase: 0.58, irradScale: 0.94 },
  south:     { tempC: 20, humidity: 75, rainP: 0.36, cloudBase: 0.62, irradScale: 0.86 },
  southwest: { tempC: 15, humidity: 62, rainP: 0.30, cloudBase: 0.55, irradScale: 0.90 },
  tibet:     { tempC:  5, humidity: 35, rainP: 0.08, cloudBase: 0.25, irradScale: 1.30 },
};

function provinceClimate(prov){
  if (/新疆|青海|西藏/.test(prov)) return 'northwest';
  if (/黑龙江|吉林|辽宁|内蒙古/.test(prov)) return 'northeast';
  if (/广东|广西|海南|福建|江西|湖南/.test(prov)) return 'south';
  if (/四川|贵州|云南|重庆/.test(prov)) return 'southwest';
  if (/河南|湖北|安徽/.test(prov)) return 'central';
  if (/上海|江苏|浙江/.test(prov)) return 'east';
  return 'north';
}

function loadCities() {
  const file = path.join(__dirname, 'geo-cities.json');
  let raw = [];
  try {
    raw = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (e) {
    console.error('[geo] load geo-cities.json failed:', e.message);
  }
  return raw.map((c) => ({
    code: c.code,
    name: c.name,
    province: c.province,
    lat: c.lat,
    lng: c.lng,
    tzOffset: c.tzOffset || 8,
    climate: c.climate ? CLIMATE_PRESETS[c.climate] || { tempC: 16, humidity: 65, rainP: 0.3, cloudBase: 0.5, irradScale: 1 }
                       : CLIMATE_PRESETS[provinceClimate(c.province)],
  }));
}

const cities = loadCities();
const byCode = new Map(cities.map((c) => [c.code, c]));

function getCity(code) {
  return byCode.get(String(code)) || null;
}

// （兼容旧引用）按名称查找首个
function findByName(name) {
  return cities.find((c) => c.name === name) || null;
}

module.exports = { cities, getCity, findByName, CLIMATE_PRESETS };
