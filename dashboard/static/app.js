const $ = (s) => document.querySelector(s);
const state = {
  classes: [],          // [{name, count, uploaded}]
  label: localStorage.getItem("label") || null,
  images: [],           // [{file, uploaded}] for current label, newest first
  selected: new Set(),
  lastSel: null,        // for shift-click range selection
  history: [],          // [{label, file}] captured this session, for undo
  bursting: false,
  lbIndex: -1,
};

// ---------- utils ----------
async function api(url, opts = {}) {
  if (opts.body && typeof opts.body !== "string") {
    opts.body = JSON.stringify(opts.body);
    opts.headers = { "Content-Type": "application/json" };
  }
  const r = await fetch(url, opts);
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.error || data.msg || r.statusText);
  return data;
}
let toastTimer;
function toast(msg, err = false) {
  const t = $("#toast");
  t.textContent = msg;
  t.className = "toast show" + (err ? " err" : "");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.className = "toast"), 2500);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const imgUrl = (label, file) => `/images/${label}/${file}`;

// ---------- connection / stream ----------
function startStream() {
  const img = $("#stream");
  img.src = "/api/stream?t=" + Date.now();
}
$("#stream").addEventListener("load", () => $("#noStream").classList.add("hidden"));
$("#stream").addEventListener("error", () => {
  $("#noStream").classList.remove("hidden");
  setTimeout(startStream, 3000);
});

const SENSORS = { 0x26: "OV2640", 0x3660: "OV3660", 0x5640: "OV5640" };
async function pollStatus() {
  const s = await api("/api/camera/status").catch(() => ({ online: false }));
  const pill = $("#status");
  if (s.online) {
    pill.className = "pill on";
    pill.textContent = `● ${SENSORS[s.pid] || "camera"} · ${s.ip} · ${s.rssi} dBm`;
    syncSettings(s);
  } else {
    pill.className = "pill off";
    pill.textContent = "● 오프라인";
  }
}

$("#saveHost").onclick = async () => {
  try {
    await api("/api/config", { method: "POST", body: { esp32_host: $("#host").value } });
    toast("ESP32 주소 저장됨");
    startStream();
    pollStatus();
  } catch (e) { toast(e.message, true); }
};

// ---------- camera settings ----------
function syncSettings(s) {
  document.querySelectorAll("[data-var]").forEach((el) => {
    if (el === document.activeElement || !(el.dataset.var in s)) return;
    if (el.type === "checkbox") el.checked = !!s[el.dataset.var];
    else el.value = s[el.dataset.var];
  });
}
document.querySelectorAll("[data-var]").forEach((el) => {
  el.addEventListener("change", async () => {
    const val = el.type === "checkbox" ? (el.checked ? 1 : 0) : el.value;
    try {
      await api("/api/camera/control", { method: "POST", body: { var: el.dataset.var, val } });
    } catch (e) { toast("설정 실패: " + e.message, true); }
  });
});

// ---------- classes ----------
async function loadClasses() {
  state.classes = await api("/api/classes");
  if (!state.classes.some((c) => c.name === state.label)) {
    state.label = state.classes[0]?.name || null;
  }
  renderChips();
}

function renderChips() {
  const box = $("#classChips");
  box.innerHTML = "";
  if (!state.classes.length) {
    box.innerHTML = '<span class="hint">클래스를 먼저 추가하세요 (예: good, defect, background)</span>';
  }
  state.classes.forEach((c, i) => {
    const el = document.createElement("div");
    el.className = "chip" + (c.name === state.label ? " active" : "");
    el.innerHTML = `${i < 9 ? `<span class="key">${i + 1}</span>` : ""}<b></b><small>${c.count}</small>`;
    el.querySelector("b").textContent = c.name;
    el.onclick = () => selectClass(c.name);
    box.appendChild(el);
  });
  $("#liveLabel").textContent = state.label || "";
  const cur = state.classes.find((c) => c.name === state.label);
  $("#galleryTitle").textContent = cur ? `${cur.name} — ${cur.count}장` : "이미지";
  const total = state.classes.reduce((a, c) => a + c.count, 0);
  $("#stats").textContent = state.classes.length
    ? state.classes.map((c) => `${c.name} ${c.count}${c.uploaded ? ` (EI ${c.uploaded})` : ""}`).join(" · ") + ` · 합계 ${total}`
    : "";
  $("#dlClassBtn").href = state.label ? `/api/download/${state.label}` : "#";
  ["#captureBtn", "#burstBtn", "#eiUploadBtn", "#deleteClassBtn", "#selectAllBtn"].forEach(
    (s) => ($(s).disabled = !state.label)
  );
}

async function selectClass(name) {
  if (state.bursting) return toast("연속 촬영 중에는 클래스를 바꿀 수 없습니다", true);
  state.label = name;
  localStorage.setItem("label", name);
  state.selected.clear();
  renderChips();
  await loadImages();
}

$("#newClassForm").onsubmit = async (e) => {
  e.preventDefault();
  const name = $("#newClass").value.trim();
  if (!/^[A-Za-z0-9_-]{1,40}$/.test(name)) return toast("영문/숫자/_/- 만 사용 가능합니다", true);
  try {
    await api("/api/classes", { method: "POST", body: { name } });
    $("#newClass").value = "";
    await loadClasses();
    selectClass(name);
  } catch (e) { toast(e.message, true); }
};

$("#deleteClassBtn").onclick = async () => {
  const c = state.classes.find((c) => c.name === state.label);
  if (!c || !confirm(`'${c.name}' 클래스와 이미지 ${c.count}장을 모두 삭제할까요?`)) return;
  await api(`/api/classes/${c.name}`, { method: "DELETE" });
  state.history = state.history.filter((h) => h.label !== c.name);
  state.label = null;
  await loadClasses();
  await loadImages();
  toast("클래스 삭제됨");
};

// ---------- gallery ----------
async function loadImages() {
  state.images = state.label ? await api(`/api/images/${state.label}`) : [];
  state.selected = new Set([...state.selected].filter((f) => state.images.some((i) => i.file === f)));
  renderGrid();
}

function thumb(img, isNew = false) {
  const el = document.createElement("div");
  el.className = "thumb" + (state.selected.has(img.file) ? " sel" : "") + (isNew ? " new" : "");
  el.dataset.file = img.file;
  el.innerHTML = `<img loading="lazy" src="${imgUrl(state.label, img.file)}">` +
    (img.uploaded ? '<span class="ei">EI</span>' : "") + '<span class="zoom">🔍</span>';
  el.title = img.file;
  el.onclick = (e) => {
    if (e.target.classList.contains("zoom")) return openLightbox(img.file);
    toggleSelect(img.file, e.shiftKey);
  };
  el.ondblclick = () => openLightbox(img.file);
  return el;
}

function renderGrid() {
  const grid = $("#grid");
  grid.innerHTML = "";
  state.images.forEach((img) => grid.appendChild(thumb(img)));
  $("#emptyMsg").classList.toggle("hidden", !!state.images.length || !state.label);
  updateSelUI();
}

function toggleSelect(file, range) {
  if (range && state.lastSel) {
    const files = state.images.map((i) => i.file);
    const [a, b] = [files.indexOf(state.lastSel), files.indexOf(file)].sort((x, y) => x - y);
    files.slice(a, b + 1).forEach((f) => state.selected.add(f));
  } else if (state.selected.has(file)) state.selected.delete(file);
  else state.selected.add(file);
  state.lastSel = file;
  document.querySelectorAll(".thumb").forEach((el) => el.classList.toggle("sel", state.selected.has(el.dataset.file)));
  updateSelUI();
}

function updateSelUI() {
  const n = state.selected.size;
  $("#selCount").textContent = n ? `(${n})` : "";
  $("#deleteSelBtn").disabled = !n;
  $("#selectAllBtn").textContent = n && n === state.images.length ? "선택 해제" : "전체 선택";
}

$("#selectAllBtn").onclick = () => {
  if (state.selected.size === state.images.length) state.selected.clear();
  else state.images.forEach((i) => state.selected.add(i.file));
  renderGrid();
};

async function deleteFiles(label, files) {
  const r = await api(`/api/images/${label}/delete`, { method: "POST", body: { files } });
  const gone = new Set(files);
  state.history = state.history.filter((h) => !(h.label === label && gone.has(h.file)));
  files.forEach((f) => state.selected.delete(f));
  const c = state.classes.find((c) => c.name === label);
  if (c) c.count = r.count;
  if (label === state.label) {
    state.images = state.images.filter((i) => !gone.has(i.file));
    renderGrid();
  }
  renderChips();
  return r.deleted;
}

$("#deleteSelBtn").onclick = async () => {
  const files = [...state.selected];
  if (!files.length || !confirm(`선택한 ${files.length}장을 삭제할까요?`)) return;
  const n = await deleteFiles(state.label, files);
  toast(`${n}장 삭제됨`);
};

// ---------- capture ----------
function flash() {
  const f = $("#flash");
  f.classList.add("on");
  requestAnimationFrame(() => requestAnimationFrame(() => f.classList.remove("on")));
}

async function capture() {
  if (!state.label) { toast("클래스를 먼저 선택하세요", true); return false; }
  const label = state.label;
  try {
    const r = await api("/api/capture", { method: "POST", body: { label } });
    flash();
    state.history.push({ label, file: r.file });
    const c = state.classes.find((c) => c.name === label);
    if (c) c.count = r.count;
    if (label === state.label) {
      const img = { file: r.file, uploaded: false };
      state.images.unshift(img);
      $("#grid").prepend(thumb(img, true));
      $("#emptyMsg").classList.add("hidden");
      updateSelUI();
    }
    renderChips();
    return true;
  } catch (e) {
    toast(e.message, true);
    return false;
  }
}

async function undo() {
  const last = state.history.pop();
  if (!last) return toast("되돌릴 촬영이 없습니다");
  await deleteFiles(last.label, [last.file]);
  toast(`삭제: ${last.file}`);
}

async function burst() {
  if (state.bursting) { state.bursting = false; return; }
  const count = Math.max(1, +$("#burstCount").value || 1);
  const interval = Math.max(0, +$("#burstInterval").value || 0);
  const btn = $("#burstBtn"), bar = $("#burstProgress");
  state.bursting = true;
  btn.innerHTML = "중지 <kbd>B</kbd>";
  btn.className = "danger";
  bar.classList.remove("hidden");
  let ok = 0, fails = 0;
  for (let i = 0; i < count && state.bursting; i++) {
    if (await capture()) { ok++; fails = 0; } else if (++fails >= 3) break;
    bar.firstElementChild.style.width = `${((i + 1) / count) * 100}%`;
    bar.lastElementChild.textContent = `${i + 1} / ${count}`;
    if (i < count - 1 && state.bursting) await sleep(interval);
  }
  state.bursting = false;
  btn.innerHTML = "연속 촬영 <kbd>B</kbd>";
  btn.className = "accent";
  setTimeout(() => bar.classList.add("hidden"), 1200);
  toast(`연속 촬영 완료: ${ok}장`);
}

$("#captureBtn").onclick = capture;
$("#undoBtn").onclick = undo;
$("#burstBtn").onclick = burst;

// ---------- Edge Impulse upload ----------
$("#eiUploadBtn").onclick = async () => {
  const label = state.label;
  const pending = state.images.filter((i) => !i.uploaded).length;
  if (!pending) return toast("새로 업로드할 이미지가 없습니다");
  const cat = $("#eiCategory").value;
  if (!confirm(`'${label}' 이미지 ${pending}장을 Edge Impulse (${cat})로 업로드할까요?`)) return;
  const btn = $("#eiUploadBtn");
  btn.disabled = true;
  btn.textContent = "업로드 중…";
  try {
    const r = await api(`/api/ei/upload/${label}`, { method: "POST", body: { category: cat } });
    toast(r.failed.length ? `업로드 ${r.uploaded}장, 실패 ${r.failed.length}장: ${r.failed[0].error}` : `업로드 완료: ${r.uploaded}장`, !!r.failed.length);
  } catch (e) { toast("업로드 실패: " + e.message, true); }
  btn.disabled = false;
  btn.textContent = "Edge Impulse 업로드";
  await loadClasses();
  await loadImages();
};

// ---------- lightbox ----------
function openLightbox(file) {
  state.lbIndex = state.images.findIndex((i) => i.file === file);
  showLightbox();
}
function showLightbox() {
  const img = state.images[state.lbIndex];
  if (!img) return closeLightbox();
  $("#lbImg").src = imgUrl(state.label, img.file);
  $("#lbName").textContent = `${img.file}  (${state.lbIndex + 1}/${state.images.length})`;
  $("#lightbox").classList.remove("hidden");
}
function closeLightbox() { $("#lightbox").classList.add("hidden"); state.lbIndex = -1; }
async function lbDelete() {
  const img = state.images[state.lbIndex];
  if (!img) return;
  await deleteFiles(state.label, [img.file]);
  state.lbIndex = Math.min(state.lbIndex, state.images.length - 1);
  showLightbox();
}
$("#lbDelete").onclick = lbDelete;
$("#lightbox").onclick = (e) => { if (e.target.id === "lightbox") closeLightbox(); };

// ---------- keyboard ----------
document.addEventListener("keydown", (e) => {
  if (e.target.matches("input, select, textarea")) return;
  const lbOpen = state.lbIndex >= 0;
  if (lbOpen) {
    if (e.key === "Escape") closeLightbox();
    else if (e.key === "ArrowRight") { state.lbIndex = Math.min(state.lbIndex + 1, state.images.length - 1); showLightbox(); }
    else if (e.key === "ArrowLeft") { state.lbIndex = Math.max(state.lbIndex - 1, 0); showLightbox(); }
    else if (e.key === "Delete" || e.key === "Backspace") lbDelete();
    else return;
    e.preventDefault();
    return;
  }
  if (e.code === "Space") { e.preventDefault(); if (!e.repeat) capture(); }
  else if (e.key === "b" || e.key === "B") burst();
  else if (e.key === "z" || e.key === "Z") undo();
  else if (e.key === "Delete" && state.selected.size) $("#deleteSelBtn").click();
  else if (/^[1-9]$/.test(e.key) && state.classes[+e.key - 1]) selectClass(state.classes[+e.key - 1].name);
});

// ---------- init ----------
(async () => {
  const cfg = await api("/api/config");
  $("#host").value = cfg.esp32_host;
  if (!cfg.ei_key_loaded) $("#eiUploadBtn").title = ".env에 EI_API_KEY가 없습니다";
  await loadClasses();
  await loadImages();
  startStream();
  pollStatus();
  setInterval(pollStatus, 5000);
})();
