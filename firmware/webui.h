#ifndef WEBUI_H
#define WEBUI_H

#include <WiFi.h>
#include <WebServer.h>

const char* AP_SSID = "M3C_Flasher";
const char* AP_PASS = "flasher123";

WebServer webServer(80);

// =====================================================================
// WEB PAGE - single-page app that mirrors the desktop Python GUI's
// functionality: Connect, Read, Write (WRITEFULL), SD file management,
// ECU info (VIN/Cal/Date/Ver), Diagnostics.
// =====================================================================

const char WEB_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>KTM Husqvarna GasGas M3C Flasher</title>
<style>
  * { box-sizing: border-box; }
  body { background:#1e1e1e; color:#d4d4d4; font-family:Consolas,monospace;
         margin:0; padding:12px; }
  h2 { color:#4ec9b0; margin:0 0 10px 0; font-size:18px; }
  h3 { color:#cccccc; font-size:13px; margin:14px 0 6px 0; font-weight:normal; }
  .btn { display:block; width:100%; padding:13px; margin:6px 0;
         border:none; border-radius:6px; font-size:15px; font-weight:bold;
         color:white; cursor:pointer; }
  .btn:disabled { background:#555 !important; color:#888 !important; }
  .connect { background:#0078d4; }
  .read { background:#107c10; }
  .write { background:#c84b2f; }
  .sd { background:#3c3c3c; font-size:13px; padding:9px; }
  .diag { background:#6b3fa0; }
  .upload { background:#0078d4; }
  #status { background:#0d0d0d; border:1px solid #3c3c3c; border-radius:6px;
            padding:10px; margin-top:8px; min-height:20px; font-size:13px; }
  #progressWrap { background:#2d2d2d; border-radius:6px; height:18px;
                  margin-top:8px; overflow:hidden; }
  #progressBar { background:#0078d4; height:100%; width:0%; transition:width .3s; }
  .infogrid { display:grid; grid-template-columns:1fr 1fr; gap:8px; margin:10px 0; }
  .infocell { background:#252526; border-radius:6px; padding:8px; }
  .infocell .lbl { color:#888; font-size:10px; }
  .infocell .val { color:#4ec9b0; font-size:14px; font-weight:bold; word-break:break-all; }
  ul#sdlist { list-style:none; padding:0; margin:8px 0; }
  ul#sdlist li { background:#0d0d0d; padding:10px; margin:4px 0;
                 border-radius:4px; font-size:12px; cursor:pointer;
                 border:2px solid transparent; }
  ul#sdlist li.selected { border-color:#0078d4; background:#0d2a3d; }
  ul#sdlist li .sz { color:#888; font-size:10px; }
  a.dl { color:#4ec9b0; text-decoration:none; }
  details { margin-top:14px; }
  summary { color:#6b3fa0; cursor:pointer; font-size:13px; padding:6px 0; }
  .row2 { display:flex; gap:6px; }
  .row2 .btn { flex:1; }
  .warn { color:#f0a500; font-size:11px; }
  .liveval { color:#4ec9b0; font-size:20px; font-weight:bold; }
</style>
</head>
<body>
<h2>KTM Husqvarna GasGas M3C Flasher</h2>

<button id="btnConnect" class="btn connect" onclick="doConnect()">CONNECT</button>

<div class="infogrid">
  <div class="infocell"><div class="lbl">VIN</div><div class="val" id="vinVal">---</div></div>
  <div class="infocell"><div class="lbl">Kalibratsioon</div><div class="val" id="calVal">---</div></div>
  <div class="infocell"><div class="lbl">Date</div><div class="val" id="dateVal">---</div></div>
  <div class="infocell"><div class="lbl">Tarkvara</div><div class="val" id="verVal">---</div></div>
</div>

<div id="progressWrap"><div id="progressBar"></div></div>
<div id="status">Ready</div>

<button id="btnRead" class="btn read" onclick="doRead()" disabled>READ FLASH</button>

<h3>SD CARD</h3>
<div class="row2">
  <button id="btnRefresh" class="btn sd" onclick="loadSD()" disabled>Refresh</button>
  <button id="btnDelete" class="btn sd" onclick="deleteSelected()" disabled>Delete</button>
</div>
<ul id="sdlist"></ul>

<button id="btnWrite" class="btn write" onclick="doWrite()" disabled>WRITE TO ECU (selected file)</button>

<details>
  <summary>Diagnostika (oletuslik, kinnitamata)</summary>
  <div class="warn">These commands are not yet confirmed from a CAN log - may not work.</div>
  <div class="row2">
    <button class="btn diag" onclick="dtcRead()">Read fault codes</button>
    <button class="btn diag" style="background:#c84b2f" onclick="dtcClear()">Clear fault codes</button>
  </div>
  <ul id="dtclist"></ul>
  <button id="btnLive" class="btn diag" onclick="toggleLive()">Start Live Data</button>
  <div class="infogrid">
    <div class="infocell"><div class="lbl">RPM</div><div class="liveval" id="liveRpm">---</div></div>
    <div class="infocell"><div class="lbl">Temp</div><div class="liveval" id="liveTemp">---</div></div>
    <div class="infocell"><div class="lbl">MAP</div><div class="liveval" id="liveMap">---</div></div>
  </div>
</details>

<script>
let sdFiles = [];
let selectedFile = null;
let connected = false;
let sdReady = false;
let liveOn = false;

function setBusy(busy) {
  document.getElementById('btnConnect').disabled = busy || connected;
  document.getElementById('btnRead').disabled = busy || !connected;
  // NOTE: reading/deleting/uploading on the SD card doesn't need an ECU
  // connection, only the SD card's own presence (sdReady). Only actually
  // writing to the ECU (btnWrite) requires connected=true.
  document.getElementById('btnRefresh').disabled = busy || !sdReady;
  document.getElementById('btnDelete').disabled = busy || !sdReady || !selectedFile;
  document.getElementById('btnWrite').disabled = busy || !connected || !selectedFile;
}

function doConnect() {
  document.getElementById('status').innerText = 'Connecting...';
  fetch('/connect').then(() => poll());
}

function doRead() {
  if (!connected) return;
  document.getElementById('status').innerText = 'Reading...';
  fetch('/read').then(() => poll());
}

function doWrite() {
  if (!selectedFile) { alert('Select a file from the list!'); return; }
  if (!confirm('Write the contents of "' + selectedFile + '" to the entire ECU flash?\n\n' +
               'This is a 2306-step process and takes several minutes.\n\n' +
               'THIS PERMANENTLY MODIFIES THE ECU! Continue?')) return;
  document.getElementById('status').innerText = 'Writing ' + selectedFile + '...';
  fetch('/writefull?name=' + encodeURIComponent(selectedFile)).then(() => poll());
}

function poll() {
  fetch('/status').then(r => r.json()).then(d => {
    document.getElementById('status').innerText = d.status;
    document.getElementById('progressBar').style.width = d.progress + '%';
    connected = d.connected;
    sdReady = d.sdReady;
    document.getElementById('vinVal').innerText = d.vin || '---';
    document.getElementById('calVal').innerText = d.cal || '---';
    document.getElementById('dateVal').innerText = d.date || '---';
    document.getElementById('verVal').innerText = d.ver || '---';
    setBusy(d.busy);
    if (d.busy) {
      setTimeout(poll, 1000);
    } else if (sdReady && sdFiles.length === 0) {
      loadSD();
    }
  });
}

function renderSD() {
  // NOTE: download link removed - file management (adding/removing files
  // on the SD card) is now done only via computer, the phone can only
  // SELECT which already-existing file to write.
  let ul = document.getElementById('sdlist');
  ul.innerHTML = '';
  sdFiles.forEach(f => {
    let li = document.createElement('li');
    li.className = (f.name === selectedFile) ? 'selected' : '';
    li.innerHTML = f.name + '<br><span class="sz">' + f.size + ' baiti</span>';
    li.onclick = () => { selectedFile = f.name; renderSD(); setBusy(false); };
    ul.appendChild(li);
  });
}

function loadSD() {
  fetch('/sdlist').then(r => r.json()).then(files => {
    sdFiles = files;
    renderSD();
    setBusy(false);
  });
}

function deleteSelected() {
  if (!selectedFile) return;
  if (!confirm('Delete ' + selectedFile + ' from the SD card?')) return;
  fetch('/sddelete?name=' + encodeURIComponent(selectedFile)).then(() => {
    selectedFile = null;
    loadSD();
  });
}

function dtcRead() {
  let ul = document.getElementById('dtclist');
  ul.innerHTML = '<li>Loading...</li>';
  fetch('/dtcread').then(r => r.json()).then(codes => {
    ul.innerHTML = '';
    codes.forEach(c => {
      let li = document.createElement('li');
      li.innerText = c;
      ul.appendChild(li);
    });
    if (codes.length === 0) ul.innerHTML = '<li>No fault codes found</li>';
  });
}

function dtcClear() {
  if (!confirm('Clear all fault codes?')) return;
  fetch('/dtcclear').then(() => {
    document.getElementById('dtclist').innerHTML = '<li>Cleared</li>';
  });
}

function toggleLive() {
  liveOn = !liveOn;
  document.getElementById('btnLive').innerText = liveOn ? 'Stop Live Data' : 'Start Live Data';
  if (liveOn) livePoll();
}

function livePoll() {
  if (!liveOn) return;
  fetch('/liveread').then(r => r.json()).then(d => {
    document.getElementById('liveRpm').innerText = d.rpm;
    document.getElementById('liveTemp').innerText = d.temp;
    document.getElementById('liveMap').innerText = d.map;
    if (liveOn) setTimeout(livePoll, 500);
  });
}

setBusy(false);
poll();
</script>
</body>
</html>
)HTML";

// =====================================================================
// BACKGROUND TASKS - Connect/Read/Write run in their own FreeRTOS
// task, so the HTTP server doesn't block while they run.
// =====================================================================

void webTaskFn(void* param) {
  String cmd = *((String*)param);
  delete (String*)param;

  webOpRunning = true;

  if (cmd == "connect") {
    webStatusText = "Connecting to ECU...";
    webProgressPct = 0;
    connected = connectECU();
    webStatusText = connected ? "Connected" : "Connect failed";
    webProgressPct = connected ? 100 : 0;
  }
  else if (cmd == "read") {
    if (!connected) {
      webStatusText = "Not connected - press Connect first";
    } else {
      webStatusText = "Reading flash...";
      webProgressPct = 0;
      reading = true;
      readFlash();
      reading = false;
      if (webReadErrors == 0) {
        webStatusText = "\u2705 Read complete - 0 errors, entire file confirmed";
      } else {
        webStatusText = "\u26a0 Read complete - " + String(webReadErrors) + " error(s)";
      }
      webProgressPct = 100;
    }
  }
  else if (cmd.startsWith("writefull:")) {
    String fname = cmd.substring(10);
    if (!connected) {
      webStatusText = "Not connected - press Connect first";
    } else {
      webStatusText = "Writing " + fname + "...";
      webProgressPct = 0;
      bool ok = writeFullFlash(fname.c_str());
      webStatusText = ok ? "\u2705 Write complete!" : "\u274c Write failed";
    }
  }

  webOpRunning = false;
  vTaskDelete(NULL);
}

void startWebTask(String cmd) {
  if (webOpRunning) return;
  String* param = new String(cmd);
  xTaskCreatePinnedToCore(webTaskFn, "webOp", 8192, param, 1, NULL, 0);
}

// =====================================================================
// HTTP HANDLERS
// =====================================================================

void handleRoot() {
  webServer.send_P(200, "text/html", WEB_PAGE);
}

void handleConnect() {
  startWebTask("connect");
  webServer.send(200, "text/plain", "OK");
}

void handleRead() {
  startWebTask("read");
  webServer.send(200, "text/plain", "OK");
}

void handleWriteFull() {
  if (!webServer.hasArg("name")) {
    webServer.send(400, "text/plain", "missing name");
    return;
  }
  String fname = webServer.arg("name");
  if (!fname.startsWith("/")) fname = "/" + fname;
  startWebTask("writefull:" + fname);
  webServer.send(200, "text/plain", "OK");
}

void handleStatus() {
  String json = "{";
  json += "\"status\":\"" + webStatusText + "\",";
  json += "\"progress\":" + String(webProgressPct) + ",";
  json += "\"busy\":" + String(webOpRunning ? "true" : "false") + ",";
  json += "\"connected\":" + String(connected ? "true" : "false") + ",";
  json += "\"sdReady\":" + String(sdReady ? "true" : "false") + ",";
  json += "\"vin\":\"" + g_vin + "\",";
  json += "\"cal\":\"" + g_cal + "\",";
  json += "\"date\":\"" + g_date + "\",";
  json += "\"ver\":\"" + g_ver + "\"";
  json += "}";
  webServer.send(200, "application/json", json);
}

void handleSDList() {
  String json = "[";
  if (sdReady) {
    File root = SD.open("/");
    File f = root.openNextFile();
    bool first = true;
    while (f) {
      if (!f.isDirectory()) {
        if (!first) json += ",";
        String nm = String(f.name());
        if (!nm.startsWith("/")) nm = "/" + nm;
        json += "{\"name\":\"" + nm + "\",\"size\":" + String(f.size()) + "}";
        first = false;
      }
      f.close();
      f = root.openNextFile();
    }
    root.close();
  }
  json += "]";
  webServer.send(200, "application/json", json);
}

void handleSDDelete() {
  if (!sdReady || !webServer.hasArg("name")) {
    webServer.send(400, "text/plain", "bad request");
    return;
  }
  String name = webServer.arg("name");
  if (!name.startsWith("/")) name = "/" + name;
  if (SD.exists(name) && SD.remove(name)) {
    webServer.send(200, "text/plain", "OK");
  } else {
    webServer.send(500, "text/plain", "ERR");
  }
}

// -------- Diagnostics (just as speculative as the Serial version) --------

void handleDTCRead() {
  String json = "[";
  bool first = true;
  uint8_t resp[8];
  send8(0x03, 0x18, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00);
  if (waitResponse(resp, 1000) && resp[0] != 0x7F) {
    uint16_t code = (resp[1] << 8) | resp[2];
    if (code != 0x0000 && code != 0xFFFF) {
      json += "\"0x" + String(code, HEX) + "\"";
      first = false;
    }
  }
  json += "]";
  webServer.send(200, "application/json", json);
}

void handleDTCClear() {
  uint8_t resp[8];
  send8(0x03, 0x14, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00);
  waitResponse(resp, 1000);
  webServer.send(200, "text/plain", "OK");
}

void handleLiveRead() {
  uint8_t resp[8];
  auto readOne = [&](uint8_t lid, uint16_t& out) -> bool {
    send8(0x02, 0x21, lid, 0x00, 0x00, 0x00, 0x00, 0x00);
    if (!waitResponse(resp, 300) || resp[0] == 0x7F) return false;
    out = (resp[2] << 8) | resp[3];
    return true;
  };
  uint16_t rpm = 0, temp = 0, mapv = 0;
  bool okR = readOne(0x01, rpm);
  bool okT = readOne(0x02, temp);
  bool okM = readOne(0x03, mapv);
  String json = "{\"rpm\":\"" + (okR ? String(rpm) : String("ERR")) + "\",";
  json += "\"temp\":\"" + (okT ? String(temp) : String("ERR")) + "\",";
  json += "\"map\":\"" + (okM ? String(mapv) : String("ERR")) + "\"}";
  webServer.send(200, "application/json", json);
}

void setupWebUI() {
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("STATUS:WIFI_AP_IP:");
  Serial.println(WiFi.softAPIP());

  webServer.on("/", handleRoot);
  webServer.on("/connect", handleConnect);
  webServer.on("/read", handleRead);
  webServer.on("/writefull", handleWriteFull);
  webServer.on("/status", handleStatus);
  webServer.on("/sdlist", handleSDList);
  webServer.on("/sddelete", handleSDDelete);
  webServer.on("/dtcread", handleDTCRead);
  webServer.on("/dtcclear", handleDTCClear);
  webServer.on("/liveread", handleLiveRead);
  webServer.begin();
  Serial.println("STATUS:WEBSERVER_STARTED");
}

void loopWebUI() {
  webServer.handleClient();
}

#endif
