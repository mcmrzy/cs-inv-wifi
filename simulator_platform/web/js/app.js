'use strict';
/*
 * app.js — 认领交互逻辑（地图页）
 */
const App = (() => {
  let current = null; // {code,name,province}

  function toast(msg) {
    const el = document.getElementById('toast');
    el.textContent = msg;
    el.classList.add('show');
    setTimeout(() => el.classList.remove('show'), 2400);
  }

  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, (c) =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  function qrFor(el, payload, size = 148) {
    el.innerHTML = '';
    try {
      QRCode.draw(payload, size, (canvas) => el.appendChild(canvas));
    } catch (e) {
      const img = document.createElement('pre');
      img.textContent = 'QR: ' + payload;
      el.appendChild(img);
    }
  }

  function showModal(html) {
    const mask = document.getElementById('modal');
    const body = document.getElementById('m-body');
    body.innerHTML = html;
    mask.classList.add('show');
  }
  function hideModal() { document.getElementById('modal').classList.remove('show'); }

  /** 已认领：展示设备卡 */
  function showDevice(dev) {
    document.getElementById('m-city').textContent = dev.city_name + '（已认领）';
    document.getElementById('m-prov').textContent = dev.province + ' · CS-L10-6K2 ' + dev.rated_kw + 'kW';
    // 二维码内容为 "SN:<sn> PIN:<pin>"（App parseQRCode 免弹窗绑定格式）
    const qrPayload = `SN:${dev.sn} PIN:${dev.pin}`;
    showModal(`
      <div class="qr-block">
        <div class="qr-label">设备二维码</div>
        <div class="qr-box" data-qr="${esc(qrPayload)}"></div>
      </div>
      <div class="info-row"><span class="lbl">SN</span><span class="val">${esc(dev.sn)}</span></div>
      <div class="info-row"><span class="lbl">PIN</span><span class="val">${esc(dev.pin)}</span></div>
      <div class="info-row"><span class="lbl">额定功率</span><span class="val">6.2 kW</span></div>
      <div class="btn-row">
        <button class="btn btn-green" onclick="App.enterScenario('${esc(dev.sn)}')">查看电站场景</button>
        <button class="btn btn-gray" onclick="App.closeModal()">关闭</button>
      </div>
    `);
    requestAnimationFrame(() => {
      const el = document.querySelector('.qr-box');
      if (el) qrFor(el, el.dataset.qr);
    });
  }

  /** 未认领：展示认领卡 */
  function showClaim(c) {
    document.getElementById('m-city').textContent = c.name;
    document.getElementById('m-prov').textContent = c.province + '· 未认领';
    showModal(`
      <div style="color:#64748b;font-size:13px;margin-bottom:14px;line-height:1.6">
        该城市当前无已认领逆变器。点击认领，将生成一台虚拟 CS-L10-6K2（6.2 kW）设备。
      </div>
      <div class="btn-row">
        <button class="btn btn-blue" onclick="App.claim('${c.code}')">认领该设备</button>
        <button class="btn btn-gray" onclick="App.closeModal()">取消</button>
      </div>
    `);
  }

  function browserId() {
    let id = localStorage.getItem('sim_browser_id');
    if (!id) { id = Math.random().toString(36).slice(2) + Date.now().toString(36); localStorage.setItem('sim_browser_id', id); }
    return id;
  }

  // 由 map.js 调用
  function openCity(code, name, province, claimed, sn) {
    current = { code, name, province };
    if (claimed && sn) {
      fetch('/api/devices/' + sn).then((r) => r.json()).then(showDevice).catch(() => toast('加载设备失败'));
    } else {
      // 免登录查询认领状态（幂等）
      fetch('/api/claims/' + code).then((r) => r.json()).then((res) => {
        if (res.claimed && res.device) showDevice(res.device);
        else showClaim(current);
      }).catch(() => showClaim(current));
    }
  }

  function claim(code) {
    fetch('/api/claims', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ city_code: code, browser_id: browserId() }),
    })
      .then((r) => r.json())
      .then((res) => {
        if (res.error) return toast('认领失败: ' + res.error);
        toast('认领成功! 设备: ' + res.device.sn);
        showDevice(res.device);
        if (window.__refreshMap) window.__refreshMap();
      })
      .catch((e) => toast('网络错误'));
  }

  function enterScenario(sn) {
    window.location.href = '/scenario.html?sn=' + encodeURIComponent(sn);
  }

  function closeModal() { hideModal(); }

  // 关闭按钮
  const mClose = document.getElementById('m-close');
  if (mClose) mClose.addEventListener('click', hideModal);


  document.addEventListener('keydown', (e) => { if (e.key === 'Escape') hideModal(); });
  document.getElementById('modal').addEventListener('click', (e) => {
    if (e.target.id === 'modal') hideModal();
  });

  return { openCity, claim, enterScenario, closeModal, showDevice };
})();

window.App = App;
