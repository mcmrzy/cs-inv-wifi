#include "tcp_provision.h"
#include "nvs_storage.h"
#include "sn_encoder.h"
#include "ota_manager.h"
#include "arm_ota.h"
#include "led_status.h"
#include "ota_result.h"
#include <string.h>
#include <Arduino.h>
#include <Update.h>

extern char g_device_sn[];

static WiFiServer* g_server = nullptr;
static ProvisionCallback g_cb = nullptr;
static bool g_ota_result_served = false;  // 首次查询后置true

// 多连接支持：同时处理多个 HTTP 请求（浏览器常并行发起多个请求）
#define MAX_HTTP_CLIENTS 4

struct HttpClientSlot {
  WiFiClient client;
  char rx_buf[1024];
  int  rx_len;
  unsigned long last_rx_time;  // 最后收到数据的时间

  void reset() {
    if (client) client.stop();
    rx_len = 0;
    last_rx_time = 0;
  }
  bool active() { return client && client.connected(); }
};

static HttpClientSlot g_slots[MAX_HTTP_CLIENTS];

// 前向声明
static void handle_wifi_scan(WiFiClient& client);
static void handle_status_page(WiFiClient& client);
static void process_http_request(HttpClientSlot& slot);

// ==================== 配网网页 HTML (精简版) ====================
static const char PROVISION_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>逆变器配网</title>
<style>
body{font-family:-apple-system,sans-serif;background:#1a2a3a;color:#fff;margin:0;padding:20px;display:flex;justify-content:center;min-height:100vh}
.c{background:#fff;color:#333;border-radius:12px;padding:24px;width:100%;max-width:360px}
h2{text-align:center;color:#2c5364;margin-bottom:4px}
.sn{text-align:center;font-size:12px;color:#999;margin-bottom:16px}
.btn{width:100%;padding:12px;border:none;border-radius:8px;font-size:15px;cursor:pointer;margin-top:8px}
.btn-p{background:#2c5364;color:#fff}
.btn-s{background:none;border:1px dashed #aaa;color:#666}
.btn:disabled{opacity:.5}
input{width:100%;padding:10px;border:2px solid #ddd;border-radius:8px;font-size:14px;outline:none;box-sizing:border-box}
input:focus{border-color:#2c5364}
label{font-size:13px;color:#555;margin-bottom:4px;display:block}
.msg{text-align:center;font-size:13px;margin-top:10px;min-height:18px}
.ok{color:#27ae60}.err{color:#e74c3c}
.wl{max-height:150px;overflow-y:auto;margin:8px 0;background:#f5f5f5;border-radius:8px}
.wi{padding:10px 12px;cursor:pointer;border-bottom:1px solid #eee;font-size:14px;display:flex}
.wi:last-child{border:none}
.wi:active{background:#e0eef5}
.wi span:last-child{margin-left:auto;font-size:11px;color:#999}
a{display:block;text-align:center;padding:10px;margin-top:12px;color:#2c5364;text-decoration:none;border:1px solid #2c5364;border-radius:8px;font-size:14px}
</style>
</head>
<body>
<div class="c">
<h2>WiFi 配网</h2>
<div class="sn">%DEV_LABEL%</div>
<button class="btn btn-s" id="scanBtn" onclick="scan()">扫描 WiFi</button>
<div class="wl" id="wl" style="display:none"></div>
<form onsubmit="return cfg(this)">
<label>WiFi 名称</label>
<input name="s" placeholder="SSID" required autocomplete="off" style="margin-bottom:8px">
<label>WiFi 密码</label>
<input name="p" type="password" placeholder="密码" autocomplete="off">
<button type="submit" class="btn btn-p" id="sub">保存并连接</button>
</form>
<div class="msg" id="msg"></div>
<a href="/status">设备状态</a>
<a href="/ota">固件升级</a>
</div>
<script>
function msg(t,o){var m=document.getElementById('msg');m.textContent=t;m.className='msg '+(o?'ok':'err')}
async function scan(){
var b=document.getElementById('scanBtn'),l=document.getElementById('wl');
b.disabled=true;b.textContent='扫描中...';l.style.display='none';l.innerHTML='';
try{
var r=await fetch('/scan'),d=await r.json();
if(d.status==='scanning'){await new Promise(function(s){setTimeout(s,3000)});r=await fetch('/scan');d=await r.json()}
if(d.networks&&d.networks.length>0){
d.networks.sort(function(a,b){return b.rssi-a.rssi});
d.networks.forEach(function(n){
var div=document.createElement('div');div.className='wi';
div.innerHTML='<span>'+(n.enc?'\uD83D\uDD12 ':'')+n.ssid+'</span><span>'+n.rssi+'dBm</span>';
div.onclick=function(){document.querySelector('[name=s]').value=n.ssid;document.querySelector('[name=p]').focus()};
l.appendChild(div)});
l.style.display='block';msg('点击名称自动填入',true);
}else{msg('未扫描到WiFi',false)}
}catch(e){msg('扫描失败: '+e,false)}
b.disabled=false;b.textContent='扫描 WiFi'
}
async function cfg(f){
var s=f.s.value.trim(),p=f.p.value;
if(!s){msg('请输入SSID',false);return false}
document.getElementById('sub').disabled=true;msg('连接中...',true);
try{
var r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,password:p})});
var d=await r.json();
if(d.result==='ok'){
var n=0,t=setInterval(async function(){
n++;try{var r=await fetch('/wifi_status'),s=await r.json();
if(s.connected){clearInterval(t);msg('连接成功! '+s.ip,true);setTimeout(function(){location.href='/status'},3000)}
else if(n>=20){clearInterval(t);msg('连接超时，请检查密码',false);document.getElementById('sub').disabled=false}
}catch(e){}
},1000);
}else{msg(d.message||'配置失败',false);document.getElementById('sub').disabled=false}
}catch(e){msg('请求失败',false);document.getElementById('sub').disabled=false}
return false}
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
  <div class="hint">访问 <a href="http://192.168.4.1/status">192.168.4.1</a> 查看此页面</div>
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
      <label>选择固件文件 (.bin)</label>
      <input type="file" id="espFile" accept=".bin" style="padding:8px;border:2px dashed #ccc;border-radius:8px;width:100%">
    </div>
    <button class="btn primary" id="espUploadBtn" onclick="uploadEspFirmware()">📤 上传 ESP 固件</button>
    <div class="progress-bar" id="espProgress"><div class="fill" id="espFill"></div></div>
    <div class="progress-text" id="espProgressText"></div>
    <div class="msg" id="espMsg"></div>
  </div>

  <!-- ARM 固件升级 -->
  <div class="section">
    <h3>ARM 芯片升级 <span class="tag">GD32F303</span></h3>
    <div class="form-group">
      <label>选择固件文件 (.bin)</label>
      <input type="file" id="armFile" accept=".bin" style="padding:8px;border:2px dashed #ccc;border-radius:8px;width:100%">
    </div>
    <button class="btn arm" id="armUploadBtn" onclick="uploadArmFirmware()">📤 上传 ARM 固件</button>
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

function uploadFirmware(fileInputId, target, msgId, btnId) {
  var fileInput = document.getElementById(fileInputId);
  if (!fileInput.files || fileInput.files.length === 0) {
    showMsg(msgId, '请选择固件文件', false);
    return;
  }
  var file = fileInput.files[0];
  if (!file.name.endsWith('.bin')) {
    showMsg(msgId, '请选择 .bin 文件', false);
    return;
  }
  if (file.size > (target === 'esp' ? 2097152 : 262144)) {
    showMsg(msgId, '文件过大 (ESP: 2MB, ARM: 256KB)', false);
    return;
  }
  document.getElementById(btnId).disabled = true;
  showMsg(msgId, '正在上传: 0%', true);
  updateProgress(target, 0, 'downloading');

  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/ota/upload', true);

  // ESP OTA 使用原始二进制流式上传（避免 multipart 解析）
  // ARM OTA 仍使用 multipart/form-data
  if (target === 'esp') {
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.upload.onprogress = function(e) {
      if (e.lengthComputable) {
        var percent = Math.round((e.loaded / e.total) * 100);
        updateProgress(target, percent, 'downloading');
        showMsg(msgId, '正在上传: ' + percent + '%', true);
      }
    };
    xhr.onload = function() {
      if (xhr.status === 200) {
        showMsg(msgId, '上传完成，正在写入...', true);
        updateProgress(target, 100, 'verifying');
        pollProgress(target);
      } else {
        showMsg(msgId, '❌ 上传失败: ' + xhr.responseText, false);
        document.getElementById(btnId).disabled = false;
      }
    };
    xhr.onerror = function() {
      showMsg(msgId, '❌ 网络错误，上传失败', false);
      document.getElementById(btnId).disabled = false;
    };
    xhr.send(file);
  } else {
    // ARM OTA: multipart/form-data（保持不变）
    var formData = new FormData();
    formData.append('file', file);
    formData.append('target', target);

    xhr.upload.onprogress = function(e) {
      if (e.lengthComputable) {
        var percent = Math.round((e.loaded / e.total) * 100);
        updateProgress(target, percent, 'downloading');
        showMsg(msgId, '正在上传: ' + percent + '%', true);
      }
    };
    xhr.onload = function() {
      if (xhr.status === 200) {
        showMsg(msgId, '上传完成，正在写入...', true);
        updateProgress(target, 100, 'verifying');
        pollProgress(target);
      } else {
        showMsg(msgId, '❌ 上传失败: ' + xhr.responseText, false);
        document.getElementById(btnId).disabled = false;
      }
    };
    xhr.onerror = function() {
      showMsg(msgId, '❌ 网络错误，上传失败', false);
      document.getElementById(btnId).disabled = false;
    };
    xhr.send(formData);
  }
}

function uploadEspFirmware() {
  uploadFirmware('espFile', 'esp', 'espMsg', 'espUploadBtn');
}

function uploadArmFirmware() {
  uploadFirmware('armFile', 'arm', 'armMsg', 'armUploadBtn');
}
</script>
</body>
</html>
)rawliteral";

// OTA 上传大缓冲区（避免栈溢出，ESP32-C3 栈空间有限）
static uint8_t g_ota_stream_buf[512];

// ==================== OTA 页面处理 ====================
static void handle_ota_page(WiFiClient& client) {
  String html = String(FPSTR(OTA_HTML));
  html.replace("%SN%", g_device_sn);
  html.replace("%ESP_VER%", ota_get_esp_version());
  html.replace("%ARM_VER%", ota_get_arm_version());
  html.replace("%HEAP%", String(ESP.getFreeHeap() / 1024) + " KB");
  send_http_response(client, 200, "text/html; charset=utf-8", html.c_str());
}

// GET /ota/progress - 查询 OTA 进度
static void handle_ota_progress(WiFiClient& client) {
  char resp[256];

  // 首次请求时检查NVS是否有保存的OTA结果（ESP重启后）
  if (!g_ota_result_served) {
    OtaResult saved;
    if (ota_result_load(&saved)) {
      snprintf(resp, sizeof(resp),
               "{\"target\":\"esp\",\"progress\":100,\"status\":\"%s\",\"message\":\"%s\",\"version\":\"%s\"}",
               saved.status, saved.message, saved.version);
      send_http_response(client, 200, "application/json", resp);
      ota_result_clear();
      g_ota_result_served = true;
      return;
    }
    g_ota_result_served = true;
  }

  // 正常返回实时进度
  const char* target_str = "none";
  const char* status_str = "idle";

  if (ota_is_busy()) {
    target_str = (ota_get_target() == OTA_TARGET_ESP) ? "esp" : "arm";
    switch (ota_get_status()) {
      case OTA_STATUS_DOWNLOADING: status_str = "downloading"; break;
      case OTA_STATUS_WRITING:     status_str = "uploading"; break;
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

// POST /ota/upload - 上传固件文件
// ESP OTA: 原始二进制流式接收（Content-Type: application/octet-stream）
// ARM OTA: multipart/form-data（保持兼容性）
static void handle_ota_upload(HttpClientSlot& slot) {
  WiFiClient& client = slot.client;
  char* buf = slot.rx_buf;
  int buf_len = slot.rx_len;

  // 检查是否正在OTA
  if (ota_is_busy()) {
    send_http_response(client, 409, "application/json",
                       "{\"result\":\"error\",\"message\":\"OTA 正在进行中\"}");
    return;
  }

  // 从缓冲区获取 Content-Length 和 Content-Type
  char* cl = strstr(buf, "Content-Length:");
  int content_length = 0;
  if (cl) {
    content_length = atoi(cl + 15);
    Serial.printf("[OTA-UPLOAD] Raw Content-Length string: '%.20s', parsed: %d\n", cl + 15, content_length);
  } else {
    Serial.println("[OTA-UPLOAD] No Content-Length found!");
  }

  char* ct = strstr(buf, "Content-Type:");
  if (!ct) {
    Serial.println("[OTA-UPLOAD] No Content-Type, returning 400");
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"缺少 Content-Type\"}");
    return;
  }

  // 打印 Content-Type 便于调试
  char ct_line[128];
  strncpy(ct_line, ct, sizeof(ct_line) - 1);
  ct_line[sizeof(ct_line) - 1] = '\0';
  char* eol = strchr(ct_line, '\r');
  if (eol) *eol = '\0';
  Serial.printf("[OTA-UPLOAD] Content-Type: %s\n", ct_line);

  // 判断是否为 ESP OTA（原始二进制）
  bool is_esp_raw = (strstr(ct, "application/octet-stream") != nullptr);
  Serial.printf("[OTA-UPLOAD] is_esp_raw=%d\n", is_esp_raw);

  // ========== ESP OTA: 原始二进制流式接收 ==========
  if (is_esp_raw) {
    Serial.printf("[OTA-UPLOAD] Checking size: %d (valid: %d, max: %d)\n",
                  content_length, content_length > 0, content_length <= 1048576);
    if (content_length <= 0 || content_length > 2097152) {
      Serial.println("[OTA-UPLOAD] Invalid size, returning 400");
      send_http_response(client, 400, "application/json",
                         "{\"result\":\"error\",\"message\":\"无效的文件大小\"}");
      return;
    }

    Serial.printf("[OTA-UPLOAD] ESP raw binary, size=%d\n", content_length);

    // 上报OTA开始状态（更新OTA管理器进度）
    ota_set_status(OTA_STATUS_DOWNLOADING, 0, "准备升级");

    if (!Update.begin(content_length)) {
      send_http_response(client, 500, "application/json",
                         "{\"result\":\"error\",\"message\":\"Update 初始化失败\"}");
      return;
    }

    led1_set(LED_FAST_BLINK);

    int written = 0;
    int header_len = 0;
    char* body_start = strstr(buf, "\r\n\r\n");
    if (body_start) {
      header_len = (body_start - buf) + 4;
      body_start += 4;
      int body_in_buf = buf_len - header_len;
      if (body_in_buf > 0) {
        size_t w = Update.write((uint8_t*)body_start, body_in_buf);
        written += w;
        Serial.printf("[OTA-UPLOAD] Wrote %d bytes from header buffer\n", w);
      }
    }

    int remaining = content_length - (buf_len - header_len);
    if (remaining < 0) remaining = 0;

    unsigned long timeout_start = millis();
    int last_progress = 0;

    while (written < content_length && (millis() - timeout_start) < 60000) {
      if (client.available()) {
        timeout_start = millis();
        int to_read = min((int)client.available(), content_length - written);
        if (to_read > (int)sizeof(g_ota_stream_buf)) to_read = sizeof(g_ota_stream_buf);
        int bytes_read = client.read(g_ota_stream_buf, to_read);
        if (bytes_read > 0) {
          size_t w = Update.write(g_ota_stream_buf, bytes_read);
          written += w;
          // 每10%上报一次进度
          int progress = (written * 100) / content_length;
          if (progress >= last_progress + 10) {
            last_progress = progress;
            ota_set_status(OTA_STATUS_WRITING, progress, 
              String("写入中: " + String(progress) + "%").c_str());
          }
        }
      } else {
        delay(1);
      }
    }

    Serial.printf("[OTA-UPLOAD] Total written: %d/%d bytes\n", written, content_length);

    if (written <= 0) {
      Update.abort();
      led1_set(LED_OFF);
      send_http_response(client, 500, "application/json",
                         "{\"result\":\"error\",\"message\":\"未接收到有效数据\"}");
      return;
    }

    if (!Update.end(true)) {
      led1_set(LED_OFF);
      ota_set_status(OTA_STATUS_ERROR, 0, "固件校验失败");
      send_http_response(client, 500, "application/json",
                         "{\"result\":\"error\",\"message\":\"校验失败\"}");
      return;
    }

    led1_set(LED_OFF);
    ota_set_status(OTA_STATUS_DONE, 100, "ESP升级成功，重启中");
    // 保存结果到NVS，重启后APP轮询时可读取
    ota_result_save("done", FIRMWARE_VERSION, "ESP升级完成");
    send_http_response(client, 200, "application/json",
                       "{\"result\":\"ok\",\"message\":\"ESP OTA 成功，重启中\"}");
    delay(500);
    ESP.restart();
    return;
  }

  // ========== ARM OTA: multipart/form-data ==========
  // 查找 boundary（使用静态缓冲区避免栈溢出）
  static char s_boundary[128];
  static char s_boundary_delim[132];  // 分隔符: --boundary
  static char s_boundary_end[134];    // 结束分隔符: \r\n--boundary
  char* bp = strstr(ct, "boundary=");
  int boundary_len = 0;
  if (bp) {
    bp += 9;
    char* be = strchr(bp, '\r');
    if (!be) be = strchr(bp, '\n');
    if (be) {
      size_t blen = be - bp;
      if (blen < sizeof(s_boundary)) {
        memcpy(s_boundary, bp, blen);
        s_boundary[blen] = '\0';
        snprintf(s_boundary_delim, sizeof(s_boundary_delim), "--%s", s_boundary);
        boundary_len = strlen(s_boundary_delim);
      }
    }
  }

  if (boundary_len == 0) {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"缺少 boundary\"}");
    return;
  }

  // 查找 body 开始位置
  char* body_start = strstr(buf, "\r\n\r\n");
  if (!body_start) {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"无效的请求格式\"}");
    return;
  }
  body_start += 4;

  int header_len_arm = body_start - buf;
  int body_in_buf = buf_len - header_len_arm;

  // 在缓冲区中查找第一个 boundary
  char* first_boundary = NULL;
  for (int i = 0; i <= body_in_buf - boundary_len; i++) {
    if (memcmp(body_start + i, s_boundary_delim, boundary_len) == 0) {
      first_boundary = body_start + i;
      break;
    }
  }

  if (!first_boundary) {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"找不到 boundary\"}");
    return;
  }

  char* part_start = first_boundary + boundary_len;
  if (memcmp(part_start, "\r\n", 2) == 0) part_start += 2;

  // 提取 target 字段
  char target[8] = "esp";
  char* target_field = strstr(part_start, "name=\"target\"");
  if (target_field) {
    char* val_start = strstr(target_field, "\r\n\r\n");
    if (val_start) {
      val_start += 4;
      char* val_end = strstr(val_start, "\r\n--");
      if (val_end) {
        size_t vlen = val_end - val_start;
        if (vlen < sizeof(target)) {
          memcpy(target, val_start, vlen);
          target[vlen] = '\0';
        }
      }
    }
  }

  char* file_part = strstr(part_start, "name=\"file\"");
  if (!file_part) {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"缺少 file 字段\"}");
    return;
  }

  char* file_data_start = strstr(file_part, "\r\n\r\n");
  if (!file_data_start) {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"无效的 multipart 格式\"}");
    return;
  }
  file_data_start += 4;

  Serial.printf("[OTA-UPLOAD] target=%s, content_length=%d\n", target, content_length);

  if (strcmp(target, "esp") != 0 && strcmp(target, "arm") != 0) {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"无效的 target\"}");
    return;
  }

  int max_size = (strcmp(target, "esp") == 0) ? 1048576 : 262144;
  int estimated_size = content_length - (file_data_start - buf) - boundary_len - 10;
  if (estimated_size <= 0 || estimated_size > max_size) {
    send_http_response(client, 400, "application/json",
                       "{\"result\":\"error\",\"message\":\"无效的文件大小\"}");
    return;
  }

  bool is_esp = (strcmp(target, "esp") == 0);

  // 上报OTA开始状态
  ota_set_status(is_esp ? OTA_STATUS_DOWNLOADING : OTA_STATUS_WRITING, 0, 
    is_esp ? "准备ESP升级" : "准备ARM升级");

  if (is_esp) {
    if (!Update.begin(estimated_size)) {
      send_http_response(client, 500, "application/json",
                         "{\"result\":\"error\",\"message\":\"Update 初始化失败\"}");
      return;
    }
  } else {
    if (!arm_ota_start_streaming(estimated_size)) {
      send_http_response(client, 500, "application/json",
                         "{\"result\":\"error\",\"message\":\"ARM OTA 初始化失败\"}");
      return;
    }
  }

  led1_set(LED_FAST_BLINK);

  int written = 0;
  int buf_remaining = body_in_buf - (file_data_start - body_start);
  if (buf_remaining > 0) {
    snprintf(s_boundary_end, sizeof(s_boundary_end), "\r\n--%s", s_boundary);
    const char* boundary_end = s_boundary_end;
    int be_len = strlen(boundary_end);

    int data_len = buf_remaining;
    for (int i = 0; i <= buf_remaining - be_len; i++) {
      if (memcmp(file_data_start + i, boundary_end, be_len) == 0) {
        data_len = i;
        break;
      }
    }

    if (data_len > 0) {
      size_t w;
      if (is_esp) {
        w = Update.write((uint8_t*)file_data_start, data_len);
      } else {
        w = arm_ota_feed((uint8_t*)file_data_start, data_len) ? data_len : 0;
      }
      written += w;
      Serial.printf("[OTA-UPLOAD] Wrote %d bytes from buffer\n", w);
    }
  }

  int remaining = content_length - buf_len;
  unsigned long timeout_start = millis();
  int stream_buf_len = 0;
  bool found_end = false;
  int last_progress = 0;
  int total_size = estimated_size;

  while (remaining > 0 && !found_end && (millis() - timeout_start) < 60000) {
    if (client.available()) {
      timeout_start = millis();
      int to_read = min((int)client.available(), (int)sizeof(g_ota_stream_buf) - stream_buf_len);
      int read = client.read(g_ota_stream_buf + stream_buf_len, to_read);
      if (read > 0) {
        stream_buf_len += read;
        remaining -= read;

        for (int i = 0; i <= stream_buf_len - boundary_len; i++) {
          if (memcmp(g_ota_stream_buf + i, s_boundary_delim, boundary_len) == 0) {
            if (i > 0) {
              if (is_esp) {
                Update.write(g_ota_stream_buf, i);
              } else {
                arm_ota_feed(g_ota_stream_buf, i);
              }
              written += i;
              // 更新进度
              if (total_size > 0) {
                int progress = (written * 100) / total_size;
                if (progress >= last_progress + 10) {
                  last_progress = progress;
                  ota_set_status(OTA_STATUS_WRITING, progress,
                    String(String(is_esp ? "ESP" : "ARM") + " 写入中: " + String(progress) + "%").c_str());
                }
              }
            }
            found_end = true;
            break;
          }
        }

        if (!found_end && stream_buf_len >= (int)sizeof(g_ota_stream_buf)) {
          int safe_len = stream_buf_len - boundary_len;
          if (safe_len > 0) {
            if (is_esp) {
              Update.write(g_ota_stream_buf, safe_len);
            } else {
              arm_ota_feed(g_ota_stream_buf, safe_len);
            }
            written += safe_len;
            memmove(g_ota_stream_buf, g_ota_stream_buf + safe_len, stream_buf_len - safe_len);
            stream_buf_len -= safe_len;
          }
        }
      }
    } else {
      delay(1);
      if (!is_esp) arm_ota_process();
    }
  }

  Serial.printf("[OTA-UPLOAD] Total written: %d bytes\n", written);

  if (written <= 0) {
    if (is_esp) Update.abort();
    else arm_ota_cancel();
    led1_set(LED_OFF);
    send_http_response(client, 500, "application/json",
                       "{\"result\":\"error\",\"message\":\"未接收到有效数据\"}");
    return;
  }

  if (is_esp) {
    if (!Update.end(true)) {
      led1_set(LED_OFF);
      ota_set_status(OTA_STATUS_ERROR, 0, "ESP固件校验失败");
      send_http_response(client, 500, "application/json",
                         "{\"result\":\"error\",\"message\":\"校验失败\"}");
      return;
    }
    led1_set(LED_OFF);
    ota_set_status(OTA_STATUS_DONE, 100, "ESP升级成功，重启中");
    send_http_response(client, 200, "application/json",
                       "{\"result\":\"ok\",\"message\":\"ESP OTA 成功，重启中\"}");
    delay(500);  // 等HTTP响应发出
    ESP.restart();
  } else {
    arm_ota_end_streaming();
    led1_set(LED_OFF);
    ota_set_status(OTA_STATUS_VERIFYING, 100, "ARM固件已传输，等待校验");
    send_http_response(client, 200, "application/json",
                       "{\"result\":\"ok\",\"message\":\"ARM 固件已传输，等待校验\"}");
  }
}

// ==================== WiFi 扫描（供网页调用） ====================
static void handle_wifi_scan(WiFiClient& client) {
  Serial.println("[PROV] WiFi scan requested");

  // 确保 WiFi 处于 AP_STA 模式才能扫描（纯 AP 模式无法扫描）
  int current_mode = WiFi.getMode();
  Serial.printf("[PROV] Current WiFi mode: %d\n", current_mode);
  
  if (current_mode != WIFI_AP_STA && current_mode != WIFI_STA) {
    Serial.printf("[PROV] Switching from mode %d to AP_STA mode for scanning\n", current_mode);
    WiFi.mode(WIFI_AP_STA);
    delay(200);
    Serial.printf("[PROV] Mode after switch: %d\n", WiFi.getMode());
  }

  // 强制清理之前的扫描结果，确保扫描状态干净
  // scanDelete() 会清理所有扫描结果并重置扫描状态
  int prev = WiFi.scanComplete();
  Serial.printf("[PROV] Previous scan state: %d\n", prev);
  
  // 无论之前状态如何，都尝试清理
  if (prev != WIFI_SCAN_FAILED) {
    WiFi.scanDelete();
    Serial.println("[PROV] Deleted previous scan result");
  }
  delay(200);  // 增加延迟，确保WiFi驱动状态稳定

  // 同步扫描（阻塞约 2-3 秒，配网阶段可接受）
  Serial.println("[PROV] Starting WiFi scan...");
  int n = WiFi.scanNetworks(false, true);  // sync, show hidden
  Serial.printf("[PROV] Scan completed, result=%d\n", n);
  
  if (n <= 0) {
    // 扫描失败，清理状态并重试一次
    Serial.println("[PROV] Scan failed, cleaning up and retrying...");
    WiFi.scanDelete();
    delay(500);  // 增加延迟
    n = WiFi.scanNetworks(false, true);
    Serial.printf("[PROV] Retry scan result=%d\n", n);
    
    if (n <= 0) {
      // 重试也失败，尝试重置WiFi模式
      Serial.println("[PROV] Retry failed, resetting WiFi mode...");
      WiFi.mode(WIFI_OFF);
      delay(200);
      WiFi.mode(WIFI_AP_STA);
      delay(500);
      WiFi.scanDelete();
      delay(200);
      n = WiFi.scanNetworks(false, true);
      Serial.printf("[PROV] Second retry scan result=%d\n", n);
      
      if (n <= 0) {
        send_http_response(client, 200, "application/json", "{\"networks\":[]}");
        Serial.println("[PROV] WiFi scan failed after all retries");
        return;
      }
    }
  }

  String json = "{\"networks\":[";
  for (int i = 0; i < n && i < 30; i++) {
    if (i > 0) json += ",";
    // 转义 SSID 中的特殊字符
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i));
    json += ",\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0) + "}";
  }
  json += "]}";
  WiFi.scanDelete();
  send_http_response(client, 200, "application/json", json.c_str());
  Serial.printf("[PROV] Scan result sent: %d networks\n", n);
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
static void process_http_request(HttpClientSlot& slot) {
  WiFiClient& client = slot.client;
  char* buf = slot.rx_buf;

  // 解析第一行: METHOD /path HTTP/1.1
  char method[16] = {0};
  char path[128] = {0};

  char* line_end = strstr(buf, "\r\n");
  if (!line_end) return;

  // 解析 METHOD 和 PATH
  char* sp1 = strchr(buf, ' ');
  if (!sp1) return;
  size_t mlen = sp1 - buf;
  if (mlen >= sizeof(method)) mlen = sizeof(method) - 1;
  strncpy(method, buf, mlen);

  char* sp2 = strchr(sp1 + 1, ' ');
  if (!sp2) return;
  size_t plen = sp2 - sp1 - 1;
  if (plen >= sizeof(path)) plen = sizeof(path) - 1;
  strncpy(path, sp1 + 1, plen);

  url_decode(path);

  Serial.printf("[HTTP] %s %s\n", method, path);

  // 找 body（\r\n\r\n 之后）
  const char* body = strstr(buf, "\r\n\r\n");
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

  // POST /ota/upload → 上传固件文件
  if (strcmp(method, "POST") == 0 && strcmp(path, "/ota/upload") == 0) {
    handle_ota_upload(slot);
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
  for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
    g_slots[i].reset();
  }
  g_server = new WiFiServer(PROVISION_TCP_PORT);
  g_server->begin();
  g_server->setNoDelay(true);
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
  for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
    g_slots[i].reset();
  }
}

// ==================== 主循环 ====================
void tcp_provision_process() {
  if (!g_server) return;

  // 1. 接受新连接，分配到空闲 slot
  while (g_server->hasClient()) {
    WiFiClient new_client = g_server->available();
    if (!new_client) break;

    // 找空闲 slot
    int free_slot = -1;
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
      if (!g_slots[i].active()) {
        free_slot = i;
        break;
      }
    }

    if (free_slot >= 0) {
      g_slots[free_slot].client = new_client;
      g_slots[free_slot].client.setTimeout(5);
      g_slots[free_slot].rx_len = 0;
      g_slots[free_slot].last_rx_time = millis();
    } else {
      // 所有 slot 满，拒绝新连接
      new_client.stop();
    }
  }

  // 2. 处理每个 slot 的数据
  for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
    HttpClientSlot& slot = g_slots[i];
    if (!slot.active()) continue;

    // 读取可用数据
    while (slot.client.available() && slot.rx_len < (int)sizeof(slot.rx_buf) - 1) {
      char c = slot.client.read();
      slot.rx_buf[slot.rx_len++] = c;
      slot.last_rx_time = millis();

      // HTTP 请求结束标志: \r\n\r\n
      if (slot.rx_len >= 4 && strncmp(slot.rx_buf + slot.rx_len - 4, "\r\n\r\n", 4) == 0) {
        slot.rx_buf[slot.rx_len] = '\0';
        // 检查 Content-Length 是否需要继续读 body
        char* cl = strstr(slot.rx_buf, "Content-Length:");
        int body_len = 0;
        if (cl) body_len = atoi(cl + 15);

        char* body_start = strstr(slot.rx_buf, "\r\n\r\n") + 4;
        int header_len = body_start - slot.rx_buf;
        int received_body = slot.rx_len - header_len;

        // 如果 body 还没收完，继续读
        if (received_body < body_len) {
          // 保护：OTA 上传等大 body 请求，缓冲区放不下，交给处理函数流式读取
          // 只检查 body 是否会溢出缓冲区（header + 完整body > 缓冲区容量）
          int max_buf = (int)sizeof(slot.rx_buf) - 1;
          if (body_len > max_buf) {
            // 大 body 请求，不预读，交给 handle_ota_upload 等函数流式处理
          } else {
            int space_left = max_buf - slot.rx_len;
            int needed = body_len - received_body;
            if (needed > space_left) {
              // 缓冲区不够，截断到缓冲区满，交给处理函数继续流式读
              needed = space_left;
            }
            unsigned long start = millis();
            int got = 0;
            while (got < needed && (millis() - start) < 2000) {
              if (slot.client.available()) {
                char b = slot.client.read();
                slot.rx_buf[slot.rx_len++] = b;
                got++;
              } else {
                delay(1);
              }
            }
          }
        }

        slot.rx_buf[slot.rx_len] = '\0';
        Serial.printf("[HTTP] Calling process_http_request for slot %d\n", i);
        process_http_request(slot);
        Serial.printf("[HTTP] process_http_request returned for slot %d\n", i);
        slot.client.flush();
        slot.reset();
        break;  // 当前 slot 处理完毕，继续下一个
      }
    }

    // 超时保护：10 秒无新数据则断开
    if (slot.rx_len > 0 && !slot.client.available()) {
      if (millis() - slot.last_rx_time > 10000) {
        slot.reset();
      }
    }

    // 连接异常检测
    if (slot.active() && !slot.client.connected()) {
      slot.reset();
    }
  }
}
