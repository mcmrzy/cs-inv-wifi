'use strict';
/*
 * routes/cities.js — 地图画点数据源
 */
const express = require('express');
const router = express.Router();
const geo = require('../geo');
const store = require('../store');

/** GET /api/cities — 全部地级市 + 认领状态 */
router.get('/', (req, res) => {
  const out = geo.cities.map((c) => {
    const s = store.getCity(c.code);
    return {
      code: c.code, name: c.name, province: c.province,
      lat: c.lat, lng: c.lng,
      claimed: !!(s && s.claimed),
      sn: s && s.claimed ? s.sn : null,
    };
  });
  res.json(out);
});

/** GET /api/cities/geo — 提供地级市坐标归档（前端加载点） */
router.get('/geo', (req, res) => {
  res.json(geo.cities);
});

module.exports = router;
