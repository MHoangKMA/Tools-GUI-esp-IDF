#include "robot_web_server.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <server_profiles.h>
#include <wifi_manager.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "application.h"
#include "board.h"
#include "display.h"
#include "music_client.h"
#include "ota.h"
#include "ota_upload.h"
#include "radio_directory.h"
#include "radio_player.h"
#include "url_util.h"
#include "robot_ap.h"
#include "robot_selftest.h"
#include "robot_controller.h"
#include "signal_pattern.h"
#include "turn_signals.h"
#include "settings.h"

#define TAG "RobotWeb"

namespace {

const char* kStateNames[] = {"unknown",  "starting",  "wifi_configuring", "idle",           "connecting",
                             "listening", "speaking", "notifying",        "upgrading",      "activating",
                             "audio_testing", "fatal_error"};

const char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Robot</title>
<style>
:root{color-scheme:dark}
body{margin:0;padding:16px;font:15px system-ui,sans-serif;background:#111;color:#eee;max-width:480px;margin:auto}
h1{font-size:18px;margin:0 0 12px}h2{font-size:14px;color:#9ab;margin:18px 0 8px;text-transform:uppercase;letter-spacing:.05em}
.card{background:#1c1f24;border-radius:12px;padding:12px}
.dpad{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;max-width:260px;margin:auto}
button,select{font:inherit;color:#eee;background:#2b3038;border:0;border-radius:10px;padding:14px 8px;touch-action:none;user-select:none;-webkit-user-select:none;-webkit-touch-callout:none;-webkit-tap-highlight-color:transparent}
body,table,h1,h2,.card,label{-webkit-user-select:none;user-select:none;-webkit-touch-callout:none}
input,textarea{-webkit-user-select:text;user-select:text}
button:active{background:#3d7be0}.stop{background:#a33}.row{display:flex;gap:8px;flex-wrap:wrap}.row>*{flex:1 1 auto}
input[type=range]{width:100%}table{width:100%;border-collapse:collapse}td{padding:3px 0}td:first-child{color:#9ab;width:42%}
#msg{min-height:1.2em;color:#fc6;margin-top:8px}
</style></head><body>
<h1>Robot</h1>
<h2>Lái robot</h2><div class="card">
<div class="dpad">
<span></span><button data-dir="forward">&#9650;</button><span></span>
<button data-dir="left">&#9664;</button><button class="stop" id="stop">&#9632;</button><button data-dir="right">&#9654;</button>
<span></span><button data-dir="backward">&#9660;</button><span></span></div>
<p>Tốc độ: <b id="spv">70</b>%<input id="speed" type="range" min="20" max="100" value="70"></p>
<div class="row"><button id="dance">Nhảy</button></div></div>
<h2>Đi theo quãng đường / góc quay</h2><div class="card">
<div class="row"><input id="cm" type="number" min="1" max="300" value="10" style="max-width:90px"><span style="align-self:center">cm</span>
<button id="fwdcm">&#9650; Tiến</button><button id="backcm">&#9660; Lùi</button></div>
<div class="row" style="margin-top:8px"><input id="deg" type="number" min="1" max="360" value="90" style="max-width:90px"><span style="align-self:center">độ</span>
<button id="turnl">&#9664; Quay trái</button><button id="turnr">Quay phải &#9654;</button></div>
<p style="font-size:13px;color:#9ab;margin:8px 0 0">Dùng tốc độ ở thanh phía trên. Không có cảm biến vòng quay nên sai số vài cm; hãy hiệu chỉnh trên đúng mặt sàn.</p>
<details style="margin-top:10px"><summary>Hiệu chỉnh (đo thực tế)</summary>
<p style="font-size:13px;color:#9ab">Đặt robot trên sàn có chỗ trống. Bấm chạy thử, dùng thước đo robot đi được bao nhiêu (hoặc quay được bao nhiêu độ), nhập số đo rồi bấm Lưu.</p>
<div class="row"><button id="calf">Chạy thử 2 giây</button><input id="calfv" type="number" step="0.1" placeholder="đi được (cm)"><button id="calfs">Lưu</button></div>
<div class="row" style="margin-top:8px"><button id="calt">Quay thử 1 giây</button><input id="caltv" type="number" step="1" placeholder="quay được (độ)"><button id="calts">Lưu</button></div>
<p id="calinfo" style="font-size:13px;color:#9ab"></p></details></div>
<h2>Wi-Fi của robot</h2><div class="card">
<p id="apinfo" style="margin:0 0 8px;font-size:14px"></p>
<div class="row"><input id="appass" type="text" placeholder="Mật khẩu mới (8-63 ký tự)"><button id="apsave">Đổi mật khẩu</button></div>
<div class="row" style="margin-top:8px"><button id="apon">Bật</button><button id="apoff">Tắt</button></div>
<p style="font-size:13px;color:#9ab;margin:8px 0 0">Robot phát mạng này song song với việc nối Wi-Fi nhà. Điện thoại nối vào đó rồi mở địa chỉ ở trên để điều khiển; điện thoại không có Internet qua robot. Đổi mật khẩu cần mã quản trị.</p></div>
<h2>Đèn xi nhan</h2><div class="card"><div class="row" id="lights"></div>
<p style="font-size:13px;color:#9ab;margin:8px 0 0">Đèn tự nháy khi robot chạy: rẽ trái nháy đèn trái, rẽ phải nháy đèn phải, tiến hoặc lùi nháy cả hai. Một số biểu cảm cũng có hiệu ứng đèn.</p></div>
<h2>Biểu cảm</h2><div class="card"><div class="row" id="faces"></div></div>
<h2>Radio</h2><div class="card"><div class="row">
<select id="radio"></select><button id="rplay">Phát</button><button id="rstop">Dừng</button></div>
<div class="row" style="margin-top:8px"><button id="rnext">Đổi đài / đổi bài</button></div>
<input id="rquery" placeholder="Tìm đài (vd: nhạc vàng, giao thông)" style="width:100%;padding:12px;margin:10px 0 8px;border-radius:10px;border:0;background:#2b3038;color:#eee">
<div class="row"><button id="rfind">Tìm và phát</button></div>
<p style="font-size:13px;color:#9ab;margin:8px 0 0">Radio phát khi robot rảnh (không đang trò chuyện).</p></div>
<h2>Nhạc theo tên bài</h2><div class="card">
<input id="song" placeholder="Tên bài hát" style="width:100%;padding:12px;margin-bottom:8px;border-radius:10px;border:0;background:#2b3038;color:#eee">
<input id="artist" placeholder="Ca sĩ (không bắt buộc)" style="width:100%;padding:12px;margin-bottom:8px;border-radius:10px;border:0;background:#2b3038;color:#eee">
<div class="row"><button id="mplay">Phát nhạc</button></div>
<p style="font-size:13px;color:#9ab;margin:12px 0 4px">Nguồn nhạc miễn phí: Jamendo client id (đăng ký miễn phí tại developer.jamendo.com)</p>
<input id="jamendo" placeholder="client id" style="width:100%;padding:12px;margin-bottom:8px;border-radius:10px;border:0;background:#2b3038;color:#eee">
<div class="row"><button id="jsave">Lưu client id</button></div>
<p style="font-size:13px;color:#9ab;margin:12px 0 4px">Hoặc máy chủ nhạc riêng (http://...)</p>
<input id="musicurl" placeholder="http://may-chu-nhac:cong" style="width:100%;padding:12px;margin-bottom:8px;border-radius:10px;border:0;background:#2b3038;color:#eee">
<div class="row"><button id="msave">Lưu máy chủ nhạc</button></div></div>
<h2>Máy chủ / Agent</h2><div class="card">
<p style="font-size:13px;color:#9ab;margin:0 0 8px">Robot kết nối tới máy chủ được chọn. Agent (tên, ngôn ngữ, giọng nói, vai trò) cấu hình trên bảng điều khiển của máy chủ đó. Chọn máy chủ khác thì robot tự khởi động lại.</p>
<div id="servers"></div>
<input id="srvname" placeholder="Tên máy chủ" style="width:100%;padding:12px;margin:10px 0 8px;border-radius:10px;border:0;background:#2b3038;color:#eee">
<input id="srvurl" placeholder="Địa chỉ OTA https://..." style="width:100%;padding:12px;margin-bottom:8px;border-radius:10px;border:0;background:#2b3038;color:#eee">
<div class="row"><button id="srvadd">Thêm máy chủ</button></div></div>
<h2>Mã quản trị và cập nhật firmware</h2><div class="card">
<p style="font-size:13px;color:#9ab;margin:0 0 8px">Mã quản trị (8 ký tự) hiện trên màn hình bài hát 0.96" của robot. Cần cho: đổi máy chủ, nguồn nhạc, cập nhật firmware.</p>
<input id="otakey" placeholder="Mã quản trị (8 ký tự)" style="width:100%;padding:12px;margin-bottom:8px;border-radius:10px;border:0;background:#2b3038;color:#eee">
<input id="otafile" type="file" accept=".bin" style="width:100%;margin-bottom:8px">
<div class="row"><button id="otaup">Tải lên và cập nhật</button></div>
<progress id="otaprog" value="0" max="100" style="width:100%;margin-top:8px;display:none"></progress></div>
<h2>Kiểm tra linh kiện</h2><div class="card"><div class="row"><button id="selftest">Chạy kiểm tra lại</button></div>
<pre id="selftestout" style="white-space:pre-wrap;font-size:13px;color:#9ab;margin:8px 0 0"></pre>
<p style="font-size:13px;color:#9ab;margin:4px 0 0">Robot tự kiểm tra từng linh kiện mỗi lần khởi động: mỗi bước có chữ trên màn hình và tiếng bíp (cao dần là tốt, thấp là lỗi). Bánh xe xoay thử tại chỗ một chút.</p></div>
<h2>Trạng thái</h2><div class="card"><table id="status"></table>
<div class="row" style="margin-top:10px"><button id="ota">Cập nhật firmware</button></div></div>
<div id="msg"></div>
<script>
const $=id=>document.getElementById(id);let token=localStorage.getItem('robot_token')||'';
async function call(path,method='POST'){
  const h={'X-Robot':'1'};if(token)h['X-Token']=token;
  let r=await fetch(path,{method,headers:h});
  if(r.status==401){token=prompt('Mã truy cập (token) của robot:')||'';localStorage.setItem('robot_token',token);return null;}
  $('msg').textContent=r.ok?'':'Lỗi: '+(await r.text());return r.ok?r.json():null;}
['neutral','relaxed','cool','bored','confident','happy','laughing','funny','silly','loving','embarrassed','winking','kissy','delicious','excited','sad','crying','worried','scared','angry','surprised','shocked','sleepy','sleeping','thinking','confused','dizzy'].forEach(f=>{
  const b=document.createElement('button');b.textContent=f;b.style.padding='10px 12px';b.onclick=()=>call('/api/emotion?name='+f);$('faces').appendChild(b);});
function adminKey(){let k=$('otakey').value.trim()||localStorage.getItem('ota_key')||'';
  if(!k)k=(prompt('Mã quản trị 8 ký tự hiện trên màn hình robot:')||'').trim();
  if(k){localStorage.setItem('ota_key',k);$('otakey').value=k;}return k;}
async function callAdmin(path){const k=adminKey();if(!k)return null;
  const r=await fetch(path,{method:'POST',headers:{'X-Robot':'1','X-Token':k}});
  if(r.status==401){localStorage.removeItem('ota_key');$('otakey').value='';$('msg').textContent='Sai mã quản trị.';return null;}
  if(r.status==429){$('msg').textContent='Thử sai nhiều lần, đợi một phút.';return null;}
  $('msg').textContent=r.ok?'':'Lỗi: '+(await r.text());return r.ok?r.json():null;}
$('speed').oninput=e=>$('spv').textContent=e.target.value;
let timer=null;
// A long press on a button must not open the browser's copy/select menu
document.addEventListener('contextmenu',e=>{if(!e.target.closest('input,textarea'))e.preventDefault();});
function go(dir){const send=()=>call('/api/move?dir='+dir+'&speed='+$('speed').value+'&ms=400');send();timer=setInterval(send,250);}
function halt(){if(timer){clearInterval(timer);timer=null;}call('/api/stop');}
document.querySelectorAll('[data-dir]').forEach(b=>{
  b.addEventListener('pointerdown',e=>{e.preventDefault();b.setPointerCapture(e.pointerId);go(b.dataset.dir);});
  ['pointerup','pointercancel','lostpointercapture'].forEach(ev=>b.addEventListener(ev,halt));});
$('stop').onclick=halt;$('dance').onclick=()=>call('/api/dance');
[['left','◀ Xi nhan trái'],['right','Xi nhan phải ▶'],['both','Cả hai'],['alternate','Xen kẽ'],['off','Tắt']].forEach(([m,l])=>{
  const b=document.createElement('button');b.textContent=l;b.style.padding='10px 12px';
  b.onclick=()=>call('/api/lights?mode='+m+'&ms=8000');$('lights').appendChild(b);});
$('rplay').onclick=()=>call('/api/radio?ch='+$('radio').value);$('rstop').onclick=()=>call('/api/radio?ch=stop');
$('ota').onclick=async()=>{$('msg').textContent='Đang kiểm tra bản cập nhật...';const r=await callAdmin('/api/ota');
  if(r)$('msg').textContent=r.started?'Có bản mới, robot sẽ tự cập nhật và khởi động lại.':(r.message||'Không có bản mới.');};
function exact(extra){call('/api/move?speed='+$('speed').value+extra);}
$('fwdcm').onclick=()=>exact('&dir=forward&cm='+$('cm').value);
$('backcm').onclick=()=>exact('&dir=backward&cm='+$('cm').value);
$('turnl').onclick=()=>exact('&dir=left&deg='+$('deg').value);
$('turnr').onclick=()=>exact('&dir=right&deg='+$('deg').value);
$('calf').onclick=()=>call('/api/cal_test?kind=fwd');
$('calt').onclick=()=>call('/api/cal_test?kind=turn');
async function calSave(kind,id){const v=$(id).value;if(!v){$('msg').textContent='Nhập số đo trước.';return;}
  const r=await call('/api/cal_save?kind='+kind+'&value='+encodeURIComponent(v));
  if(r){$('msg').textContent='Đã lưu hiệu chỉnh.';$(id).value='';refresh();}}
$('calfs').onclick=()=>calSave('fwd','calfv');$('calts').onclick=()=>calSave('turn','caltv');
$('apsave').onclick=async()=>{const p=$('appass').value;if(p.length<8){$('msg').textContent='Mật khẩu tối thiểu 8 ký tự.';return;}
  const r=await callAdmin('/api/ap?pass='+encodeURIComponent(p));if(r){$('msg').textContent='Đã đổi. Nối lại Wi-Fi của robot bằng mật khẩu mới.';$('appass').value='';}};
$('selftest').onclick=()=>call('/api/selftest');
$('apon').onclick=()=>callAdmin('/api/ap?on=1');$('apoff').onclick=()=>callAdmin('/api/ap?on=0');
async function refresh(){try{const r=await fetch('/api/status');const s=await r.json();
  $('selftestout').textContent=s.selftest_running?'Đang kiểm tra...':(s.selftest||'Chưa chạy lần nào.');
  $('apinfo').textContent=s.ap_on?(s.ap_active?('Đang phát: '+s.ap_ssid+'. Điện thoại nối vào rồi mở '+s.ap_url):('Đã bật, sẽ phát khi robot nối Wi-Fi nhà: '+s.ap_ssid)):'Đã tắt';
  if(document.activeElement!==$('musicurl')&&!$('musicurl').value)$('musicurl').value=s.music_server||'';
  if(!$('radio').options.length)s.channels.forEach(c=>{const o=document.createElement('option');o.value=c.id;o.textContent=c.name;$('radio').appendChild(o);});
  const rows=[['Phiên bản',s.version],['Hiệu chỉnh',(s.cal_cm||'?')+' cm/s, '+(s.cal_deg||'?')+' độ/s (ở 100%)'],['Wi-Fi',s.ssid+' ('+s.rssi+' dBm)'],['IP',s.ip],['Trạng thái',s.state],
    ['Đèn',s.lights||'off'],['Radio',(s.radio||'-')+(s.radio_more?' (còn '+s.radio_more+' ứng viên)':'')],['Bộ nhớ trống',Math.round(s.free_heap/1024)+' KB'],['Hoạt động',Math.round(s.uptime_s/60)+' phút']];
  $('status').innerHTML=rows.map(r=>'<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>').join('');}catch(e){}}
$('rnext').onclick=async()=>{const r=await call('/api/next');if(r)$('msg').textContent='Đang đổi...';};
$('rfind').onclick=async()=>{$('msg').textContent='Đang tìm đài...';
  const r=await call('/api/radio_find?q='+encodeURIComponent($('rquery').value)+'&country=VN');
  if(r)$('msg').textContent='Tìm thấy '+r.stations.length+' đài: '+r.stations.join(', ')+'. Nếu đài không phát, robot tự đổi đài.';};
$('jsave').onclick=async()=>{const r=await callAdmin('/api/jamendo?id='+encodeURIComponent($('jamendo').value.trim()));
  if(r)$('msg').textContent='Đã lưu client id Jamendo.';};
$('mplay').onclick=async()=>{$('msg').textContent='Đang tìm bài...';
  const r=await call('/api/music?song='+encodeURIComponent($('song').value)+'&artist='+encodeURIComponent($('artist').value));
  if(r)$('msg').textContent='Đang phát: '+r.title+' (bắt đầu khi robot rảnh)';};
$('msave').onclick=async()=>{const r=await callAdmin('/api/music_server?url='+encodeURIComponent($('musicurl').value));
  if(r)$('msg').textContent='Đã lưu máy chủ nhạc.';};
$('otakey').value=localStorage.getItem('ota_key')||'';
$('otaup').onclick=()=>{
  const f=$('otafile').files[0];if(!f){$('msg').textContent='Chọn file .bin trước.';return;}
  if(!confirm('Cập nhật firmware từ file '+f.name+'? Robot sẽ khởi động lại.'))return;
  localStorage.setItem('ota_key',$('otakey').value);
  const x=new XMLHttpRequest();x.open('POST','/api/ota_upload');
  x.setRequestHeader('X-Robot','1');x.setRequestHeader('X-Token',$('otakey').value);
  x.setRequestHeader('Content-Type','application/octet-stream');
  $('otaprog').style.display='block';
  x.upload.onprogress=e=>{if(e.lengthComputable)$('otaprog').value=e.loaded*100/e.total;};
  x.onload=()=>{let m='';try{const j=JSON.parse(x.responseText);m=j.success?'Xong, robot đang khởi động lại.':'Lỗi: '+j.error;}catch(e){m=x.status==401?'Sai mã OTA.':'Lỗi '+x.status+' '+x.responseText;}
    $('msg').textContent=m;};
  x.onerror=()=>{$('msg').textContent='Mất kết nối (nếu đã tải xong 100% thì robot có thể đang khởi động lại).';};
  x.send(f);};
async function loadServers(){try{const d=await (await fetch('/api/servers')).json();
  const box=$('servers');box.innerHTML='';
  const rows=[{name:'Máy chủ mặc định',url:d.default_url,builtin:true}].concat(d.profiles.map((p,i)=>({name:p.name,url:p.url,index:i})));
  rows.forEach(r=>{const active=r.url===d.active_url;const row=document.createElement('div');row.className='row';row.style.marginBottom='8px';
    const label=document.createElement('div');label.style.cssText='flex:1 1 100%;font-size:13px;word-break:break-all';
    label.textContent=(active?'✓ ':'')+r.name+' — '+r.url;row.appendChild(label);
    const use=document.createElement('button');use.textContent=active?'Đang dùng':'Dùng';use.disabled=active;
    use.onclick=async()=>{if(confirm('Đổi sang máy chủ "'+r.name+'"? Robot sẽ khởi động lại.')){const x=await callAdmin('/api/server_select?url='+encodeURIComponent(r.url));
      if(x)$('msg').textContent='Đang khởi động lại để dùng máy chủ mới...';}};
    row.appendChild(use);
    if(!r.builtin){const del=document.createElement('button');del.textContent='Xóa';
      del.onclick=async()=>{await callAdmin('/api/server_remove?index='+r.index);loadServers();};row.appendChild(del);}
    box.appendChild(row);});}catch(e){}}
$('srvadd').onclick=async()=>{const r=await callAdmin('/api/server_add?name='+encodeURIComponent($('srvname').value.trim())+'&url='+encodeURIComponent($('srvurl').value.trim()));
  if(r){$('srvname').value='';$('srvurl').value='';loadServers();}};
loadServers();
refresh();setInterval(refresh,3000);
</script></body></html>)HTML";

// ---------- helpers ----------

bool Authorized(httpd_req_t* req) {
    if (httpd_req_get_hdr_value_len(req, "X-Robot") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing X-Robot header");
        return false;
    }
    Settings settings("robot", false);
    std::string token = settings.GetString("web_token");
    if (token.empty()) {
        return true;
    }
    char given[65] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Token", given, sizeof(given)) == ESP_OK && token == given) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}

std::string Query(httpd_req_t* req, const char* key, const std::string& fallback = "") {
    char query[640];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return fallback;
    }
    char value[400];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return fallback;
    }
    return url::Decode(value);
}

// Token that protects firmware upload. It is generated on first use, kept in NVS namespace
// "robot" key "ota_token", and shown on the robot's own screen / log (physical access), never
// sent over the network by the robot.
std::string OtaToken() {
    Settings settings("robot", true);
    std::string token = settings.GetString("ota_token");
    if (token.empty()) {
        uint8_t random[4];
        esp_fill_random(random, sizeof(random));
        char text[9];
        snprintf(text, sizeof(text), "%02x%02x%02x%02x", random[0], random[1], random[2], random[3]);
        token = text;
        settings.SetString("ota_token", token);
    }
    return token;
}

// Five wrong keys block every admin call for a minute (the key is 32 bits, the server is slow,
// but nothing else stops a loop of guesses)
bool AuthorizedOta(httpd_req_t* req) {
    static int failures = 0;
    static int64_t blocked_until_us = 0;
    if (httpd_req_get_hdr_value_len(req, "X-Robot") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing X-Robot header");
        return false;
    }
    if (esp_timer_get_time() < blocked_until_us) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "too many wrong keys, wait a minute", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    std::string token = OtaToken();
    char given[33] = {};
    bool ok = httpd_req_get_hdr_value_str(req, "X-Token", given, sizeof(given)) == ESP_OK &&
              strlen(given) == token.size();
    if (ok) {
        uint8_t diff = 0;  // constant-time compare
        for (size_t i = 0; i < token.size(); i++) {
            diff |= static_cast<uint8_t>(given[i] ^ token[i]);
        }
        ok = diff == 0;
    }
    if (ok) {
        failures = 0;
    } else {
        if (++failures >= 5) {
            failures = 0;
            blocked_until_us = esp_timer_get_time() + 60LL * 1000000;
        }
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
    }
    return ok;
}

esp_err_t SendJson(httpd_req_t* req, const std::string& json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
}

// ---------- handlers ----------

esp_err_t IndexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t StatusHandler(httpd_req_t* req) {
    auto& wifi = WifiManager::GetInstance();
    auto& radio = RadioPlayer::GetInstance();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "board", BOARD_NAME);
    cJSON_AddStringToObject(root, "ssid", wifi.GetSsid().c_str());
    cJSON_AddStringToObject(root, "ip", wifi.GetIpAddress().c_str());
    cJSON_AddNumberToObject(root, "rssi", wifi.GetRssi());
    int state = static_cast<int>(Application::GetInstance().GetDeviceState());
    cJSON_AddStringToObject(root, "state", state >= 0 && state < 12 ? kStateNames[state] : "unknown");
    cJSON_AddBoolToObject(root, "moving", RobotController::GetInstance().IsMoving());
    cJSON_AddStringToObject(root, "lights", SignalName(TurnSignals::GetInstance().Current()));
    cJSON_AddBoolToObject(root, "radio_playing", radio.IsPlaying());
    cJSON_AddStringToObject(root, "radio", radio.NowPlaying().c_str());
    cJSON_AddStringToObject(root, "music_server", MusicClient::ServerUrl().c_str());
    cJSON_AddBoolToObject(root, "jamendo", !MusicClient::JamendoClientId().empty());
    cJSON_AddNumberToObject(root, "radio_more", radio.Remaining());
    cJSON_AddNumberToObject(root, "cal_cm", RobotController::GetInstance().cm_per_s_full());
    cJSON_AddNumberToObject(root, "cal_deg", RobotController::GetInstance().deg_per_s_full());
    cJSON_AddBoolToObject(root, "ap_on", RobotAp::GetInstance().Enabled());
    cJSON_AddBoolToObject(root, "ap_active", RobotAp::GetInstance().Active());
    cJSON_AddStringToObject(root, "ap_ssid", RobotAp::GetInstance().Ssid().c_str());
    cJSON_AddStringToObject(root, "ap_url", RobotAp::GetInstance().Url().c_str());
    cJSON_AddBoolToObject(root, "selftest_running", StartupSelfTest::GetInstance().Running());
    cJSON_AddStringToObject(root, "selftest", StartupSelfTest::GetInstance().Report().c_str());
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON* channels = cJSON_AddArrayToObject(root, "channels");
    for (const auto& channel : RadioPlayer::Channels()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", channel.id);
        cJSON_AddStringToObject(item, "name", channel.name);
        cJSON_AddItemToArray(channels, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string json(text);
    cJSON_free(text);
    cJSON_Delete(root);
    return SendJson(req, json);
}

esp_err_t MoveHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    auto& robot = RobotController::GetInstance();
    int speed = atoi(Query(req, "speed", std::to_string(robot.default_speed())).c_str());
    int ms = atoi(Query(req, "ms", "400").c_str());
    std::string error;
    const std::string dir = Query(req, "dir");
    const std::string cm = Query(req, "cm");
    const std::string deg = Query(req, "deg");
    bool ok;
    if (!cm.empty()) {
        ok = robot.MoveDistance(dir, atoi(cm.c_str()), speed, &error);
    } else if (!deg.empty()) {
        ok = robot.Turn(dir, atoi(deg.c_str()), speed, &error);
    } else {
        ok = robot.Move(dir, speed, ms, &error);
    }
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t CalTestHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    std::string error;
    int kind = Query(req, "kind", "fwd") == "turn" ? 1 : 0;
    if (!RobotController::GetInstance().RunTest(kind, &error)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t CalSaveHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    std::string error;
    int kind = Query(req, "kind", "fwd") == "turn" ? 1 : 0;
    float value = static_cast<float>(atof(Query(req, "value", "0").c_str()));
    auto& robot = RobotController::GetInstance();
    if (!robot.SaveCalibration(kind, value, &error)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    char text[96];
    snprintf(text, sizeof(text), "{\"ok\":true,\"cal_cm\":%.1f,\"cal_deg\":%.0f}", robot.cm_per_s_full(),
             robot.deg_per_s_full());
    return SendJson(req, text);
}

// Robot hotspot settings. Admin key: whoever is on the router's network must not be able to read or change them
esp_err_t ApHandler(httpd_req_t* req) {
    if (!AuthorizedOta(req)) {
        return ESP_OK;
    }
    std::string pass = Query(req, "pass");
    std::string on = Query(req, "on");
    if (!pass.empty()) {
        bool printable = true;
        for (char c : pass) {
            printable = printable && c >= 0x20 && c < 0x7f;
        }
        if (pass.size() < 8 || pass.size() > 63 || !printable) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password: 8-63 printable ASCII characters");
        }
    }
    {
        Settings settings("robot", true);
        if (!pass.empty()) {
            settings.SetString("ap_pass", pass);
        }
        if (!on.empty()) {
            settings.SetBool("ap_on", on == "1");
        }
    }
    SendJson(req, "{\"ok\":true}");
    RobotAp::GetInstance().Apply();  // after the answer: a phone on the hotspot loses the link right here
    return ESP_OK;
}

esp_err_t SelfTestHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    if (!StartupSelfTest::GetInstance().RunAgain()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a test is already running");
    }
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t StopHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    RobotController::GetInstance().Stop();
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t DanceHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    RobotController::GetInstance().Dance();
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t LightsHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    SignalPattern pattern;
    if (!ParseSignalMode(Query(req, "mode"), &pattern)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be left, right, both, alternate, fast, slow or off");
    }
    int ms = std::clamp(atoi(Query(req, "ms", "5000").c_str()), 500, 30000);
    TurnSignals::GetInstance().SetManual(pattern, ms);
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t EmotionHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    std::string name = Query(req, "name", "neutral");
    Application::GetInstance().Schedule(
        [name]() { Board::GetInstance().GetDisplay()->SetEmotion(name.c_str()); });
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t RadioHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    std::string channel = Query(req, "ch");
    if (channel == "stop") {
        RadioPlayer::GetInstance().Stop();
        return SendJson(req, "{\"ok\":true}");
    }
    // Only the built-in channel ids are accepted from the web page (no arbitrary URLs)
    std::string error;
    bool known = false;
    for (const auto& item : RadioPlayer::Channels()) {
        known = known || channel == item.id;
    }
    if (!known || !RadioPlayer::GetInstance().Play(channel, &error)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, known ? error.c_str() : "unknown channel");
    }
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t MusicHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    std::string title;
    std::string error;
    if (!MusicClient::PlaySong(Query(req, "song"), Query(req, "artist"), &title, &error)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "title", title.c_str());
    char* text = cJSON_PrintUnformatted(root);
    std::string json(text);
    cJSON_free(text);
    cJSON_Delete(root);
    return SendJson(req, json);
}

// Searches the radio directory and plays the result list with automatic fallback
esp_err_t RadioFindHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    std::vector<RadioSource> stations;
    std::string error;
    if (!RadioDirectory::Search(Query(req, "q"), Query(req, "country", "VN"), Query(req, "tag"), 8, &stations,
                                &error)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* names = cJSON_AddArrayToObject(root, "stations");
    for (const auto& station : stations) {
        cJSON_AddItemToArray(names, cJSON_CreateString(station.name.c_str()));
    }
    if (!RadioPlayer::GetInstance().PlayList(std::move(stations), false, &error)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string json(text);
    cJSON_free(text);
    cJSON_Delete(root);
    return SendJson(req, json);
}

esp_err_t NextHandler(httpd_req_t* req) {
    if (!Authorized(req)) {
        return ESP_OK;
    }
    std::string error;
    if (!RadioPlayer::GetInstance().Next(&error)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t JamendoHandler(httpd_req_t* req) {
    if (!AuthorizedOta(req)) {
        return ESP_OK;
    }
    if (!MusicClient::SetJamendoClientId(Query(req, "id"))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "client id must be letters and digits only");
    }
    return SendJson(req, "{\"ok\":true}");
}

// ---------- server profiles ("which server / agent does the robot talk to") ----------

esp_err_t ServersHandler(httpd_req_t* req) {
    return SendJson(req, ServerProfiles::ToJson());
}

esp_err_t ServerAddHandler(httpd_req_t* req) {
    if (!AuthorizedOta(req)) {
        return ESP_OK;
    }
    std::string error;
    if (!ServerProfiles::Add(Query(req, "name"), Query(req, "url"), &error)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t ServerRemoveHandler(httpd_req_t* req) {
    if (!AuthorizedOta(req)) {
        return ESP_OK;
    }
    if (!ServerProfiles::Remove(static_cast<size_t>(atoi(Query(req, "index", "-1").c_str())))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cannot remove");
    }
    return SendJson(req, "{\"ok\":true}");
}

// The server is read when the robot activates, so a reboot applies the change
esp_err_t ServerSelectHandler(httpd_req_t* req) {
    if (!AuthorizedOta(req)) {
        return ESP_OK;
    }
    std::string error;
    if (!ServerProfiles::Select(Query(req, "url"), &error)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    esp_err_t result = SendJson(req, "{\"ok\":true,\"reboot\":true}");
    Application::GetInstance().Schedule([]() { Application::GetInstance().Reboot(); });
    return result;
}

esp_err_t MusicServerHandler(httpd_req_t* req) {
    if (!AuthorizedOta(req)) {
        return ESP_OK;
    }
    if (!MusicClient::SetServerUrl(Query(req, "url"))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url must start with http:// or https://");
    }
    return SendJson(req, "{\"ok\":true}");
}

// Firmware upload from the browser (raw .bin body). Needs the on-device OTA token.
esp_err_t OtaUploadHandler(httpd_req_t* req) {
    if (!AuthorizedOta(req)) {
        return ESP_OK;
    }
    RadioPlayer::GetInstance().Stop();
    RobotController::GetInstance().Stop();
    return ota_upload::Handle(req);
}

// Checks the OTA server and, when a newer firmware exists, upgrades through the same
// Application::UpgradeFirmware() path the boot-time check uses. Runs in its own task.
esp_err_t OtaHandler(httpd_req_t* req) {
    if (!AuthorizedOta(req)) {
        return ESP_OK;
    }
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle) {
        return SendJson(req, "{\"started\":false,\"message\":\"Robot dang ban, thu lai sau.\"}");
    }
    Ota ota;
    auto check = ota.CheckVersion();
    if (!check) {
        return SendJson(req, "{\"started\":false,\"message\":\"Khong ket noi duoc may chu cap nhat.\"}");
    }
    if (!ota.HasNewVersion()) {
        return SendJson(req, "{\"started\":false,\"message\":\"Dang la ban moi nhat.\"}");
    }
    struct Job {
        std::string url;
        std::string version;
    };
    auto* job = new Job{ota.GetFirmwareUrl(), ota.GetFirmwareVersion()};
    BaseType_t created = xTaskCreate(
        [](void* arg) {
            std::unique_ptr<Job> job(static_cast<Job*>(arg));
            Application::GetInstance().UpgradeFirmware(job->url, job->version);
            vTaskDelete(nullptr);
        },
        "web_ota", 8192, job, 2, nullptr);
    if (created != pdPASS) {
        delete job;
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot start update task");
    }
    return SendJson(req, "{\"started\":true}");
}

}  // namespace

std::string RobotWebServer::EnsureAdminKey() { return OtaToken(); }

RobotWebServer& RobotWebServer::GetInstance() {
    static RobotWebServer instance;
    return instance;
}

void RobotWebServer::Initialize() {
    // WifiManager creates the default loop later and tolerates it already existing
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Cannot create event loop: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &RobotWebServer::OnEvent, this));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, &RobotWebServer::OnEvent, this));
}

void RobotWebServer::OnEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<RobotWebServer*>(arg);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        self->Start();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        if (RobotAp::JustStarted() || RobotAp::OwnsAp()) {
            return;  // the robot's own hotspot, not the open setup portal
        }
        self->Stop();  // setup hotspot is open: never expose the controls there
    }
}

void RobotWebServer::Start() {
    if (server_ != nullptr) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;  // the Wi-Fi setup server keeps the default 32768
    config.max_uri_handlers = 30;
    config.recv_wait_timeout = 10;  // firmware upload over Wi-Fi
    config.stack_size = 8192;  // OTA check runs in the handler
    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start web server");
        server_ = nullptr;
        return;
    }
    struct Route {
        const char* uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t*);
    };
    const Route routes[] = {
        {"/", HTTP_GET, IndexHandler},          {"/api/status", HTTP_GET, StatusHandler},
        {"/api/move", HTTP_POST, MoveHandler},  {"/api/stop", HTTP_POST, StopHandler},
        {"/api/cal_test", HTTP_POST, CalTestHandler}, {"/api/cal_save", HTTP_POST, CalSaveHandler},
        {"/api/ap", HTTP_POST, ApHandler}, {"/api/selftest", HTTP_POST, SelfTestHandler},
        {"/api/dance", HTTP_POST, DanceHandler}, {"/api/emotion", HTTP_POST, EmotionHandler},
        {"/api/radio", HTTP_POST, RadioHandler}, {"/api/ota", HTTP_POST, OtaHandler},
        {"/api/music", HTTP_POST, MusicHandler}, {"/api/music_server", HTTP_POST, MusicServerHandler},
        {"/api/ota_upload", HTTP_POST, OtaUploadHandler},
        {"/api/radio_find", HTTP_POST, RadioFindHandler}, {"/api/next", HTTP_POST, NextHandler},
        {"/api/jamendo", HTTP_POST, JamendoHandler},
        {"/api/lights", HTTP_POST, LightsHandler},
        {"/api/servers", HTTP_GET, ServersHandler},
        {"/api/server_add", HTTP_POST, ServerAddHandler},
        {"/api/server_remove", HTTP_POST, ServerRemoveHandler},
        {"/api/server_select", HTTP_POST, ServerSelectHandler},
    };
    for (const auto& route : routes) {
        httpd_uri_t uri = {};
        uri.uri = route.uri;
        uri.method = route.method;
        uri.handler = route.handler;
        httpd_register_uri_handler(server_, &uri);
    }
    ESP_LOGI(TAG, "Robot page: http://%s:8080/", WifiManager::GetInstance().GetIpAddress().c_str());
    // Show the firmware-upload key on the robot itself
    std::string token = OtaToken();
    ESP_LOGW(TAG, "Firmware upload key: %s", token.c_str());
    Application::GetInstance().Schedule([token]() {
        Board::GetInstance().GetDisplay()->ShowNotification("OTA key: " + token, 30000);
    });
}

void RobotWebServer::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}
