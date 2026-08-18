'use strict';
/*
 * qrcode.js — 二维码渲染适配层
 *
 * 底层使用 vendored 的标准库 qrcode-generator（web/vendor/qrcode-generator.js，
 * 由 npm 包拷贝，MIT © Kazuhiko Arase），比手写编码器可靠、可扫描。
 *
 * 注意：index.html 需先 <script src="vendor/qrcode-generator.js"></script>
 * 由它全局暴露 `qrcode`（函数）。
 */
(function (global) {
  'use strict';
  if (typeof global.qrcode !== 'function') {
    // 库未加载时不至于崩溃，给个错误提示
    global.QRCode = {
      draw(payload, size, cb) {
        const el = document.createElement('div');
        el.textContent = 'QR 库未加载: ' + payload;
        cb(el);
      },
    };
    return;
  }

  const QRCode = {
    /**
     * 绘制二维码到 canvas 并交给回调
     * @param {string} payload 编码内容
     * @param {number} sizePx 目标像素
     * @param {(canvas)=>void} cb
     */
    draw(payload, sizePx, cb) {
      try {
        const qr = global.qrcode(0, 'M'); // 0=自动版本, 纠错 M
        qr.addData(payload);
        qr.make();

        const count = qr.getModuleCount();
        const quiet = 2; // 四周留白
        const cell = Math.floor(sizePx / (count + quiet * 2)) || 1;
        const canvas = document.createElement('canvas');
        const px = cell * (count + quiet * 2);
        canvas.width = px;
        canvas.height = px;
        const ctx = canvas.getContext('2d');
        ctx.fillStyle = '#ffffff';
        ctx.fillRect(0, 0, px, px);
        ctx.fillStyle = '#000000';
        for (let r = 0; r < count; r++) {
          for (let c = 0; c < count; c++) {
            if (qr.isDark(r, c)) {
              ctx.fillRect((c + quiet) * cell, (r + quiet) * cell, cell, cell);
            }
          }
        }
        canvas.style.width = sizePx + 'px';
        canvas.style.height = sizePx + 'px';
        cb(canvas);
      } catch (e) {
        const el = document.createElement('div');
        el.textContent = 'QR 生成失败: ' + payload;
        cb(el);
      }
    },
  };

  global.QRCode = QRCode;
})(typeof window !== 'undefined' ? window : this);
