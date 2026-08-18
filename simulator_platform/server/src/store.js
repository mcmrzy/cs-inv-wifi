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

    // 迁移：二维码内容统一为 "SN:<sn> PIN:<pin>"（App parseQRCode 免弹窗绑定格式），
    // 旧版可能是纯 "SN:<sn>" 或 JSON 对象字符串
    let migrated = false;
    for (const dev of Object.values(state.devices)) {
      if (dev && dev.qrcode !== `SN:${dev.sn} PIN:${dev.pin}`) {
        dev.qrcode = `SN:${dev.sn} PIN:${dev.pin}`;
        migrated = true;
      }
    }
    if (migrated) persist();

    // 国家码迁移：NC → CN（App 白名单），需在 qrcode 迁移后执行
    migrateCountryNC();
  } catch (e) {
    console.error('[store] load failed:', e.message);
  }
}

// 国家码迁移：旧版默认国家码 NC（内测自造码，不在手机 App ISO 3166 白名单，
// 扫码会报 "SN 号格式不正确"）→ CN。H1NC 前缀的老 SN 原地替换国家码并重算校验位，
// 保持设备/城市绑定/遥测不变。
function migrateCountryNC() {
  const factory = require('./device-factory');
  let changed = false;
  for (const dev of Object.values(state.devices)) {
    if (dev && dev.sn && /^H1NC/.test(dev.sn) && dev.sn.length === 16) {
      const newSn = 'H1CN' + dev.sn.slice(4, 15) + factory.computeCheckDigit('H1CN' + dev.sn.slice(4, 15));
      if (newSn !== dev.sn) {
        // 先迁移 city 绑定（sn 引用）
        for (const city of Object.values(state.cities)) {
          if (city.sn === dev.sn) city.sn = newSn;
        }
        if (state.telemetry[dev.sn]) {
          state.telemetry[newSn] = state.telemetry[dev.sn];
          delete state.telemetry[dev.sn];
        }
        delete state.devices[dev.sn];
        dev.sn = newSn;
        dev.qrcode = `SN:${newSn} PIN:${dev.pin}`;
        if (dev.country === 'NC') dev.country = 'CN';
        state.devices[newSn] = dev;
        changed = true;
        console.log(`[store] migrate SN ${dev.sn} → ${newSn}`);
      }
    }
  }
  if (changed) persist();
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
