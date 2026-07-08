/**
 * tcp_provision.c — TCP 配网服务器 (ESP-IDF lwIP BSD socket)
 *
 * 在 AP 模式下提供 HTTP 配网页面，支持 WiFi 扫描、配置和 OTA 升级
 */

#include "tcp_provision.h"
#include "nvs_storage.h"
#include "sn_encoder.h"
#include "led_status.h"
#include "arm_ota.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "PROV";

extern char g_device_sn[];

static int g_server_fd = -1;
static ProvisionCallback g_cb = NULL;

#define MAX_HTTP_CLIENTS 2

struct HttpClientSlot {
    int fd;
    char rx_buf[2048];
    int rx_len;
    int64_t last_rx_time;
};

static struct HttpClientSlot g_slots[MAX_HTTP_CLIENTS];

// ==================== HTML 模板 ====================

static const char PROVISION_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>逆变器配网</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,sans-serif;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);min-height:100vh;display:flex;align-items:center;justify-content:center}
.card{background:#fff;border-radius:16px;padding:30px 24px;width:92%;max-width:380px;box-shadow:0 20px 60px rgba(0,0,0,.3)}
.logo{text-align:center;margin-bottom:8px}
.logo .icon{font-size:42px}
.logo h2{font-size:18px;color:#2c5364;margin-top:4px}
.sn{text-align:center;font-size:12px;color:#999;margin-bottom:20px}
.form-group{margin-bottom:16px}
.form-group label{display:block;font-size:13px;color:#555;margin-bottom:6px;font-weight:500}
.form-group input{width:100%;padding:12px 14px;border:2px solid #e0e0e0;border-radius:10px;font-size:15px;transition:border .2s;outline:none}
.form-group input:focus{border-color:#2c5364}
.btn{width:100%;padding:14px;background:linear-gradient(135deg,#2c5364,#203a43);color:#fff;border:none;border-radius:10px;font-size:16px;font-weight:600;cursor:pointer;transition:opacity .2s;margin-top:6px}
.btn:active{opacity:.85}
.btn:disabled{opacity:.5;cursor:not-allowed}
.msg{text-align:center;font-size:13px;margin-top:14px;min-height:20px}
.msg.ok{color:#27ae60}
.msg.err{color:#e74c3c}
.hint{text-align:center;font-size:11px;color:#bbb;margin-top:12px}
.spinner{display:none;width:20px;height:20px;border:3px solid #ddd;border-top-color:#2c5364;border-radius:50%;animation:spin .7s linear infinite;margin:10px auto 0}
@keyframes spin{to{transform:rotate(360deg)}}
.wifi-list{margin-bottom:14px;max-height:160px;overflow-y:auto;background:#f9f9f9;border-radius:8px;padding:4px}
.wifi-item{display:flex;align-items:center;padding:10px 12px;cursor:pointer;border-bottom:1px solid #eee;font-size:14px;color:#333}
.wifi-item:last-child{border-bottom:none}
.wifi-item:active{background:#e0eef5}
.wifi-item .rssi{margin-left:auto;font-size:11px;color:#999}
.scan-btn{width:100%;padding:10px;background:none;border:1px dashed #aaa;border-radius:8px;color:#777;font-size:13px;cursor:pointer;margin-bottom:14px}
.scan-btn:disabled{opacity:.5}
.status-link{display:block;text-align:center;padding:12px;margin-top:16px;background:linear-gradient(135deg,#f8f9fa,#e9ecef);color:#2c5364;font-size:14px;font-weight:500;text-decoration:none;border:2px solid #2c5364;border-radius:10px}
.status-link:active{background:#e0eef5}
</style>
</head>
<body>
<div class="card">
<div class="logo"><div class="icon">☀️</div><h2>辰烁科技逆变器 WiFi 配网</h2></div>
<div class="sn" id="snLine">%DEV_LABEL%</div>
<button class="scan-btn" id="scanBtn" onclick="scanWiFi()">📡 扫描附近 WiFi</button>
<div class="wifi-list" id="wifiList" style="display:none"></div>
<form id="configForm">
<div class="form-group"><label>WiFi 名称 (SSID)</label><input type="text" id="ssid" placeholder="请输入 WiFi 名称" required autocomplete="off"></div>
<div class="form-group"><label>WiFi 密码</label><input type="password" id="pass" placeholder="请输入 WiFi 密码" autocomplete="off"></div>
<button type="submit" class="btn" id="submitBtn">⚡ 保存并连接</button>
</form>
<div class="spinner" id="spinner"></div>
<div class="msg" id="msg"></div>
<a href="/status" class="status-link">📊 查看网络连接状态</a>
<a href="/ota" class="status-link" style="margin-top:8px;background:linear-gradient(135deg,#fff3e0,#ffe0b2);border-color:#e65100;color:#e65100">🔄 固件升级</a>
<div class="hint">也可通过 辰烁科技App 配网</div>
</div>
<script>
function showMsg(t,o){var m=document.getElementById('msg');m.textContent=t;m.className='msg '+(o?'ok':'err')}
async function scanWiFi(){
var b=document.getElementById('scanBtn'),l=document.getElementById('wifiList');
b.disabled=true;b.textContent='\u23f3 正在扫描...';showMsg('WiFi 扫描中，请稍候...',true);
l.style.display='none';l.innerHTML='';
try{var r=await fetch('/scan'),d=await r.json();
if(d.status==='scanning'){await new Promise(function(s){setTimeout(s,2500)});r=await fetch('/scan');d=await r.json()}
if(d.networks&&d.networks.length>0){
d.networks.sort(function(a,b){return b.rssi-a.rssi});
d.networks.forEach(function(n){
var div=document.createElement('div');div.className='wifi-item';
div.innerHTML='<span>'+(n.enc?'🔒 ':'')+n.ssid+'</span><span class="rssi">'+n.rssi+'dBm</span>';
div.onclick=function(){document.getElementById('ssid').value=n.ssid;document.getElementById('pass').focus()};
l.appendChild(div)});l.style.display='block';showMsg('点击 WiFi 名称自动填入',true)
}else{showMsg('未扫描到 WiFi，请手动输入',false)}
}catch(e){showMsg('扫描失败，请手动输入',false)}
b.disabled=false;b.textContent='📡 扫描附近 WiFi'}
document.getElementById('configForm').onsubmit=async function(e){
e.preventDefault();var ssid=document.getElementById('ssid').value.trim(),pass=document.getElementById('pass').value;
if(!ssid){showMsg('请输入 WiFi 名称',false);return}
document.getElementById('submitBtn').disabled=true;document.getElementById('spinner').style.display='block';
showMsg('正在配置...',true);
try{var r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,password:pass})}),d=await r.json();
if(d.result==='ok'){showMsg('✅ 配置成功! 正在检查 WiFi 连接...',true);
var c=0,i=setInterval(async function(){c++;try{var resp=await fetch('/wifi_status'),s=await resp.json();
if(s.connected){clearInterval(i);showMsg('✅ WiFi 连接成功! SSID: '+s.ssid+' IP: '+s.ip,true);
setTimeout(function(){window.location.href='/status'},3000)}
else if(c>=20){clearInterval(i);showMsg('⚠ WiFi 连接超时，请检查密码是否正确',false);
document.getElementById('submitBtn').disabled=false;document.getElementById('spinner').style.display='none'}}
catch(e){}},1000)
}else{showMsg('❌ '+(d.message||'配置失败'),false);document.getElementById('submitBtn').disabled=false;
document.getElementById('spinner').style.display='none'}
}catch(e){showMsg('❌ 请求失败',false);document.getElementById('submitBtn').disabled=false;
document.getElementById('spinner').style.display='none'}}};
</script>
</body></html>
)rawliteral";

static const char STATUS_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>设备状态</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,sans-serif;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.card{background:#fff;border-radius:16px;padding:24px;width:92%;max-width:400px;box-shadow:0 20px 60px rgba(0,0,0,.3)}
.header{text-align:center;margin-bottom:20px}
.header .icon{font-size:36px}
.header h2{font-size:18px;color:#2c5364;margin-top:4px}
.si{display:flex;justify-content:space-between;padding:12px 0;border-bottom:1px solid #eee;font-size:14px}
.si:last-child{border-bottom:none}
.si .l{color:#666}
.si .v{color:#333;font-weight:500}
.si .v.ok{color:#27ae60}
.si .v.err{color:#e74c3c}
.si .v.warn{color:#f39c12}
.btn{width:100%;padding:12px;background:linear-gradient(135deg,#2c5364,#203a43);color:#fff;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer;margin-top:16px;text-decoration:none;display:inline-block;text-align:center}
.btn:active{opacity:.85}
.hint{text-align:center;font-size:11px;color:#999;margin-top:12px}
</style>
</head>
<body>
<div class="card">
<div class="header"><div class="icon">📊</div><h2>逆变器网络连接状态</h2></div>
<div class="si"><span class="l">设备序列号</span><span class="v">%SN%</span></div>
<div class="si"><span class="l">WiFi 连接</span><span class="v %WIFI_CLASS%">%WIFI_STATUS%</span></div>
<div class="si"><span class="l">WiFi 名称</span><span class="v">%WIFI_SSID%</span></div>
<div class="si"><span class="l">STA IP 地址</span><span class="v">%STA_IP%</span></div>
<div class="si"><span class="l">AP IP 地址</span><span class="v">%AP_IP%</span></div>
<div class="si"><span class="l">服务器连接</span><span class="v %MQTT_CLASS%">%MQTT_STATUS%</span></div>
<div class="si"><span class="l">信号强度</span><span class="v">%RSSI%</span></div>
<div class="si"><span class="l">运行时间</span><span class="v">%UPTIME%</span></div>
<div class="si"><span class="l">剩余内存</span><span class="v">%HEAP%</span></div>
<a href="/" class="btn">⚙️ 配网设置</a>
<a href="/ota" class="btn" style="margin-top:10px;background:linear-gradient(135deg,#e67e22,#d35400)">🔄 固件升级</a>
<div class="hint">访问 <a href="http://192.168.4.1/status">192.168.4.1</a> 查看此页面</div>
</div>
</body></html>
)rawliteral";

static const char OTA_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>固件升级</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,sans-serif;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.card{background:#fff;border-radius:16px;padding:24px;width:92%;max-width:420px;box-shadow:0 20px 60px rgba(0,0,0,.3)}
.header{text-align:center;margin-bottom:20px}
.header .icon{font-size:36px}
.header h2{font-size:18px;color:#2c5364;margin-top:4px}
.ir{display:flex;justify-content:space-between;padding:8px 0;font-size:13px;border-bottom:1px solid #f0f0f0}
.ir .l{color:#999}
.ir .v{color:#333;font-weight:500}
.sec{margin-top:20px;padding:16px;background:#f8f9fa;border-radius:12px}
.sec h3{font-size:15px;color:#2c5364;margin-bottom:12px}
.sec h3 .tag{font-size:11px;background:#2c5364;color:#fff;padding:2px 8px;border-radius:10px;margin-left:8px;font-weight:400}
.fg{margin-bottom:12px}
.fg label{display:block;font-size:12px;color:#666;margin-bottom:4px}
.fg input{width:100%;padding:10px 12px;border:2px solid #e0e0e0;border-radius:8px;font-size:14px;outline:none;transition:border .2s}
.fg input:focus{border-color:#2c5364}
.pb{width:100%;height:8px;background:#e0e0e0;border-radius:4px;overflow:hidden;margin:12px 0;display:none}
.pb .f{height:100%;background:linear-gradient(90deg,#27ae60,#2ecc71);border-radius:4px;transition:width .3s;width:0%}
.pb .f.err{background:linear-gradient(90deg,#e74c3c,#c0392b)}
.pt{text-align:center;font-size:12px;color:#666;min-height:18px}
.btn{width:100%;padding:12px;border:none;border-radius:10px;font-size:14px;font-weight:600;cursor:pointer;transition:opacity .2s;margin-top:6px}
.btn:active{opacity:.85}
.btn:disabled{opacity:.5;cursor:not-allowed}
.btn.p{background:linear-gradient(135deg,#2c5364,#203a43);color:#fff}
.btn.a{background:linear-gradient(135deg,#e67e22,#d35400);color:#fff}
.btn.d{background:#e74c3c;color:#fff}
.msg{text-align:center;font-size:13px;margin-top:10px;min-height:20px}
.msg.ok{color:#27ae60}
.msg.err{color:#e74c3c}
.hint{text-align:center;font-size:11px;color:#bbb;margin-top:12px}
.bl{display:block;text-align:center;padding:10px;margin-top:12px;color:#2c5364;font-size:13px;text-decoration:none}
</style>
</head>
<body>
<div class="card">
<div class="header"><div class="icon">🔄</div><h2>固件升级 (OTA)</h2></div>
<div class="ir"><span class="l">设备序列号</span><span class="v">%SN%</span></div>
<div class="ir"><span class="l">ESP 固件版本</span><span class="v">%ESP_VER%</span></div>
<div class="ir"><span class="l">ARM 固件版本</span><span class="v">%ARM_VER%</span></div>
<div class="ir"><span class="l">剩余内存</span><span class="v">%HEAP%</span></div>
<div class="sec">
<h3>ESP 模块升级 <span class="tag">WiFi</span></h3>
<div class="fg"><label>选择固件文件 (.bin)</label><input type="file" id="espFile" accept=".bin" style="padding:8px;border:2px dashed #ccc;border-radius:8px;width:100%"></div>
<button class="btn p" id="espUploadBtn" onclick="uploadEspFirmware()">📤 上传 ESP 固件</button>
<div class="pb" id="espProgress"><div class="f" id="espFill"></div></div>
<div class="pt" id="espProgressText"></div>
<div class="msg" id="espMsg"></div>
</div>
<div class="sec">
<h3>ARM 芯片升级 <span class="tag">GD32F303</span></h3>
<div class="fg"><label>选择固件文件 (.bin)</label><input type="file" id="armFile" accept=".bin" style="padding:8px;border:2px dashed #ccc;border-radius:8px;width:100%"></div>
<button class="btn a" id="armUploadBtn" onclick="uploadArmFirmware()">📤 上传 ARM 固件</button>
<div class="pb" id="armProgress"><div class="f" id="armFill"></div></div>
<div class="pt" id="armProgressText"></div>
<div class="msg" id="armMsg"></div>
</div>
<a href="/status" class="bl">📊 返回设备状态</a>
<div class="hint">升级过程中请勿断电，ESP 升级成功后自动重启</div>
</div>
<script>
function showMsg(id,t,o){var m=document.getElementById(id);m.textContent=t;m.className='msg '+(o?'ok':'err')}
function updateProgress(t,p,s){
var b=document.getElementById(t+'Progress'),f=document.getElementById(t+'Fill'),x=document.getElementById(t+'ProgressText');
b.style.display='block';f.style.width=p+'%';f.className='f'+(s==='error'?' err':'');
var sm={downloading:'下载中...',writing:'写入中...',verifying:'校验中...',done:'完成',error:'失败'};
x.textContent=p+'% - '+(sm[s]||s)}
var pt=null;
function pollProgress(t){
if(pt)clearInterval(pt);pt=setInterval(function(){
fetch('/ota/progress').then(function(r){return r.json()}).then(function(d){
if(d.target===t||d.target==='none'){updateProgress(t,d.progress,d.status);
if(d.status==='done'||d.status==='error'||d.status==='idle'){clearInterval(pt);pt=null;
if(d.status==='done')showMsg(t+'Msg','✅ 升级成功!',true);
else if(d.status==='error'){showMsg(t+'Msg','❌ '+(d.message||'升级失败'),false);
document.getElementById(t+'Btn').disabled=false}}}}).catch(function(){})},1000)}
function uploadFirmware(fi,t,m,b){
var f=document.getElementById(fi);if(!f.files||f.files.length===0){showMsg(m,'请选择固件文件',false);return}
var file=f.files[0];if(!file.name.endsWith('.bin')){showMsg(m,'请选择 .bin 文件',false);return}
if(file.size>(t==='esp'?2097152:262144)){showMsg(m,'文件过大 (ESP: 2MB, ARM: 256KB)',false);return}
document.getElementById(b).disabled=true;showMsg(m,'正在上传: 0%',true);updateProgress(t,0,'downloading');
var x=new XMLHttpRequest();x.open('POST','/ota/upload',true);
if(t==='esp'){x.setRequestHeader('Content-Type','application/octet-stream');
x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round((e.loaded/e.total)*100);
updateProgress(t,p,'downloading');showMsg(m,'正在上传: '+p+'%',true)}};
x.onload=function(){if(x.status===200){showMsg(m,'上传完成，正在写入...',true);updateProgress(t,100,'verifying');pollProgress(t)}
else{showMsg(m,'❌ 上传失败: '+x.responseText,false);document.getElementById(b).disabled=false}};
x.onerror=function(){showMsg(m,'❌ 网络错误，上传失败',false);document.getElementById(b).disabled=false};
x.send(file)}else{
var fd=new FormData();fd.append('file',file);fd.append('target',t);
x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round((e.loaded/e.total)*100);
updateProgress(t,p,'downloading');showMsg(m,'正在上传: '+p+'%',true)}};
x.onload=function(){if(x.status===200){showMsg(m,'上传完成，正在写入...',true);updateProgress(t,100,'verifying');pollProgress(t)}
else{showMsg(m,'❌ 上传失败: '+x.responseText,false);document.getElementById(b).disabled=false}};
x.onerror=function(){showMsg(m,'❌ 网络错误，上传失败',false);document.getElementById(b).disabled=false};
x.send(fd)}}
function uploadEspFirmware(){uploadFirmware('espFile','esp','espMsg','espUploadBtn')}
function uploadArmFirmware(){uploadFirmware('armFile','arm','armMsg','armUploadBtn')}
</script>
</body></html>
)rawliteral";

static uint8_t g_ota_stream_buf[512];

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

static void send_http_response(int fd, int code, const char* content_type, const char* body) {
    const char* reason = "OK";
    if (code == 400) reason = "Bad Request";
    else if (code == 404) reason = "Not Found";
    else if (code == 405) reason = "Method Not Allowed";
    else if (code == 409) reason = "Conflict";
    else if (code == 500) reason = "Internal Server Error";
    else if (code == 503) reason = "Service Unavailable";

    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "\r\n",
                        code, reason, content_type, (int)strlen(body));
    send(fd, header, hlen, 0);
    send(fd, body, strlen(body), 0);
}

static void send_http_404(int fd) {
    send_http_response(fd, 404, "text/plain", "not found");
}

static void send_json(int fd, const char* json) {
    send_http_response(fd, 200, "application/json", json);
}

// ==================== HTML 模板替换（流式发送） ====================

#define HTML_BUF_SIZE 4096

static void send_html_template(int fd, const char* template_str,
                               const char* keys[], const char* values[], int num_pairs) {
    static char buf[HTML_BUF_SIZE];
    int buf_pos = 0;
    const char* cursor = template_str;

    while (*cursor) {
        const char* mark = strchr(cursor, '%');
        if (!mark) {
            // 没有更多占位符，发送剩余
            int remaining = strlen(cursor);
            while (remaining > 0) {
                int space = HTML_BUF_SIZE - buf_pos;
                int chunk = remaining < space ? remaining : space;
                memcpy(buf + buf_pos, cursor, chunk);
                buf_pos += chunk;
                remaining -= chunk;
                if (buf_pos >= HTML_BUF_SIZE) {
                    send(fd, buf, buf_pos, 0);
                    buf_pos = 0;
                }
            }
            break;
        }

        // 复制 % 之前的内容
        int prefix_len = mark - cursor;
        while (prefix_len > 0) {
            int space = HTML_BUF_SIZE - buf_pos;
            int chunk = prefix_len < space ? prefix_len : space;
            memcpy(buf + buf_pos, cursor, chunk);
            buf_pos += chunk;
            prefix_len -= chunk;
            if (buf_pos >= HTML_BUF_SIZE) {
                send(fd, buf, buf_pos, 0);
                buf_pos = 0;
            }
        }

        // 查找结束 %
        const char* end_mark = strchr(mark + 1, '%');
        if (!end_mark) {
            // 没有匹配的结束 %，发送字面 %
            if (buf_pos >= HTML_BUF_SIZE) { send(fd, buf, buf_pos, 0); buf_pos = 0; }
            buf[buf_pos++] = '%';
            cursor = mark + 1;
            continue;
        }

        // 提取 key 并查找替换值
        int key_len = end_mark - mark - 1;
        const char* value = NULL;
        for (int i = 0; i < num_pairs; i++) {
            if ((int)strlen(keys[i]) == key_len && strncmp(keys[i], mark + 1, key_len) == 0) {
                value = values[i];
                break;
            }
        }

        if (value) {
            int vlen = strlen(value);
            while (vlen > 0) {
                int space = HTML_BUF_SIZE - buf_pos;
                int chunk = vlen < space ? vlen : space;
                memcpy(buf + buf_pos, value, chunk);
                buf_pos += chunk;
                vlen -= chunk;
                if (buf_pos >= HTML_BUF_SIZE) {
                    send(fd, buf, buf_pos, 0);
                    buf_pos = 0;
                }
            }
        }
        cursor = end_mark + 1;
    }

    if (buf_pos > 0) {
        send(fd, buf, buf_pos, 0);
    }
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

// ==================== WiFi 扫描 ====================

static void handle_wifi_scan(int fd) {
    ESP_LOGI(TAG, "WiFi scan requested");

    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);
    ESP_LOGI(TAG, "Current WiFi mode: %d", current_mode);

    if (current_mode != WIFI_MODE_APSTA && current_mode != WIFI_MODE_STA) {
        ESP_LOGI(TAG, "Switching to AP_STA mode for scanning");
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_scan_config_t scan_config = {
        .show_hidden = true,
    };

    ESP_LOGI(TAG, "Starting WiFi scan...");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed, cleaning up and retrying...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_scan_start(&scan_config, true);
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Scan completed, result=%d", ap_count);

    if (ap_count == 0) {
        send_json(fd, "{\"networks\":[]}");
        ESP_LOGI(TAG, "WiFi scan: 0 networks");
        return;
    }

    if (ap_count > 30) ap_count = 30;
    wifi_ap_record_t* ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        send_json(fd, "{\"networks\":[]}");
        return;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    // 估算 JSON 大小: 每个 AP 约 80 字节
    size_t json_size = 32 + ap_count * 80;
    char* json = malloc(json_size);
    if (!json) {
        free(ap_records);
        send_json(fd, "{\"networks\":[]}");
        return;
    }

    int pos = snprintf(json, json_size, "{\"networks\":[");
    for (int i = 0; i < ap_count; i++) {
        if (i > 0) json[pos++] = ',';

        // 转义 SSID 中的特殊字符
        char escaped_ssid[65];
        int ei = 0;
        const char* s = (const char*)ap_records[i].ssid;
        for (int j = 0; s[j] && ei < 63; j++) {
            if (s[j] == '"' || s[j] == '\\') {
                if (ei < 62) escaped_ssid[ei++] = '\\';
            }
            escaped_ssid[ei++] = s[j];
        }
        escaped_ssid[ei] = '\0';

        pos += snprintf(json + pos, json_size - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%d}",
                        escaped_ssid, ap_records[i].rssi,
                        ap_records[i].authmode != WIFI_AUTH_OPEN ? 1 : 0);
    }
    snprintf(json + pos, json_size - pos, "]}");

    free(ap_records);

    send_json(fd, json);
    ESP_LOGI(TAG, "Scan result sent: %d networks", ap_count);
    free(json);
}

// ==================== 状态查询页面 ====================

extern bool mqtt_is_connected(void);

static void handle_status_page(int fd) {
    bool wifi_connected = false;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        wifi_connected = true;
    }

    char sta_ip_str[16] = "--";
    char ap_ip_str[16] = "--";
    char rssi_str[16] = "--";
    char ssid_str[33] = "--";

    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (sta_netif && wifi_connected) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            snprintf(sta_ip_str, sizeof(sta_ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
        strncpy(ssid_str, (const char*)ap_info.ssid, sizeof(ssid_str) - 1);
        snprintf(rssi_str, sizeof(rssi_str), "%d dBm", ap_info.rssi);
    }

    if (ap_netif) {
        esp_netif_ip_info_t ap_ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ap_ip_info) == ESP_OK) {
            snprintf(ap_ip_str, sizeof(ap_ip_str), IPSTR, IP2STR(&ap_ip_info.ip));
        }
    }

    bool mqtt_conn = mqtt_is_connected();

    int64_t uptime_ms = esp_timer_get_time() / 1000;
    unsigned long uptime_sec = uptime_ms / 1000;
    char uptime_str[32];
    int hours = uptime_sec / 3600;
    int mins = (uptime_sec % 3600) / 60;
    int secs = uptime_sec % 60;
    if (hours > 0) {
        snprintf(uptime_str, sizeof(uptime_str), "%d小时%d分%d秒", hours, mins, secs);
    } else if (mins > 0) {
        snprintf(uptime_str, sizeof(uptime_str), "%d分%d秒", mins, secs);
    } else {
        snprintf(uptime_str, sizeof(uptime_str), "%d秒", secs);
    }

    char heap_str[16];
    snprintf(heap_str, sizeof(heap_str), "%lu KB", (unsigned long)(esp_get_free_heap_size() / 1024));

    const char* wifi_status = wifi_connected ? "已连接" : "未连接";
    const char* wifi_class = wifi_connected ? "ok" : "err";
    const char* mqtt_status = mqtt_conn ? "已连接" : "未连接";
    const char* mqtt_class = mqtt_conn ? "ok" : (wifi_connected ? "warn" : "err");

    const char* keys[] = {
        "SN", "WIFI_STATUS", "WIFI_CLASS", "WIFI_SSID",
        "STA_IP", "AP_IP", "MQTT_STATUS", "MQTT_CLASS",
        "RSSI", "UPTIME", "HEAP"
    };
    const char* values[] = {
        g_device_sn, wifi_status, wifi_class, ssid_str,
        sta_ip_str, ap_ip_str, mqtt_status, mqtt_class,
        rssi_str, uptime_str, heap_str
    };

    // 发送 HTTP 头
    const char* header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/html; charset=utf-8\r\n"
                         "Connection: close\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "\r\n";
    send(fd, header, strlen(header), 0);

    send_html_template(fd, STATUS_HTML, keys, values, 11);
}

// ==================== OTA 页面处理 ====================

static void handle_ota_page(int fd) {
    char esp_ver[32] = FIRMWARE_VERSION;
    char arm_ver[32] = "--";
    char heap_str[16];
    snprintf(heap_str, sizeof(heap_str), "%lu KB", (unsigned long)(esp_get_free_heap_size() / 1024));

    const char* keys[] = {"SN", "ESP_VER", "ARM_VER", "HEAP"};
    const char* values[] = {g_device_sn, esp_ver, arm_ver, heap_str};

    const char* header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/html; charset=utf-8\r\n"
                         "Connection: close\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "\r\n";
    send(fd, header, strlen(header), 0);

    send_html_template(fd, OTA_HTML, keys, values, 4);
}

static void handle_ota_progress(int fd) {
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"target\":\"none\",\"progress\":0,\"status\":\"idle\",\"message\":\"\"}");
    send_json(fd, resp);
}

static void handle_ota_info(int fd) {
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"esp_version\":\"%s\",\"arm_version\":\"--\",\"free_heap\":%lu,\"chip\":\"ESP32-C2\"}",
             FIRMWARE_VERSION, (unsigned long)esp_get_free_heap_size());
    send_json(fd, resp);
}

// ==================== OTA 上传处理 ====================

static void handle_ota_upload(struct HttpClientSlot* slot) {
    int fd = slot->fd;
    char* buf = slot->rx_buf;
    int buf_len = slot->rx_len;

    // 获取 Content-Length
    char* cl = strstr(buf, "Content-Length:");
    int content_length = 0;
    if (cl) content_length = atoi(cl + 15);

    // 获取 Content-Type
    char* ct = strstr(buf, "Content-Type:");
    if (!ct) {
        send_http_response(fd, 400, "application/json",
                           "{\"result\":\"error\",\"message\":\"缺少 Content-Type\"}");
        return;
    }

    bool is_esp_raw = (strstr(ct, "application/octet-stream") != NULL);

    if (is_esp_raw) {
        // ESP OTA: 原始二进制流式接收
        if (content_length <= 0 || content_length > 2097152) {
            send_http_response(fd, 400, "application/json",
                               "{\"result\":\"error\",\"message\":\"无效的文件大小\"}");
            return;
        }

        ESP_LOGI(TAG, "[OTA] ESP raw binary, size=%d", content_length);

        const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
        if (!update_partition) {
            send_http_response(fd, 500, "application/json",
                               "{\"result\":\"error\",\"message\":\"无法获取 OTA 分区\"}");
            return;
        }

        esp_ota_handle_t ota_handle;
        esp_err_t ota_err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
        if (ota_err != ESP_OK) {
            send_http_response(fd, 500, "application/json",
                               "{\"result\":\"error\",\"message\":\"OTA 初始化失败\"}");
            return;
        }

        led1_set(LED_FAST_BLINK);

        int written = 0;

        // 写入缓冲区中已有的 body 数据
        char* body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            int body_in_buf = buf_len - (body_start - buf);
            if (body_in_buf > 0) {
                esp_ota_write(ota_handle, (uint8_t*)body_start, body_in_buf);
                written += body_in_buf;
                ESP_LOGI(TAG, "[OTA] Wrote %d bytes from header buffer", body_in_buf);
            }
        }

        // 从 socket 流式读取剩余数据
        int remaining = content_length - written;
        int64_t timeout_start = esp_timer_get_time() / 1000;

        while (written < content_length && (esp_timer_get_time() / 1000 - timeout_start) < 60000) {
            int to_read = content_length - written;
            if (to_read > (int)sizeof(g_ota_stream_buf)) to_read = sizeof(g_ota_stream_buf);
            int bytes_read = recv(fd, g_ota_stream_buf, to_read, 0);
            if (bytes_read > 0) {
                timeout_start = esp_timer_get_time() / 1000;
                esp_ota_write(ota_handle, g_ota_stream_buf, bytes_read);
                written += bytes_read;
            } else if (bytes_read == 0) {
                break;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }

        ESP_LOGI(TAG, "[OTA] Total written: %d/%d bytes", written, content_length);

        if (written <= 0) {
            esp_ota_abort(ota_handle);
            led1_set(LED_OFF);
            send_http_response(fd, 500, "application/json",
                               "{\"result\":\"error\",\"message\":\"未接收到有效数据\"}");
            return;
        }

        ota_err = esp_ota_end(ota_handle);
        if (ota_err != ESP_OK) {
            led1_set(LED_OFF);
            send_http_response(fd, 500, "application/json",
                               "{\"result\":\"error\",\"message\":\"校验失败\"}");
            return;
        }

        esp_ota_set_boot_partition(update_partition);

        led1_set(LED_OFF);
        send_http_response(fd, 200, "application/json",
                           "{\"result\":\"ok\",\"message\":\"ESP OTA 成功，重启中\"}");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }

    // ARM OTA: multipart/form-data
    static char s_boundary[128];
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
                boundary_len = blen + 2; // --boundary
            }
        }
    }

    if (boundary_len == 0) {
        send_http_response(fd, 400, "application/json",
                           "{\"result\":\"error\",\"message\":\"缺少 boundary\"}");
        return;
    }

    char boundary_delim[132];
    snprintf(boundary_delim, sizeof(boundary_delim), "--%s", s_boundary);
    boundary_len = strlen(boundary_delim);

    char* body_start = strstr(buf, "\r\n\r\n");
    if (!body_start) {
        send_http_response(fd, 400, "application/json",
                           "{\"result\":\"error\",\"message\":\"无效的请求格式\"}");
        return;
    }
    body_start += 4;

    int body_in_buf = buf_len - (body_start - buf);

    // 查找第一个 boundary
    char* first_boundary = NULL;
    for (int i = 0; i <= body_in_buf - boundary_len; i++) {
        if (memcmp(body_start + i, boundary_delim, boundary_len) == 0) {
            first_boundary = body_start + i;
            break;
        }
    }

    if (!first_boundary) {
        send_http_response(fd, 400, "application/json",
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
        send_http_response(fd, 400, "application/json",
                           "{\"result\":\"error\",\"message\":\"缺少 file 字段\"}");
        return;
    }

    char* file_data_start = strstr(file_part, "\r\n\r\n");
    if (!file_data_start) {
        send_http_response(fd, 400, "application/json",
                           "{\"result\":\"error\",\"message\":\"无效的 multipart 格式\"}");
        return;
    }
    file_data_start += 4;

    bool is_esp = (strcmp(target, "esp") == 0);
    int max_size = is_esp ? 2097152 : 262144;
    int estimated_size = content_length - (file_data_start - buf) - boundary_len - 10;
    if (estimated_size <= 0 || estimated_size > max_size) {
        send_http_response(fd, 400, "application/json",
                           "{\"result\":\"error\",\"message\":\"无效的文件大小\"}");
        return;
    }

    const esp_partition_t* update_partition = NULL;
    esp_ota_handle_t ota_handle;

    if (is_esp) {
        update_partition = esp_ota_get_next_update_partition(NULL);
        if (!update_partition || esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle) != ESP_OK) {
            send_http_response(fd, 500, "application/json",
                               "{\"result\":\"error\",\"message\":\"OTA 初始化失败\"}");
            return;
        }
    } else {
        if (!arm_ota_start_streaming(estimated_size)) {
            send_http_response(fd, 500, "application/json",
                               "{\"result\":\"error\",\"message\":\"ARM OTA 初始化失败\"}");
            return;
        }
    }

    led1_set(LED_FAST_BLINK);

    int written = 0;
    int buf_remaining = body_in_buf - (file_data_start - body_start);
    if (buf_remaining > 0) {
        char boundary_end[134];
        snprintf(boundary_end, sizeof(boundary_end), "\r\n--%s", s_boundary);
        int be_len = strlen(boundary_end);

        int data_len = buf_remaining;
        for (int i = 0; i <= buf_remaining - be_len; i++) {
            if (memcmp(file_data_start + i, boundary_end, be_len) == 0) {
                data_len = i;
                break;
            }
        }

        if (data_len > 0) {
            if (is_esp) {
                esp_ota_write(ota_handle, (uint8_t*)file_data_start, data_len);
            } else {
                arm_ota_feed((uint8_t*)file_data_start, data_len);
            }
            written += data_len;
        }
    }

    int remaining = content_length - buf_len;
    int64_t timeout_start = esp_timer_get_time() / 1000;
    int stream_buf_len = 0;
    bool found_end = false;

    while (remaining > 0 && !found_end && (esp_timer_get_time() / 1000 - timeout_start) < 60000) {
        int to_read = (int)sizeof(g_ota_stream_buf) - stream_buf_len;
        if (to_read > remaining) to_read = remaining;
        int bytes_read = recv(fd, g_ota_stream_buf + stream_buf_len, to_read, 0);
        if (bytes_read > 0) {
            timeout_start = esp_timer_get_time() / 1000;
            stream_buf_len += bytes_read;
            remaining -= bytes_read;

            for (int i = 0; i <= stream_buf_len - boundary_len; i++) {
                if (memcmp(g_ota_stream_buf + i, boundary_delim, boundary_len) == 0) {
                    if (i > 0) {
                        if (is_esp) {
                            esp_ota_write(ota_handle, g_ota_stream_buf, i);
                        } else {
                            arm_ota_feed(g_ota_stream_buf, i);
                        }
                        written += i;
                    }
                    found_end = true;
                    break;
                }
            }

            if (!found_end && stream_buf_len >= (int)sizeof(g_ota_stream_buf)) {
                int safe_len = stream_buf_len - boundary_len;
                if (safe_len > 0) {
                    if (is_esp) {
                        esp_ota_write(ota_handle, g_ota_stream_buf, safe_len);
                    } else {
                        arm_ota_feed(g_ota_stream_buf, safe_len);
                    }
                    written += safe_len;
                    memmove(g_ota_stream_buf, g_ota_stream_buf + safe_len, stream_buf_len - safe_len);
                    stream_buf_len -= safe_len;
                }
            }
        } else if (bytes_read <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            if (!is_esp) arm_ota_process();
        }
    }

    ESP_LOGI(TAG, "[OTA] Total written: %d bytes", written);

    if (written <= 0) {
        if (is_esp) esp_ota_abort(ota_handle);
        else arm_ota_cancel();
        led1_set(LED_OFF);
        send_http_response(fd, 500, "application/json",
                           "{\"result\":\"error\",\"message\":\"未接收到有效数据\"}");
        return;
    }

    if (is_esp) {
        if (esp_ota_end(ota_handle) != ESP_OK) {
            led1_set(LED_OFF);
            send_http_response(fd, 500, "application/json",
                               "{\"result\":\"error\",\"message\":\"校验失败\"}");
            return;
        }
        esp_ota_set_boot_partition(update_partition);
        led1_set(LED_OFF);
        send_http_response(fd, 200, "application/json",
                           "{\"result\":\"ok\",\"message\":\"ESP OTA 成功，重启中\"}");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        arm_ota_end_streaming();
        led1_set(LED_OFF);
        send_http_response(fd, 200, "application/json",
                           "{\"result\":\"ok\",\"message\":\"ARM 固件已传输，等待校验\"}");
    }
}

// ==================== HTTP 请求处理 ====================

static void process_http_request(struct HttpClientSlot* slot) {
    int fd = slot->fd;
    char* buf = slot->rx_buf;

    char method[16] = {0};
    char path[128] = {0};

    char* line_end = strstr(buf, "\r\n");
    if (!line_end) return;

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

    ESP_LOGI(TAG, "%s %s", method, path);

    const char* body = strstr(buf, "\r\n\r\n");
    char empty_body[] = "";
    if (body) body += 4; else body = empty_body;

    // ===== 路由 =====

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        char dev_label[64];
        if (strncmp(g_device_sn, "MAC", 3) == 0) {
            size_t slen = strlen(g_device_sn);
            const char* tail = (slen > 8) ? g_device_sn + slen - 8 : g_device_sn;
            char upper[9] = {0};
            int i;
            for (i = 0; i < 8 && tail[i]; i++) upper[i] = tail[i];
            upper[i] = '\0';
            for (char* p = upper; *p; p++) {
                if (*p >= 'a' && *p <= 'f') *p = *p - 'a' + 'A';
            }
            snprintf(dev_label, sizeof(dev_label), "设备: CS_INV_%s", upper);
        } else {
            snprintf(dev_label, sizeof(dev_label), "设备: CS_INV_%s", g_device_sn);
        }

        const char* keys[] = {"DEV_LABEL"};
        const char* values[] = {dev_label};

        const char* header = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/html; charset=utf-8\r\n"
                             "Connection: close\r\n"
                             "Access-Control-Allow-Origin: *\r\n"
                             "\r\n";
        send(fd, header, strlen(header), 0);

        send_html_template(fd, PROVISION_HTML, keys, values, 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/scan") == 0) {
        handle_wifi_scan(fd);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/status") == 0) {
        handle_status_page(fd);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/wifi_status") == 0) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            char resp[128];
            esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            char ip_str[16] = "0.0.0.0";
            if (sta_netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                }
            }
            snprintf(resp, sizeof(resp),
                     "{\"connected\":true,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
                     (const char*)ap_info.ssid, ip_str, ap_info.rssi);
            send_json(fd, resp);
        } else {
            send_json(fd, "{\"connected\":false}");
        }
        return;
    }

    // ===== OTA 路由 =====

    if (strcmp(method, "GET") == 0 && strcmp(path, "/ota") == 0) {
        handle_ota_page(fd);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/ota/progress") == 0) {
        handle_ota_progress(fd);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/ota/info") == 0) {
        handle_ota_info(fd);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/ota/upload") == 0) {
        handle_ota_upload(slot);
        return;
    }

    // ===== 配网路由 =====

    if ((strcmp(method, "POST") == 0 && strcmp(path, "/config") == 0) ||
        (strcmp(method, "POST") == 0 && strcmp(path, "/") == 0)) {

        char ssid[33] = {0};
        char pass[65] = {0};

        bool got_ssid = json_extract(body, "ssid", ssid, sizeof(ssid));
        bool got_pass = json_extract(body, "password", pass, sizeof(pass));

        char cmd[16] = {0};
        bool got_cmd = json_extract(body, "cmd", cmd, sizeof(cmd));
        if (got_cmd && strcmp(cmd, "set_wifi") == 0) {
            got_ssid = json_extract(body, "ssid", ssid, sizeof(ssid));
            got_pass = json_extract(body, "password", pass, sizeof(pass));
        }

        if (got_ssid && got_pass) {
            send_json(fd, "{\"result\":\"ok\",\"message\":\"connecting, please wait\"}");
            ESP_LOGI(TAG, "WiFi: %s", ssid);
            if (g_cb) g_cb(ssid, pass);
        } else {
            send_http_response(fd, 400, "application/json",
                               "{\"result\":\"error\",\"message\":\"missing ssid or password\"}");
        }
        return;
    }

    // 404
    send_http_404(fd);
}

// ==================== 初始化 ====================

void tcp_provision_init(void) {
    ESP_LOGI(TAG, "HTTP server on port %d", PROVISION_TCP_PORT);

    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        g_slots[i].fd = -1;
        g_slots[i].rx_len = 0;
    }

    g_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_server_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return;
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PROVISION_TCP_PORT);

    if (bind(g_server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed");
        close(g_server_fd);
        g_server_fd = -1;
        return;
    }

    if (listen(g_server_fd, 2) < 0) {
        ESP_LOGE(TAG, "Listen failed");
        close(g_server_fd);
        g_server_fd = -1;
        return;
    }

    // 设置非阻塞
    int flags = fcntl(g_server_fd, F_GETFL, 0);
    fcntl(g_server_fd, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "HTTP server started on port %d", PROVISION_TCP_PORT);
}

void tcp_provision_set_callback(ProvisionCallback cb) {
    g_cb = cb;
}

void tcp_provision_stop(void) {
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        if (g_slots[i].fd >= 0) {
            close(g_slots[i].fd);
            g_slots[i].fd = -1;
        }
        g_slots[i].rx_len = 0;
    }
}

// ==================== 主循环 ====================

void tcp_provision_process(void) {
    if (g_server_fd < 0) return;

    // 1. select() 等待活动（100ms 超时）
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(g_server_fd, &readfds);
    int max_fd = g_server_fd;

    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        if (g_slots[i].fd >= 0) {
            FD_SET(g_slots[i].fd, &readfds);
            if (g_slots[i].fd > max_fd) max_fd = g_slots[i].fd;
        }
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    select(max_fd + 1, &readfds, NULL, NULL, &tv);

    // 2. 接受新连接
    if (FD_ISSET(g_server_fd, &readfds)) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int new_fd = accept(g_server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (new_fd >= 0) {
            int free_slot = -1;
            for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
                if (g_slots[i].fd < 0) {
                    free_slot = i;
                    break;
                }
            }

            if (free_slot >= 0) {
                struct timeval timeout;
                timeout.tv_sec = 5;
                timeout.tv_usec = 0;
                setsockopt(new_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

                int flags = fcntl(new_fd, F_GETFL, 0);
                fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);

                g_slots[free_slot].fd = new_fd;
                g_slots[free_slot].rx_len = 0;
                g_slots[free_slot].last_rx_time = esp_timer_get_time() / 1000;
            } else {
                close(new_fd);
            }
        }
    }

    // 3. 处理每个 slot 的数据
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        struct HttpClientSlot* slot = &g_slots[i];
        if (slot->fd < 0) continue;

        if (!FD_ISSET(slot->fd, &readfds)) {
            // 超时保护：10 秒无新数据则断开
            if (slot->rx_len > 0 &&
                (esp_timer_get_time() / 1000 - slot->last_rx_time) > 10000) {
                close(slot->fd);
                slot->fd = -1;
                slot->rx_len = 0;
            }
            continue;
        }

        // 读取数据（逐字节，直到 \r\n\r\n 或缓冲区满）
        while (slot->rx_len < (int)sizeof(slot->rx_buf) - 1) {
            char c;
            int n = recv(slot->fd, &c, 1, 0);
            if (n <= 0) break;
            slot->rx_buf[slot->rx_len++] = c;
            slot->last_rx_time = esp_timer_get_time() / 1000;

            // HTTP 请求头结束标志
            if (slot->rx_len >= 4 &&
                strncmp(slot->rx_buf + slot->rx_len - 4, "\r\n\r\n", 4) == 0) {

                slot->rx_buf[slot->rx_len] = '\0';

                // 检查 Content-Length，继续读 body
                char* cl = strstr(slot->rx_buf, "Content-Length:");
                int body_len = 0;
                if (cl) body_len = atoi(cl + 15);

                char* body_start = strstr(slot->rx_buf, "\r\n\r\n") + 4;
                int header_len = body_start - slot->rx_buf;
                int received_body = slot->rx_len - header_len;

                if (received_body < body_len && body_len > 0) {
                    // 大 body 请求（OTA），不预读，交给处理函数流式读取
                    int max_buf = (int)sizeof(slot->rx_buf) - 1;
                    if (body_len <= max_buf) {
                        int space_left = max_buf - slot->rx_len;
                        int needed = body_len - received_body;
                        if (needed > space_left) needed = space_left;
                        int64_t start = esp_timer_get_time() / 1000;
                        int got = 0;
                        while (got < needed && (esp_timer_get_time() / 1000 - start) < 2000) {
                            char b;
                            int n2 = recv(slot->fd, &b, 1, 0);
                            if (n2 > 0) {
                                slot->rx_buf[slot->rx_len++] = b;
                                got++;
                            } else {
                                vTaskDelay(pdMS_TO_TICKS(1));
                            }
                        }
                    }
                }

                slot->rx_buf[slot->rx_len] = '\0';
                ESP_LOGI(TAG, "Calling process_http_request for slot %d", i);
                process_http_request(slot);
                ESP_LOGI(TAG, "process_http_request returned for slot %d", i);
                close(slot->fd);
                slot->fd = -1;
                slot->rx_len = 0;
                break;
            }
        }

        // 连接异常检测
        if (slot->fd >= 0) {
            char test;
            int peek_result = recv(slot->fd, &test, 1, MSG_PEEK | MSG_DONTWAIT);
            if (peek_result == 0) {
                close(slot->fd);
                slot->fd = -1;
                slot->rx_len = 0;
            }
        }
    }
}
