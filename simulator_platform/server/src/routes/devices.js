'use strict';
/*
 * routes/devices.js — 设备信息 + 遥测快照 + 状态/控制/告警/命令日志
 */
const express = require('express');
const router = express.Router();
const store = require('../store');
const simulation = require('../simulation');
const { computePin } = require('../device-factory');

/** GET /api/devices/:sn */
router.get('/:sn', (req, res) => {
  const dev = store.getDevice(req.params.sn);
  if (!dev) return res.status(404).json({ error: 'device:not_found' });
  res.json({ ...dev, pin: computePin(dev.sn) });
});

/** GET /api/devices/:sn/telemetry — 最近快照（场景页轮询） */
router.get('/:sn/telemetry', (req, res) => {
  const t = store.getTelemetry(req.params.sn);
  if (!t) return res.status(404).json({ error: 'device:no_telemetry' });
  res.json(t);
});

/** GET /api/devices/:sn/status — 完整状态（电站详情页） */
router.get('/:sn/status', (req, res) => {
  const dev = store.getDevice(req.params.sn);
  if (!dev) return res.status(404).json({ error: 'device:not_found' });
  const sim = simulation.getSim(req.params.sn);
  if (!sim) return res.status(404).json({ error: 'device:not_simulating' });
  res.json(sim.getStatus());
});

/** GET /api/devices/:sn/alarms — 告警历史 */
router.get('/:sn/alarms', (req, res) => {
  const dev = store.getDevice(req.params.sn);
  if (!dev) return res.status(404).json({ error: 'device:not_found' });
  const sim = simulation.getSim(req.params.sn);
  res.json({ alarms: sim ? sim._alarms : [] });
});

/** GET /api/devices/:sn/cmdlog — 命令响应日志 */
router.get('/:sn/cmdlog', (req, res) => {
  const dev = store.getDevice(req.params.sn);
  if (!dev) return res.status(404).json({ error: 'device:not_found' });
  const sim = simulation.getSim(req.params.sn);
  res.json({ log: sim ? sim._cmdLog.slice(0, 20) : [] });
});

/** POST /api/devices/:sn/control — 控制命令 */
router.post('/:sn/control', (req, res) => {
  const dev = store.getDevice(req.params.sn);
  if (!dev) return res.status(404).json({ error: 'device:not_found' });
  const { action, args } = req.body || {};
  if (!action) return res.status(400).json({ error: 'missing action' });

  // 尝试从 MQTT 路由，同步写模拟器状态
  const sim = simulation.getSim(req.params.sn);
  if (!sim) {
    return res.status(404).json({ error: 'device:not_simulating' });
  }

  const value = args && args.value != null ? args.value
    : args && args[0] != null ? args[0]
    : (args && typeof args === 'object' ? null : args);

  const r = sim.applyCmd(action, value);
  console.log(`[api] ${req.params.sn} control action=${action} value=${value} → ${JSON.stringify(r)}`);

  res.json({
    sn: req.params.sn,
    action,
    args: value != null ? value : null,
    result: r.result,
    applied_value: r.applied_value,
    revision: r.revision,
    ts: Date.now(),
  });
});

module.exports = router;
