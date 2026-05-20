#include "tcp_provision.h"
#include "nvs_storage.h"
#include "sn_encoder.h"
#include "ip_geolocation.h"
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
      <label>🔑 设备序列号 (16位)</label>
      <input type="text" id="sn" placeholder="如 H1CNA0013500001O" maxlength="16"
             autocomplete="off" style="font-family:monospace;letter-spacing:1px;text-transform:uppercase;">
      <div class="sn-check" id="snCheck"></div>
      <div class="sn-hint">格式: 厂商+国家+客户+日期+序号+校验位，最后一位自动校验</div>
    </div>
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
  <a href="/ap_config" class="status-link" style="margin-top:8px;">📡 修改热点名称</a>
  <a href="/ipgeo_config" class="status-link" style="margin-top:8px;">🌍 IP定位API设置</a>
  <div class="hint">也可通过 辰烁科技App 配网</div>
</div>
<script>
// ====== SN 校验 (CRC-16-CCITT 与 C++ 端一致) ======
var SN_CHECK = {};
SN_CHECK.yearCodes = '123456789ABCDEFGHJKLMNPQRSTUVWXYZ';
SN_CHECK.monthCodes = { '1':1,'2':2,'3':3,'4':4,'5':5,'6':6,'7':7,'8':8,'9':9,'A':10,'B':11,'C':12 };
SN_CHECK.validGrades = ['A','B','C','D','E','P','X'];

SN_CHECK.crc16 = function(data) {
  var crc = 0xFFFF;
  for (var i = 0; i < data.length; i++) {
    crc ^= (data.charCodeAt(i) << 8);
    for (var j = 0; j < 8; j++) {
      if (crc & 0x8000) crc = ((crc << 1) ^ 0x1021) & 0xFFFF;
      else crc = (crc << 1) & 0xFFFF;
    }
  }
  return crc;
};

SN_CHECK.valueToChar = function(v) {
  if (v < 10) return String.fromCharCode(48 + v);
  return String.fromCharCode(65 + v - 10);
};

SN_CHECK.computeCheck = function(prefix) {
  if (prefix.length !== 15) return '?';
  var crc = this.crc16(prefix);
  return this.valueToChar(crc % 36);
};

SN_CHECK.validateFormat = function(sn) {
  sn = sn.toUpperCase();
  if (sn.length !== 16) return false;
  if (!/^[A-Z0-9]{16}$/.test(sn)) return false;
  if (!/[A-Z]/.test(sn[0])) return false;
  if (this.validGrades.indexOf(sn[4]) < 0 && !/[0-9]/.test(sn[4])) return false;
  if (!/^[0-9]{3}$/.test(sn.substring(5,8))) return false;
  if (this.yearCodes.indexOf(sn[8]) < 0) return false;
  if (!(sn[9] in this.monthCodes)) return false;
  if (!/^[0-9]{5}$/.test(sn.substring(10,15))) return false;
  return true;
};

SN_CHECK.validate = function(sn) {
  if (!this.validateFormat(sn)) return false;
  sn = sn.toUpperCase();
  var expected = this.computeCheck(sn.substring(0,15));
  return expected === sn[15];
};

SN_CHECK.checkSnInput = function() {
  var input = document.getElementById('sn');
  var sn = input.value.trim().toUpperCase();
  input.value = sn;
  var check = document.getElementById('snCheck');
  if (sn.length === 0) {
    input.className = '';
    check.textContent = '';
    check.className = 'sn-check';
  } else if (sn.length < 16) {
    input.className = '';
    check.textContent = '\u8fd8\u9700 ' + (16-sn.length) + ' \u4f4d';
    check.className = 'sn-check';
  } else if (!SN_CHECK.validateFormat(sn)) {
    input.className = 'invalid';
    check.textContent = '\u2717 \u683c\u5f0f\u9519\u8bef\uff0c\u8bf7\u68c0\u67e5\u5404\u6bb5\u4f4d\u662f\u5426\u6b63\u786e';
    check.className = 'sn-check err';
  } else if (!SN_CHECK.validate(sn)) {
    input.className = 'invalid';
    var expect = SN_CHECK.computeCheck(sn.substring(0,15));
    check.textContent = '\u2717 \u6821\u9a8c\u4f4d\u9519\u8bef\uff0c\u5e94\u4e3a "' + expect + '"\uff0c\u5f53\u524d\u4e3a "' + sn[15] + '"';
    check.className = 'sn-check err';
  } else {
    input.className = 'valid';
    check.textContent = '\u2713 \u5e8f\u5217\u53f7\u6709\u6548';
    check.className = 'sn-check ok';
  }
};

// 绑定 SN 输入框事件
document.getElementById('sn').addEventListener('input', SN_CHECK.checkSnInput.bind(SN_CHECK));

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
  var snInput = document.getElementById('sn').value.trim().toUpperCase();
  var ssid = document.getElementById('ssid').value.trim();
  var pass = document.getElementById('pass').value;

  // 序列号必填校验
  if (!snInput) { showMsg('请输入设备序列号（16位）', false); return; }
  if (snInput.length !== 16) { showMsg('序列号长度必须为 16 位', false); return; }
  if (!SN_CHECK.validateFormat(snInput)) { showMsg('序列号格式错误，请检查各段位', false); return; }
  if (!SN_CHECK.validate(snInput)) { 
    var expect = SN_CHECK.computeCheck(snInput.substring(0,15));
    showMsg('校验位错误，应为 "' + expect + '"，当前为 "' + snInput[15] + '"', false); 
    return; 
  }
  if (!ssid) { showMsg('请输入 WiFi 名称', false); return; }
  document.getElementById('submitBtn').disabled = true;
  document.getElementById('spinner').style.display = 'block';
  showMsg('正在配置...', true);
  try {
    var r = await fetch('/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sn: snInput, ssid: ssid, password: pass })
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
static void send_http_response(WiFiClient& client, int code, const char* type, const char* body) {
  client.printf("HTTP/1.1 %d OK\r\n", code);
  client.printf("Content-Type: %s\r\n", type);
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

// ==================== AP 热点名称配置页面 HTML ====================
static const char AP_CONFIG_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>修改热点名称</title>
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
.current { text-align:center; font-size:13px; color:#666; margin-bottom:16px; padding:10px; 
           background:#f5f5f5; border-radius:8px; }
.current .label { color:#999; font-size:11px; }
.form-group { margin-bottom:16px; }
.form-group label { display:block; font-size:13px; color:#555; margin-bottom:6px; font-weight:500; }
.form-group input { width:100%; padding:12px 14px; border:2px solid #e0e0e0; border-radius:10px;
                    font-size:15px; transition:border .2s; outline:none; }
.form-group input:focus { border-color:#2c5364; }
.form-group .hint { font-size:11px; color:#999; margin-top:4px; }
.btn { width:100%; padding:14px; background:linear-gradient(135deg,#2c5364,#203a43);
       color:#fff; border:none; border-radius:10px; font-size:16px; font-weight:600;
       cursor:pointer; transition:opacity .2s; margin-top:6px; }
.btn:active { opacity:.85; }
.btn:disabled { opacity:.5; cursor:not-allowed; }
.btn.secondary { background:#f5f5f5; color:#666; margin-top:10px; }
.msg { text-align:center; font-size:13px; margin-top:14px; min-height:20px; }
.msg.ok { color:#27ae60; }
.msg.err { color:#e74c3c; }
</style>
</head>
<body>
<div class="card">
  <div class="logo">
    <div class="icon">📡</div>
    <h2>修改热点名称</h2>
  </div>
  <div class="current">
    <div class="label">当前热点名称</div>
    <div id="currentAp">%CURRENT_AP%</div>
  </div>
  <form id="apForm">
    <div class="form-group">
      <label>新热点名称</label>
      <input type="text" id="apSsid" placeholder="请输入热点名称" maxlength="31" required autocomplete="off">
      <div class="hint">留空则使用默认格式 CS_INV_完整SN号，保存后自动重启</div>
    </div>
    <button type="submit" class="btn" id="submitBtn">💾 保存设置</button>
    <button type="button" class="btn secondary" onclick="resetAp()">🔄 恢复默认</button>
  </form>
  <div class="msg" id="msg"></div>
</div>
<script>
function showMsg(text, isOk) {
  var m = document.getElementById('msg');
  m.textContent = text;
  m.className = 'msg ' + (isOk ? 'ok' : 'err');
}

document.getElementById('apForm').onsubmit = async function(e) {
  e.preventDefault();
  var ssid = document.getElementById('apSsid').value.trim();
  document.getElementById('submitBtn').disabled = true;
  showMsg('正在保存...', true);
  try {
    var r = await fetch('/ap_config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ap_ssid: ssid })
    });
    var data = await r.json();
    if (data.result === 'ok') {
      showMsg('✅ ' + (data.message || '保存成功'), true);
      document.getElementById('currentAp').textContent = ssid || 'CS_INV_完整SN号 (默认)';
    } else {
      showMsg('❌ ' + (data.message || '保存失败'), false);
    }
  } catch(e) {
    showMsg('❌ 请求失败', false);
  }
  document.getElementById('submitBtn').disabled = false;
};

async function resetAp() {
  if (!confirm('确定要恢复默认热点名称吗？')) return;
  document.getElementById('submitBtn').disabled = true;
  showMsg('正在恢复...', true);
  try {
    var r = await fetch('/ap_config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ap_ssid: '' })
    });
    var data = await r.json();
    if (data.result === 'ok') {
      showMsg('✅ 已恢复默认设置，重启后生效', true);
      document.getElementById('currentAp').textContent = 'CS_INV_完整SN号 (默认)';
      document.getElementById('apSsid').value = '';
    } else {
      showMsg('❌ ' + (data.message || '恢复失败'), false);
    }
  } catch(e) {
    showMsg('❌ 请求失败', false);
  }
  document.getElementById('submitBtn').disabled = false;
}
</script>
</body>
</html>
)rawliteral";

// ==================== IP 定位 API 配置页面 HTML ====================
static const char IPGEO_CONFIG_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>IP定位API配置</title>
<style>
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif; 
       background:linear-gradient(135deg,#0f2027,#203a43,#2c5364); min-height:100vh;
       display:flex; align-items:center; justify-content:center; }
.card { background:#fff; border-radius:16px; padding:30px 24px; width:92%; max-width:400px;
        box-shadow:0 20px 60px rgba(0,0,0,.3); }
.logo { text-align:center; margin-bottom:8px; }
.logo .icon { font-size:42px; }
.logo h2 { font-size:18px; color:#2c5364; margin-top:4px; }
.current { text-align:center; font-size:12px; color:#666; margin-bottom:16px; padding:10px; 
           background:#f5f5f5; border-radius:8px; word-break:break-all; }
.current .label { color:#999; font-size:11px; }
.form-group { margin-bottom:14px; }
.form-group label { display:block; font-size:13px; color:#555; margin-bottom:6px; font-weight:500; }
.form-group input { width:100%; padding:12px 14px; border:2px solid #e0e0e0; border-radius:10px;
                    font-size:15px; transition:border .2s; outline:none; }
.form-group input:focus { border-color:#2c5364; }
.form-group .hint { font-size:11px; color:#999; margin-top:4px; }
.btn { width:100%; padding:14px; background:linear-gradient(135deg,#2c5364,#203a43);
       color:#fff; border:none; border-radius:10px; font-size:16px; font-weight:600;
       cursor:pointer; transition:opacity .2s; margin-top:6px; }
.btn:active { opacity:.85; }
.btn:disabled { opacity:.5; cursor:not-allowed; }
.btn.secondary { background:#f5f5f5; color:#666; margin-top:10px; }
.msg { text-align:center; font-size:13px; margin-top:14px; min-height:20px; }
.msg.ok { color:#27ae60; }
.msg.err { color:#e74c3c; }
</style>
</head>
<body>
<div class="card">
  <div class="logo">
    <div class="icon">🌍</div>
    <h2>IP 定位 API 配置</h2>
  </div>
  <div class="current">
    <div class="label">当前 API 地址</div>
    <div id="currentUrl">%CURRENT_URL%</div>
  </div>
  <form id="ipgeoForm">
    <div class="form-group">
      <label>API 主机 (Host)</label>
      <input type="text" id="host" placeholder="如 ipapi.co" maxlength="63" required autocomplete="off">
      <div class="hint">例如: ipapi.co, ip-api.com, api.ipify.org</div>
    </div>
    <div class="form-group">
      <label>端口 (Port)</label>
      <input type="number" id="port" placeholder="443" min="1" max="65535" required autocomplete="off">
      <div class="hint">HTTPS 通常使用 443 端口</div>
    </div>
    <div class="form-group">
      <label>请求路径 (Path)</label>
      <input type="text" id="path" placeholder="/json/" maxlength="63" required autocomplete="off">
      <div class="hint">例如: /json/, /api/location</div>
    </div>
    <button type="submit" class="btn" id="submitBtn">💾 保存设置</button>
    <button type="button" class="btn secondary" onclick="resetIpgeo()">🔄 恢复默认</button>
  </form>
  <div class="msg" id="msg"></div>
</div>
<script>
function showMsg(text, isOk) {
  var m = document.getElementById('msg');
  m.textContent = text;
  m.className = 'msg ' + (isOk ? 'ok' : 'err');
}

document.getElementById('ipgeoForm').onsubmit = async function(e) {
  e.preventDefault();
  var host = document.getElementById('host').value.trim();
  var port = parseInt(document.getElementById('port').value);
  var path = document.getElementById('path').value.trim();
  if (!host || !port || !path) { showMsg('请填写完整信息', false); return; }
  document.getElementById('submitBtn').disabled = true;
  showMsg('正在保存...', true);
  try {
    var r = await fetch('/ipgeo_config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host: host, port: port, path: path })
    });
    var data = await r.json();
    if (data.result === 'ok') {
      showMsg('✅ ' + (data.message || '保存成功，无需重启立即生效'), true);
      document.getElementById('currentUrl').textContent = 
        'https://' + host + ':' + port + path;
    } else {
      showMsg('❌ ' + (data.message || '保存失败'), false);
    }
  } catch(e) {
    showMsg('❌ 请求失败', false);
  }
  document.getElementById('submitBtn').disabled = false;
};

async function resetIpgeo() {
  if (!confirm('确定要恢复默认 API 地址吗？')) return;
  document.getElementById('submitBtn').disabled = true;
  showMsg('正在恢复...', true);
  try {
    var r = await fetch('/ipgeo_config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ reset: true })
    });
    var data = await r.json();
    if (data.result === 'ok') {
      showMsg('✅ 已恢复默认设置，立即生效', true);
      document.getElementById('currentUrl').textContent = data.default_url || '';
      document.getElementById('host').value = 'ipapi.co';
      document.getElementById('port').value = '443';
      document.getElementById('path').value = '/json/';
    } else {
      showMsg('❌ ' + (data.message || '恢复失败'), false);
    }
  } catch(e) {
    showMsg('❌ 请求失败', false);
  }
  document.getElementById('submitBtn').disabled = false;
}
</script>
</body>
</html>
)rawliteral";

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
  <div class="hint">访问 <a href="http://192.168.4.1:8888/status">192.168.4.1:8888</a> 查看此页面</div>
</div>
</body>
</html>
)rawliteral";

// ==================== AP 热点名称配置页面处理 ====================
static void handle_ap_config_page(WiFiClient& client) {
  String html = String(FPSTR(AP_CONFIG_HTML));

  // 获取当前热点名称
  char current_ap[AP_SSID_MAX_LEN];
  if (storage_get_ap_ssid(current_ap, sizeof(current_ap))) {
    html.replace("%CURRENT_AP%", String(current_ap) + " (自定义)");
  } else {
    html.replace("%CURRENT_AP%", "CS_INV_完整SN号 (默认)");
  }

  send_http_response(client, 200, "text/html; charset=utf-8", html.c_str());
}

static void handle_ap_config_post(WiFiClient& client, const char* body) {
  char ap_ssid[AP_SSID_MAX_LEN] = {0};

  // 解析 JSON
  const char* key = "\"ap_ssid\":\"";
  const char* p = strstr(body, key);
  if (p) {
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != '"' && i < AP_SSID_MAX_LEN - 1) {
      ap_ssid[i++] = *p++;
    }
    ap_ssid[i] = '\0';
  }

  if (strlen(ap_ssid) == 0) {
    // 恢复默认
    storage_clear_ap_ssid();
    const char* resp = "{\"result\":\"ok\",\"message\":\"已恢复默认热点名称，即将重启\"}";
    send_http_response(client, 200, "application/json", resp);
    Serial.printf("[PROV] AP SSID reset to default, restarting...\n");
  } else if (strlen(ap_ssid) < 2 || strlen(ap_ssid) > 31) {
    const char* resp = "{\"result\":\"error\",\"message\":\"热点名称长度需在 2-31 个字符之间\"}";
    send_http_response(client, 400, "application/json", resp);
    return;  // 不重启
  } else {
    // 保存自定义热点名称
    storage_set_ap_ssid(ap_ssid);
    const char* resp = "{\"result\":\"ok\",\"message\":\"热点名称已保存，即将重启\"}";
    send_http_response(client, 200, "application/json", resp);
    Serial.printf("[PROV] AP SSID set to: %s, restarting...\n", ap_ssid);
  }

  // 等待响应发送完毕后重启
  delay(500);
  ESP.restart();
}

// ==================== IP 定位 API 配置页面处理 ====================
static void handle_ipgeo_config_page(WiFiClient& client) {
  String html = String(FPSTR(IPGEO_CONFIG_HTML));

  // 获取当前配置
  char host[IPGEO_HOST_MAX_LEN];
  uint16_t port;
  char path[IPGEO_PATH_MAX_LEN];
  ipgeo_get_config(host, sizeof(host), &port, path, sizeof(path));

  String current_url = String("https://") + host + ":" + String(port) + path;
  html.replace("%CURRENT_URL%", current_url);

  send_http_response(client, 200, "text/html; charset=utf-8", html.c_str());
}

static void handle_ipgeo_config_post(WiFiClient& client, const char* body) {
  // 检查是否为重置请求
  const char* reset_key = "\"reset\":";
  const char* p_reset = strstr(body, reset_key);
  if (p_reset) {
    // 跳过 key 和空格，找 true
    const char* val = p_reset + strlen(reset_key);
    while (*val == ' ' || *val == '\t') val++;
    if (strncmp(val, "true", 4) == 0) {
      ipgeo_reset_config();
      char resp[256];
      snprintf(resp, sizeof(resp),
        "{\"result\":\"ok\",\"message\":\"已恢复默认\",\"default_url\":\"https://%s:%d%s\"}",
        IPGEO_DEFAULT_HOST, IPGEO_DEFAULT_PORT, IPGEO_DEFAULT_PATH);
      send_http_response(client, 200, "application/json", resp);
      return;
    }
  }

  // 解析 JSON: {"host":"...","port":443,"path":"/json/"}
  auto extract_json_str = [](const char* json, const char* key, char* out, size_t out_len) -> bool {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
      out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
  };

  auto extract_json_int = [](const char* json, const char* key, int* out) -> bool {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    *out = atoi(p);
    return true;
  };

  char host[IPGEO_HOST_MAX_LEN] = {0};
  int port_val = 0;
  char path[IPGEO_PATH_MAX_LEN] = {0};

  bool got_host = extract_json_str(body, "host", host, sizeof(host));
  bool got_port = extract_json_int(body, "port", &port_val);
  bool got_path = extract_json_str(body, "path", path, sizeof(path));

  if (!got_host || !got_port || !got_path || strlen(host) == 0 || 
      port_val < 1 || port_val > 65535 || strlen(path) == 0) {
    const char* resp = "{\"result\":\"error\",\"message\":\"参数不完整或格式错误\"}";
    send_http_response(client, 400, "application/json", resp);
    return;
  }

  if (ipgeo_set_config(host, (uint16_t)port_val, path)) {
    char resp[256];
    snprintf(resp, sizeof(resp),
      "{\"result\":\"ok\",\"message\":\"API 地址已保存，无需重启\"}");
    send_http_response(client, 200, "application/json", resp);
    Serial.printf("[PROV] IPGEO API URL updated: %s:%d%s\n", host, port_val, path);
  } else {
    const char* resp = "{\"result\":\"error\",\"message\":\"保存失败\"}";
    send_http_response(client, 500, "application/json", resp);
  }
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

  // GET /ap_config → 返回 AP 热点名称配置页面
  if (strcmp(method, "GET") == 0 && strcmp(path, "/ap_config") == 0) {
    handle_ap_config_page(client);
    return;
  }

  // POST /ap_config → 设置 AP 热点名称（网页和 App 共用接口）
  if (strcmp(method, "POST") == 0 && strcmp(path, "/ap_config") == 0) {
    handle_ap_config_post(client, body);
    return;
  }

  // GET /ap_info → 获取当前 AP 热点名称（供 App 查询）
  if (strcmp(method, "GET") == 0 && strcmp(path, "/ap_info") == 0) {
    char ap_ssid[AP_SSID_MAX_LEN];
    if (storage_get_ap_ssid(ap_ssid, sizeof(ap_ssid))) {
      char resp[128];
      snprintf(resp, sizeof(resp), "{\"result\":\"ok\",\"ap_ssid\":\"%s\",\"type\":\"custom\"}", ap_ssid);
      send_http_response(client, 200, "application/json", resp);
    } else {
      send_http_response(client, 200, "application/json", "{\"result\":\"ok\",\"ap_ssid\":\"\",\"type\":\"default\"}");
    }
    return;
  }

  // GET /ipgeo_config → 返回 IP 定位 API 配置页面
  if (strcmp(method, "GET") == 0 && strcmp(path, "/ipgeo_config") == 0) {
    handle_ipgeo_config_page(client);
    return;
  }

  // POST /ipgeo_config → 设置 IP 定位 API URL
  if (strcmp(method, "POST") == 0 && strcmp(path, "/ipgeo_config") == 0) {
    handle_ipgeo_config_post(client, body);
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

  if ((strcmp(method, "POST") == 0 && strcmp(path, "/config") == 0) ||
      (strcmp(method, "POST") == 0 && strcmp(path, "/") == 0)) {

    // 提取 body 部分
    const char* json_body = body;

    char ssid[33] = {0};
    char pass[65] = {0};
    char sn[SN_MAX_LEN] = {0};

    // 尝试 JSON 解析
    bool got_ssid = json_extract(json_body, "ssid", ssid, sizeof(ssid));
    bool got_pass = json_extract(json_body, "password", pass, sizeof(pass));
    bool got_sn   = json_extract(json_body, "sn", sn, sizeof(sn));

    // 兼容旧版 JSON API: {"cmd":"set_wifi","ssid":"...","password":"..."}
    char cmd[16] = {0};
    bool got_cmd = json_extract(json_body, "cmd", cmd, sizeof(cmd));
    if (got_cmd && strcmp(cmd, "set_wifi") == 0) {
      got_ssid = json_extract(json_body, "ssid", ssid, sizeof(ssid));
      got_pass = json_extract(json_body, "password", pass, sizeof(pass));
    }

    // ---- SN 校验 (CS-SN-STD-001 V1.1) ----
    if (got_sn && strlen(sn) > 0) {
      if (!sn_validate(sn)) {
        Serial.printf("[PROV] SN REJECTED (invalid): %s\n", sn);
        const char* resp = "{\"result\":\"error\",\"message\":\"序列号无效，请检查校验位\"}";
        send_http_response(client, 400, "application/json", resp);
        return;
      }
      // 存储 SN
      storage_set_sn(sn);
      strncpy(g_device_sn, sn, SN_MAX_LEN - 1);
      g_device_sn[SN_MAX_LEN - 1] = '\0';
      SNFields sf;
      if (sn_parse(sn, &sf)) {
        char readable[32];
        sn_printable(sn, readable, sizeof(readable));
        Serial.printf("[PROV] SN stored (valid): %s | Mfr=%s Country=%s Cust=%s\n",
                      readable, sf.manufacturer, sf.country, sf.customer);
      }
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
