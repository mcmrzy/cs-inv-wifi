'use strict';
/*
 * store.js — 认领关系 / 设备 / 遥测快照 持久化
 *
 * 默认实现为 JSON 文件（data/store.json），无需原生编译依赖，
 * 便于 Docker 部署；对上层暴露与存储后端无关的接口，可平滑替换为
 * SQLite / Postgres（见 README §9）。
 */
const fs = require('fs');
const path = require('path');

let dataDir = process.env.DATA_DIR || path.join(__dirname, '..', 'data');
let file = path.join(dataDir, 'store.json');

/* 内存态 */
const state = {
  devices: {},    // sn -> {sn, pin, qrcode, rated_w, city_code, claimed, claimed_by, claimed_at}
  cities: {},     // city_code -> {code, name, lat, lng, claimed, sn}
  telemetry: {},  // sn -> {ts, data} 最近快照
};

let saveTimer = null;
const SAVE_DEBOUNCE_MS = 500;

function ensureDir() {
  if (!fs.existsSync(dataDir)) fs.mkdirSync(dataDir, { recursive: true });
}

function load() {
  ensureDir();
  if (!fs.existsSync(file)) return;
  try {
    const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
    if (raw.devices) Object.assign(state.devices, raw.devices);
    if (raw.cities) Object.assign(state.cities, raw.cities);
    if (raw.telemetry) Object.assign(state.telemetry, raw.telemetry);
  } catch (e) {
    console.error('[store] load failed:', e.message);
  }
}

function persist() {
  if (saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = null;
    try {
      ensureDir();
      fs.writeFileSync(file, JSON.stringify(state, null, 2));
    } catch (e) {
      console.error('[store] persist failed:', e.message);
    }
  }, SAVE_DEBOUNCE_MS);
}

/* ---------- 城市 ---------- */
function updateCity(city) {
  state.cities[city.code] = city;
  persist();
}

function getCity(code) {
  return state.cities[code] || null;
}

function allCities() {
  return Object.values(state.cities);
}

/* ---------- 设备 ---------- */
function upsertDevice(dev) {
  state.devices[dev.sn] = dev;
  persist();
}

function getDevice(sn) {
  return state.devices[sn] || null;
}

function allDevices() {
  return Object.values(state.devices);
}

function claimedDevices() {
  return Object.values(state.devices).filter((d) => d.claimed);
}

/* ---------- 遥测 ---------- */
function setTelemetry(sn, ts, data) {
  state.telemetry[sn] = { ts, data };
  persist();
}

function getTelemetry(sn) {
  return state.telemetry[sn] || null;
}

module.exports = {
  load,
  updateCity,
  getCity,
  allCities,
  upsertDevice,
  getDevice,
  allDevices,
  claimedDevices,
  setTelemetry,
  getTelemetry,
  dataDir,
};
