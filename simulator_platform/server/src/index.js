'use strict';
/*
 * index.js — 认领模拟平台入口
 *  http: 静态前端 + REST API
 *  mqtt: 每被认领设备一个客户端上报/订阅
 *  simulation: 每分钟步进被认领设备的发电/负载/电池模拟
 */
require('dotenv').config();
const path = require('path');
const express = require('express');

const store = require('./store');
const mqtt = require('./mqtt');
const simulation = require('./simulation');

const PORT = process.env.PORT || 8080;

async function main() {
  store.load();

  const app = express();
  app.use(express.json());

  // REST API
  app.use('/api/claims', require('./routes/claims'));
  app.use('/api/cities', require('./routes/cities'));
  app.use('/api/devices', require('./routes/devices'));

  // 静态前端（index.js 位于 server/src/ → 前端在 ../web）
  // 静态前端（本地运行: server/src -> ../../web; Docker 扁平布局: /app/web）
  // 四种候选路径取第一个实际存在者，兼容 local 与容器两种布局
  const fs = require('fs');
  const webCandidates = [
    path.join(__dirname, '..', '..', 'web'),   // 本地: D:\...\server\src => D:\...\web
    path.join(__dirname, '..', 'web'),         // Docker: /app/src => /app/web
  ];
  const webDir = webCandidates.find((p) => fs.existsSync(p)) || webCandidates[0];
  // 静态前端：长缓存图片/模型/地图，短缓存 css/js（加速二次访问）
  app.use(express.static(webDir, {
    maxAge: '1h',
    setHeaders(res, filePath) {
      if (/\.(png|jpg|jpeg|gif|webp|svg|geojson|glb|gltf|woff2?)$/i.test(filePath)) {
        res.setHeader('Cache-Control', 'public, max-age=86400');
      } else if (/\.(html|css|js)$/i.test(filePath)) {
        // 页面/样式/脚本一律新鲜送达（靠 URL ?v= 版本号控制更新）
        res.setHeader('Cache-Control', 'no-cache');
      }
    }
  }));
  console.log('[http] static web:', webDir);

  // 健康检查
  app.get('/api/health', (req, res) => res.json({ ok: true, devices: store.claimedDevices().length }));

  // 启动 MQTT + 模拟
  await mqtt.attachClaimed();
  simulation.start(mqtt.onSimHeartbeat);

  // 友好退出
  process.on('SIGINT', () => { mqtt.shutDown(); process.exit(0); });
  process.on('SIGTERM', () => { mqtt.shutDown(); process.exit(0); });

  app.listen(PORT, () => {
    console.log(`[http] simulator platform listening on :${PORT}`);
    console.log(`[mqtt] broker=${process.env.MQTT_HOST}:${process.env.MQTT_PORT} tls=${process.env.MQTT_TLS}`);
    console.log(`[store] dataDir=${store.dataDir}, cities=${store.allCities().length}, claimed=${store.claimedDevices().length}`);
  });
}

main().catch((e) => { console.error('fatal:', e); process.exit(1); });
