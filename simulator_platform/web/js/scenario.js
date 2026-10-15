'use strict';
/*
 * scenario.js — 别墅电站场景页（3D 房子 + 天气面板）
 * 读取 /api/devices/:sn + /api/devices/:sn/telemetry，轮询刷新。
 */
(function () {
  const params = new URLSearchParams(location.search);
  const sn = params.get('sn') || '';
  const $ = (id) => document.getElementById(id);

  function fmtW(w) {
    w = Math.max(0, Math.round(w || 0));
    return w >= 1000 ? (w / 1000).toFixed(2) + ' kW' : w + ' W';
  }

  async function loadDevice() {
    if (!sn) { $('s-title').textContent = '未指定设备'; return; }
    try {
      const res = await fetch('/api/devices/' + sn);
      if (!res.ok) { $('s-title').textContent = '设备不存在'; return; }
      const dev = await res.json();
      document.querySelector('.s-title').textContent = (dev.city_name || '') + ' 别墅电站';
      $('s-sn').textContent = 'SN ' + dev.sn;
    } catch (e) { /* 忽略 */ }
  }

  // 天气文本/图标（据云量/白天）
  function weatherLabel(m) {
    const cloud = m.cloudCover != null ? m.cloudCover : 0;
    const night = m.isDay === false;
    if (night) return { icon: '🌙', txt: '晴夜' };
    if (cloud < 40) return { icon: '☀️', txt: '晴' };
    if (cloud < 75) return { icon: '⛅', txt: '多云' };
    return { icon: '☁️', txt: '阴' };
  }

  function fmtHhmm(s) {
    if (!s) return '—';
    const d = new Date(s);
    if (isNaN(d)) return s;
    return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
  }

  function render(t) {
    const m = t.data || {};
    const pvW = m.pvW || 0;
    const loadW = m.loadW || 0;
    const soc = m.soc || 0;
    const cloud = m.cloudCover ?? 0;
    const irr = m.irradiance ?? 0;
    const night = m.isDay === false;
    const surplus = pvW - loadW;

    // KPI
    $('k-pv').textContent = fmtW(pvW);
    $('k-load').textContent = fmtW(loadW);
    $('k-soc').textContent = soc + '%';
    // 逆变器输出：白天≈供电 或 夜间电池放电兜底负载
    $('k-out').textContent = fmtW(Math.max(pvW, night ? loadW : 0));

    // 3D 房子标签
    $('l-pv').textContent = '☀ ' + fmtW(pvW);
    $('l-inv').textContent = '⚡ ' + fmtW(pvW || (night ? loadW : 0));
    $('l-bat').textContent = (surplus >= 0 ? '🔋 充 ' : '🔋 放 ') + fmtW(Math.abs(surplus));
    $('l-load').textContent = '🏠 ' + fmtW(loadW);
    // 电池条
    $('batpct').textContent = soc + '%';
    $('batfill').style.width = soc + '%';

    // 白天/夜间
    const stage = document.getElementById('stage');
    stage.classList.toggle('night', night);

    // 天气面板
    const wl = weatherLabel(m);
    $('wx_ic').textContent = wl.icon;
    $('wx_txt').textContent = wl.txt;
    $('wx_temp').textContent = (m.tempC != null ? m.tempC + '°C' : '—');
    $('wx_cloud').textContent = cloud + '%';
    $('wx_hum').textContent = (m.humidity != null ? m.humidity + '%' : '—');
    $('wx_irr').textContent = Math.round(irr) + ' W/m²';
    $('wx_rise').textContent = fmtHhmm(m.sunrise);
    $('wx_set').textContent = fmtHhmm(m.sunset);
    $('amb').textContent =
      (night ? '夜间' : '白天') + ' · 云量 ' + cloud + '% · 辐照 ' + Math.round(irr) + ' W/m²' +
      (m.sunrise ? ' · 日出 ' + fmtHhmm(m.sunrise) : '') +
      (m.sunset ? ' 日落 ' + fmtHhmm(m.sunset) : '');
  }

  async function tick() {
    if (!sn) return;
    try {
      const res = await fetch('/api/devices/' + sn + '/telemetry');
      if (!res.ok) return;
      const t = await res.json();
      render(t);
      $('s-updated').textContent = t.ts ? '· 更新于 ' + new Date(t.ts * 1000).toLocaleTimeString('zh-CN', { hour12: false }) : '';
    } catch (e) { /* 静默 */ }
  }

  loadDevice();
  tick();
  setInterval(tick, 5000);
})();
