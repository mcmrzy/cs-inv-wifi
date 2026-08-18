'use strict';
/*
 * routes/claims.js — 认领管理（免登录开放认领）
 */
const express = require('express');
const router = express.Router();
const store = require('../store');
const geo = require('../geo');
const { randomSN, computePin, qrcodePayload, createDevice } = require('../device-factory');
const mqtt = require('../mqtt');
const simulation = require('../simulation');

/** POST /api/claims  { city_code, browser_id } */
router.post('/', async (req, res) => {
  const cityCode = String(req.body?.city_code || '');
  const browserId = String(req.body?.browser_id || 'anon');

  const city = geo.getCity(cityCode);
  if (!city) return res.status(400).json({ error: 'geo:unknown_city' });

  const existing = store.getCity(cityCode);
  if (existing && existing.claimed && store.getDevice(existing.sn)) {
    // 已认领：返回既有设备（幂等）
    const dev = store.getDevice(existing.sn);
    return res.json({ claimed: true, device: dev });
  }

  const dev = createDevice({ city_code: city.code });
  dev.claimed = true;
  dev.claimed_by = browserId;
  dev.claimed_at = Math.floor(Date.now() / 1000);
  // 保证唯一（createDevice 已分配 SN，防并发重复）
  while (store.getDevice(dev.sn)) {
    const sn2 = randomSN();
    dev.sn = sn2;
    dev.pin = computePin(sn2);
  }
  dev.qrcode = qrcodePayload(dev);
  store.upsertDevice(dev);
  store.updateCity({ ...city, claimed: true, sn: dev.sn });

  // 启动该设备模拟 + MQTT；先取真实天气，使首个快照即带实时数据
  const sim = simulation.ensureDevice(dev);
  sim.onHeartbeat = mqtt.onSimHeartbeat;
  try { await sim._refreshRealWeather(); } catch (e) { /* 天气失败不阻塞认领 */ }
  try { sim.step(); } catch (e) { console.error('[claims] first step err', e && e.message); } // 立即产一条遥测
  mqtt.attachDevice(dev);
  mqtt.publishInfo(dev.sn, dev);
  mqtt.publishConfig(dev.sn);

  return res.status(201).json({ claimed: true, device: dev });
});

/** GET /api/claims/:city — 查询认领状态（免登录重进场景用） */
router.get('/:city', (req, res) => {
  const city = store.getCity(req.params.city);
  if (!city || !city.claimed || !store.getDevice(city.sn)) {
    return res.json({ claimed: false, city_code: req.params.city });
  }
  const dev = store.getDevice(city.sn);
  return res.json({ claimed: true, device: dev });
});

module.exports = router;
