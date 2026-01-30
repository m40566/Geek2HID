#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <OneButton.h>

#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"
#include "LCD_Driver.h"
#include "GUI_Paint.h"
#include "USB.h"

// =======================================================
// USB HID
// =======================================================
USBHIDKeyboard hidKeyboard;
USBHIDMouse hidMouse;

// =======================================================
// WiFi + Web
// =======================================================
WebServer http(80);
WebSocketsServer ws(81);

// =======================================================
// Settings (NVS)
// =======================================================
Preferences prefs;

struct Settings {
  bool apMode = true; // true=AP, false=STA

  char apSsid[33] = "ESP32-S3-GEEK";
  char apPass[65] = "Waveshare";

  char staSsid[33] = "";
  char staPass[65] = "";

  float sensitivity = 1.0f;
  float scrollSensitivity = 1.0f;
  bool tapToClick = true;
} S;

// =======================================================
// Scroll simulation state (human reading)
// =======================================================
volatile bool gSimActive  = false;   // Engage/Stop
volatile uint8_t gSimSpeed = 6;      // 1..20 (aggressiveness)

// =======================================================
// Hardware button (toggle sim scroll)
// =======================================================
#ifndef PIN_INPUT
#define PIN_INPUT 0   // Waveshare BOOT button (active-low)
#endif

OneButton hwBtn(PIN_INPUT, true);
static uint32_t gLastBtnToggleMs = 0;

enum SimState { SIM_PAUSE, SIM_BURST };
SimState simState = SIM_PAUSE;

uint32_t simNextChangeMs = 0;  // when to transition state
uint32_t simNextTickMs   = 0;  // next scroll tick during burst
int8_t   simDir = -1;          // -1 scroll down, +1 scroll up
uint16_t simTicksLeft = 0;     // wheel ticks left in this burst

uint32_t simRandSeed = 0x12345678;

static inline uint32_t simRand32() {
  uint32_t x = simRandSeed;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  simRandSeed = x;
  return x;
}
static inline uint32_t simRandRange(uint32_t lo, uint32_t hi) {
  if (hi <= lo) return lo;
  uint32_t r = simRand32();
  return lo + (r % (hi - lo + 1));
}

// Throttle LCD redraws to state changes
uint8_t gLastLcdSimState = 255;
static uint8_t getSimUiState() {
  return gSimActive ? 1 : 0; // 0=OFF, 1=ACTIVE
}

// =======================================================
// Helpers
// =======================================================
void loadSettings() {
  prefs.begin("geekhid", true);

  S.apMode = prefs.getBool("apMode", true);

  prefs.getString("apSsid", S.apSsid, sizeof(S.apSsid));
  prefs.getString("apPass", S.apPass, sizeof(S.apPass));

  prefs.getString("staSsid", S.staSsid, sizeof(S.staSsid));
  prefs.getString("staPass", S.staPass, sizeof(S.staPass));

  S.sensitivity       = prefs.getFloat("sens", 1.0f);
  S.scrollSensitivity = prefs.getFloat("scroll", 1.0f);
  S.tapToClick        = prefs.getBool("tap", true);

  prefs.end();

  if (strlen(S.apSsid) == 0) strncpy(S.apSsid, "ESP32-S3-GEEK", sizeof(S.apSsid));
  if (strlen(S.apPass) == 0) strncpy(S.apPass, "Waveshare", sizeof(S.apPass));
}

void saveSettings() {
  prefs.begin("geekhid", false);

  prefs.putBool("apMode", S.apMode);

  prefs.putString("apSsid", S.apSsid);
  prefs.putString("apPass", S.apPass);

  prefs.putString("staSsid", S.staSsid);
  prefs.putString("staPass", S.staPass);

  prefs.putFloat("sens", S.sensitivity);
  prefs.putFloat("scroll", S.scrollSensitivity);
  prefs.putBool("tap", S.tapToClick);

  prefs.end();
}

// HID helpers
void hidMouseMove(int dx, int dy) {
  dx = constrain(dx, -127, 127);
  dy = constrain(dy, -127, 127);
  hidMouse.move(dx, dy);
}
void hidMouseClick(uint8_t buttonMask) {
  hidMouse.press(buttonMask);
  delay(10);
  hidMouse.release(buttonMask);
}
void hidMouseScroll(int8_t wheel) {
  wheel = constrain(wheel, -127, 127);
  hidMouse.move(0, 0, wheel); // known-working path (manual buttons)
}

// ✅ Shared scroll path for both buttons and simulation
void applyScrollSteps(int steps) {
  int wheel = (int)(steps * S.scrollSensitivity);
  wheel = constrain(wheel, -127, 127);
  hidMouseScroll((int8_t)wheel);
}

// =======================================================
// Human-like scroll simulation engine
// =======================================================
void simInitIfNeeded() {
  if (simRandSeed == 0x12345678) {
    simRandSeed = (uint32_t)micros() ^ (uint32_t)millis() ^ (uint32_t)ESP.getEfuseMac();
  }
}

void simSchedulePause() {
  uint32_t baseLo = 25, baseHi = 70;
  uint32_t speedAdj = (uint32_t)(20 - constrain((int)gSimSpeed, 1, 20));
  uint32_t extra = (uint32_t)(speedAdj * 2);

  uint32_t lo = baseLo + extra;
  uint32_t hi = baseHi + extra + 20;

  bool longPause = (simRandRange(1, 100) <= 10);
  // Human reading pause: 30s – 120s
  uint32_t pauseSec = simRandRange(30, 120);

  // Faster speeds reduce pause slightly
  pauseSec = pauseSec - (gSimSpeed * 2);
  if (pauseSec < 15) pauseSec = 15;

  simState = SIM_PAUSE;
  simNextChangeMs = millis() + pauseSec * 1000UL;
}

void simScheduleBurst() {
  uint32_t roll = simRandRange(1, 100);
  if (roll <= 82) simDir = -1; // mostly down
  else simDir = +1;            // occasionally up

  if (simDir < 0) {
    // scroll down: 1–3 ticks only
    simTicksLeft = simRandRange(1, 3);
  } else {
    // scroll up: usually 1 tick, occasionally 2
    simTicksLeft = simRandRange(1, 2);
  }

  uint32_t burstMs = simRandRange(600, 2200 - (uint32_t)gSimSpeed * 40);
  simState = SIM_BURST;
  simNextChangeMs = millis() + burstMs;
  simNextTickMs = millis() + simRandRange(60, 140);
}

void simStartHuman() {
  simInitIfNeeded();
  gSimActive = true;
  simSchedulePause(); // start with a pause
}

void simStopHuman() {
  gSimActive = false;
  simState = SIM_PAUSE;
  simTicksLeft = 0;
  simNextChangeMs = 0;
  simNextTickMs = 0;
}

// Toggle sim scroll via hardware button
void onHwButtonClick() {
  uint32_t now = millis();
  if (now - gLastBtnToggleMs < 250) return; // debounce/guard
  gLastBtnToggleMs = now;

  if (gSimActive) simStopHuman();
  else            simStartHuman();

  lcdDrawStatus();
}

// =======================================================
// LCD status
// =======================================================
void lcdDrawStatus() {
  uint8_t st = getSimUiState();
  if (st == gLastLcdSimState) return;
  gLastLcdSimState = st;

  LCD_Clear(BLACK);

  if (S.apMode) {
    Paint_DrawString_EN(10, 20, "AP Mode", &Font20, BLACK, WHITE);
    String ip = WiFi.softAPIP().toString();
    Paint_DrawString_EN(10, 55, ip.c_str(), &Font16, BLACK, WHITE);
  } else {
    Paint_DrawString_EN(10, 20, "STA Mode", &Font20, BLACK, WHITE);

    String ssid = WiFi.SSID();
    if (ssid.length() == 0) ssid = "(no ssid)";
    Paint_DrawString_EN(10, 55, ssid.c_str(), &Font16, BLACK, WHITE);

    String ip = WiFi.localIP().toString();
    Paint_DrawString_EN(10, 75, ip.c_str(), &Font16, BLACK, WHITE);
  }

    UWORD color = gSimActive ? GREEN : 0x7BEF; // green when active, gray when off
  const char* label = gSimActive ? "ScrollSim: ACTIVE" : "ScrollSim: OFF";
  Paint_DrawString_EN(10, 110, label, &Font16, BLACK, color);

  Paint_DrawString_EN(10, 135, "/settings", &Font16, BLACK, WHITE);
}

// =======================================================
// WiFi start
// =======================================================
void startWifi() {
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);

  if (S.apMode) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(S.apSsid, (strlen(S.apPass) >= 8) ? S.apPass : nullptr);
    Serial.printf("WIFI: AP SSID=%s IP=%s\n", S.apSsid, WiFi.softAPIP().toString().c_str());
    lcdDrawStatus();
    return;
  }

  WiFi.mode(WIFI_STA);

  if (strlen(S.staSsid) == 0) {
    Serial.println("WIFI: STA not configured, fallback AP");
    S.apMode = true;
    saveSettings();
    startWifi();
    return;
  }

  Serial.printf("WIFI: STA connecting SSID=%s\n", S.staSsid);
  WiFi.begin(S.staSsid, S.staPass);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WIFI: STA connected SSID=%s IP=%s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    lcdDrawStatus();
    return;
  }

  Serial.println("WIFI: STA failed, fallback AP");
  S.apMode = true;
  saveSettings();
  startWifi();
}

// =======================================================
// Pages
// =======================================================
static const char SETTINGS_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no"/>
  <style>
    body { font-family: Arial; margin: 0; padding: 16px; background:#0b0b0b; color:#eee; }
    .card { background:#151515; border:1px solid #2a2a2a; border-radius:14px; padding:14px; margin-bottom:12px; }
    .row { display:flex; gap:10px; flex-wrap:wrap; align-items:center; }
    label { font-size: 14px; opacity:.9; display:block; margin:10px 0 6px; }
    input, select, button { font-size:16px; padding:10px; border-radius:12px; border:1px solid #333; background:#0f0f0f; color:#eee; }
    input, select { width: 100%; box-sizing:border-box; }
    button { cursor:pointer; background:#151515; }
    .small { font-size: 13px; opacity:.8; }
    .ok { color:#7CFC98; }
    .warn { color:#FFD166; }
  </style>
</head>
<body>
  <div class="card">
    <div class="row" style="justify-content:space-between;">
      <div>
        <div style="font-size:20px;font-weight:700;">GeekHID Settings</div>
        <div class="small">Save will reboot and apply Wi-Fi settings.</div>
      </div>
      <button onclick="location.href='/'">Control</button>
    </div>
  </div>

  <div class="card">
    <div style="font-size:16px;font-weight:700;">Wi-Fi Mode</div>
    <label for="mode">Mode</label>
    <select id="mode">
      <option value="ap">AP (device creates Wi-Fi)</option>
      <option value="sta">STA (join existing Wi-Fi)</option>
    </select>
    <div class="small warn">If STA fails, device falls back to AP automatically.</div>
  </div>

  <div class="card">
    <div style="font-size:16px;font-weight:700;">AP Settings</div>
    <label for="apSsid">AP SSID</label>
    <input id="apSsid" placeholder="ESP32-S3-GEEK"/>
    <label for="apPass">AP Password (8+ chars or blank for open)</label>
    <input id="apPass" placeholder="Waveshare"/>
  </div>

  <div class="card">
    <div style="font-size:16px;font-weight:700;">STA Settings</div>
    <div class="row">
      <button onclick="scan()">Scan Networks</button>
      <div id="scanStatus" class="small"></div>
    </div>
    <label for="staSsid">STA SSID</label>
    <select id="staSsid"></select>
    <label for="staPass">STA Password</label>
    <input id="staPass" type="password" placeholder="Your Wi-Fi password"/>
  </div>

  <div class="card">
    <div style="font-size:16px;font-weight:700;">Input Tuning</div>
    <label for="sens">Mouse Sensitivity (<span id="sensV"></span>)</label>
    <input id="sens" type="range" min="0.2" max="5.0" step="0.1" value="1.0"/>
    <label for="scroll">Scroll Sensitivity (<span id="scrollV"></span>)</label>
    <input id="scroll" type="range" min="0.2" max="10.0" step="0.2" value="1.0"/>
    <label><input id="tap" type="checkbox" /> Tap-to-click</label>
  </div>

  <div class="card">
    <div class="row">
      <button onclick="save()" style="font-weight:700;">Save & Reboot</button>
      <button onclick="reboot()">Reboot Only</button>
      <div id="status" class="small"></div>
    </div>
  </div>

<script>
  const mode = document.getElementById('mode');
  const apSsid = document.getElementById('apSsid');
  const apPass = document.getElementById('apPass');
  const staSsid = document.getElementById('staSsid');
  const staPass = document.getElementById('staPass');

  const sens = document.getElementById('sens');
  const scroll = document.getElementById('scroll');
  const tap = document.getElementById('tap');

  const sensV = document.getElementById('sensV');
  const scrollV = document.getElementById('scrollV');

  const status = document.getElementById('status');
  const scanStatus = document.getElementById('scanStatus');

  function setStatus(msg, good=false){
    status.textContent = msg;
    status.className = "small " + (good ? "ok" : "");
  }
  function refreshLabels(){
    sensV.textContent = Number(sens.value).toFixed(1);
    scrollV.textContent = Number(scroll.value).toFixed(1);
  }
  sens.addEventListener('input', refreshLabels);
  scroll.addEventListener('input', refreshLabels);

  async function load(){
    setStatus("Loading...");
    const r = await fetch('/api/settings');
    const j = await r.json();

    mode.value = j.apMode ? "ap" : "sta";
    apSsid.value = j.apSsid || "";
    apPass.value = j.apPass || "";

    staSsid.innerHTML = "";
    const opt = document.createElement('option');
    opt.value = j.staSsid || "";
    opt.textContent = j.staSsid || "(not set)";
    staSsid.appendChild(opt);

    staPass.value = j.staPass || "";

    sens.value = j.sensitivity || 1.0;
    scroll.value = j.scrollSensitivity || 1.0;
    tap.checked = !!j.tapToClick;

    refreshLabels();
    setStatus("Loaded.", true);
  }

  async function scan(){
    scanStatus.textContent = "Scanning...";
    staSsid.disabled = true;
    try{
      const r = await fetch('/api/scan');
      const j = await r.json();
      staSsid.innerHTML = "";

      if(!j.networks || j.networks.length === 0){
        const opt = document.createElement('option');
        opt.value = "";
        opt.textContent = "(no networks found)";
        staSsid.appendChild(opt);
        scanStatus.textContent = "No networks found.";
        return;
      }

      j.networks.forEach(n=>{
        const opt = document.createElement('option');
        opt.value = n.ssid;
        opt.textContent = `${n.ssid}  (${n.rssi} dBm)${n.open ? " [open]" : ""}`;
        staSsid.appendChild(opt);
      });

      scanStatus.textContent = "Scan complete.";
    }catch(e){
      scanStatus.textContent = "Scan failed.";
    }finally{
      staSsid.disabled = false;
    }
  }

  async function save(){
    setStatus("Saving...");
    const body = {
      apMode: (mode.value === "ap"),
      apSsid: apSsid.value,
      apPass: apPass.value,
      staSsid: staSsid.value,
      staPass: staPass.value,
      sensitivity: Number(sens.value),
      scrollSensitivity: Number(scroll.value),
      tapToClick: tap.checked
    };

    const r = await fetch('/api/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });

    if(r.ok){
      setStatus("Saved. Rebooting...", true);
      await fetch('/api/reboot', { method:'POST' });
    }else{
      setStatus("Save failed.");
    }
  }

  async function reboot(){
    setStatus("Rebooting...");
    await fetch('/api/reboot', { method:'POST' });
  }

  load();
</script>
</body>
</html>
)HTML";

static const char CONTROL_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no"/>
  <style>
    body { font-family: -apple-system, Arial; margin: 0; padding: 0; background:#0b0b0b; color:#eee; }
    #top { padding: 10px; display:flex; justify-content:space-between; align-items:flex-start; gap:10px; }
    #pad {
      height: 40vh;
      background: #111;
      border-top:1px solid #222;
      border-bottom:1px solid #222;
      display:flex;
      align-items:center;
      justify-content:center;
      touch-action:none;
      user-select:none;
      -webkit-user-select:none;
    }
    #bar { padding: 10px; display:flex; gap:10px; flex-wrap:wrap; align-items:center; }
    button, input, label { font-size: 16px; }
    button, input {
      padding: 10px; border-radius:12px; border:1px solid #333; background:#151515; color:#eee;
    }
    input[type="text"] { flex: 1; min-width: 180px; background:#0f0f0f; }
    .small { font-size: 13px; opacity:.85; }
    .status {
      font-weight: 800;
      font-size: 14px;
      padding: 4px 10px;
      border-radius: 999px;
      border: 1px solid #333;
      display:inline-block;
    }
    .ok { color:#7CFC98; }
    .bad { color:#FF5A5A; }

    .toggleRow { display:flex; align-items:center; gap:8px; }
    .toggleRow input[type="checkbox"] { width: 22px; height: 22px; margin: 0; }

    .kbdWrap { padding: 10px; padding-top: 0; }
    .row { display:flex; gap:8px; margin-bottom:8px; }
    .key {
      flex:1;
      padding: 12px 0;
      text-align:center;
      border-radius:12px;
      border:1px solid #333;
      background:#151515;
      user-select:none;
      -webkit-user-select:none;
      touch-action:manipulation;
    }
    .key.wide  { flex: 1.7; }
    .key.wider { flex: 2.4; }
    .key.space { flex: 5.5; }
    .key.active { outline: 2px solid #7CFC98; }
  </style>
</head>
<body>
  <div id="top">
    <div>
      <div style="font-size:18px;font-weight:800;">GeekHID Control</div>
      <div id="conn" class="status bad">Disconnected</div>
      <div class="small">Tap = left click • Long-press = right click</div>
    </div>
    <button onclick="location.href='/settings'">Settings</button>
  </div>

  <div id="pad">Touchpad</div>

  <div id="bar">
    <button onclick="send({t:'click', b:1})">Left</button>
    <button onclick="send({t:'click', b:2})">Right</button>
    <button onclick="send({t:'scroll', d:+1})">Scroll Up</button>
    <button onclick="send({t:'scroll', d:-1})">Scroll Down</button>

    <input id="text" type="text" placeholder="Type here (Enter sends)"
      autocapitalize="none" autocomplete="off" autocorrect="off" spellcheck="false"/>

    <div class="toggleRow">
      <input id="live" type="checkbox">
      <label for="live" style="font-weight:700;">Live keys</label>
    </div>

    <button onclick="sendText()">Send</button>
  </div>

  <div id="bar" style="padding-top:0;">
    <label for="simSpeed" class="small">Speed</label>
    <input id="simSpeed" type="range" min="1" max="20" step="1" value="6" style="width:180px;">
    <span id="simSpeedV" class="small" style="min-width:40px; display:inline-block;">6</span>

    <button onclick="simEngage()">Engage</button>
    <button onclick="simStop()">Stop</button>

    <span id="simState" class="small">Sim: OFF</span>
  </div>

  <div class="kbdWrap">
    <div id="kbd"></div>
  </div>

<script>
  const conn = document.getElementById('conn');
  const ws = new WebSocket(`ws://${location.hostname}:81/`);
  ws.onopen = ()=>{
    conn.textContent = "Connected";
    conn.classList.remove('bad');
    conn.classList.add('ok');
      // Ensure device sim is stopped on page load/reconnect
    send({t:'sim', speed: Number(simSpeed.value)});
    send({t:'sim', active:false});
    simState.textContent = "Sim: OFF";
  };
  ws.onclose = ()=>{
    conn.textContent = "Disconnected";
    conn.classList.remove('ok');
    conn.classList.add('bad');
  };
  function send(obj){ if(ws.readyState===1) ws.send(JSON.stringify(obj)); }

  const pad = document.getElementById('pad');
  const text = document.getElementById('text');
  const live = document.getElementById('live');

  live.checked = (localStorage.getItem('liveKeys') === '1');
  live.addEventListener('change', ()=>{
    localStorage.setItem('liveKeys', live.checked ? '1' : '0');
  });  const simSpeed = document.getElementById('simSpeed');
  const simSpeedV = document.getElementById('simSpeedV');
  const simState = document.getElementById('simState');  simSpeed.value = localStorage.getItem('simSpeed') || '6';
  simSpeedV.textContent = simSpeed.value;

  simSpeed.addEventListener('input', ()=>{
    simSpeedV.textContent = simSpeed.value;
    localStorage.setItem('simSpeed', simSpeed.value);
    send({t:'sim', speed: Number(simSpeed.value)});
  });

  function simEngage(){
    simState.textContent = "Sim: ACTIVE";
    send({t:'sim', active:true, speed: Number(simSpeed.value)});
  }
  function simStop(){
    simState.textContent = "Sim: OFF";
    send({t:'sim', active:false});
  }

  let lastX=null, lastY=null;
  let downTime = 0;
  let moved = false;
  let longPressTimer = null;

  pad.addEventListener('pointerdown', (e)=>{
    pad.setPointerCapture(e.pointerId);
    lastX = e.clientX; lastY = e.clientY;
    downTime = performance.now();
    moved = false;

    longPressTimer = setTimeout(()=>{
      send({t:'click', b:2});
      longPressTimer = null;
    }, 600);
  });

  pad.addEventListener('pointermove', (e)=>{
    if(lastX===null) return;
    const dx = e.clientX - lastX;
    const dy = e.clientY - lastY;
    lastX = e.clientX; lastY = e.clientY;

    if (Math.abs(dx) > 2 || Math.abs(dy) > 2) moved = true;
    if (moved && longPressTimer) { clearTimeout(longPressTimer); longPressTimer = null; }

    send({t:'move', dx, dy});
  });

  pad.addEventListener('pointerup', ()=>{
    if (longPressTimer) { clearTimeout(longPressTimer); longPressTimer = null; }
    const dt = performance.now() - downTime;
    if (!moved && dt < 250) send({t:'tap'});
    lastX=null; lastY=null;
  });

  function sendText(){
    const v = text.value;
    if(!v) return;
    send({t:'text', v});
    text.value = '';
  }

  text.addEventListener('keydown', (e)=>{
    if(e.key === 'Enter'){
      e.preventDefault();
      sendText();
      return;
    }
    if(!live.checked) return;

    if(e.key.length === 1){
      send({t:'key', k:e.key, mods:getMods()});
      consumeOneShotShift();
      e.preventDefault();
    } else if(e.key === 'Backspace'){
      send({t:'key', k:'backspace', mods:getMods()});
      e.preventDefault();
    }
  });

  const kbd = document.getElementById('kbd');

  let layer = 'abc';
  let shift = false;
  let caps = false;
  let lastShiftTap = 0;

  function getMods(){
    return { shift: (shift || caps) };
  }
  function consumeOneShotShift(){
    if(shift && !caps){
      shift = false;
      renderKeyboard();
    }
  }

  function renderKeyboard(){
    kbd.innerHTML = "";

    const abc = [
      ["q","w","e","r","t","y","u","i","o","p"],
      ["a","s","d","f","g","h","j","k","l"],
      ["shift","z","x","c","v","b","n","m","backspace"],
      ["?123","space","enter"]
    ];

    const sym1 = [
      ["1","2","3","4","5","6","7","8","9","0"],
      ["-","/",";",":","(",")","$","&","@","\""],
      [".",",","?","!","'"],
      ["#+=","space","enter","backspace"],
      ["ABC"]
    ];

    const sym2 = [
      ["[","]","{","}","#","%","^","*","+","="],
      ["_","\\","|","~","<",">","€","£","¥","·"],
      [".",",","?","!","'"],
      ["?123","space","enter","backspace"],
      ["ABC"]
    ];

    const rows = (layer==='abc') ? abc : (layer==='sym1' ? sym1 : sym2);

    rows.forEach((r)=>{
      const row = document.createElement('div');
      row.className = "row";

      r.forEach((k)=>{
        const key = document.createElement('div');
        key.className = "key";

        if(k === "space") key.classList.add("space");
        if(k === "backspace") key.classList.add("wider");
        if(k === "enter") key.classList.add("wider");
        if(k === "shift") key.classList.add("wide");
        if(k === "?123" || k === "#+=") key.classList.add("wide");
        if(k === "ABC") key.classList.add("wide");

        let label = k;
        if(k === "backspace") label = "BKSP";
        if(k === "enter") label = "Return";
        if(k === "space") label = "Space";
        if(k === "shift") label = caps ? "Caps" : "Shift";
        if(k === "ABC") label = "ABC";

        if(layer==='abc' && label.length===1 && /[a-z]/.test(label) && (shift||caps)) label = label.toUpperCase();
        key.textContent = label;

        if(k === "shift" && (shift||caps)) key.classList.add("active");

        let repeatTimer = null;
        let repeatInterval = null;

        function fireKey(){
          if(live.checked){
            send({t:'key', k, mods:getMods()});
          }else{
            if(k === "backspace") text.value = text.value.slice(0,-1);
            else if(k === "enter") sendText();
            else if(k === "space") text.value += " ";
            else if(k.length === 1){
              let ch = k;
              if(layer==='abc' && (shift||caps) && ch>='a' && ch<='z') ch = String.fromCharCode(ch.charCodeAt(0)-32);
              text.value += ch;
            }
          }
          if(layer==='abc') consumeOneShotShift();
        }

        key.addEventListener('pointerdown', (e)=>{
          e.preventDefault();

          if(k === "shift"){
            const now = Date.now();
            if(now - lastShiftTap < 350){
              caps = !caps;
              shift = false;
            } else {
              if(caps){ caps = false; shift = true; }
              else shift = !shift;
            }
            lastShiftTap = now;
            renderKeyboard();
            return;
          }
          if(k === "?123"){ layer='sym1'; renderKeyboard(); return; }
          if(k === "#+="){ layer='sym2'; renderKeyboard(); return; }
          if(k === "ABC"){ layer='abc'; renderKeyboard(); return; }

          fireKey();

          if(k === "backspace"){
            repeatTimer = setTimeout(()=>{
              repeatInterval = setInterval(()=>fireKey(), 70);
            }, 350);
          }
        });

        key.addEventListener('pointerup', ()=>{
          if(repeatTimer){ clearTimeout(repeatTimer); repeatTimer=null; }
          if(repeatInterval){ clearInterval(repeatInterval); repeatInterval=null; }
        });
        key.addEventListener('pointercancel', ()=>{
          if(repeatTimer){ clearTimeout(repeatTimer); repeatTimer=null; }
          if(repeatInterval){ clearInterval(repeatInterval); repeatInterval=null; }
        });

        row.appendChild(key);
      });

      kbd.appendChild(row);
    });
  }

  renderKeyboard();
</script>
</body>
</html>
)HTML";

// =======================================================
// HTTP handlers
// =======================================================
void handleRoot() { http.send_P(200, "text/html", CONTROL_HTML); }
void handleSettingsPage() { http.send_P(200, "text/html", SETTINGS_HTML); }

void handleGetSettings() {
  String json = "{";
  json += "\"apMode\":" + String(S.apMode ? "true" : "false") + ",";
  json += "\"apSsid\":\"" + String(S.apSsid) + "\",";
  json += "\"apPass\":\"" + String(S.apPass) + "\",";
  json += "\"staSsid\":\"" + String(S.staSsid) + "\",";
  json += "\"staPass\":\"" + String(S.staPass) + "\",";
  json += "\"sensitivity\":" + String(S.sensitivity, 3) + ",";
  json += "\"scrollSensitivity\":" + String(S.scrollSensitivity, 3) + ",";
  json += "\"tapToClick\":" + String(S.tapToClick ? "true" : "false");
  json += "}";
  http.send(200, "application/json", json);
}

static String jsonGetString(const String& body, const char* key) {
  String k = String("\"") + key + "\":";
  int i = body.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  while (i < (int)body.length() && body[i] == ' ') i++;
  if (i >= (int)body.length()) return "";

  if (body[i] == '"') {
    int s = i + 1;
    int e = body.indexOf('"', s);
    if (e < 0) return "";
    return body.substring(s, e);
  }
  int e = body.indexOf(',', i);
  if (e < 0) e = body.indexOf('}', i);
  if (e < 0) e = body.length();
  return body.substring(i, e);
}

static bool jsonGetBool(const String& body, const char* key, bool defVal) {
  String v = jsonGetString(body, key);
  v.trim();
  if (v == "true") return true;
  if (v == "false") return false;
  return defVal;
}

static float jsonGetFloat(const String& body, const char* key, float defVal) {
  String v = jsonGetString(body, key);
  v.trim();
  if (v.length() == 0) return defVal;
  return v.toFloat();
}

void handlePostSettings() {
  if (!http.hasArg("plain")) { http.send(400, "text/plain", "Missing body"); return; }

  String body = http.arg("plain");

  S.apMode = jsonGetBool(body, "apMode", true);

  String apSsid = jsonGetString(body, "apSsid"); apSsid.trim();
  String apPass = jsonGetString(body, "apPass"); apPass.trim();
  String staSsid = jsonGetString(body, "staSsid"); staSsid.trim();
  String staPass = jsonGetString(body, "staPass"); staPass.trim();

  if (apSsid.length() > 0) strncpy(S.apSsid, apSsid.c_str(), sizeof(S.apSsid));
  strncpy(S.apPass, apPass.c_str(), sizeof(S.apPass));

  if (staSsid.length() > 0) strncpy(S.staSsid, staSsid.c_str(), sizeof(S.staSsid));
  strncpy(S.staPass, staPass.c_str(), sizeof(S.staPass));

  float sensitivity = jsonGetFloat(body, "sensitivity", S.sensitivity);
  float scrollSensitivity = jsonGetFloat(body, "scrollSensitivity", S.scrollSensitivity);
  bool tap = jsonGetBool(body, "tapToClick", S.tapToClick);

  S.sensitivity = constrain(sensitivity, 0.2f, 5.0f);
  S.scrollSensitivity = constrain(scrollSensitivity, 0.2f, 10.0f);
  S.tapToClick = tap;

  saveSettings();
  http.send(200, "application/json", "{\"ok\":true}");
}

void handleScan() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  delay(100);

  int n = WiFi.scanNetworks(false, true);

  String json = "{ \"networks\": [";
  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");

    json += "{";
    json += "\"ssid\":\"" + ssid + "\",";
    json += "\"rssi\":" + String(rssi) + ",";
    json += "\"open\":" + String(open ? "true" : "false");
    json += "}";
  }
  json += "] }";

  WiFi.scanDelete();
  http.send(200, "application/json", json);
}

void handleReboot() {
  http.send(200, "application/json", "{\"rebooting\":true}");
  delay(250);
  ESP.restart();
}

// =======================================================
// WebSocket handler
// =======================================================
void onWsEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type != WStype_TEXT) return;

  String msg((char*)payload, length);

  // Scroll simulation control (Engage/Stop only)
  if (msg.indexOf("\"t\":\"sim\"") >= 0) {
    // Optional speed update
    int si = msg.indexOf("\"speed\":");
    if (si >= 0) {
      uint8_t sp = (uint8_t)msg.substring(si + 8).toInt();
      gSimSpeed = (uint8_t)constrain((int)sp, 1, 20);
    }

    // Engage/Stop
    if (msg.indexOf("\"active\":true") >= 0)  simStartHuman();
    if (msg.indexOf("\"active\":false") >= 0) simStopHuman();

    lcdDrawStatus();
    return;
  }


  // Tap to click
  if (msg.indexOf("\"t\":\"tap\"") >= 0) {
    if (S.tapToClick) hidMouseClick(0x01);
    return;
  }

  // Live keys / on-screen keyboard (single key events)
  if (msg.indexOf("\"t\":\"key\"") >= 0) {
    // Extract key string: "k":"a" or "k":"backspace"
    String key = "";
    int ks = msg.indexOf("\"k\":\"");
    if (ks >= 0) {
      ks += 5;
      int ke = msg.indexOf("\"", ks);
      if (ke > ks) key = msg.substring(ks, ke);
    }

    bool modShift = (msg.indexOf("\"shift\":true") >= 0);
    bool modCtrl  = (msg.indexOf("\"ctrl\":true")  >= 0);
    bool modAlt   = (msg.indexOf("\"alt\":true")   >= 0);
    bool modMeta  = (msg.indexOf("\"meta\":true")  >= 0);

    // Apply modifiers (best-effort)
    if (modCtrl)  hidKeyboard.press(KEY_LEFT_CTRL);
    if (modAlt)   hidKeyboard.press(KEY_LEFT_ALT);
    if (modMeta)  hidKeyboard.press(KEY_LEFT_GUI);
    if (modShift) hidKeyboard.press(KEY_LEFT_SHIFT);

    auto sendCharWithShiftMap = [&](char c){
      // If shift is held, map common US keyboard symbols
      if (!modShift) {
        hidKeyboard.write((uint8_t)c);
        return;
      }
      if (c >= 'a' && c <= 'z') { hidKeyboard.write((uint8_t)(c - 32)); return; } // to upper
      if (c >= 'A' && c <= 'Z') { hidKeyboard.write((uint8_t)c); return; }

      switch (c) {
        case '1': hidKeyboard.write((uint8_t)'!'); return;
        case '2': hidKeyboard.write((uint8_t)'@'); return;
        case '3': hidKeyboard.write((uint8_t)'#'); return;
        case '4': hidKeyboard.write((uint8_t)'$'); return;
        case '5': hidKeyboard.write((uint8_t)'%'); return;
        case '6': hidKeyboard.write((uint8_t)'^'); return;
        case '7': hidKeyboard.write((uint8_t)'&'); return;
        case '8': hidKeyboard.write((uint8_t)'*'); return;
        case '9': hidKeyboard.write((uint8_t)'('); return;
        case '0': hidKeyboard.write((uint8_t)')'); return;
        case '-': hidKeyboard.write((uint8_t)'_'); return;
        case '=': hidKeyboard.write((uint8_t)'+'); return;
        case '[': hidKeyboard.write((uint8_t)'{'); return;
        case ']': hidKeyboard.write((uint8_t)'}'); return;
        case '\\': hidKeyboard.write((uint8_t)'|'); return;
        case ';': hidKeyboard.write((uint8_t)':'); return;
        case '\'': hidKeyboard.write((uint8_t)'"'); return;
        case ',': hidKeyboard.write((uint8_t)'<'); return;
        case '.': hidKeyboard.write((uint8_t)'>'); return;
        case '/': hidKeyboard.write((uint8_t)'?'); return;
        case '`': hidKeyboard.write((uint8_t)'~'); return;
        default:
          // For anything else, just send the char as-is.
          hidKeyboard.write((uint8_t)c);
          return;
      }
    };

    if (key == "backspace") {
      hidKeyboard.write(KEY_BACKSPACE);
    } else if (key == "enter") {
      hidKeyboard.write(KEY_RETURN);
    } else if (key == "tab") {
      hidKeyboard.write(KEY_TAB);
    } else if (key == "esc") {
      hidKeyboard.write(KEY_ESC);
    } else if (key == "space") {
      hidKeyboard.write((uint8_t)' ');
    } else if (key.length() == 1) {
      sendCharWithShiftMap(key[0]);
    }

    // Release modifiers
    if (modShift) hidKeyboard.release(KEY_LEFT_SHIFT);
    if (modMeta)  hidKeyboard.release(KEY_LEFT_GUI);
    if (modAlt)   hidKeyboard.release(KEY_LEFT_ALT);
    if (modCtrl)  hidKeyboard.release(KEY_LEFT_CTRL);

    return;
  }


  // Mouse move
  if (msg.indexOf("\"t\":\"move\"") >= 0) {
    int dx = 0, dy = 0;
    int i = msg.indexOf("\"dx\":"); if (i >= 0) dx = msg.substring(i + 5).toInt();
    i = msg.indexOf("\"dy\":");     if (i >= 0) dy = msg.substring(i + 5).toInt();

    dx = (int)(dx * S.sensitivity);
    dy = (int)(dy * S.sensitivity);

    hidMouseMove(dx, dy);
    return;
  }

  // Click
  if (msg.indexOf("\"t\":\"click\"") >= 0) {
    int b = 1;
    int i = msg.indexOf("\"b\":"); if (i >= 0) b = msg.substring(i + 4).toInt();
    uint8_t mask = (b == 2) ? 0x02 : 0x01;
    hidMouseClick(mask);
    return;
  }

  // Manual scroll buttons (shared path)
  if (msg.indexOf("\"t\":\"scroll\"") >= 0) {
    int d = 0;
    int i = msg.indexOf("\"d\":"); if (i >= 0) d = msg.substring(i + 4).toInt();
    applyScrollSteps(d);
    return;
  }

  // Buffered text
  if (msg.indexOf("\"t\":\"text\"") >= 0) {
    int i = msg.indexOf("\"v\":\"");
    if (i >= 0) {
      String v = msg.substring(i + 5);
      int end = v.lastIndexOf("\"");
      if (end > 0) v = v.substring(0, end);
      v.replace("\\n", "\n");
      hidKeyboard.print(v);
    }
    return;
  }
}

// =======================================================
// Arduino
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(800);

  USB.begin();
  hidKeyboard.begin();
  hidMouse.begin();

  Config_Init();
  LCD_Init();
  LCD_SetBacklight(100);
  Paint_NewImage(LCD_WIDTH, LCD_HEIGHT, 90, WHITE);
  Paint_SetRotate(90);
  LCD_Clear(BLACK);

  // Hardware button (BOOT) toggles sim scroll
  pinMode(PIN_INPUT, INPUT_PULLUP);
  hwBtn.attachClick([](){ onHwButtonClick(); });

  loadSettings();
  startWifi();

  http.on("/", handleRoot);
  http.on("/settings", handleSettingsPage);
  http.on("/api/settings", HTTP_GET, handleGetSettings);
  http.on("/api/settings", HTTP_POST, handlePostSettings);
  http.on("/api/scan", HTTP_GET, handleScan);
  http.on("/api/reboot", HTTP_POST, handleReboot);
  http.begin();

  ws.begin();
  ws.onEvent(onWsEvent);

  Serial.println("READY: / (control) and /settings");
}

void loop() {
  http.handleClient();
  ws.loop();
  hwBtn.tick();

  // Human-like scroll simulation (uses same logic as scroll buttons)
  if (gSimActive) {
    uint32_t now = millis();

    if (simNextChangeMs != 0 && (int32_t)(now - simNextChangeMs) >= 0) {
      if (simState == SIM_PAUSE) simScheduleBurst();
      else simSchedulePause();
      lcdDrawStatus();
    }

    if (simState == SIM_BURST && simTicksLeft > 0 && (int32_t)(now - simNextTickMs) >= 0) {
      applyScrollSteps(simDir); // ✅ shared path
      simTicksLeft--;
      simNextTickMs = millis() + simRandRange(350, 700);

      if (simTicksLeft == 0) {
        simNextChangeMs = now + simRandRange(300, 900);
      }
    }
  }
}
