#ifndef LOCAL_CONTROL_PAGE_H_
#define LOCAL_CONTROL_PAGE_H_

// Mobile-friendly control panel served at "/".
static const char kIndexHtml[] = R"PAGEEOF(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>小智遥控器</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif;
 background:linear-gradient(160deg,#101526,#1b2340 60%,#101526);color:#eef1f8;min-height:100vh}
.wrap{max-width:520px;margin:0 auto;padding:16px 14px 40px}
header{display:flex;align-items:center;justify-content:space-between;margin:6px 2px 14px}
h1{font-size:20px;margin:0;letter-spacing:1px}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:#34d399;margin-right:6px;
 box-shadow:0 0 8px #34d399}
.card{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.08);border-radius:16px;
 padding:14px;margin-bottom:14px;backdrop-filter:blur(6px)}
.card h2{font-size:14px;font-weight:600;margin:0 0 10px;color:#9fb0d8}
textarea{width:100%;min-height:74px;resize:vertical;border:none;border-radius:12px;padding:12px;
 font-size:16px;background:rgba(0,0,0,.28);color:#fff;outline:none}
textarea::placeholder{color:#7280a3}
.row{display:flex;gap:10px;margin-top:10px}
button{border:none;border-radius:12px;padding:13px 16px;font-size:16px;font-weight:600;color:#fff;
 background:linear-gradient(135deg,#4f6bff,#7a5cff);flex:1;cursor:pointer;transition:transform .08s}
button:active{transform:scale(.96)}
button.ghost{background:rgba(255,255,255,.1)}
button.danger{background:linear-gradient(135deg,#f87171,#ef4444)}
button:disabled{opacity:.5}
.btnrow{display:flex;gap:10px}
.slider{margin:14px 0 4px}
.slider label{display:flex;justify-content:space-between;font-size:14px;color:#c4cdea;margin-bottom:8px}
input[type=range]{width:100%;height:28px;accent-color:#7a8cff}
#shot{width:100%;border-radius:12px;margin-top:12px;display:none;background:rgba(0,0,0,.3)}
#status{font-size:12px;color:#8b98bd}
#toast{position:fixed;left:50%;bottom:34px;transform:translateX(-50%);background:rgba(20,26,46,.95);
 border:1px solid rgba(255,255,255,.15);padding:10px 18px;border-radius:24px;font-size:14px;
 opacity:0;transition:opacity .25s;pointer-events:none;max-width:90vw;text-align:center}
#toast.show{opacity:1}
.spin{display:inline-block;width:14px;height:14px;border:2px solid #fff5;border-top-color:#fff;
 border-radius:50%;animation:r .7s linear infinite;vertical-align:-2px;margin-right:6px}
@keyframes r{to{transform:rotate(360deg)}}
</style>
</head>
<body>
<div class="wrap">
 <header>
  <h1>🎛️ 小智遥控器</h1>
  <div id="status"><span class="dot"></span>连接中…</div>
 </header>

 <div class="card">
  <h2>文字对话（小智会语音回答）</h2>
  <textarea id="text" placeholder="输入你想对小智说的话，例如：你好，介绍一下你自己"></textarea>
  <div class="row"><button id="send">📤 发送</button></div>
 </div>

 <div class="card">
  <h2>摄像头 / 屏幕</h2>
  <div class="btnrow">
   <button class="ghost" id="photo">📷 拍张照片</button>
   <button class="ghost" id="screen">🖥️ 屏幕截图</button>
  </div>
  <img id="shot" alt="预览">
 </div>

 <div class="card">
  <h2>🚜 小车电机（点按测试，每次转 1.5 秒自动停）</h2>
  <div class="btnrow">
   <button id="mforward">⬆️ 前进</button>
   <button class="danger" id="mstop">⏹ 停车</button>
   <button id="mbackward">⬇️ 后退</button>
  </div>
 </div>

 <div class="card">
  <h2>音量与亮度</h2>
  <div class="slider">
   <label><span>🔊 音量</span><span id="volv">--</span></label>
   <input type="range" id="vol" min="0" max="100" step="1">
  </div>
  <div class="slider">
   <label><span>💡 亮度</span><span id="brv">--</span></label>
   <input type="range" id="br" min="0" max="100" step="1">
  </div>
  <div class="btnrow" style="margin-top:12px">
   <button class="ghost" id="light">☀️ 浅色</button>
   <button class="ghost" id="dark">🌙 深色</button>
  </div>
 </div>

 <div class="card">
  <h2>系统</h2>
  <div class="row"><button class="danger" id="reboot">🔄 重启设备</button></div>
 </div>
</div>
<div id="toast"></div>

<script>
const $=s=>document.querySelector(s);
let toastTimer;
function toast(msg,ms=2200){const t=$('#toast');t.textContent=msg;t.classList.add('show');
 clearTimeout(toastTimer);toastTimer=setTimeout(()=>t.classList.remove('show'),ms);}
function setBusy(btn,busy,html){btn.disabled=busy;if(html)btn.innerHTML=busy?'<span class="spin"></span>'+html:html;}

async function post(path,body){
 const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify(body)});
 const j=await r.json().catch(()=>({}));
 if(!r.ok||j.ok===false)throw new Error(j.error||('HTTP '+r.status));
 return j;
}

$('#send').onclick=async()=>{
 const text=$('#text').value.trim();
 if(!text){toast('请先输入内容');return;}
 const btn=$('#send');setBusy(btn,true,'发送中…');
 try{await post('/api/chat',{text});$('#text').value='';toast('已发送，听小智回答 🔊');}
 catch(e){toast('发送失败：'+e.message);}
 setBusy(btn,false,'📤 发送');
};

function grab(url){
 const img=$('#shot');img.style.display='block';img.src=url+'?t='+Date.now();
}
async function imgAction(btn,url,name){
 const old=btn.innerHTML;setBusy(btn,true,'拍摄中…');
 try{
  const r=await fetch(url+'?t='+Date.now());
  if(!r.ok){const j=await r.json().catch(()=>({}));throw new Error(j.error||r.status);}
  grab(url);toast(name+'完成');
 }catch(e){toast(name+'失败：'+e.message);}
 setBusy(btn,false,old);
}
$('#photo').onclick=function(){imgAction(this,'/api/photo.jpg','拍照');};
$('#screen').onclick=function(){imgAction(this,'/api/screen.jpg','截图');};

function debounce(fn,wait){let t;return (...a)=>{clearTimeout(t);t=setTimeout(()=>fn(...a),wait);};}
$('#vol').addEventListener('input',debounce(async e=>{
 const v=+e.target.value;$('#volv').textContent=v;
 try{await post('/api/volume',{volume:v});}catch(err){toast(err.message);}
},200));
$('#br').addEventListener('input',debounce(async e=>{
 const v=+e.target.value;$('#brv').textContent=v;
 try{await post('/api/brightness',{brightness:v});}catch(err){toast(err.message);}
},200));

async function setTheme(name){try{await post('/api/theme',{theme:name});toast('已切换'+(name==='dark'?'深色':'浅色')+'主题');}
 catch(e){toast(e.message);}}
$('#light').onclick=()=>setTheme('light');
$('#dark').onclick=()=>setTheme('dark');

async function motorAction(btn,name,action){
 const old=btn.innerHTML;setBusy(btn,true,name+'中…');
 try{
  await post('/api/motor',{action,speed:70,duration_ms:1500});
  toast(action==='stop'?'已停车 ⏹':name+' 1.5 秒');
 }catch(e){toast(name+'失败：'+e.message);}
 setBusy(btn,false,old);
}
$('#mforward').onclick=function(){motorAction(this,'前进','forward');};
$('#mbackward').onclick=function(){motorAction(this,'后退','backward');};
$('#mstop').onclick=function(){motorAction(this,'停车','stop');};

$('#reboot').onclick=async()=>{
 if(!confirm('确定重启小智吗？'))return;
 try{await post('/api/reboot',{});toast('设备正在重启，约 20 秒后恢复…',6000);
  setTimeout(refreshStatus,20000);}catch(e){toast(e.message);}
};

async function refreshStatus(){
 try{
  const j=await (await fetch('/api/status?t='+Date.now())).json();
  const c=j.control||{};
  if(typeof c.volume==='number'){$('#vol').value=c.volume;$('#volv').textContent=c.volume;}
  if(typeof c.brightness==='number'){$('#br').value=c.brightness;$('#brv').textContent=c.brightness;}
  const up=c.uptime_seconds?Math.floor(c.uptime_seconds):0;
  $('#status').innerHTML='<span class="dot"></span>在线 · 已运行 '+up+' 秒';
 }catch(e){$('#status').textContent='⚠️ 无法连接设备';}
}
refreshStatus();
setInterval(refreshStatus,5000);
</script>
</body>
</html>
)PAGEEOF";

#endif  // LOCAL_CONTROL_PAGE_H_
