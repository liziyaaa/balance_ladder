import * as THREE from "three";

const BLE_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const BLE_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
const BLE_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

const state = {
  transport: null,
  bleDevice: null,
  bleServer: null,
  rxChar: null,
  txChar: null,
  ws: null,
  connected: false,
  telemetry: null,
  history: [],
  viewZero: 0,
};

const $ = (id) => document.getElementById(id);
const encoder = new TextEncoder();
const decoder = new TextDecoder();

const els = {
  badge: $("connectionBadge"),
  bleConnect: $("bleConnectBtn"),
  bleDisconnect: $("bleDisconnectBtn"),
  wifiConnect: $("wifiConnectBtn"),
  wifiDisconnect: $("wifiDisconnectBtn"),
  wsUrl: $("wsUrl"),
  httpUrl: $("httpUrl"),
  log: $("log"),
  commandForm: $("commandForm"),
  commandInput: $("commandInput"),
  hint: $("telemetryHint"),
  stateValue: $("stateValue"),
  angleValue: $("angleValue"),
  targetValue: $("targetValue"),
  errorValue: $("errorValue"),
  gyroValue: $("gyroValue"),
  cmdValue: $("cmdValue"),
  plot: $("plot"),
  scene: $("scene"),
  zeroView: $("zeroViewBtn"),
};

function log(line, type = "info") {
  const time = new Date().toLocaleTimeString();
  const prefix = type === "tx" ? ">" : type === "err" ? "!" : "<";
  els.log.textContent = `${time} ${prefix} ${line}\n${els.log.textContent}`.slice(0, 9000);
}

function setConnected(connected, transport = null) {
  state.connected = connected;
  state.transport = connected ? transport : null;
  els.badge.textContent = connected ? `${transport} 已连接` : "未连接";
  els.badge.className = `badge ${connected ? "badge-on" : "badge-off"}`;
  els.bleConnect.disabled = connected;
  els.bleDisconnect.disabled = !connected || transport !== "BLE";
  els.wifiConnect.disabled = connected;
  els.wifiDisconnect.disabled = !connected || transport !== "WiFi";
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function wrapError(target, measured) {
  let error = target - measured;
  while (error > 180) error -= 360;
  while (error < -180) error += 360;
  return error;
}

function parseTelemetry(line) {
  const clean = line.trim();
  if (!clean) return null;
  if (!clean.startsWith("T,")) {
    return { response: clean };
  }
  const parts = clean.split(",");
  if (parts.length < 10) return { response: clean };
  return {
    t: Number(parts[1]),
    state: parts[2],
    angle: Number(parts[3]),
    target: Number(parts[4]),
    error: Number(parts[5]),
    gyro: Number(parts[6]),
    cmd: Number(parts[7]),
    key: Number(parts[8]),
    fault: parts[9],
    raw: clean,
  };
}

function updateTelemetry(t) {
  if (!Number.isFinite(t.angle)) return;
  state.telemetry = t;
  state.history.push(t);
  if (state.history.length > 180) state.history.shift();
  els.hint.textContent = `t=${t.t} ms key=${t.key} fault=${t.fault}`;
  els.stateValue.textContent = t.state ?? "--";
  els.angleValue.textContent = `${t.angle.toFixed(2)}°`;
  els.targetValue.textContent = `${t.target.toFixed(2)}°`;
  els.errorValue.textContent = `${t.error.toFixed(2)}°`;
  els.gyroValue.textContent = `${t.gyro.toFixed(2)}°/s`;
  els.cmdValue.textContent = t.cmd.toFixed(3);
  updateModel(t);
  drawPlot();
}

function onReceiveText(text) {
  for (const line of text.split(/\r?\n/)) {
    if (!line.trim()) continue;
    const parsed = parseTelemetry(line);
    if (parsed?.response) {
      log(parsed.response);
    } else if (parsed) {
      updateTelemetry(parsed);
    }
  }
}

async function connectBle() {
  if (!navigator.bluetooth) {
    log("当前浏览器不支持 Web Bluetooth。请使用 Android Chrome/Edge。", "err");
    return;
  }
  try {
    const device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: "BalanceLadder" }],
      optionalServices: [BLE_SERVICE],
    });
    state.bleDevice = device;
    device.addEventListener("gattserverdisconnected", () => {
      setConnected(false);
      log("BLE 已断开", "err");
    });
    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(BLE_SERVICE);
    state.rxChar = await service.getCharacteristic(BLE_RX);
    state.txChar = await service.getCharacteristic(BLE_TX);
    await state.txChar.startNotifications();
    state.txChar.addEventListener("characteristicvaluechanged", (event) => {
      onReceiveText(decoder.decode(event.target.value));
    });
    state.bleServer = server;
    setConnected(true, "BLE");
    log("BLE 连接成功");
    await sendCommand("status");
  } catch (error) {
    log(`BLE 连接失败: ${error.message}`, "err");
  }
}

function disconnectBle() {
  state.bleDevice?.gatt?.disconnect();
  setConnected(false);
}

function connectWifi() {
  try {
    const ws = new WebSocket(els.wsUrl.value.trim());
    ws.addEventListener("open", () => {
      state.ws = ws;
      setConnected(true, "WiFi");
      log("WiFi WebSocket 连接成功");
      sendCommand("status");
    });
    ws.addEventListener("message", (event) => onReceiveText(String(event.data)));
    ws.addEventListener("close", () => {
      setConnected(false);
      log("WiFi WebSocket 已断开", "err");
    });
    ws.addEventListener("error", () => log("WiFi WebSocket 错误，可能被 HTTPS 页面拦截", "err"));
  } catch (error) {
    log(`WiFi 连接失败: ${error.message}`, "err");
  }
}

function disconnectWifi() {
  state.ws?.close();
  state.ws = null;
  setConnected(false);
}

async function sendCommand(command) {
  const clean = command.trim();
  if (!clean) return;
  log(clean, "tx");

  try {
    if (state.transport === "BLE" && state.rxChar) {
      await state.rxChar.writeValue(encoder.encode(clean));
      return;
    }
    if (state.transport === "WiFi" && state.ws?.readyState === WebSocket.OPEN) {
      state.ws.send(clean);
      return;
    }

    const httpUrl = els.httpUrl.value.trim();
    if (httpUrl) {
      const url = `${httpUrl}${httpUrl.includes("?") ? "&" : "?"}c=${encodeURIComponent(clean)}`;
      const res = await fetch(url, { method: "GET", cache: "no-store" });
      onReceiveText(await res.text());
      return;
    }
    log("未连接。请先连接 BLE 或 WiFi。", "err");
  } catch (error) {
    log(`发送失败: ${error.message}`, "err");
  }
}

function drawPlot() {
  const canvas = els.plot;
  const ctx = canvas.getContext("2d");
  const width = canvas.width;
  const height = canvas.height;
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "#0a1320";
  ctx.fillRect(0, 0, width, height);
  ctx.strokeStyle = "#253650";
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = (height / 4) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }
  const data = state.history;
  if (data.length < 2) return;

  function drawLine(getValue, color, scale) {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    data.forEach((item, index) => {
      const x = (index / Math.max(1, data.length - 1)) * width;
      const y = height / 2 - clamp(getValue(item) / scale, -1, 1) * (height * 0.42);
      if (index === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  drawLine((t) => t.error, "#ffcf70", 35);
  drawLine((t) => t.cmd * 35, "#55d6be", 35);
  ctx.fillStyle = "#8ea0b8";
  ctx.font = "24px Microsoft YaHei, sans-serif";
  ctx.fillText("黄: error  青: motor cmd", 16, 30);
}

let scene;
let camera;
let renderer;
let ladder;
let targetLine;

function init3d() {
  scene = new THREE.Scene();
  scene.background = new THREE.Color(0x08111f);
  camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
  camera.position.set(0, 2.4, 6);
  renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setPixelRatio(window.devicePixelRatio || 1);
  els.scene.appendChild(renderer.domElement);

  const light = new THREE.DirectionalLight(0xffffff, 1.8);
  light.position.set(3, 4, 5);
  scene.add(light);
  scene.add(new THREE.AmbientLight(0x7890aa, 0.8));

  const grid = new THREE.GridHelper(6, 12, 0x29405e, 0x1a2b43);
  grid.position.y = -1.6;
  scene.add(grid);

  ladder = new THREE.Group();
  const material = new THREE.MeshStandardMaterial({ color: 0x2f8cff, roughness: 0.5 });
  const accent = new THREE.MeshStandardMaterial({ color: 0x55d6be, roughness: 0.4 });

  const railGeo = new THREE.BoxGeometry(0.18, 2.8, 0.1);
  const barGeo = new THREE.BoxGeometry(1.35, 0.16, 0.12);
  const left = new THREE.Mesh(railGeo, material);
  const right = new THREE.Mesh(railGeo, material);
  left.position.x = -0.7;
  right.position.x = 0.7;
  const crossTop = new THREE.Mesh(barGeo, accent);
  crossTop.position.y = 0.75;
  const crossMid = new THREE.Mesh(barGeo, accent);
  crossMid.position.y = 0;
  const motor = new THREE.Mesh(new THREE.CylinderGeometry(0.28, 0.28, 0.28, 32), new THREE.MeshStandardMaterial({ color: 0xffcf70 }));
  motor.rotation.x = Math.PI / 2;
  motor.position.y = 1.25;
  ladder.add(left, right, crossTop, crossMid, motor);
  scene.add(ladder);

  const lineMat = new THREE.LineBasicMaterial({ color: 0xffcf70 });
  const points = [new THREE.Vector3(0, -1.5, 0), new THREE.Vector3(0, 1.7, 0)];
  targetLine = new THREE.Line(new THREE.BufferGeometry().setFromPoints(points), lineMat);
  scene.add(targetLine);

  window.addEventListener("resize", resize3d);
  resize3d();
  animate3d();
}

function resize3d() {
  const rect = els.scene.getBoundingClientRect();
  renderer.setSize(rect.width, rect.height, false);
  camera.aspect = rect.width / Math.max(1, rect.height);
  camera.updateProjectionMatrix();
}

function updateModel(t) {
  const error = Number.isFinite(t.error) ? t.error : wrapError(t.target, t.angle);
  const tilt = THREE.MathUtils.degToRad(clamp(-error + state.viewZero, -55, 55));
  ladder.rotation.z = tilt;
  ladder.rotation.y = THREE.MathUtils.degToRad(clamp(t.cmd * 22, -22, 22));
}

function animate3d() {
  requestAnimationFrame(animate3d);
  renderer.render(scene, camera);
}

function bindUi() {
  $("bleTab").addEventListener("click", () => switchTab("ble"));
  $("wifiTab").addEventListener("click", () => switchTab("wifi"));
  els.bleConnect.addEventListener("click", connectBle);
  els.bleDisconnect.addEventListener("click", disconnectBle);
  els.wifiConnect.addEventListener("click", connectWifi);
  els.wifiDisconnect.addEventListener("click", disconnectWifi);
  els.zeroView.addEventListener("click", () => {
    state.viewZero = state.telemetry ? state.telemetry.error : 0;
    log("已将当前 3D 姿态作为显示参考");
  });

  document.querySelectorAll("[data-command]").forEach((button) => {
    button.addEventListener("click", () => sendCommand(button.dataset.command));
  });
  document.querySelectorAll("[data-param]").forEach((button) => {
    button.addEventListener("click", () => {
      const value = $(button.dataset.input).value.trim();
      sendCommand(`${button.dataset.param}=${value}`);
    });
  });
  els.commandForm.addEventListener("submit", (event) => {
    event.preventDefault();
    sendCommand(els.commandInput.value);
    els.commandInput.value = "";
  });
}

function switchTab(name) {
  $("bleTab").classList.toggle("active", name === "ble");
  $("wifiTab").classList.toggle("active", name === "wifi");
  $("blePane").classList.toggle("active", name === "ble");
  $("wifiPane").classList.toggle("active", name === "wifi");
}

async function registerServiceWorker() {
  if ("serviceWorker" in navigator) {
    try {
      await navigator.serviceWorker.register("./sw.js");
    } catch (error) {
      log(`PWA 注册失败: ${error.message}`, "err");
    }
  }
}

bindUi();
init3d();
drawPlot();
registerServiceWorker();
