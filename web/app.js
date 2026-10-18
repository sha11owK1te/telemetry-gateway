"use strict";
const $ = id => document.getElementById(id);
const state = { selected: null, paused: false, busy: false, history: [], devices: [], healthy: false };
const number = value => Number(value).toLocaleString("zh-CN");
const clock = ms => new Date(ms).toLocaleTimeString("zh-CN", { hour12: false });
function notice(message) { $("notice").textContent = message; $("notice").hidden = !message; }
async function api(path, options) {
  const response = await fetch(path, options);
  const body = await response.json();
  if (!response.ok) throw new Error(body.error || `请求失败 (${response.status})`);
  return body;
}
function deviceName(id) { return `环境节点 ${String(id).padStart(2, "0")}`; }
function renderDevices(devices) {
  $("online").textContent = devices.filter(d => d.online).length;
  $("device-total").textContent = `/ ${devices.length}`;
  $("fleet-count").textContent = devices.length;
  const select = $("device-select");
  select.replaceChildren();
  if (!devices.length) { const option = document.createElement("option"); option.textContent = "暂无设备"; select.append(option); }
  for (const d of devices) {
    const option = document.createElement("option"); option.value = d.device; option.textContent = deviceName(d.device); select.append(option);
  }
  if (devices.length && !devices.some(d => d.device === state.selected)) state.selected = devices[0].device;
  if (state.selected !== null) select.value = String(state.selected);
  $("device-rows").replaceChildren();
  $("fleet-empty").hidden = devices.length > 0;
  for (const d of devices) {
    const tr = document.createElement("tr");
    tr.className = `device-row${d.device === state.selected ? " selected" : ""}`;
    tr.innerHTML = `<td><div class="node-name"><span class="node-icon">▦</span><div><strong>${deviceName(d.device)}</strong><small>NODE-${String(d.device).padStart(3, "0")}</small></div></div></td><td><span class="status${d.online ? "" : " off"}">${d.online ? "● 在线" : "○ 离线"}</span></td><td>${Number(d.temperature).toFixed(2)} <small>°C</small></td><td>${Number(d.humidity).toFixed(2)} <small>%</small></td><td>#${number(d.sequence)}</td><td>${clock(d.received_ms)}</td><td class="row-arrow">↗</td>`;
    tr.addEventListener("click", () => selectDevice(d.device));
    $("device-rows").append(tr);
  }
  $("export").disabled = !devices.length;
}
function renderStats(s) {
  for (const [id, key] of Object.entries({ stored: "stored", bytes: "received_bytes", crc: "crc_errors", timeouts: "timeouts", queue: "queue_depth", dropped: "queue_dropped" })) $(id).textContent = number(s[key]);
  $("queue-capacity").textContent = `/ ${number(s.queue_capacity)}`;
  $("queue-bar").style.width = `${100 * s.queue_depth / s.queue_capacity}%`;
  $("pipe-sources").textContent = `${s.sources} 来源`;
  $("pipe-frames").textContent = `${number(s.frames)} 帧`;
  $("pipe-peak").textContent = `峰值 ${number(s.queue_high_water)}`;
  $("pipe-rows").textContent = `${number(s.database_rows)} 条`;
  $("health-text").textContent = s.storage_errors ? `存储错误 ${s.storage_errors} 次，请检查事件` : s.queue_dropped ? `已丢弃 ${s.queue_dropped} 条拥塞数据` : s.frames ? "采集与存储通道正常" : "等待数据流入";
  const elapsed = Math.floor(s.uptime_ms / 1000);
  $("uptime").textContent = `UPTIME ${String(Math.floor(elapsed / 3600)).padStart(2, "0")}:${String(Math.floor(elapsed / 60) % 60).padStart(2, "0")}:${String(elapsed % 60).padStart(2, "0")}`;
  $("event-count").textContent = s.events.length;
  $("events").replaceChildren();
  for (const event of [...s.events].reverse().slice(0, 20)) {
    const row = document.createElement("div"); row.className = `event ${event.level}`;
    const dot = document.createElement("span"); dot.className = "event-dot";
    const text = document.createElement("span"); text.className = "event-message";
    text.textContent = event.message.replace("Gateway started; waiting for telemetry", "网关已启动，等待遥测数据")
      .replace("CRC mismatch:", "CRC 校验失败：").replace("Partial frame expired:", "半帧超时回收：")
      .replace("Storage queue full; newest reading dropped", "存储队列已满，丢弃最新数据");
    const time = document.createElement("time"); time.textContent = clock(event.time_ms);
    row.append(dot, text, time); $("events").append(row);
  }
}
async function loadHistory() {
  const selected = state.selected;
  if (selected === null) { state.history = []; drawChart(); return; }
  const history = await api(`/api/history?device=${selected}&limit=120`);
  if (selected !== state.selected) return;
  state.history = history;
  $("chart-subtitle").textContent = `${deviceName(selected)} · ${history.length} 条有效采样`;
  drawChart();
}
async function selectDevice(id) {
  state.selected = Number(id); renderDevices(state.devices);
  try { await loadHistory(); } catch (error) { notice(error.message); }
}
async function refresh() {
  if (state.busy || state.paused) return;
  state.busy = true;
  try {
    const [stats, devices] = await Promise.all([api("/api/stats"), api("/api/devices")]);
    state.devices = devices; renderStats(stats); renderDevices(devices); await loadHistory();
    state.healthy = true; $("connection").className = "connection";
    $("connection").replaceChildren(); const dot = document.createElement("i"); $("connection").append(dot, "网关已连接");
    $("last-updated").textContent = `最近更新 ${clock(Date.now())}`;
    if ($("notice").dataset.networkError === "1") { notice(""); delete $("notice").dataset.networkError; }
  } catch (error) {
    state.healthy = false; $("connection").className = "connection offline";
    $("connection").replaceChildren(); const dot = document.createElement("i"); $("connection").append(dot, "连接已中断");
    notice(`无法更新数据：${error.message}。请确认网关仍在运行。`); $("notice").dataset.networkError = "1";
  } finally { state.busy = false; }
}
function drawChart() {
  const canvas = $("chart"), ctx = canvas.getContext("2d"), rect = canvas.getBoundingClientRect();
  const width = rect.width, height = rect.height, dpr = window.devicePixelRatio || 1;
  canvas.width = width * dpr; canvas.height = height * dpr; ctx.scale(dpr, dpr);
  $("chart-empty").hidden = state.history.length > 0;
  if (!state.history.length) return;
  const left = 36, right = width - 35, top = 16, bottom = height - 30;
  const temperatures = state.history.map(r => r.temperature);
  const low = Math.floor(Math.min(...temperatures) / 5) * 5 - 2;
  const high = Math.ceil(Math.max(...temperatures) / 5) * 5 + 2;
  ctx.font = '9px "Segoe UI",sans-serif'; ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = top + (bottom - top) * i / 4;
    ctx.strokeStyle = "#edf1ec"; ctx.setLineDash([3, 4]); ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke(); ctx.setLineDash([]);
    ctx.fillStyle = "#97a592"; ctx.textAlign = "right"; ctx.fillText((high - (high - low) * i / 4).toFixed(0) + "°", left - 9, y + 3);
    ctx.textAlign = "left"; ctx.fillText(String(100 - 25 * i), right + 8, y + 3);
  }
  const rows = state.history;
  const t0 = rows[0].received_ms, t1 = Math.max(t0 + 1, rows.at(-1).received_ms);
  const x = r => rows.length === 1 ? (left + right) / 2 : left + (right - left) * (r.received_ms - t0) / (t1 - t0);
  for (const [key, color] of [["humidity", "#a5c6bc"], ["temperature", "#208468"]]) {
    const y = r => bottom - (bottom - top) * (key === "humidity" ? r.humidity / 100 : (r.temperature - low) / (high - low));
    if (key === "temperature" && rows.length > 1) {
      const gradient = ctx.createLinearGradient(0, top, 0, bottom); gradient.addColorStop(0, "#218b6c1a"); gradient.addColorStop(1, "#218b6c00");
      ctx.fillStyle = gradient; ctx.beginPath(); ctx.moveTo(x(rows[0]), bottom); rows.forEach(r => ctx.lineTo(x(r), y(r))); ctx.lineTo(x(rows.at(-1)), bottom); ctx.closePath(); ctx.fill();
    }
    ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.beginPath(); rows.forEach((r, i) => i ? ctx.lineTo(x(r), y(r)) : ctx.moveTo(x(r), y(r))); ctx.stroke();
    const last = rows.at(-1); ctx.fillStyle = color; ctx.beginPath(); ctx.arc(x(last), y(last), 3, 0, Math.PI * 2); ctx.fill();
  }
  ctx.fillStyle = "#a0ac9b"; ctx.font = '8px "Segoe UI",sans-serif';
  const labels = rows.length === 1 ? [0] : [0, Math.floor((rows.length - 1) / 2), rows.length - 1];
  labels.forEach((index, i) => { ctx.textAlign = i === 0 ? "left" : i === labels.length - 1 ? "right" : "center"; ctx.fillText(clock(rows[index].received_ms), x(rows[index]), height - 9); });
}
function crc16(bytes) {
  let crc = 65535;
  for (const byte of bytes) { crc ^= byte; for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xa001 : 0); }
  return crc;
}
document.querySelectorAll("[data-fault]").forEach(button => button.addEventListener("click", async () => {
  button.disabled = true;
  try {
    let bytes;
    if (button.dataset.fault === "crc") {
      const body = Uint8Array.from([11, 1, 64, 0, 1, 0, 0, 0, 0xc4, 9, 0x88, 0x13]);
      const crc = crc16(body) ^ 1; bytes = Uint8Array.from([170, 85, ...body, crc & 255, crc >> 8]);
    } else bytes = Uint8Array.from([170, 85, 11, 1]);
    await api(`/api/ingest/lab-${button.dataset.fault}`, { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: bytes });
    notice(button.dataset.fault === "crc" ? "已注入 CRC 错帧，可在错误统计和最近事件中查看。" : "已发送半帧，请等待超时计数更新（默认约 500 ms）。");
    await refresh();
  } catch (error) { notice(`注入失败：${error.message}`); }
  finally { button.disabled = false; }
}));
$("device-select").addEventListener("change", e => selectDevice(e.target.value));
$("pause").addEventListener("click", () => { state.paused = !state.paused; $("pause").textContent = state.paused ? "▷ 恢复刷新" : "Ⅱ 暂停刷新"; if (!state.paused) refresh(); });
$("export").addEventListener("click", () => { if (state.selected !== null) window.location.assign(`/api/export.csv?device=${state.selected}`); });
window.addEventListener("resize", drawChart);
refresh(); setInterval(refresh, 1000);
