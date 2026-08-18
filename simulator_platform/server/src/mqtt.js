'use strict';
/*
 * mqtt.js — 每设备一个 MQTT 客户端（clientID=SN，唯一），对齐固件协议
 *
 * - broker: mqtts://mqtt.jiuxiaoyw.online:8883（跳过证书校验，对齐固件）
 * - 凭据：username/password 来自环境变量（对齐固件 CONFIG_CS_INV_MQTT_*）
 * - topic 前缀：cs_inv/{sn}/...
 * - envelope: {"t":<unix秒>,"v":2,"data":<body>}
 * - heartbeat: 直接发心跳体（无额外envelope），因为mqtt_publish已包envelope
 * - 上报 heartbeat/info/config/status；订阅 cmd，回报 cmd/response
 * - 仅被认领设备建立连接（按需）
 */
const mqtt = require('mqtt');
const store = require('./store');
const simulation = require('./simulation');

const HOST = process.env.MQTT_HOST || 'mqtt.jiuxiaoyw.online';
const PORT = process.env.MQTT_PORT || 8883;
const USER = process.env.MQTT_USERNAME || 'CSKJ-INV-DEVICE-6K2';
const PASS = process.env.MQTT_PASSWORD || 'CSKJINVDEVICE6K2';
const TLS = process.env.MQTT_TLS !== '0';

const ProtocolTopics = {
  HEARTBEAT: 'heartbeat',
  INFO:      'info',
  CONFIG:    'config',
  STATUS:    'status',
  CMD:       'cmd',
  CMD_RESP:  'cmd/response',
  ONLINE:    'online',
};

const clients = new Map(); // sn -> client

function fullTopic(sn, sub) { return 'cs_inv/' + sn + '/' + sub; }

/**
 * 发布到 MQTT（自动包 envelope: {"t":unix_sec,"v":2,"data":...}）
 * heartbeat 用 mqtt_publish 已包 envelope，所以这里直接发心跳体
 */
function publish(sn, topic, body, opts) {
  opts = opts || {};
  const c = clients.get(sn);
  if (!c || !c.connected) return false;
  const t = fullTopic(sn, topic);
  // heartbeat 已经是完整的心跳体（含envelope），直接发
  // info/config/cmd_resp 也直接发（都是完整信封）
  const payload = typeof body === 'string' ? body : JSON.stringify(body);
  c.publish(t, payload, { qos: opts.qos || 1, retain: !!opts.retain });
  // Log a preview of the payload (truncated for readability)
  var preview = payload.length > 200 ? payload.slice(0, 200) + '...' : payload;
  console.log('[mqtt] ' + sn + ' -> ' + topic + ' (' + payload.length + ' bytes) ' + preview);
  return true;
}

/**
 * 连接一台设备（对齐固件 mqtt_handler 行为）
 */
function connectDevice(dev) {
  const sn = dev.sn;
  if (clients.has(sn)) return clients.get(sn);

  const url = (TLS ? 'mqtts' : 'mqtt') + '://' + HOST + ':' + PORT;
  const client = mqtt.connect(url, {
    clientId:          sn,
    username:          USER,
    password:          PASS,
    rejectUnauthorized: false,
    keepalive:         60,
    reconnectPeriod:   5000,
    clean:             false,
    // LWT: 设备离线时发离线状态（对齐固件 mqtt_handler LWT）
    will: {
      topic:   fullTopic(sn, ProtocolTopics.STATUS),
      payload: JSON.stringify({ t: Math.floor(Date.now() / 1000), v: 2,
                               data: { online: false } }),
      qos:    1,
      retain: true,
    },
  });

  client.on('connect', () => {
    console.log('[mqtt] ' + sn + ' connected');
    // 立即上报 info + 在线状态 + heartbeat（对齐固件 telemetry_on_mqtt_connected）
    publishInfo(sn, store.getDevice(sn) || { sn: sn, city_name: 'unknown' });
    publishOnline(sn);
    // 触发一次即时 heartbeat
    var simInst = simulation.getSim(sn);
    if (simInst && simInst.lastSnap) {
      var snap = simInst.lastSnap;
      var hbBody = {
        t: snap.t || Math.floor(Date.now() / 1000),
        sys: snap.sys, pv: snap.pv, ac: snap.ac,
        chr: snap.chr, bat: snap.bat, eng: snap.eng,
        fan: snap.fan, diag: snap.diag, sock: snap.sock,
      };
      publishHeartbeat(sn, hbBody);
    }
    // 订阅下行命令
    client.subscribe(fullTopic(sn, ProtocolTopics.CMD), { qos: 1 }, (e) => {
      if (e) console.error('[mqtt] ' + sn + ' sub cmd err', e.message);
    });
  });
  client.on('close',    () => console.log('[mqtt] ' + sn + ' closed'));
  client.on('error',   (e) => console.warn('[mqtt] ' + sn + ' err:', e.message));
  client.on('message', (topic, payload) => handleDownlink(sn, topic, payload.toString()));

  clients.set(sn, client);
  return client;
}

function disconnectDevice(sn) {
  const c = clients.get(sn);
  if (c) { c.end(true); clients.delete(sn); }
}

/**
 * 发布 heartbeat
 * hbBody = { sys:[...], pv:[...], ac:[...], chr:[...], bat:[...],
 *            eng:[...], fan:[...], diag:[...], sock:[...], t:unix_sec }
 * 对齐固件 mqtt_publish: 自动包 {"t":unix_sec,"v":2,"data":<body>}
 */
function publishHeartbeat(sn, hbBody) {
  // envelope 已有 t，data 内部不要再塞 t（避免 {"data":{"t":...}} 双重 t）
  var body = Object.assign({}, hbBody);
  delete body.t;
  const snap = {
    t: hbBody.t || Math.floor(Date.now() / 1000),
    v: 2,
    data: body,
  };
  return publish(sn, ProtocolTopics.HEARTBEAT, snap);
}

/**
 * 发布 info（固件 telemetry.c publish_info 格式）
 * info 已经是含envelope的完整body
 */
function publishInfo(sn, dev) {
  const infoBody = {
    sn: dev.sn,
    model: 'CS-L10-6K2',
    manufacturer: '辰烁科技',
    firmware_arm: 'V1.0',
    firmware_esp: 'V1.0.sim',
    device_type: 'off_grid_inverter',
    phase: 'single',
    rated_power: 6200,
    rated_voltage: '230.0',
    rated_frequency: '50.00',
    battery_nominal_voltage: '51.2',
    battery_type: 'LiFePO4',
    city: dev.city_name,
  };
  const env = JSON.stringify({ t: Math.floor(Date.now() / 1000), v: 2, data: infoBody });
  return publish(sn, ProtocolTopics.INFO, env);
}

/**
 * 发布 config
 */
function publishConfig(sn) {
  const cfg = { rev: 0, params: {} };
  return publish(sn, ProtocolTopics.CONFIG, JSON.stringify({ t: Math.floor(Date.now() / 1000), v: 2, data: cfg }));
}

/**
 * 发布在线状态（90s一次，对齐固件 publish_online）
 * payload: {"t":unix_sec,"v":2,"data":{"uptime":秒数}}
 */
function publishOnline(sn) {
  const env = JSON.stringify({
    t: Math.floor(Date.now() / 1000),
    v: 2,
    data: { uptime: Math.floor(process.uptime()) },
  });
  return publish(sn, ProtocolTopics.ONLINE, env, { qos: 0 });
}

/**
 * 处理下行命令（对齐固件 cmd_handler）
 */
function handleDownlink(sn, topic, payload) {
  let j;
  try { j = JSON.parse(payload); } catch { return; }
  const cmd = j.cmd || j.name || j.action;
  const value = j.value != null ? j.value
                   : (j.args && j.args.value != null ? j.args.value : null);
  const taskId = j.task_id || '';

  const sim = simulation.getSim(sn);
  let resp;
  if (sim) {
    const r = sim.applyCmd(cmd, value);
    resp = { task_id: taskId, cmd, result: r.result, err: 0,
             applied_value: r.applied_value, revision: r.revision };
    console.log('[mqtt] ' + sn + ' cmd=' + cmd + ' value=' + value + ' -> result=' + r.result);
  } else {
    resp = { task_id: taskId, cmd, result: 'OK', err: 0, applied_args: [value] };
  }
  const env = JSON.stringify({ t: Math.floor(Date.now() / 1000), v: 2, data: resp });
  publish(sn, ProtocolTopics.CMD_RESP, env, { qos: 1 });
}

/** 连接所有已认领设备（启动/认领时调用） */
function attachClaimed() {
  for (const dev of store.claimedDevices()) connectDevice(dev);
}
function attachDevice(dev) { return connectDevice(dev); }

/** 模拟引擎心跳回调 -> 发布 */
function onSimHeartbeat(sn, snap) {
  publishHeartbeat(sn, snap);
}

/** 定时发在线状态（90s，对齐固件） */
let onlineInterval = null;
function startOnlineReports(sn) {
  publishOnline(sn);
  if (!onlineInterval) {
    onlineInterval = setInterval(() => {
      for (const [s, client] of clients) {
        if (client.connected) publishOnline(s);
      }
    }, 90 * 1000);
  }
}

function shutDown() {
  if (onlineInterval) { clearInterval(onlineInterval); onlineInterval = null; }
  for (const [sn, c] of clients) { try { c.end(true); } catch {} }
  clients.clear();
}

module.exports = {
  attachClaimed,
  attachDevice,
  disconnectDevice,
  onSimHeartbeat,
  publishInfo,
  publishConfig,
  publishOnline,
  shutDown,
  clients,
};
