#include "tcp_provision.h"
#include "nvs_storage.h"
#include "sn_encoder.h"
#include "ota_manager.h"
#include <string.h>

extern char g_device_sn[];

static WiFiServer* g_server = nullptr;
static WiFiClient   g_client;
static ProvisionCallback g_cb = nullptr;

static char g_rx_buf[1024];
static int  g_rx_len = 0;

// 前向声明
static void handle_wifi_scan(WiFiClient& client);
static void handle_status_page(WiFiClient& client);
static void process_http_request(WiFiClient& client);

// ==================== 配网网页 HTML ====================
// %SN_DISP% → 页面显示的 SN 文本
// %DEV_LABEL% → 设备标签（真实SN显示完整，MAC显示CS_INV_xxxxxx）
static const char PROVISION_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>逆变器配网</title>
<style>
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif; 
       background:linear-gradient(135deg,#0f2027,#203a43,#2c5364); min-height:100vh;
       display:flex; align-items:center; justify-content:center; }
.card { background:#fff; border-radius:16px; padding:30px 24px; width:92%; max-width:380px;
        box-shadow:0 20px 60px rgba(0,0,0,.3); }
.logo { text-align:center; margin-bottom:8px; }
.logo .icon { font-size:42px; }
.logo h2 { font-size:18px; color:#2c5364; margin-top:4px; }
.sn { text-align:center; font-size:12px; color:#999; margin-bottom:20px; }
.form-group { margin-bottom:16px; }
.form-group label { display:block; font-size:13px; color:#555; margin-bottom:6px; font-weight:500; }
.form-group input { width:100%; padding:12px 14px; border:2px solid #e0e0e0; border-radius:10px;
                    font-size:15px; transition:border .2s; outline:none; }
.form-group input:focus { border-color:#2c5364; }
.form-group input.valid { border-color:#27ae60; background:#f0faf4; }
.form-group input.invalid { border-color:#e74c3c; background:#fef5f5; }
.sn-check { font-size:11px; margin-top:4px; min-height:16px; }
.sn-check.ok { color:#27ae60; }
.sn-check.err { color:#e74c3c; }
.sn-hint { font-size:11px; color:#aaa; margin-top:2px; }
.btn { width:100%; padding:14px; background:linear-gradient(135deg,#2c5364,#203a43);
       color:#fff; border:none; border-radius:10px; font-size:16px; font-weight:600;
       cursor:pointer; transition:opacity .2s; margin-top:6px; }
.btn:active { opacity:.85; }
.btn:disabled { opacity:.5; cursor:not-allowed; }
.msg { text-align:center; font-size:13px; margin-top:14px; min-height:20px; }
.msg.ok { color:#27ae60; }
.msg.err { color:#e74c3c; }
.hint { text-align:center; font-size:11px; color:#bbb; margin-top:12px; }
.spinner { display:none; width:20px; height:20px; border:3px solid #ddd;
           border-top-color:#2c5364; border-radius:50%; animation:spin .7s linear infinite;
           margin:10px auto 0; }
@keyframes spin { to{transform:rotate(360deg);} }
.wifi-list { margin-bottom:14px; max-height:160px; overflow-y:auto; background:#f9f9f9;
             border-radius:8px; padding:4px; }
.wifi-item { display:flex; align-items:center; padding:10px 12px; cursor:pointer;
             border-bottom:1px solid #eee; font-size:14px; color:#333; }
.wifi-item:last-child { border-bottom:none; }
.wifi-item:active { background:#e0eef5; }
.wifi-item .rssi { margin-left:auto; font-size:11px; color:#999; }
.wifi-item .lock { font-size:12px; margin-left:6px; }
.scan-btn { width:100%; padding:10px; background:none; border:1px dashed #aaa; 
            border-radius:8px; color:#777; font-size:13px; cursor:pointer; margin-bottom:14px; }
.scan-btn:disabled { opacity:.5; }
.status-link { display:block; text-align:center; padding:12px; margin-top:16px; 
               background:linear-gradient(135deg,#f8f9fa,#e9ecef); color:#2c5364; 
               font-size:14px; font-weight:500; text-decoration:none; 
               border:2px solid #2c5364; border-radius:10px; }
.status-link:active { background:#e0eef5; }
</style>
</head>
<body>
<div class="card">
  <div class="logo">
    <div class="icon">☀️</div>
    <h2>辰烁科技逆变器 WiFi 配网</h2>
  </div>
  <div class="sn" id="snLine">%DEV_LABEL%</div>
  <button class="scan-btn" id="scanBtn" onclick="scanWiFi()">📡 扫描附近 WiFi</button>
  <div class="wifi-list" id="wifiList" style="display:none"></div>
  <form id="configForm">
    <div class="form-group">
      <label>WiFi 名称 (SSID)</label>
      <input type="text" id="ssid" placeholder="请输入 WiFi 名称" required autocomplete="off">
    </div>
    <div class="form-group">
      <label>WiFi 密码</label>
      <input type="password" id="pass" placeholder="请输入 WiFi 密码" autocomplete="off">
    </div>
    <button type="submit" class="btn" id="submitBtn">⚡ 保存并连接</button>
  </form>
  <div class="spinner" id="spinner"></div>
  <div class="msg" id="msg"></div>
  <a href="/status" class="status-link">📊 查看网络连接状态</a>
  <a href="/ota" class="status-link" style="margin-top:8px;background:linear-gradient(135deg,#fff3e0,#ffe0b2);border-color:#e65100;color:#e65100;">🔄 固件升级</a>
  <div class="hint">也可通过 辰烁科技App 配网</div>
</div>
<script>
// ====== 原有功能 ======
function showMsg(text, isOk) {
  var m = document.getElementById('msg');
  m.textContent = text;
  m.className = 'msg ' + (isOk ? 'ok' : 'err');
}

async function scanWiFi() {
  var btn = document.getElementById('scanBtn');
  var list = document.getElementById('wifiList');
  btn.disabled = true;
  btn.textContent = '\u23f3 正在扫描...';
  showMsg('WiFi 扫描中，请稍候...', true);
  list.style.display = 'none';
  list.innerHTML = '';
  try {
    var r = await fetch('/scan');
    var data = await r.json();
    if (data.status === 'scanning') {
      // 服务端还在扫描，再等一会轮询一次
      await new Promise(function(resolve) { setTimeout(resolve, 2500); });
      r = await fetch('/scan');
      data = await r.json();
    }
    if (data.networks && data.networks.length > 0) {
      data.networks.sort(function(a,b) { return b.rssi - a.rssi; });
      data.networks.forEach(function(n) {
        var div = document.createElement('div');
        div.className = 'wifi-item';
        var icon = n.enc ? '\uD83D\uDD12 ' : '';
        div.innerHTML = '<span>' + icon + n.ssid + '</span><span class="rssi">' + n.rssi + 'dBm</span>';
        div.onclick = function() {
          document.getElementById('ssid').value = n.ssid;
          document.getElementById('pass').focus();
        };
        list.appendChild(div);
      });
      list.style.display = 'block';
      showMsg('点击 WiFi \u540d\u79f0\u81ea\u52a8\u586b\u5165', true);
    } else {
      showMsg('\u672a\u626b\u63cf\u5230 WiFi\uff0c\u8bf7\u624b\u52a8\u8f93\u5165', false);
    }
  } catch(e) {
    showMsg('\u626b\u63cf\u5931\u8d25\uff0c\u8bf7\u624b\u52a8\u8f93\u5165', false);
  }
  btn.disabled = false;
  btn.textContent = '\uD83D\uDCE1 \u626b\u63cf\u9644\u8fd1 WiFi';
}

document.getElementById('configForm').onsubmit = async function(e) {
  e.preventDefault();
  var ssid = document.getElementById('ssid').value.trim();
  var pass = document.getElementById('pass').value;

  if (!ssid) { showMsg('请输入 WiFi 名称', false); return; }
  document.getElementById('submitBtn').disabled = true;
  document.getElementById('spinner').style.display = 'block';
  showMsg('正在配置...', true);
  try {
    var r = await fetch('/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid: ssid, password: pass })
    });
    var data = await r.json();
    if (data.result === 'ok') {
      showMsg('\u2705 \u914d\u7f6e\u6210\u529f! \u6b63\u5728\u68c0\u67e5 WiFi \u8fde\u63a5...', true);
      // 轮询检查 WiFi 连接状态，最多等待 20 秒
      var checkCount = 0;
      var checkInterval = setInterval(async function() {
        checkCount++;
        try {
          var resp = await fetch('/wifi_status');
          var status = await resp.json();
          if (status.connected) {
            clearInterval(checkInterval);
            showMsg('\u2705 WiFi \u8fde\u63a5\u6210\u529f! SSID: ' + status.ssid + ' IP: ' + status.ip, true);
            // 3 秒后跳转到状态页
            setTimeout(function() { window.location.href = '/status'; }, 3000);
          } else if (checkCount >= 20) {
            clearInterval(checkInterval);
            showMsg('\u26a0 WiFi \u8fde\u63a5\u8d85\u65f6\uff0c\u8bf7\u68c0\u67e5\u5bc6\u7801\u662f\u5426\u6b63\u786e', false);
            document.getElementById('submitBtn').disabled = false;
            document.getElementById('spinner').style.display = 'none';
          }
        } catch(e) {}
      }, 1000);
    } else {
      showMsg('\u274c ' + (data.message || '\u914d\u7f6e\u5931\u8d25'), false);
      document.getElementById('submitBtn').disabled = false;
      document.getElementById('spinner').style.display = 'none';
    }
  } catch(e) {
    showMsg('\u274c \u8bf7\u6c42\u5931\u8d25', false);
    document.getElementById('submitBtn').disabled = false;
    document.getElementById('spinner').style.display = 'none';
  }
};
</script>
</body>
</html>
)rawliteral";

// ==================== JSON 提取 ====================
static bool json_extract(const char* json, const char* key, char* value, size_t val_len) {
  char search[64];
  snprintf(search, sizeof(search), "\"%s\":\"", key);
  const char* p = strstr(json, search);
  if (!p) return false;
  p += strlen(search);
  size_t i = 0;
  while (*p && *p != '"' && i < val_len - 1) {
    value[i++] = *p++;
  }
  value[i] = '\0';
  return true;
}

// ==================== HTTP 响应 ====================
static void send_http_response(WiFiClient& client, int code, const char* content_type, const char* body) {
  const char* reason = "OK";
  if (code == 400) reason = "Bad Request";
  else if (code == 404) reason = "Not Found";
  else if (code == 405) reason = "Method Not Allowed";
  else if (code == 500) reason = "Internal Server Error";
  else if (code == 503) reason = "Service Unavailable";

  client.printf("HTTP/1.1 %d %s\r\n", code, reason);
  client.printf("Content-Type: %s\r\n", content_type);
  client.printf("Content-Length: %d\r\n", strlen(body));
  client.printf("Connection: close\r\n");
  client.printf("Access-Control-Allow-Origin: *\r\n");
  client.printf("\r\n");
  client.print(body);
}

static void send_http_404(WiFiClient& client) {
  const char* body = "not found";
  client.printf("HTTP/1.1 404 Not Found\r\n");
  client.printf("Content-Type: text/plain\r\n");
  client.printf("Content-Length: %d\r\n", strlen(body));
  client.printf("Connection: close\r\n");
  client.printf("\r\n");
  client.print(body);
}

// ==================== URL 解码 ====================
static void url_decode(char* str) {
  char* p = str;
  char* q = str;
  while (*p) {
    if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
      char hi = p[1], lo = p[2];
      hi -= (hi <= '9') ? '0' : ((hi <= 'F') ? 'A' - 10 : 'a' - 10);
      lo -= (lo <= '9') ? '0' : ((lo <= 'F') ? 'A' - 10 : 'a' - 10);
      *q++ = (hi << 4) | lo;
      p += 3;
    } else if (*p == '+') {
      *q++ = ' ';
      p++;
    } else {
      *q++ = *p++;
    }
  }
  *q = '\0';
}

// ==================== 状态查询页面 HTML ====================
static const char STATUS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>设备状态</title>
<style>
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif; 
       background:linear-gradient(135deg,#0f2027,#203a43,#2c5364); min-height:100vh;
       display:flex; align-items:center; justify-content:center; padding:20px; }
.card { background:#fff; border-radius:16px; padding:24px; width:92%; max-width:400px;
        box-shadow:0 20px 60px rgba(0,0,0,.3); }
.header { text-align:center; margin-bottom:20px; }
.header .icon { font-size:36px; }
.header h2 { font-size:18px; color:#2c5364; margin-top:4px; }
.status-item { display:flex; justify-content:space-between; padding:12px 0; 
               border-bottom:1px solid #eee; font-size:14px; }
.status-item:last-child { border-bottom:none; }
.status-item .label { color:#666; }
.status-item .value { color:#333; font-weight:500; }
.status-item .value.ok { color:#27ae60; }
.status-item .value.err { color:#e74c3c; }
.status-item .value.warn { color:#f39c12; }
.btn { width:100%; padding:12px; background:linear-gradient(135deg,#2c5364,#203a43);
       color:#fff; border:none; border-radius:10px; font-size:15px; font-weight:600;
       cursor:pointer; margin-top:16px; text-decoration:none; display:inline-block; text-align:center; }
.btn:active { opacity:.85; }
.hint { text-align:center; font-size:11px; color:#999; margin-top:12px; }
</style>
</head>
<body>
<div class="card">
  <div class="header">
    <div class="icon">📊</div>
    <h2>逆变器网络连接状态</h2>
  </div>
  <div class="status-item">
    <span class="label">设备序列号</span>
    <span class="value">%SN%</span>
  </div>
  <div class="status-item">
    <span class="label">WiFi 连接</span>
    <span class="value %WIFI_CLASS%">%WIFI_STATUS%</span>
  </div>
  <div class="status-item">
    <span class="label">WiFi 名称</span>
    <span class="value">%WIFI_SSID%</span>
  </div>
  <div class="status-item">
    <span class="label">STA IP 地址</span>
    <span class="value">%STA_IP%</span>
  </div>
  <div class="status-item">
    <span class="label">AP IP 地址</span>
    <span class="value">%AP_IP%</span>
  </div>
  <div class="status-item">
    <span class="label">服务器连接</span>
    <span class="value %MQTT_CLASS%">%MQTT_STATUS%</span>
  </div>
  <div class="status-item">
    <span class="label">信号强度</span>
    <span class="value">%RSSI%</span>
  </div>
  <div class="status-item">
    <span class="label">运行时间</span>
    <span class="value">%UPTIME%</span>
  </div>
  <div class="status-item">
    <span class="label">剩余内存</span>
    <span class="value">%HEAP%</span>
  </div>
  <a href="/" class="btn">⚙️ 配网设置</a>
  <a href="/ota" class="btn" style="margin-top:10px;background:linear-gradient(135deg,#e67e22,#d35400);">🔄 固件升级</a>
  <div class="hint">访问 <a href="http://192.168.4.1:8888/status">192.168.4.1:8888</a> 查看此页面</div>
</div>
</body>
</html>
)rawliteral";

// ==================== OTA 升级页面 HTML ====================
static const char OTA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>固件升级</title>
<style>
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
       background:linear-gradient(135deg,#0f2027,#203a43,#2c5364); min-height:100vh;
       display:flex; align-items:center; justify-content:center; padding:20px; }
.card { background:#fff; border-radius:16px; padding:24px; width:92%; max-width:420px;
        box-shadow:0 20px 60px rgba(0,0,0,.3); }
.header { text-align:center; margin-bottom:20px; }
.header .icon { font-size:36px; }
.header h2 { font-size:18px; color:#2c5364; margin-top:4px; }
.info-row { display:flex; justify-content:space-between; padding:8px 0; font-size:13px; border-bottom:1px solid #f0f0f0; }
.info-row .label { color:#999; }
.info-row .value { color:#333; font-weight:500; }
.section { margin-top:20px; padding:16px; background:#f8f9fa; border-radius:12px; }
.section h3 { font-size:15px; color:#2c5364; margin-bottom:12px; }
.section h3 .tag { font-size:11px; background:#2c5364; color:#fff; padding:2px 8px;
                   border-radius:10px; margin-left:8px; font-weight:400; }
.form-group { margin-bottom:12px; }
.form-group label { display:block; font-size:12px; color:#666; margin-bottom:4px; }
.form-group input { width:100%; padding:10px 12px; border:2px solid #e0e0e0; border-radius:8px;
                    font-size:14px; outline:none; transition:border .2s; }
.form-group input:focus { border-color:#2c5364; }
.progress-bar { width:100%; height:8px; background:#e0e0e0; border-radius:4px; overflow:hidden;
                margin:12px 0; display:none; }
.progress-bar .fill { height:100%; background:linear-gradient(90deg,#27ae60,#2ecc71);
                      border-radius:4px; transition:width .3s; width:0%; }
.progress-bar .fill.error { background:linear-gradient(90deg,#e74c3c,#c0392b); }
.progress-text { text-align:center; font-size:12px; color:#666; min-height:18px; }
.btn { width:100%; padding:12px; border:none; border-radius:10px; font-size:14px; font-weight:600;
       cursor:pointer; transition:opacity .2s; margin-top:6px; }
.btn:active { opacity:.85; }
.btn:disabled { opacity:.5; cursor:not-allowed; }
.btn.primary { background:linear-gradient(135deg,#2c5364,#203a43); color:#fff; }
.btn.arm { background:linear-gradient(135deg,#e67e22,#d35400); color:#fff; }
.btn.danger { background:#e74c3c; color:#fff; }
.msg { text-align:center; font-size:13px; margin-top:10px; min-height:20px; }
.msg.ok { color:#27ae60; }
.msg.err { color:#e74c3c; }
.hint { text-align:center; font-size:11px; color:#bbb; margin-top:12px; }
.back-link { display:block; text-align:center; padding:10px; margin-top:12px;
             color:#2c5364; font-size:13px; text-decoration:none; }
</style>
</head>
<body>
<div class="card">
  <div class="header">
    <div class="icon">🔄</div>
    <h2>固件升级 (OTA)</h2>
  </div>
  <div class="info-row">
    <span class="label">设备序列号</span>
    <span class="value">%SN%</span>
  </div>
  <div class="info-row">
    <span class="label">ESP 固件版本</span>
    <span class="value">%ESP_VER%</span>
  </div>
  <div class="info-row">
    <span class="label">ARM 固件版本</span>
    <span class="value">%ARM_VER%</span>
  </div>
  <div class="info-row">
    <span class="label">剩余内存</span>
    <span class="value">%HEAP%</span>
  </div>

  <!-- ESP 固件升级 -->
  <div class="section">
    <h3>ESP 模块升级 <span class="tag">WiFi</span></h3>
    <div class="form-group">
      <label>固件下载地址 (URL)</label>
      <input type="text" id="espUrl" placeholder="http://your-server/firmware.bin" autocomplete="off">
    </div>
    <button class="btn primary" id="espBtn" onclick="startEspOta()">⬇️ 升级 ESP 固件</button>
    <div class="progress-bar" id="espProgress"><div class="fill" id="espFill"></div></div>
    <div class="progress-text" id="espProgressText"></div>
    <div class="msg" id="espMsg"></div>
  </div>

  <!-- ARM 固件升级 -->
  <div class="section">
    <h3>ARM 芯片升级 <span class="tag">GD32F303</span></h3>
    <div class="form-group">
      <label>固件下载地址 (URL)</label>
      <input type="text" id="armUrl" placeholder="http://your-server/arm_firmware.bin" autocomplete="off">
    </div>
    <button class="btn arm" id="armBtn" onclick="startArmOta()">⬇️ 升级 ARM 固件</button>
    <div class="progress-bar" id="armProgress"><div class="fill" id="armFill"></div></div>
    <div class="progress-text" id="armProgressText"></div>
    <div class="msg" id="armMsg"></div>
  </div>

  <a href="/status" class="back-link">📊 返回设备状态</a>
  <div class="hint">升级过程中请勿断电，ESP 升级成功后自动重启</div>
</div>
<script>
function showMsg(id, text, isOk) {
  var m = document.getElementById(id);
  m.textContent = text;
  m.className = 'msg ' + (isOk ? 'ok' : 'err');
}

function updateProgress(target, percent, status) {
  var bar = document.getElementById(target + 'Progress');
  var fill = document.getElementById(target + 'Fill');
  var text = document.getElementById(target + 'ProgressText');
  bar.style.display = 'block';
  fill.style.width = percent + '%';
  fill.className = 'fill' + (status === 'error' ? ' error' : '');
  var statusMap = {downloading:'下载中...',writing:'写入中...',verifying:'校验中...',done:'完成',error:'失败'};
  text.textContent = percent + '% - ' + (statusMap[status] || status);
}

var pollTimer = null;

function pollProgress(target) {
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(function() {
    fetch('/ota/progress').then(function(r) { return r.json(); }).then(function(data) {
      if (data.target === target || data.target === 'none') {
        updateProgress(target, data.progress, data.status);
        if (data.status === 'done' || data.status === 'error' || data.status === 'idle') {
          clearInterval(pollTimer);
          pollTimer = null;
          if (data.status === 'done') {
            showMsg(target + 'Msg', '✅ 升级成功!', true);
          } else if (data.status === 'error') {
            showMsg(target + 'Msg', '❌ ' + (data.message || '升级失败'), false);
            document.getElementById(target + 'Btn').disabled = false;
          }
        }
      }
    }).catch(function() {});
  }, 1000);
}

function startEspOta() {
  var url = document.getElementById('espUrl').value.trim();
  if (!url) { showMsg('espMsg', '请输入固件 URL', false); return; }
  document.getElementById('espBtn').disabled = true;
  showMsg('espMsg', '正在启动升级...', true);
  fetch('/ota/esp', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({url: url})
  }).then(function(r) { return r.json(); }).then(function(data) {
    if (data.result === 'ok') {
      showMsg('espMsg', '升级已开始，请勿断电...', true);
      pollProgress('esp');
    } else {
      showMsg('espMsg', '❌ ' + (data.message || '启动失败'), false);
      document.getElementById('espBtn').disabled = false;
    }
  }).catch(function() {
    showMsg('espMsg', '❌ 请求失败', false);
    document.getElementById('espBtn').disabled = false;
  });
}

function startArmOta() {
  var url = document.getElementById('armUrl').value.trim();
  if (!url) { showMsg('armMsg', '请输入固件 URL', false); return; }
  document.getElementById('armBtn').disabled = true;
  showMsg('armMsg', '正在启动升级...', true);
  fetch('/ota/arm', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({url: url})
  }).then(function(r) { return r.json(); }).then(function(data) {
    if (data.result === 'ok') {
      showMsg('armMsg', '升级已开始，请勿断电...', true);
      pollProgress('arm');
    } else {
      showMsg('armMsg', '❌ ' + (data.message || '启动失败'), false);
      document.getElementById('armBtn').disabled = false;
    }
  }).catch(function() {
    showMsg('armMsg', '❌ 请求失败', false);
    document.getElementById('armBtn').disabled = false;
  });
}
</script>
</body>
</html>
)rawliteral";

// ==================== OTA 页面处理 ====================
static void handle_ota_page(WiFiClient& client) {
  String html = String(FPSTR(OTA_HTML));
  html.replace("%SN%", g_device_sn);
  html.replace("%ESP_VER%", ota_get_esp_version());
  html.replace("%ARM_VER%", ota_get_arm_version());
  html.replace("%HEAP%", String(ESP.getFreeHeap() / 1024) + " KB");
  send_http_response(client, 200, "text/html; charset=utf-8", html.c_str());
}

// POST /ota/esp - 触发 ESP 自身 OTA
static void handle_ota_esp(WiFiClient& client, const char* body) {
  char url[256] = {0};
  if (!json_extract(body, "url", url, sizeof(url)) || strlen(url) == 0) {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"缺少 url 参数\"}");
    return;
  }

  if (ota_is_busy()) {
    send_http_response(client, 409, "application/json",
                       "{\"result\":\"error\",\"message\":\"OTA 正在进行中\"}");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    send_http_response(client, 503, "application/json",
                       "{\"result\":\"error\",\"message\":\"WiFi 未连接\"}");
    return;
  }

  Serial.printf("[OTA] ESP OTA from: %s\n", url);
  bool ok = ota_start_esp(url);

  if (ok) {
    send_http_response(client, 200, "application/json",
                       "{\"result\":\"ok\",\"message\":\"ESP OTA started\"}");
  } else {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"URL 无效或参数错误\"}");
  }
}

// POST /ota/arm - 触发 ARM 固件 OTA
static void handle_ota_arm(WiFiClient& client, const char* body) {
  char url[256] = {0};
  if (!json_extract(body, "url", url, sizeof(url)) || strlen(url) == 0) {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"缺少 url 参数\"}");
    return;
  }

  if (ota_is_busy()) {
    send_http_response(client, 409, "application/json",
                       "{\"result\":\"error\",\"message\":\"OTA 正在进行中\"}");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    send_http_response(client, 503, "application/json",
                       "{\"result\":\"error\",\"message\":\"WiFi 未连接\"}");
    return;
  }

  Serial.printf("[OTA] ARM OTA from: %s\n", url);
  bool ok = ota_start_arm(url);

  if (ok) {
    send_http_response(client, 200, "application/json",
                       "{\"result\":\"ok\",\"message\":\"ARM OTA started\"}");
  } else {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"URL 无效或参数错误\"}");
  }
}

// GET /ota/progress - 查询 OTA 进度
static void handle_ota_progress(WiFiClient& client) {
  char resp[256];
  const char* target_str = "none";
  const char* status_str = "idle";

  if (ota_is_busy()) {
    target_str = (ota_get_target() == OTA_TARGET_ESP) ? "esp" : "arm";
    switch (ota_get_status()) {
      case OTA_STATUS_DOWNLOADING: status_str = "downloading"; break;
      case OTA_STATUS_WRITING:     status_str = "writing"; break;
      case OTA_STATUS_VERIFYING:   status_str = "verifying"; break;
      case OTA_STATUS_DONE:        status_str = "done"; break;
      case OTA_STATUS_ERROR:       status_str = "error"; break;
      default:                     status_str = "idle"; break;
    }
  } else if (ota_get_status() == OTA_STATUS_DONE) {
    target_str = (ota_get_target() == OTA_TARGET_ESP) ? "esp" : "arm";
    status_str = "done";
  } else if (ota_get_status() == OTA_STATUS_ERROR) {
    target_str = (ota_get_target() == OTA_TARGET_ESP) ? "esp" : "arm";
    status_str = "error";
  }

  snprintf(resp, sizeof(resp),
           "{\"target\":\"%s\",\"progress\":%d,\"status\":\"%s\",\"message\":\"%s\"}",
           target_str, ota_get_progress(), status_str, ota_get_message());
  send_http_response(client, 200, "application/json", resp);
}

// GET /ota/info - 查询设备固件信息
static void handle_ota_info(WiFiClient& client) {
  char resp[256];
  snprintf(resp, sizeof(resp),
           "{\"esp_version\":\"%s\",\"arm_version\":\"%s\",\"free_heap\":%d,\"chip\":\"%s\"}",
           ota_get_esp_version(), ota_get_arm_version(), ESP.getFreeHeap(),
#ifdef CHIP_ESP32C3
           "ESP32-C3"
#else
           "ESP8266"
#endif
  );
  send_http_response(client, 200, "application/json", resp);
}

// ==================== WiFi 扫描（供网页调用） ====================
static void handle_wifi_scan(WiFiClient& client) {
  // 清理之前的扫描结果
  int prev = WiFi.scanComplete();
  if (prev > 0) WiFi.scanDelete();
  else if (prev == WIFI_SCAN_RUNNING) {
    send_http_response(client, 200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }

  // 同步扫描（阻塞约 2-3 秒，配网阶段可接受）
  int n = WiFi.scanNetworks(false, true);  // sync, show hidden
  if (n <= 0) {
    send_http_response(client, 200, "application/json", "{\"networks\":[]}");
    return;
  }

  String json = "{\"networks\":[";
  for (int i = 0; i < n && i < 30; i++) {
    if (i > 0) json += ",";
    // 转义 SSID 中的特殊字符
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i));
    json += ",\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0) + "}";
  }
  json += "]}";
  WiFi.scanDelete();
  send_http_response(client, 200, "application/json", json.c_str());
}

// ==================== 状态查询页面处理 ====================
extern bool mqtt_is_connected();  // 来自 mqtt_handler.cpp

static void handle_status_page(WiFiClient& client) {
  String html = String(FPSTR(STATUS_HTML));
  
  // 替换 SN
  html.replace("%SN%", g_device_sn);
  
  // WiFi 状态
  bool wifi_connected = (WiFi.status() == WL_CONNECTED);
  html.replace("%WIFI_STATUS%", wifi_connected ? "已连接" : "未连接");
  html.replace("%WIFI_CLASS%", wifi_connected ? "ok" : "err");
  
  // WiFi 名称 (SSID)
  html.replace("%WIFI_SSID%", wifi_connected ? WiFi.SSID().c_str() : "--");
  
  // STA IP
  html.replace("%STA_IP%", wifi_connected ? WiFi.localIP().toString().c_str() : "--");
  
  // AP IP（总是显示，因为 AP 保持开启）
  html.replace("%AP_IP%", WiFi.softAPIP().toString().c_str());
  
  // MQTT 状态
  bool mqtt_conn = mqtt_is_connected();
  html.replace("%MQTT_STATUS%", mqtt_conn ? "已连接" : "未连接");
  html.replace("%MQTT_CLASS%", mqtt_conn ? "ok" : (wifi_connected ? "warn" : "err"));
  
  // 信号强度
  html.replace("%RSSI%", wifi_connected ? String(WiFi.RSSI()) + " dBm" : "--");
  
  // 运行时间
  unsigned long uptime = millis() / 1000;
  char uptime_str[32];
  int hours = uptime / 3600;
  int mins = (uptime % 3600) / 60;
  int secs = uptime % 60;
  if (hours > 0) {
    snprintf(uptime_str, sizeof(uptime_str), "%d小时%d分%d秒", hours, mins, secs);
  } else if (mins > 0) {
    snprintf(uptime_str, sizeof(uptime_str), "%d分%d秒", mins, secs);
  } else {
    snprintf(uptime_str, sizeof(uptime_str), "%d秒", secs);
  }
  html.replace("%UPTIME%", uptime_str);
  
  // 剩余内存
  html.replace("%HEAP%", String(ESP.getFreeHeap() / 1024) + " KB");
  
  send_http_response(client, 200, "text/html; charset=utf-8", html.c_str());
}

// ==================== 处理 HTTP 请求 ====================
static void process_http_request(WiFiClient& client) {
  // 解析第一行: METHOD /path HTTP/1.1
  char method[16] = {0};
  char path[128] = {0};

  char* line_end = strstr(g_rx_buf, "\r\n");
  if (!line_end) return;

  // 解析 METHOD 和 PATH
  char* sp1 = strchr(g_rx_buf, ' ');
  if (!sp1) return;
  size_t mlen = sp1 - g_rx_buf;
  if (mlen >= sizeof(method)) mlen = sizeof(method) - 1;
  strncpy(method, g_rx_buf, mlen);

  char* sp2 = strchr(sp1 + 1, ' ');
  if (!sp2) return;
  size_t plen = sp2 - sp1 - 1;
  if (plen >= sizeof(path)) plen = sizeof(path) - 1;
  strncpy(path, sp1 + 1, plen);

  url_decode(path);

  Serial.printf("[HTTP] %s %s\n", method, path);

  // 找 body（\r\n\r\n 之后）
  const char* body = strstr(g_rx_buf, "\r\n\r\n");
  char empty_body[] = "";
  if (body) body += 4; else body = empty_body;

  // ========== 路由 ==========

  // GET / → 返回配网页面
  // GET /scan → WiFi 扫描
  // POST /config → 配网 (JSON 或 form)
  // POST / → 兼容旧版 JSON API

  if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
    // 构建设备标签
    // 真实 SN → 显示完整: 设备: CS_INV_{完整SN}
    // MAC 替换的 SN → 显示: 设备: CS_INV_{末８位}
    String html = String(FPSTR(PROVISION_HTML));
    String dev_label;

    if (strncmp(g_device_sn, "MAC", 3) == 0) {
      // MAC 生成的假 SN，只显示末 8 位
      size_t slen = strlen(g_device_sn);
      const char* tail = (slen > 8) ? g_device_sn + slen - 8 : g_device_sn;
      char upper[9] = {0};
      int i;
      for (i = 0; i < 8 && tail[i]; i++) upper[i] = tail[i];
      upper[i] = '\0';
      for (char* p = upper; *p; p++) {
        if (*p >= 'a' && *p <= 'f') *p = *p - 'a' + 'A';
      }
      dev_label = String("\u8bbe\u5907: CS_INV_") + upper;  // 设备: CS_INV_XXXXXX
    } else {
      // 真实 SN → 显示完整
      dev_label = String("\u8bbe\u5907: CS_INV_") + g_device_sn;
    }
    html.replace("%DEV_LABEL%", dev_label);
    send_http_response(client, 200, "text/html; charset=utf-8", html.c_str());
    return;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/scan") == 0) {
    handle_wifi_scan(client);
    return;
  }

  // GET /status → 返回设备运行状态（AP+STA 双模式时本地设备可查看）
  if (strcmp(method, "GET") == 0 && strcmp(path, "/status") == 0) {
    handle_status_page(client);
    return;
  }

  // GET /wifi_status → 返回 WiFi 连接状态（用于配网后轮询检查）
  if (strcmp(method, "GET") == 0 && strcmp(path, "/wifi_status") == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      char resp[128];
      snprintf(resp, sizeof(resp), 
               "{\"connected\":true,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
               WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      send_http_response(client, 200, "application/json", resp);
    } else {
      send_http_response(client, 200, "application/json", "{\"connected\":false}");
    }
    return;
  }

  // ========== OTA 路由 ==========

  // GET /ota → 返回 OTA 升级页面
  if (strcmp(method, "GET") == 0 && strcmp(path, "/ota") == 0) {
    handle_ota_page(client);
    return;
  }

  // POST /ota/esp → 触发 ESP 自身 OTA
  if (strcmp(method, "POST") == 0 && strcmp(path, "/ota/esp") == 0) {
    handle_ota_esp(client, body);
    return;
  }

  // POST /ota/arm → 触发 ARM 固件 OTA
  if (strcmp(method, "POST") == 0 && strcmp(path, "/ota/arm") == 0) {
    handle_ota_arm(client, body);
    return;
  }

  // GET /ota/progress → 查询 OTA 进度
  if (strcmp(method, "GET") == 0 && strcmp(path, "/ota/progress") == 0) {
    handle_ota_progress(client);
    return;
  }

  // GET /ota/info → 查询设备固件信息
  if (strcmp(method, "GET") == 0 && strcmp(path, "/ota/info") == 0) {
    handle_ota_info(client);
    return;
  }

  if ((strcmp(method, "POST") == 0 && strcmp(path, "/config") == 0) ||
      (strcmp(method, "POST") == 0 && strcmp(path, "/") == 0)) {

    // 提取 body 部分
    const char* json_body = body;

    char ssid[33] = {0};
    char pass[65] = {0};

    // 尝试 JSON 解析
    bool got_ssid = json_extract(json_body, "ssid", ssid, sizeof(ssid));
    bool got_pass = json_extract(json_body, "password", pass, sizeof(pass));

    // 兼容旧版 JSON API: {"cmd":"set_wifi","ssid":"...","password":"..."}
    char cmd[16] = {0};
    bool got_cmd = json_extract(json_body, "cmd", cmd, sizeof(cmd));
    if (got_cmd && strcmp(cmd, "set_wifi") == 0) {
      got_ssid = json_extract(json_body, "ssid", ssid, sizeof(ssid));
      got_pass = json_extract(json_body, "password", pass, sizeof(pass));
    }

    if (got_ssid && got_pass) {
      const char* resp = "{\"result\":\"ok\",\"message\":\"connecting, please wait\"}";
      send_http_response(client, 200, "application/json", resp);
      Serial.printf("[PROV] WiFi: %s\n", ssid);
      if (g_cb) g_cb(ssid, pass);
    } else {
      const char* resp = "{\"result\":\"error\",\"message\":\"missing ssid or password\"}";
      send_http_response(client, 400, "application/json", resp);
    }
    return;
  }

  // 404
  send_http_404(client);
}

// ==================== 初始化 ====================
void tcp_provision_init() {
  Serial.printf("[PROV] HTTP server on port %d\n", PROVISION_TCP_PORT);

  if (g_server) {
    g_server->stop();
    delete g_server;
  }
  g_server = new WiFiServer(PROVISION_TCP_PORT);
  g_server->begin();
  g_server->setNoDelay(true);
  g_rx_len = 0;
}

void tcp_provision_set_callback(ProvisionCallback cb) {
  g_cb = cb;
}

void tcp_provision_stop() {
  if (g_server) {
    g_server->stop();
    delete g_server;
    g_server = nullptr;
  }
  if (g_client) {
    g_client.stop();
  }
}

// ==================== 主循环 ====================
void tcp_provision_process() {
  if (!g_server) return;

  // 新连接
  if (g_server->hasClient()) {
    if (g_client) g_client.stop();
    g_client = g_server->available();
    g_client.setTimeout(5);  // 5ms 超时，不阻塞 loop
    g_rx_len = 0;
  }

  if (!g_client || !g_client.connected()) return;

  // 读取数据
  while (g_client.available() && g_rx_len < (int)sizeof(g_rx_buf) - 1) {
    char c = g_client.read();
    g_rx_buf[g_rx_len++] = c;

    // HTTP 请求结束标志: \r\n\r\n 或 缓冲超限
    if (g_rx_len >= 4 && strncmp(g_rx_buf + g_rx_len - 4, "\r\n\r\n", 4) == 0) {
      g_rx_buf[g_rx_len] = '\0';
      // 检查 Content-Length 是否需要继续读 body
      char* cl = strstr(g_rx_buf, "Content-Length:");
      int body_len = 0;
      if (cl) body_len = atoi(cl + 15);

      char* body_start = strstr(g_rx_buf, "\r\n\r\n") + 4;
      int header_len = body_start - g_rx_buf;
      int received_body = g_rx_len - header_len;

      // 如果 body 还没收完，继续读
      if (received_body < body_len) {
        unsigned long start = millis();
        while (received_body < body_len && (millis() - start) < 2000) {
          if (g_client.available()) {
            char b = g_client.read();
            g_rx_buf[g_rx_len++] = b;
            received_body++;
          } else {
            delay(1);
          }
        }
      }

      g_rx_buf[g_rx_len] = '\0';
      process_http_request(g_client);
      g_client.flush();
      g_client.stop();
      g_rx_len = 0;
      return;
    }
  }

  // 超时保护：缓冲太久无新数据
  if (g_rx_len > 0 && !g_client.available()) {
    static unsigned long last_rx = 0;
    if (last_rx == 0) last_rx = millis();
    if (millis() - last_rx > 3000) {
      g_client.stop();
      g_rx_len = 0;
      last_rx = 0;
    }
  }
}
