#ifndef WEBUI_H
#define WEBUI_H

#include <WiFi.h>
#include <WebServer.h>

const char* AP_SSID = "M3C_Flasher";
const char* AP_PASS = "flasher123";

WebServer webServer(80);

// =====================================================================
// WEB PAGE - a single-page app that mirrors the desktop Python GUI's
// functionality: Connect, Read, Write (WRITEFULL), SD file management,
// ECU info (VIN/Cal/Date/Ver), Diagnostics.
// =====================================================================

const char WEB_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>KTM M3C Flasher</title>
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
  .upload { background:#0078d4; }
  .reset { background:#8a6d00; font-size:13px; padding:9px; margin-top:18px; }
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
  .wiringbtn { background:#3c3c3c; font-size:13px; padding:9px; }
  #wiring { display:none; background:#252526; border-radius:6px; padding:10px; margin:6px 0; }
  #wiring img { width:100%; height:auto; background:#fff; border-radius:4px; }
  #wiring table { width:100%; border-collapse:collapse; font-size:12px; margin-top:6px; }
  #wiring td { padding:5px 4px; border-bottom:1px solid #3c3c3c; vertical-align:top; }
  #wiring .dot { display:inline-block; width:12px; height:12px; border:1px solid #888; margin-right:4px; vertical-align:middle; }
  #wiring .pin { color:#fff; font-weight:bold; white-space:nowrap; }
  #wiring .fn { color:#4ec9b0; white-space:nowrap; }
</style>
</head>
<body>
<h2>KTM M3C Flasher</h2>

<button id="btnConnect" class="btn connect" onclick="doConnect()">CONNECT</button>

<div class="infogrid">
  <div class="infocell"><div class="lbl">VIN</div><div class="val" id="vinVal">---</div></div>
  <div class="infocell"><div class="lbl">Calibration</div><div class="val" id="calVal">---</div></div>
  <div class="infocell"><div class="lbl">Date</div><div class="val" id="dateVal">---</div></div>
  <div class="infocell"><div class="lbl">Software</div><div class="val" id="verVal">---</div></div>
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

<button id="btnWiring" class="btn wiringbtn" onclick="toggleWiring()">WIRING DIAGRAM</button>
<div id="wiring">
  <h3 style="margin-top:0">ECU connector A (front view)</h3>
  <img alt="ECU connector A pinout" src="data:image/png;base64,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">
  <h3>ECU side</h3>
  <table>
    <tr><td class="pin"><span class="dot" style="background:#fff01e"></span>B1</td><td class="fn">CAN-H</td><td>CAN transceiver CANH</td></tr>
    <tr><td class="pin"><span class="dot" style="background:#1e34ff"></span>C1</td><td class="fn">CAN-L</td><td>CAN transceiver CANL</td></tr>
    <tr><td class="pin"><span class="dot" style="background:#e81f1b"></span>A4, B4</td><td class="fn">+12 V</td><td>+12 V supply (also acts as ignition)</td></tr>
    <tr><td class="pin"><span class="dot" style="background:#000"></span>M4</td><td class="fn">GND</td><td>Supply GND + ESP32/transceiver GND</td></tr>
  </table>
  <h3>ESP32 side</h3>
  <table>
    <tr><td class="pin">GPIO 4</td><td>CAN transceiver TX (CTX)</td></tr>
    <tr><td class="pin">GPIO 5</td><td>CAN transceiver RX (CRX)</td></tr>
    <tr><td class="pin">3.3 V / GND</td><td>CAN transceiver VCC / GND</td></tr>
    <tr><td class="pin">GPIO 15 / 18 / 17 / 16</td><td>SD card CS / SCK / MISO / MOSI</td></tr>
  </table>
  <div class="warn" style="margin-top:8px">The ESP32/transceiver GND MUST be connected to ECU GND (M4), otherwise CAN will not work. CAN speed is 500 kbit/s. Make sure the connector and cables cannot move while writing.</div>
</div>

<button id="btnReset" class="btn reset" onclick="doReset()">RESET ESP32</button>

<script>
let sdFiles = [];
let selectedFile = null;
let connected = false;
let sdReady = false;
let wasBusy = false;

function setBusy(busy) {
  document.getElementById('btnConnect').disabled = busy || connected;
  document.getElementById('btnRead').disabled = busy || !connected;
  // CHANGE: reading/deleting/uploading SD card files does not require
  // an ECU connection, only the SD card itself to be present
  // (sdReady). Only writing to the real ECU (btnWrite) requires
  // connected=true.
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
  let name = prompt('File name for the read (e.g. the owner\'s name).\n' +
                    'Leave empty for flash_dump. ".bin" is added automatically.', '');
  if (name === null) return;   // Cancel
  document.getElementById('status').innerText = 'Reading...';
  fetch('/read?name=' + encodeURIComponent(name.trim())).then(() => poll());
}

function doWrite() {
  if (!selectedFile) { alert('Select a file from the list!'); return; }
  if (!confirm('Write the contents of "' + selectedFile + '" into the entire ECU flash?\n\n' +
               'This is a 2306-step process and takes several minutes.\n\n' +
               'THIS PERMANENTLY CHANGES THE ECU! Continue?')) return;
  document.getElementById('status').innerText = 'Writing ' + selectedFile + '...';
  fetch('/writefull?name=' + encodeURIComponent(selectedFile)).then(() => poll());
}

function toggleWiring() {
  let w = document.getElementById('wiring');
  let open = w.style.display !== 'block';
  w.style.display = open ? 'block' : 'none';
  document.getElementById('btnWiring').innerText = open ? 'HIDE WIRING DIAGRAM' : 'WIRING DIAGRAM';
}

function doReset() {
  if (!confirm('Restart the ESP32?\n\nAny operation in progress will be stopped and the ECU ' +
               'connection is lost - you will need to press CONNECT again.\n\n' +
               'Do NOT reset while writing to the ECU unless the write is already stuck.')) return;
  document.getElementById('status').innerText = 'Restarting ESP32... reconnect to the WiFi if the page does not come back.';
  connected = false;
  sdFiles = [];
  selectedFile = null;
  renderSD();
  setBusy(true);
  fetch('/reset').catch(() => {});
  // The ESP32 needs a few seconds to boot and start the WiFi AP again
  setTimeout(function retry() {
    fetch('/status').then(() => { location.reload(); })
                    .catch(() => setTimeout(retry, 2000));
  }, 5000);
}

function poll() {
  fetch('/status').then(r => r.json()).then(d => {
    // Simple status line: what is happening + percentage while it runs
    let txt = d.status;
    if (d.busy && d.progress > 0 && d.progress < 100) txt += '  ' + d.progress + '%';
    document.getElementById('status').innerText = txt;
    document.getElementById('progressBar').style.width = d.progress + '%';
    connected = d.connected;
    sdReady = d.sdReady;
    document.getElementById('vinVal').innerText = d.vin || '---';
    document.getElementById('calVal').innerText = d.cal || '---';
    document.getElementById('dateVal').innerText = d.date || '---';
    document.getElementById('verVal').innerText = d.ver || '---';
    setBusy(d.busy);
    if (d.busy) {
      wasBusy = true;
      setTimeout(poll, 1000);
    } else if (wasBusy) {
      // An operation just finished - a write may have created a new
      // "_CHK_OK" file and a read a new dump, so reload the SD list.
      wasBusy = false;
      if (sdReady) loadSD();
    } else if (sdReady && sdFiles.length === 0) {
      loadSD();
    }
  });
}

function renderSD() {
  // CHANGE: the download link has been removed - file management
  // (adding/removing files on the SD card) now happens only through
  // the computer; the phone can only SELECT which existing file to
  // write.
  let ul = document.getElementById('sdlist');
  ul.innerHTML = '';
  sdFiles.forEach(f => {
    let li = document.createElement('li');
    li.className = (f.name === selectedFile) ? 'selected' : '';
    li.innerHTML = f.name + '<br><span class="sz">' + f.size + ' bytes</span>';
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

setBusy(false);
poll();
</script>
</body>
</html>
)HTML";

// =====================================================================
// BACKGROUND TASKS - Connect/Read/Write run in their own FreeRTOS
// task, so the HTTP server doesn't block at the same time.
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
      if (webReadDone == 0) {
        webStatusText = "❌ Read failed - the ECU did not answer, no file was saved";
      } else if (webReadErrors == 0) {
        webStatusText = "✅ Read complete - 0 errors, entire file confirmed";
      } else {
        webStatusText = "⚠ Read complete - " + String(webReadErrors) + " addresses missing (0xFF in the file)";
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
      if (ok) {
        webProgressPct = 100;
        if (webFixedFile.length() > 0) {
          webStatusText = "✅ Write complete - checksum was corrected, wrote " + webFixedFile;
        } else {
          webStatusText = "✅ Write complete - checksum OK";
        }
      } else {
        String reason = webFailReason.length() > 0 ? webFailReason : String("unknown error");
        webStatusText = "❌ Write failed: " + reason;
        // A failure after erasing has started leaves the ECU incomplete
        if (reason.startsWith("ECU did not confirm")) {
          webStatusText += ". The ECU is NOT complete - fix the connection and write again before switching it off!";
        }
      }
    }
  }

  webOpRunning = false;
  vTaskDelete(NULL);
}

void startWebTask(String cmd) {
  if (webOpRunning) return;
  // Safety lock: never start a flash operation while diagnostics is
  // using the bus (see diagnostics.h).
  if (diagActive()) {
    webStatusText = "Diagnostics is running - stop live data in the app first";
    return;
  }
  // Set before the task starts, so loop() stops touching the CAN bus
  // right away (the task runs on the other CPU core).
  webOpRunning = true;
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
  readDumpName = webServer.hasArg("name") ? webServer.arg("name") : "";
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

// Escapes text for use inside a JSON string (file names can contain
// quotes or backslashes, which would otherwise break the page).
String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if ((uint8_t)c < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

void handleStatus() {
  String json = "{";
  json += "\"status\":\"" + jsonEscape(webStatusText) + "\",";
  json += "\"progress\":" + String(webProgressPct) + ",";
  json += "\"busy\":" + String(webOpRunning ? "true" : "false") + ",";
  json += "\"connected\":" + String(connected ? "true" : "false") + ",";
  json += "\"sdReady\":" + String(sdReady ? "true" : "false") + ",";
  json += "\"vin\":\"" + jsonEscape(g_vin) + "\",";
  json += "\"cal\":\"" + jsonEscape(g_cal) + "\",";
  json += "\"date\":\"" + jsonEscape(g_date) + "\",";
  json += "\"ver\":\"" + jsonEscape(g_ver) + "\"";
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
        json += "{\"name\":\"" + jsonEscape(nm) + "\",\"size\":" + String(f.size()) + "}";
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

// Restarts the ESP32. The response is sent first so the browser gets an
// answer before the WiFi access point goes down.
void handleReset() {
  webServer.send(200, "text/plain", "RESETTING");
  delay(300);
  doEspRestart();
}

void setupWebUI() {
  WiFi.softAP(AP_SSID, AP_PASS);
  Out.print("STATUS:WIFI_AP_IP:");
  Out.println(WiFi.softAPIP());

  webServer.on("/", handleRoot);
  webServer.on("/connect", handleConnect);
  webServer.on("/read", handleRead);
  webServer.on("/writefull", handleWriteFull);
  webServer.on("/status", handleStatus);
  webServer.on("/sdlist", handleSDList);
  webServer.on("/sddelete", handleSDDelete);
  webServer.on("/reset", handleReset);
  webServer.begin();
  Out.println("STATUS:WEBSERVER_STARTED");
}

void loopWebUI() {
  webServer.handleClient();
}

#endif