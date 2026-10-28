#pragma once
// Web dashboard served by the ESP32 at "/"
static const char DASHBOARD_HTML[] = R"rawliteral(<!doctype html>
<html lang="ko"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Classifier</title>
<style>
:root{--bg:#0f1216;--panel:#171b21;--line:#262c35;--text:#e6e9ee;--muted:#8a94a3;--ok:#34d399;--bad:#f87171}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.5 system-ui,"Segoe UI","Malgun Gothic",sans-serif}
header{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;padding:12px 20px;border-bottom:1px solid var(--line);background:var(--panel)}
h1{font-size:18px;margin:0}h1 span{color:var(--muted);font-weight:400}
h2{font-size:13px;margin:0 0 10px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.04em}
.stats{display:flex;gap:14px;color:var(--muted);font-size:13px;flex-wrap:wrap}.stats b{color:var(--text);font-variant-numeric:tabular-nums}
.pill{padding:2px 10px;border-radius:99px;font-size:12px}.on{background:#10b98126;color:var(--ok)}.off{background:#ef444426;color:var(--bad)}
main{display:grid;grid-template-columns:minmax(0,1.4fr) minmax(300px,1fr);gap:16px;padding:16px}
@media(max-width:860px){main{grid-template-columns:1fr}}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px;min-width:0}
.col{display:grid;gap:16px;align-content:start}
.viewer{position:relative;aspect-ratio:4/3;background:#000;border-radius:8px;overflow:hidden}
.viewer img{width:100%;height:100%;object-fit:contain;display:block}
.crop{position:absolute;top:0;bottom:0;left:12.5%;width:75%;border:3px solid var(--c,#fff);border-radius:4px;transition:border-color .2s}
.shade{position:absolute;top:0;bottom:0;width:12.5%;background:#0008}.shade.l{left:0}.shade.r{right:0}
.tag{position:absolute;left:-3px;top:-3px;background:var(--c,#fff);color:#000;font-weight:700;padding:2px 10px;border-radius:4px 0 6px 0;font-size:15px}
.nostream{position:absolute;inset:0;display:grid;place-items:center;color:var(--muted)}
.hint{color:var(--muted);font-size:12px;margin:8px 0 0}
.top{display:flex;align-items:baseline;justify-content:space-between;gap:8px}
.top .lbl{font-size:34px;font-weight:800;color:var(--c,#fff);word-break:break-all}
.top .pct{font-size:26px;font-weight:700;font-variant-numeric:tabular-nums}
.warn{color:#f59e0b;font-size:13px;min-height:20px}
.bar{margin-top:10px}.bar .row{display:flex;justify-content:space-between;font-size:13px}
.bar .track{height:10px;background:#0c0f13;border-radius:99px;overflow:hidden;margin-top:3px}
.bar .fill{height:100%;background:var(--c);width:0;transition:width .2s}
.thr{position:relative}.thr .mark{position:absolute;top:0;bottom:0;width:2px;background:#fff6}
.inputwrap{display:flex;gap:14px;align-items:center}
.inputwrap img{width:144px;height:144px;image-rendering:pixelated;border-radius:6px;border:1px solid var(--line);background:#000}
.timeline{display:flex;gap:2px;height:28px}.timeline i{flex:1;border-radius:2px;background:var(--line)}
.legend{display:flex;gap:12px;flex-wrap:wrap;margin-top:8px;font-size:12px;color:var(--muted)}
.legend span::before{content:"";display:inline-block;width:10px;height:10px;border-radius:2px;background:var(--c);margin-right:5px}
.log{max-height:180px;overflow:auto;font-size:13px;margin:0;padding:0;list-style:none}
.log li{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid var(--line)}
.log time{color:var(--muted);font-variant-numeric:tabular-nums}
label{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:13px}
.ctrls{display:grid;gap:10px}.ctrls input[type=range]{flex:1}
.toggles{display:flex;gap:16px}
</style></head><body>
<header>
  <h1>ESP32-S3 Sense <span>실시간 분류</span></h1>
  <div class="stats">
    <span id="conn" class="pill off">연결 중…</span>
    <span>추론 <b id="fps">-</b> fps</span>
    <span>전처리 <b id="dsp">-</b> ms</span>
    <span>모델 <b id="nn">-</b> ms</span>
    <span>Wi-Fi <b id="rssi">-</b> dBm</span>
    <span>누적 <b id="count">-</b></span>
  </div>
</header>
<main>
  <section class="panel">
    <h2>카메라</h2>
    <div class="viewer">
      <img id="stream" alt="">
      <div id="nostream" class="nostream">스트림 연결 중…</div>
      <div class="shade l"></div><div class="shade r"></div>
      <div class="crop" id="crop"><span class="tag" id="tag">-</span></div>
    </div>
    <p class="hint">테두리 안 정사각형 영역만 96x96으로 줄여서 모델에 입력됩니다. 테두리 색은 현재 예측 클래스입니다.</p>
  </section>

  <div class="col">
    <section class="panel">
      <h2>현재 예측</h2>
      <div class="top"><span class="lbl" id="topLabel">-</span><span class="pct" id="topPct">-</span></div>
      <div class="warn" id="warn"></div>
      <div id="bars"></div>
    </section>

    <section class="panel">
      <h2>모델 입력 이미지</h2>
      <div class="inputwrap">
        <img id="input" alt="">
        <p class="hint">ESP32가 실제로 분류하는 96x96 이미지 (1초마다 갱신)</p>
      </div>
    </section>

    <section class="panel">
      <h2>최근 예측 흐름</h2>
      <div class="timeline" id="timeline"></div>
      <div class="legend" id="legend"></div>
      <h2 style="margin-top:14px">변경 기록</h2>
      <ul class="log" id="log"></ul>
    </section>

    <section class="panel">
      <h2>설정</h2>
      <div class="ctrls">
        <label>확신 임계값 <input type="range" id="thr" min="0" max="1" step="0.05"><b id="thrVal"></b></label>
        <label>밝기 <input type="range" id="brightness" min="-2" max="2" step="1"></label>
        <div class="toggles">
          <label><input type="checkbox" id="vflip"> 상하 반전</label>
          <label><input type="checkbox" id="hmirror"> 좌우 반전</label>
        </div>
      </div>
    </section>
  </div>
</main>
<script>
const $=id=>document.getElementById(id);
const COLORS=["#60a5fa","#f472b6","#34d399","#fbbf24","#a78bfa","#fb923c","#22d3ee","#f87171"];
const HIST=60;let labels=[],history=[],lastTop=null,failures=0;

function startStream(){$("stream").src=`http://${location.hostname}:81/stream?t=${Date.now()}`}
$("stream").onload=()=>$("nostream").style.display="none";
$("stream").onerror=()=>{$("nostream").style.display="grid";setTimeout(startStream,2000)};

function setup(ls){
  labels=ls;
  $("bars").innerHTML=ls.map((l,i)=>`<div class="bar" style="--c:${COLORS[i%8]}"><div class="row"><span>${l}</span><b id="v${i}">-</b></div><div class="track thr"><div class="fill" id="f${i}"></div><div class="mark" id="m${i}"></div></div></div>`).join("");
  $("legend").innerHTML=ls.map((l,i)=>`<span style="--c:${COLORS[i%8]}">${l}</span>`).join("")+`<span style="--c:#475569">확신 낮음</span>`;
  $("timeline").innerHTML="<i></i>".repeat(HIST);
}

function render(r){
  if(labels.join()!==r.labels.join())setup(r.labels);
  const top=r.top,p=r.scores[top],sure=p>=r.threshold,c=sure?COLORS[top%8]:"#94a3b8";
  document.documentElement.style.setProperty("--c",c);
  $("topLabel").textContent=r.labels[top];$("topPct").textContent=(p*100).toFixed(1)+"%";
  $("tag").textContent=sure?`${r.labels[top]} ${(p*100).toFixed(0)}%`:"확신 낮음";
  $("warn").textContent=sure?"":`확신도가 임계값(${Math.round(r.threshold*100)}%)보다 낮습니다`;
  r.scores.forEach((s,i)=>{$("v"+i).textContent=(s*100).toFixed(1)+"%";$("f"+i).style.width=s*100+"%";$("m"+i).style.left=r.threshold*100+"%"});
  $("fps").textContent=r.fps.toFixed(1);$("dsp").textContent=r.dsp_ms;$("nn").textContent=r.nn_ms;
  $("rssi").textContent=r.rssi;$("count").textContent=r.count;
  if(document.activeElement!==$("thr")){$("thr").value=r.threshold}
  $("thrVal").textContent=Math.round($("thr").value*100)+"%";

  history.push(sure?top:-1);if(history.length>HIST)history.shift();
  [...$("timeline").children].forEach((el,i)=>{const h=history[i-(HIST-history.length)];el.style.background=h===undefined?"":h<0?"#475569":COLORS[h%8]});

  const key=sure?r.labels[top]:null;
  if(key!==lastTop){
    lastTop=key;const li=document.createElement("li");
    li.innerHTML=`<span style="color:${c};font-weight:600">${key??"확신 낮음"}</span><time>${new Date().toLocaleTimeString()} · ${(p*100).toFixed(0)}%</time>`;
    $("log").prepend(li);while($("log").children.length>50)$("log").lastChild.remove();
  }
}

async function poll(){
  try{
    const r=await (await fetch("/result")).json();
    if(r.count>0)render(r);
    $("conn").className="pill on";$("conn").textContent="● 연결됨";failures=0;
  }catch(e){if(++failures>2){$("conn").className="pill off";$("conn").textContent="● 연결 끊김"}}
  setTimeout(poll,250);
}
function refreshInput(){$("input").src="/input.bmp?t="+Date.now()}
$("input").onload=$("input").onerror=()=>setTimeout(refreshInput,1000);

const control=(v,val)=>fetch(`/control?var=${v}&val=${val}`);
$("thr").oninput=()=>$("thrVal").textContent=Math.round($("thr").value*100)+"%";
$("thr").onchange=()=>control("threshold",$("thr").value);
$("brightness").onchange=()=>control("brightness",$("brightness").value);
["vflip","hmirror"].forEach(k=>$(k).onchange=()=>control(k,$(k).checked?1:0));

fetch("/status").then(r=>r.json()).then(s=>{$("vflip").checked=!!s.vflip;$("hmirror").checked=!!s.hmirror;$("brightness").value=s.brightness}).catch(()=>{});
startStream();poll();refreshInput();
</script></body></html>)rawliteral";
