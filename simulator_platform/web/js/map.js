'use strict';
/*
 * map.js — 中国地图（离线 GeoJSON + Albers 等积投影 + 科技风）
 */

const DEG = Math.PI / 180;
const NS  = 'http://www.w3.org/2000/svg';

// ── Albers 等积投影（标准纬线 25°/47°，中心经线 105°E）────────────────
// 标准公式：n=(sinφ1+sinφ2)/2, C=cos²φ1+2n·sinφ1, ρ=√(C-2n·sinφ)/n
// 投影输出单位为球面单位，之后统一线性缩放/平移到 viewBox
const LAT1 = 25, LAT2 = 47, LAMBDA0 = 105;
const N = (Math.sin(LAT1*DEG) + Math.sin(LAT2*DEG)) / 2;
const C = Math.cos(LAT1*DEG)**2 + 2*N*Math.sin(LAT1*DEG);

const VIEW_W = 1000, VIEW_H = 820;
// 全局投影范围（首次 drawBase 时计算），用于归一化到 viewBox
let PROJ = null; // {minX,maxX,minY,maxY}

function albersRaw(lng, lat) {
  const phi = DEG * lat;
  const lambda = DEG * (lng - LAMBDA0);
  const rho = Math.sqrt(Math.max(0, C - 2*N*Math.sin(phi))) / N;
  const theta = N * lambda;
  // x 向右，y 向上（内部坐标，y 越小纬度越高）
  const x = rho * Math.sin(theta);
  const y = rho * Math.cos(theta);
  if (!isFinite(x) || !isFinite(y)) return [NaN, NaN];
  return [x, y];
}

// 归一化：把原始投影坐标映射到 viewBox 内（含 padding）
function normalize(p) {
  if (!PROJ) return p;
  const sx = (VIEW_W - 80) / Math.max(1e-9, PROJ.maxX - PROJ.minX);
  const sy = (VIEW_H - 80) / Math.max(1e-9, PROJ.maxY - PROJ.minY);
  const s = Math.min(sx, sy);
  const ox = (VIEW_W - (PROJ.minX + PROJ.maxX) * s) / 2;
  const oy = (VIEW_H - (PROJ.minY + PROJ.maxY) * s) / 2;
  return [p[0] * s + ox, p[1] * s + oy];
}
function project(lng, lat) { return normalize(albersRaw(lng, lat)); }

// SVG helper
const SVG = (name, attrs) => {
  const e = document.createElementNS(NS, name);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  return e;
};

// ── 路径生成 ──────────────────────────────────────────────────────────
function ringPath(ring) {
  var parts = [];
  for (var i = 0; i < ring.length; i++) {
    var pt = ring[i];
    var p = project(pt[0], pt[1]);
    var x = p[0], y = p[1];
    parts.push((i === 0 ? 'M' : 'L') + x.toFixed(2) + ' ' + y.toFixed(2));
  }
  parts.push('Z');
  return parts.join(' ');
}
function geomPath(g) {
  if (g.type === 'Polygon') {
    var segs = [];
    var rings = g.coordinates;
    for (var ri = 0; ri < rings.length; ri++) segs.push(ringPath(rings[ri]));
    return segs.join(' ');
  }
  if (g.type === 'MultiPolygon') {
    var segs2 = [];
    var polys = g.coordinates;
    for (var pi = 0; pi < polys.length; pi++) {
      var polySegs = [];
      for (var ri = 0; ri < polys[pi].length; ri++) polySegs.push(ringPath(polys[pi][ri]));
      segs2.push(polySegs.join(' '));
    }
    return segs2.join(' ');
  }
  return '';
}


// ── 交互状态 ──────────────────────────────────────────────────────────
let svg = null, content = null;
let zoom = 1, panX = 0, panY = 0;
let drag = false, sx = 0, sy = 0, ox = 0, oy = 0;
const MIN_ZOOM = 0.5, MAX_ZOOM = 14;

function applyTransform() {
  if (!content) return;
  content.setAttribute('transform', 'translate(' + panX + ' ' + panY + ') scale(' + zoom + ')');
  // ── 圆点：半径保持基准值（content 的 scale 会让它随缩放自然变大/变小）──
  content.querySelectorAll('[data-base-r]').forEach(el => {
    if (el.getAttribute('data-hovered') === '1') return;
    const base = parseFloat(el.getAttribute('data-base-r'));
    el.setAttribute('r', base.toFixed(2));
  });
  // ── 城市名：zoom 足够大才显示；字号恒定屏幕大小（8px），不随缩放增大 → 不堆叠 ──
  // 屏幕字号 = font-size × zoom → 设 font-size = 8/zoom 保持屏幕上恒定 8px
  const fontSizeScreen = 8; // 屏幕上恒定像素
  const inv = 1 / zoom;
  const showTxt = zoom >= 2.5;
  content.querySelectorAll('.city-name').forEach(t => {
    t.setAttribute('opacity', showTxt ? '1' : '0');
    t.setAttribute('font-size', (fontSizeScreen * inv).toFixed(3));
  });
  if (window.__zoomInfo) window.__zoomInfo(zoom);
}

function zoomAt(cx, cy, factor) {
  if (!svg || !content) return;
  // 把鼠标的屏幕像素坐标映射到 viewBox 用户坐标（1000×820 空间）
  const r = svg.getBoundingClientRect();
  const ux = (cx - r.left) / r.width * VIEW_W;
  const uy = (cy - r.top) / r.height * VIEW_H;
  const old = zoom;
  zoom = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, zoom * factor));
  const k = zoom / old;
  // 保持鼠标下的点不动：P_view = P_content*zoom + pan，缩放前后 P_view 不变
  panX = ux - (ux - panX) * k;
  panY = uy - (uy - panY) * k;
  applyTransform();
}
function resetView() { zoom = 1; panX = 0; panY = 0; applyTransform(); }

// ── 交互事件 ──────────────────────────────────────────────────────────
function setupInteraction() {
  svg.addEventListener('wheel', e => {
    e.preventDefault();
    zoomAt(e.clientX, e.clientY, e.deltaY < 0 ? 1.12 : 0.89);
  }, { passive: false });

  svg.addEventListener('mousedown', e => {
    drag = true; sx = e.clientX; sy = e.clientY; ox = panX; oy = panY;
  });
  window.addEventListener('mousemove', e => {
    if (!drag) return;
    if (!svg) return;
    const r = svg.getBoundingClientRect();
    // 屏幕位移 → 用户坐标位移
    const dx = (e.clientX - sx) / r.width * VIEW_W;
    const dy = (e.clientY - sy) / r.height * VIEW_H;
    panX = ox + dx; panY = oy + dy; applyTransform();
  });
  window.addEventListener('mouseup', () => { drag = false; });
  svg.addEventListener('dblclick', e => { e.preventDefault(); zoomAt(e.clientX, e.clientY, 1.3); });

  let lastTouch = null, pinch = null;
  svg.addEventListener('touchstart', e => {
    e.preventDefault();
    if (e.touches.length === 1) {
      lastTouch = { x: e.touches[0].clientX, y: e.touches[0].clientY, px: panX, py: panY };
    } else if (e.touches.length === 2) {
      lastTouch = null;
      pinch = Math.hypot(e.touches[0].clientX - e.touches[1].clientX, e.touches[0].clientY - e.touches[1].clientY);
    }
  }, { passive: false });
  svg.addEventListener('touchmove', e => {
    e.preventDefault();
    if (e.touches.length === 1 && lastTouch && svg) {
      const rr = svg.getBoundingClientRect();
      const dx = (e.touches[0].clientX - lastTouch.x) / rr.width * VIEW_W;
      const dy = (e.touches[0].clientY - lastTouch.y) / rr.height * VIEW_H;
      panX = lastTouch.px + dx;
      panY = lastTouch.py + dy;
      applyTransform();
    } else if (e.touches.length === 2 && pinch) {
      const d = Math.hypot(e.touches[0].clientX - e.touches[1].clientX, e.touches[0].clientY - e.touches[1].clientY);
      const f = d / pinch;
      if (f > 1.05 || f < 0.95) {
        const mx = (e.touches[0].clientX + e.touches[1].clientX) / 2;
        const my = (e.touches[0].clientY + e.touches[1].clientY) / 2;
        pinch = d;
        zoomAt(mx, my, f);
      }
    }
  }, { passive: false });
  svg.addEventListener('touchend', () => { lastTouch = null; pinch = null; });
}

// ── 底图：省界 + 装饰 ────────────────────────────────────────────────
function drawBase(mapData) {
  if (!svg) svg = document.getElementById('china');
  svg.innerHTML = '';
  svg.setAttribute('viewBox', '0 0 ' + VIEW_W + ' ' + VIEW_H);

  // ── 计算投影范围（用于把 Albers 原始坐标归一化到 viewBox）──
  PROJ = null;
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  (mapData.features || []).forEach(f => {
    const track = (coords) => {
      coords.forEach(pt => {
        const p = albersRaw(pt[0], pt[1]);
        if (!isFinite(p[0]) || !isFinite(p[1])) return;
        if (p[0] < minX) minX = p[0]; if (p[0] > maxX) maxX = p[0];
        if (p[1] < minY) minY = p[1]; if (p[1] > maxY) maxY = p[1];
      });
    };
    const g = f.geometry;
    if (!g) return;
    if (g.type === 'Polygon') g.coordinates.forEach(r => track(r));
    else if (g.type === 'MultiPolygon') g.coordinates.forEach(poly => poly.forEach(r => track(r)));
  });
  if (isFinite(minX) && minX < maxX) {
    PROJ = { minX, maxX, minY, maxY };
    console.log('[SimMap] proj range:', PROJ.minX.toFixed(2), PROJ.maxX.toFixed(2), PROJ.minY.toFixed(2), PROJ.maxY.toFixed(2));
  }

  // 深色底
  svg.appendChild(SVG('rect', { width: VIEW_W, height: VIEW_H, fill: '#060d1a' }));

  // 网格线（经纬线，弱化）
  const gridLines = SVG('g', { opacity: '0.07', stroke: '#00d4ff', 'stroke-width': '0.5', 'fill': 'none' });
  // 经线：75°E–135°E 每 10°
  for (let lng = 75; lng <= 135; lng += 10) {
    const gridPts = [];
    for (let lat = 3; lat <= 54; lat += 2) gridPts.push(project(lng, lat));
    if (gridPts.length > 1) {
      const d = gridPts.map(([x,y],i) => (i===0?'M':'L')+x.toFixed(1)+' '+y.toFixed(1)).join(' ');
      gridLines.appendChild(SVG('path', { d }));
    }
  }
  // 纬线：10°N–54°N 每 10°
  for (let lat = 10; lat <= 54; lat += 10) {
    const gridPts = [];
    for (let lng = 73; lng <= 136; lng += 2) gridPts.push(project(lng, lat));
    if (gridPts.length > 1) {
      const d = gridPts.map(([x,y],i) => (i===0?'M':'L')+x.toFixed(1)+' '+y.toFixed(1)).join(' ');
      gridLines.appendChild(SVG('path', { d }));
    }
  }
  svg.appendChild(gridLines); // ← 修复：网格线必须挂载到 svg

  content = SVG('g', { class: 'map-content' });

  // 省份
  const feats = [...(mapData.features || [])].sort((a, b) => {
    const aA = a.properties && a.properties.面积 || 0;
    const bA = b.properties && b.properties.面积 || 0;
    return bA - aA;
  });

  feats.forEach(f => {
    if (!f.geometry) return;
    const d = geomPath(f.geometry);
    if (!d) return;
    const name = f.properties && (f.properties.name || f.properties.NAME || f.properties.省名);
    const p = SVG('path', {
      d,
      fill: '#0d1f3c',
      stroke: '#1e4080',
      'stroke-width': '1.2',
      'stroke-linejoin': 'round',
      'data-name': name || ''
    });
    p.style.cursor = 'pointer';
    p.style.transition = 'fill 0.2s';
    if (name) {
      p.addEventListener('mouseenter', () => { p.setAttribute('fill', '#16345f'); p.setAttribute('stroke', '#00d4ff'); });
      p.addEventListener('mouseleave', () => { p.setAttribute('fill', '#0d1f3c'); p.setAttribute('stroke', '#1e4080'); });
    }
    content.appendChild(p);
  });

  // 九段线
  const jdd = [[131,12,133,13],[125,11,129,15],[122,11,125,12],[119,11,122,12],[117,11,119,12],[115,9,117,12],[114,7,115,10],[111,6,114,9],[110,5,111,7]];
  jdd.forEach(seg => {
    const [x1,y1] = project(seg[0], seg[1]);
    const [x2,y2] = project(seg[2], seg[3]);
    content.appendChild(SVG('line', { x1, y1, x2, y2, stroke: '#2a4d85', 'stroke-width': '1.2' }));
  });

  
  // Debug: count paths rendered
  var pathCount = content.querySelectorAll('path').length;
  console.log('[SimMap] drawBase done, paths:', pathCount, 'svg children:', svg.children.length);
  // 标注（右下角）——必须先挂载 content，drawMeta 里 insertBefore 才不报错
  svg.appendChild(content);
  applyTransform();
  drawMeta();
}

function drawMeta() {
  // 比例尺
  const bx = 28, by = VIEW_H - 18;
  const g = SVG('g', { transform: 'translate(' + bx + ',' + by + ')' });
  // 计算 500km 宽度
  const [px1] = project(105, 25);
  const [px2] = project(105 + 500/111, 25);
  const barW = Math.abs(px2 - px1) * 0.9;
  g.appendChild(SVG('line', { x1: 0, y1: 0, x2: barW, y2: 0, stroke: '#00d4ff', 'stroke-width': '1.5', opacity: '0.7' }));
  g.appendChild(SVG('line', { x1: 0, y1: -4, x2: 0, y2: 4, stroke: '#00d4ff', 'stroke-width': '1.5', opacity: '0.7' }));
  g.appendChild(SVG('line', { x1: barW, y1: -4, x2: barW, y2: 4, stroke: '#00d4ff', 'stroke-width': '1.5', opacity: '0.7' }));
  const t = SVG('text', { x: barW/2, y: -7, 'text-anchor': 'middle', 'font-size': '9', fill: '#00d4ff', opacity: '0.7', 'font-family': 'monospace' });
  t.textContent = '500 km';
  g.appendChild(t);
  svg.insertBefore(g, content || svg.lastChild);

  // 指北针（右上）
  const ax = VIEW_W - 38, ay = 38;
  const ng = SVG('g', { transform: 'translate(' + ax + ',' + ay + ')', opacity: '0.7' });
  ng.appendChild(SVG('circle', { cx: 0, cy: 0, r: 22, fill: '#060d1a', stroke: '#1e4080', 'stroke-width': '1.5' }));
  // 箭头
  ng.appendChild(SVG('polygon', { points: '0,-17 -7,7 0,2 7,7', fill: '#00d4ff' }));
  const nt = SVG('text', { x: 0, y: 16, 'text-anchor': 'middle', 'font-size': '10', fill: '#00d4ff', 'font-weight': 'bold' });
  nt.textContent = 'N';
  ng.appendChild(nt);
  svg.insertBefore(ng, content || svg.lastChild);
}

// ── 城市点 ────────────────────────────────────────────────────────────
function drawCities(cities) {
  if (!content || !svg) return;
  content.querySelectorAll('.city-grp').forEach(n => n.remove());

  let claimed = 0, unclaimed = 0;
  cities.forEach(c => {
    if (c.claimed) claimed++; else unclaimed++;
    const [x, y] = project(c.lng, c.lat);
    if (!isFinite(x) || !isFinite(y)) return;
    if (x < -200 || x > VIEW_W + 200 || y < -200 || y > VIEW_H + 200) return;

    const g = SVG('g', { transform: 'translate(' + x.toFixed(1) + ',' + y.toFixed(1) + ')', class: 'city-grp' });
    g.style.cursor = 'pointer';

    // 外发光（点小：全国视图不互相遮挡；内容 scale 会随缩放自然放大/缩小）
    const glowColor = c.claimed ? 'rgba(0,224,184,0.45)' : 'rgba(0,212,255,0.45)';
    const dotColor  = c.claimed ? '#00e0b8' : '#00d4ff';
    // 基准半径存 data-base-r（缩放时由 applyTransform 维护）
    const glowR = c.claimed ? 4.5 : 4.0;
    const dotR  = c.claimed ? 2.2 : 1.8;
    const glow = SVG('circle', { r: glowR, fill: glowColor, 'data-base-r': glowR, 'data-type': 'glow' });
    const dot = SVG('circle', {
      r: dotR, fill: dotColor,
      class: 'city-dot ' + (c.claimed ? 'green' : 'blue'),
      'data-code': c.code || '', 'data-base-r': dotR, 'data-type': 'dot'
    });
    g.appendChild(glow);
    g.appendChild(dot);

    // 城市名（默认隐藏，放大到 zoom>=3 显示；字号由 applyTransform 维护）
    const t = SVG('text', {
      x: 0, y: c.claimed ? 14 : -12,
      'text-anchor': 'middle', 'font-size': '6', 'font-weight': '600',
      fill: c.claimed ? '#00e0b8' : '#5bb8e8', 'font-family': 'system-ui, sans-serif',
      class: 'city-name', opacity: '0',
      'data-base-size': '6'
    });
    t.textContent = trimName(c.name);
    g.appendChild(t);

    // 悬停放大提示（r 由 applyTransform 统一维护，hover 时放大并在移出后恢复）
    g.addEventListener('mouseenter', () => {
      dot.setAttribute('data-hovered', '1');
      glow.setAttribute('data-hovered', '1');
      dot.setAttribute('r', dotR * 2.2);
      glow.setAttribute('r', glowR * 1.9);
    });
    g.addEventListener('mouseleave', () => {
      dot.removeAttribute('data-hovered');
      glow.removeAttribute('data-hovered');
      dot.setAttribute('r', dotR);
      glow.setAttribute('r', glowR);
    });

    g.addEventListener('click', () => {
      if (window.App) App.openCity(c.code, c.name, c.province, c.claimed, c.sn);
    });
    content.appendChild(g);
  });

  const cc = document.getElementById('claimedCount');
  const uc = document.getElementById('unclaimedCount');
  if (cc) cc.textContent = claimed;
  if (uc) uc.textContent = unclaimed;
}

function trimName(n) {
  return String(n || '').replace(/市$|地区$|自治州$/, '').slice(0, 5);
}

// ── 刷新 ──────────────────────────────────────────────────────────────
async function refresh() {
  try {
    const geojson = await fetch('geo/china-provinces.geojson').then(r => r.json());
    drawBase(geojson);
  } catch (e) {
    if (!content) drawBase({ features: [] });
  }
  const cities = await fetch('/api/cities').then(r => r.json()).catch(() => []);
  if (!content) drawBase({ features: [] });
  drawCities(cities);
}

function init() {
  svg = document.getElementById('china');
  setupInteraction();
  const zi = document.getElementById('z-in');
  const zo = document.getElementById('z-out');
  const zr = document.getElementById('z-reset');
  if (zi) zi.addEventListener('click', e => zoomAt(e.clientX, e.clientY, 1.25));
  if (zo) zo.addEventListener('click', e => zoomAt(e.clientX, e.clientY, 0.8));
  if (zr) zr.addEventListener('click', resetView);
  refresh();
  setInterval(refresh, 15000);
}

if (typeof window !== 'undefined') {
  window.SimMap = { init, drawCities, drawBase, project, resetView, zoomAt };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
}
