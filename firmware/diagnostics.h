#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

// =====================================================================
// DIAGNOSTICS MODE - live data, fault codes, ECU info
//
// Ported from the standalone m3c_diagnostics firmware, which was tested
// on a real 2020 Husqvarna TE 300i (Continental M3C ECU):
//   - live data the ECU broadcasts by itself (0x6B1 / 0x6B3)
//   - standard OBD-II mode 01 PIDs on 0x7E0 / 0x7E8
//   - ECU identification (0x1A), fault code read and clear
//
// Diagnostics NEVER does security access and never writes the flash.
//
// SAFETY LOCK - flash mode and diagnostics mode never run together:
//   * While a flash session is active (CONNECT done, READ or WRITEFULL
//     running, or a web UI operation running) every diagnostics command
//     is refused with  ERR:LOCKED:FLASH  and no diagnostic frame is sent.
//     Extra traffic on the bus during a read/write could corrupt the
//     transfer and leave the ECU half-written (bricked).
//   * While diagnostics is active (live data streaming or a diagnostic
//     request running) CONNECT / READ / WRITEFULL are refused with
//     ERR:LOCKED:DIAG. Stop live data first (DIAG_LIVE:0).
//   * ECU_RELEASE ends a finished flash session (connected = false) so
//     diagnostics can be used again. Refused while a read/write runs.
//     Switch the ignition off and on before using diagnostics after
//     a write.
//
// Commands (Serial and BLE, one per line):
//   MODE                 -> MODE:IDLE | MODE:FLASH | MODE:DIAG
//   ECU_RELEASE          -> STATUS:ECU_RELEASED (+ MODE:IDLE)
//   DIAG_LIVE:1 / :0     -> start/stop live data. While on:
//        LIVE:RPM=..;THR=..;CLT=..;FLT=..;LOAD=..;IAT=..;MAP=..;ADV=..;
//             STFT=..;LTFT=..;BARO=..;RUN=..;MIL=..;DTCN=..   (4 per s)
//        LIVE:NONE   when the ECU is silent (ignition off / engine off)
//        Values that were not received yet are sent as '?'.
//   INFO                 -> INFO:VIN:.. INFO:CAL:.. INFO:DATE:.. INFO:VER:..
//                           INFO_DONE
//   DTC_READ             -> DIAG:.. / DTC:<method>:<raw>:<Pcode>:<status>
//                           DTCCOUNT:<method>:<family>:<n>  DTC_READ_DONE
//   DTC_CLEAR:KWP|OBD|UDS-> CLEAR:<family>:OK | NRC:.. | NORESP | UNEXPECTED
//   OBD:1 / OBD:0        -> raw OBD PID lines OBD:<pid>:<hex> (desktop GUI)
//   MON:1 / MON:0        -> raw CAN monitor F:<id>:<hex>
//   SEND:<id>:<hex>      -> one raw frame (testing only)
// =====================================================================

#define DIAG_ECU_REQ_ID   0x7E0
#define DIAG_OBD_FUNC_ID  0x7DF

struct DtcMethod {
  const char* name;
  const char* family;   // KWP / OBD / UDS - used for DTC_CLEAR
  uint16_t reqId;
  uint8_t len;
  uint8_t data[4];
};

bool diagLiveOn = false;        // LIVE: lines streaming (phone app)
bool diagObdRawOn = false;      // OBD: raw lines (desktop diagnostics GUI)
bool diagMonitorOn = false;
volatile bool diagBusy = false; // a diagnostic request is running

// ---- live values ----
uint16_t dgRpm = 0;
uint8_t dgThrRaw = 0;
int dgCoolant = -100;           // -100 = not received yet
uint8_t dgFaults = 0;
bool dgFaultsSeen = false;
unsigned long dgLastBroadcastMs = 0;
unsigned long dgLastLivePrintMs = 0;

// OBD values, cached: value + "received" flag
struct ObdVal { float v; bool ok; };
ObdVal dgLoad = {0, false}, dgIat = {0, false}, dgMap = {0, false}, dgAdv = {0, false};
ObdVal dgStft = {0, false}, dgLtft = {0, false}, dgBaro = {0, false}, dgRun = {0, false};
ObdVal dgMil = {0, false}, dgDtcN = {0, false};

const uint8_t DIAG_OBD_PIDS[] = {0x0C, 0x11, 0x05, 0x0F, 0x0B, 0x0E, 0x04,
                                 0x0C, 0x11, 0x06, 0x07, 0x33, 0x1F, 0x01};
const int DIAG_OBD_PID_COUNT = sizeof(DIAG_OBD_PIDS) / sizeof(DIAG_OBD_PIDS[0]);
int dgObdIndex = 0;
unsigned long dgLastObdPollMs = 0;
#define DIAG_OBD_POLL_INTERVAL_MS 70

// =====================================================================
// Lock state
// =====================================================================
bool flashSessionActive() {
  return connected || reading || writingFull || webOpRunning;
}

bool diagActive() {
  return diagLiveOn || diagObdRawOn || diagBusy;
}

void printMode() {
  if (flashSessionActive()) Out.println("MODE:FLASH");
  else if (diagActive()) Out.println("MODE:DIAG");
  else Out.println("MODE:IDLE");
}

// Called before every diagnostics command. Prints the refusal itself.
bool diagAllowed() {
  if (flashSessionActive()) {
    Out.println("ERR:LOCKED:FLASH");
    return false;
  }
  return true;
}

// Called before CONNECT / READ / WRITEFULL. Prints the refusal itself.
bool flashAllowed() {
  if (diagActive()) {
    Out.println("ERR:LOCKED:DIAG");
    return false;
  }
  return true;
}

// =====================================================================
// Helpers
// =====================================================================
void diagPrintHex(const uint8_t* d, int n) {
  for (int i = 0; i < n; i++) {
    if (d[i] < 0x10) Out.print('0');
    Out.print(d[i], HEX);
    if (i < n - 1) Out.print(' ');
  }
}

String diagHex2(uint8_t b) {
  char s[3];
  snprintf(s, sizeof(s), "%02X", b);
  return String(s);
}

// Every received frame passes through here, also while waiting for a
// diagnostic answer, so live data keeps updating.
void diagProcessFrame(const CanFrame& f) {
  if (diagMonitorOn) {
    Out.print("F:");
    Out.print(f.identifier, HEX);
    Out.print(':');
    diagPrintHex(f.data, f.data_length_code);
    Out.println();
  }
  if (f.identifier == 0x6B1 && f.data_length_code >= 7) {
    dgRpm = ((uint16_t)f.data[0] << 8) | f.data[1];
    dgThrRaw = f.data[2];
    dgLastBroadcastMs = millis();
  } else if (f.identifier == 0x6B3 && f.data_length_code >= 7) {
    dgFaults = f.data[5];
    dgFaultsSeen = true;
    dgCoolant = (int)f.data[6] - 40;
    dgLastBroadcastMs = millis();
  }
}

void diagPumpFrames(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (ESP32Can.readFrame(rxFrame, 2)) diagProcessFrame(rxFrame);
  }
}

bool diagSendFrame(uint16_t id, const uint8_t* data, uint8_t len) {
  CanFrame tx = {0};
  tx.identifier = id;
  tx.extd = 0;
  tx.data_length_code = 8;          // the ECU expects 8-byte frames
  memset(tx.data, 0, 8);
  memcpy(tx.data, data, len > 8 ? 8 : len);
  for (int i = 0; i < 3; i++) {
    if (ESP32Can.writeFrame(tx)) return true;
    delay(2);
  }
  return false;
}

const char* diagNrcText(uint8_t nrc) {
  switch (nrc) {
    case 0x10: return "generalReject";
    case 0x11: return "serviceNotSupported";
    case 0x12: return "subFunctionNotSupported";
    case 0x13: return "incorrectLength";
    case 0x21: return "busyRepeatRequest";
    case 0x22: return "conditionsNotCorrect";
    case 0x31: return "requestOutOfRange";
    case 0x33: return "securityAccessDenied";
    case 0x78: return "responsePending";
    case 0x7E: return "notSupportedInActiveSession";
    case 0x7F: return "serviceNotSupportedInActiveSession";
    default:   return "unknown";
  }
}

// ISO-TP request (single frame) / answer (single or multi frame).
// The answer payload (service byte first) goes to `out`.
// Returns 1 = answer received, 0 = no answer.
int diagRequest(uint16_t reqId, const uint8_t* payload, uint8_t plen,
                uint8_t* out, int& outLen, int maxOut, unsigned long timeoutMs = 600) {
  outLen = 0;
  if (flashSessionActive()) return 0;   // last line of defence
  uint8_t frame[8] = {0};
  frame[0] = plen;                       // single frame PCI
  memcpy(frame + 1, payload, plen);
  if (!diagSendFrame(reqId, frame, 8)) {
    Out.println("ERR:TX_FAIL");
    return 0;
  }
  unsigned long start = millis();
  int expected = -1;
  uint8_t nextSn = 1;
  while (millis() - start < timeoutMs) {
    if (!ESP32Can.readFrame(rxFrame, 5)) continue;
    if (rxFrame.identifier < 0x7E8 || rxFrame.identifier > 0x7EF) {
      diagProcessFrame(rxFrame);
      continue;
    }
    if (diagMonitorOn) diagProcessFrame(rxFrame);
    uint8_t* d = rxFrame.data;
    uint8_t type = d[0] & 0xF0;
    if (type == 0x00) {                  // single frame
      int len = d[0] & 0x0F;
      if (len >= 3 && d[1] == 0x7F && d[3] == 0x78) {
        start = millis();                // "response pending" - wait longer
        timeoutMs = 3000;
        continue;
      }
      for (int i = 0; i < len && i < 7 && outLen < maxOut; i++) out[outLen++] = d[1 + i];
      return 1;
    } else if (type == 0x10) {           // first frame
      expected = ((d[0] & 0x0F) << 8) | d[1];
      for (int i = 2; i < 8 && outLen < maxOut && outLen < expected; i++) out[outLen++] = d[i];
      uint8_t fc[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};   // flow control: send all
      diagSendFrame(DIAG_ECU_REQ_ID, fc, 8);
      nextSn = 1;
      start = millis();
    } else if (type == 0x20 && expected > 0) {   // consecutive frame
      if ((d[0] & 0x0F) != (nextSn & 0x0F)) Out.println("WARN:ISOTP_SEQUENCE");
      nextSn++;
      for (int i = 1; i < 8 && outLen < maxOut && outLen < expected; i++) out[outLen++] = d[i];
      start = millis();
      if (outLen >= expected || outLen >= maxOut) return 1;
    }
  }
  return outLen > 0 ? 1 : 0;
}

// =====================================================================
// ECU information (read only)
// =====================================================================
bool diagTesterPresent() {
  uint8_t p[] = {0x3E, 0x01};
  uint8_t r[16]; int n;
  if (!diagRequest(DIAG_ECU_REQ_ID, p, 2, r, n, sizeof(r), 500)) return false;
  return n >= 1 && r[0] == 0x7E;
}

void diagPrintIdField(const char* name, uint8_t ident, bool bcdDate) {
  uint8_t p[] = {0x1A, ident};
  uint8_t r[80]; int n;
  Out.print("INFO:"); Out.print(name); Out.print(':');
  if (!diagRequest(DIAG_ECU_REQ_ID, p, 2, r, n, sizeof(r), 600) || n < 2 || r[0] != 0x5A) {
    Out.println("?");
    return;
  }
  if (bcdDate && n >= 5) {
    auto bcd = [](uint8_t b) { return (int)((b >> 4) * 10 + (b & 0x0F)); };
    char s[16];
    snprintf(s, sizeof(s), "%02d.%02d.%02d", bcd(r[2]), bcd(r[3]), bcd(r[4]));
    Out.println(s);
  } else {
    for (int i = 2; i < n; i++) {
      char c = (char)r[i];
      Out.print((c >= 32 && c < 127) ? c : '.');
    }
    Out.println();
  }
}

void diagReadInfo() {
  Out.println("STEP:INFO");
  if (!diagTesterPresent()) {
    Out.println("ERR:ECU_NO_RESPONSE");
    Out.println("INFO_DONE");
    return;
  }
  diagPrintIdField("VIN", 0x90, false);
  diagPrintIdField("CAL", 0x93, false);
  diagPrintIdField("DATE", 0x86, true);
  diagPrintIdField("VER", 0x95, false);
  Out.println("INFO_DONE");
}

// =====================================================================
// Fault codes. The ECU does not broadcast stored codes, so several
// standard read-only requests are tried one after another.
// =====================================================================
const DtcMethod DTC_METHODS[] = {
  {"KWP_18_02", "KWP", DIAG_ECU_REQ_ID, 4, {0x18, 0x02, 0xFF, 0x00}},
  {"KWP_18_00", "KWP", DIAG_ECU_REQ_ID, 4, {0x18, 0x00, 0xFF, 0x00}},
  {"KWP_17",    "KWP", DIAG_ECU_REQ_ID, 3, {0x17, 0xFF, 0x00}},
  {"OBD_03",    "OBD", DIAG_ECU_REQ_ID, 1, {0x03}},
  {"OBD_03F",   "OBD", DIAG_OBD_FUNC_ID, 1, {0x03}},
  {"OBD_07",    "OBD", DIAG_ECU_REQ_ID, 1, {0x07}},
  {"UDS_19",    "UDS", DIAG_ECU_REQ_ID, 3, {0x19, 0x02, 0xFF}},
};
const int DTC_METHOD_COUNT = sizeof(DTC_METHODS) / sizeof(DTC_METHODS[0]);

String diagPcode(uint16_t code) {
  const char letters[] = {'P', 'C', 'B', 'U'};
  char s[8];
  snprintf(s, sizeof(s), "%c%01X%03X", letters[(code >> 14) & 3], (code >> 12) & 3, code & 0x0FFF);
  return String(s);
}

void diagReportDtc(const char* method, uint32_t code, int codeBytes, int status) {
  Out.print("DTC:");
  Out.print(method);
  Out.print(':');
  for (int b = codeBytes - 1; b >= 0; b--) Out.print(diagHex2((code >> (8 * b)) & 0xFF));
  Out.print(':');
  Out.print(diagPcode((uint16_t)(codeBytes == 3 ? (code >> 8) : code)));
  Out.print(':');
  if (status >= 0) Out.println(diagHex2((uint8_t)status)); else Out.println("-");
}

int diagDecodeDtcAnswer(const DtcMethod& m, const uint8_t* r, int n) {
  uint8_t svc = r[0];
  int count = 0;
  if (svc == 0x58 && n >= 2) {                      // KWP 0x18 answer
    int num = r[1];
    for (int i = 2; i + 2 < n && count < num; i += 3) {
      uint16_t code = ((uint16_t)r[i] << 8) | r[i + 1];
      diagReportDtc(m.name, code, 2, r[i + 2]);
      count++;
    }
    return count;
  }
  if (svc == 0x57 && n >= 2) {                      // KWP 0x17 answer
    for (int i = 2; i + 2 < n; i += 3) {
      uint16_t code = ((uint16_t)r[i] << 8) | r[i + 1];
      if (code == 0 || code == 0xFFFF) continue;
      diagReportDtc(m.name, code, 2, r[i + 2]);
      count++;
    }
    return count;
  }
  if (svc == 0x43 || svc == 0x47) {                 // OBD mode 03 / 07 answer
    int start = (n >= 2 && (n - 2) % 2 == 0) ? 2 : 1;
    for (int i = start; i + 1 < n; i += 2) {
      uint16_t code = ((uint16_t)r[i] << 8) | r[i + 1];
      if (code == 0) continue;
      diagReportDtc(m.name, code, 2, -1);
      count++;
    }
    return count;
  }
  if (svc == 0x59 && n >= 3) {                      // UDS 0x19 02 answer
    for (int i = 3; i + 3 < n; i += 4) {
      uint32_t code = ((uint32_t)r[i] << 16) | ((uint32_t)r[i + 1] << 8) | r[i + 2];
      diagReportDtc(m.name, code, 3, r[i + 3]);
      count++;
    }
    return count;
  }
  return -1;
}

void diagReadDtcs() {
  Out.println("STEP:DTC_READ");
  bool tp = diagTesterPresent();
  Out.print("DIAG:TESTER_PRESENT:");
  Out.println(tp ? "OK" : "NORESP");

  for (int k = 0; k < DTC_METHOD_COUNT; k++) {
    const DtcMethod& m = DTC_METHODS[k];
    uint8_t r[128]; int n;
    Out.print("DIAG:"); Out.print(m.name); Out.print(":TX:");
    Out.print(m.reqId, HEX); Out.print(':');
    diagPrintHex(m.data, m.len);
    Out.println();

    if (!diagRequest(m.reqId, m.data, m.len, r, n, sizeof(r), 700)) {
      Out.print("DIAG:"); Out.print(m.name); Out.println(":NORESP");
      diagPumpFrames(50);
      continue;
    }
    Out.print("DIAG:"); Out.print(m.name); Out.print(":RX:");
    diagPrintHex(r, n);
    Out.println();

    if (n >= 3 && r[0] == 0x7F) {
      Out.print("DIAG:"); Out.print(m.name); Out.print(":NRC:");
      Out.print(diagHex2(r[2])); Out.print(':');
      Out.println(diagNrcText(r[2]));
    } else {
      int c = diagDecodeDtcAnswer(m, r, n);
      Out.print("DTCCOUNT:"); Out.print(m.name); Out.print(':');
      Out.print(m.family); Out.print(':');
      if (c < 0) Out.println("?"); else Out.println(c);
    }
    diagPumpFrames(50);
  }
  Out.println("DTC_READ_DONE");
}

void diagClearDtcs(const String& family) {
  uint8_t req[4]; uint8_t len; uint8_t okSvc; uint16_t id = DIAG_ECU_REQ_ID;
  if (family == "KWP")      { req[0] = 0x14; req[1] = 0xFF; req[2] = 0x00; len = 3; okSvc = 0x54; }
  else if (family == "OBD") { req[0] = 0x04; len = 1; okSvc = 0x44; }
  else if (family == "UDS") { req[0] = 0x14; req[1] = 0xFF; req[2] = 0xFF; req[3] = 0xFF; len = 4; okSvc = 0x54; }
  else { Out.println("ERR:CLEAR_UNKNOWN_METHOD"); return; }

  diagTesterPresent();
  Out.print("DIAG:CLEAR_"); Out.print(family); Out.print(":TX:");
  Out.print(id, HEX); Out.print(':'); diagPrintHex(req, len); Out.println();

  uint8_t r[32]; int n;
  if (!diagRequest(id, req, len, r, n, sizeof(r), 2000)) {
    Out.print("CLEAR:"); Out.print(family); Out.println(":NORESP");
    return;
  }
  Out.print("DIAG:CLEAR_"); Out.print(family); Out.print(":RX:");
  diagPrintHex(r, n); Out.println();
  Out.print("CLEAR:"); Out.print(family); Out.print(':');
  if (n >= 1 && r[0] == okSvc) Out.println("OK");
  else if (n >= 3 && r[0] == 0x7F) { Out.print("NRC:"); Out.print(diagHex2(r[2])); Out.print(':'); Out.println(diagNrcText(r[2])); }
  else Out.println("UNEXPECTED");
}

// SEND:<id hex>:<up to 8 data bytes hex> - testing only
void diagSendRaw(const String& arg) {
  int sep = arg.indexOf(':');
  if (sep < 0) { Out.println("ERR:SEND_FORMAT"); return; }
  uint16_t id = (uint16_t)strtol(arg.substring(0, sep).c_str(), NULL, 16);
  String hex = arg.substring(sep + 1);
  hex.replace(" ", "");
  uint8_t data[8] = {0};
  int n = 0;
  for (int i = 0; i + 1 < (int)hex.length() && n < 8; i += 2) {
    data[n++] = (uint8_t)strtol(hex.substring(i, i + 2).c_str(), NULL, 16);
  }
  if (id == 0 || id > 0x7FF || n == 0) { Out.println("ERR:SEND_FORMAT"); return; }
  Out.print("TX:"); Out.print(id, HEX); Out.print(':'); diagPrintHex(data, 8); Out.println();
  if (!diagSendFrame(id, data, 8)) { Out.println("ERR:TX_FAIL"); return; }
  unsigned long start = millis();
  int answers = 0;
  while (millis() - start < 1000) {
    if (!ESP32Can.readFrame(rxFrame, 5)) continue;
    if (rxFrame.identifier >= 0x7E8 && rxFrame.identifier <= 0x7EF) {
      Out.print("RX:"); Out.print(rxFrame.identifier, HEX); Out.print(':');
      diagPrintHex(rxFrame.data, rxFrame.data_length_code); Out.println();
      answers++;
    } else {
      diagProcessFrame(rxFrame);
    }
  }
  Out.print("SEND_DONE:"); Out.println(answers);
}

// =====================================================================
// Live data
// =====================================================================
void diagStoreObd(uint8_t pid, const uint8_t* d, int n) {
  if (n < 1) return;
  float A = d[0], B = n >= 2 ? d[1] : 0;
  switch (pid) {
    case 0x04: dgLoad = {A * 100.0f / 255.0f, true}; break;
    case 0x0F: dgIat  = {A - 40.0f, true}; break;
    case 0x0B: dgMap  = {A, true}; break;
    case 0x0E: dgAdv  = {A / 2.0f - 64.0f, true}; break;
    case 0x06: dgStft = {(A - 128.0f) * 100.0f / 128.0f, true}; break;
    case 0x07: dgLtft = {(A - 128.0f) * 100.0f / 128.0f, true}; break;
    case 0x33: dgBaro = {A, true}; break;
    case 0x1F: if (n >= 2) dgRun = {A * 256.0f + B, true}; break;
    case 0x01: dgMil = {(float)((d[0] & 0x80) ? 1 : 0), true};
               dgDtcN = {(float)(d[0] & 0x7F), true}; break;
    default: break;
  }
}

// One OBD PID per call, round robin. Only while the ECU is awake
// (broadcast frames seen), so a dead bus is not flooded with requests.
void diagPollObd() {
  if (!diagLiveOn && !diagObdRawOn) return;
  if (millis() - dgLastObdPollMs < DIAG_OBD_POLL_INTERVAL_MS) return;
  dgLastObdPollMs = millis();
  if (dgLastBroadcastMs == 0 || millis() - dgLastBroadcastMs > 1500) return;

  uint8_t pid = DIAG_OBD_PIDS[dgObdIndex];
  dgObdIndex = (dgObdIndex + 1) % DIAG_OBD_PID_COUNT;
  uint8_t req[] = {0x01, pid};
  uint8_t r[16]; int n;
  diagBusy = true;
  int ok = diagRequest(DIAG_ECU_REQ_ID, req, 2, r, n, sizeof(r), 150);
  diagBusy = false;
  if (!ok || n < 3 || r[0] != 0x41 || r[1] != pid) return;
  diagStoreObd(pid, r + 2, n - 2);
  if (diagObdRawOn) {
    Out.print("OBD:");
    Out.print(diagHex2(pid));
    Out.print(':');
    diagPrintHex(r + 2, n - 2);
    Out.println();
  }
}

void diagPrintVal(const char* key, const ObdVal& v, int decimals) {
  Out.print(';'); Out.print(key); Out.print('=');
  if (v.ok) Out.print(v.v, decimals); else Out.print('?');
}

void diagPrintLive() {
  if (dgLastBroadcastMs == 0 || millis() - dgLastBroadcastMs > 1500) {
    Out.println("LIVE:NONE");
    return;
  }
  float thr = dgThrRaw * 100.0f / 147.0f;
  if (thr > 100.0f) thr = 100.0f;
  Out.print("LIVE:RPM=");
  Out.print(dgRpm);
  Out.print(";THR=");
  Out.print(thr, 1);
  Out.print(";CLT=");
  if (dgCoolant == -100) Out.print('?'); else Out.print(dgCoolant);
  Out.print(";FLT=");
  if (dgFaultsSeen) Out.print(diagHex2(dgFaults)); else Out.print('?');
  diagPrintVal("LOAD", dgLoad, 0);
  diagPrintVal("IAT", dgIat, 0);
  diagPrintVal("MAP", dgMap, 0);
  diagPrintVal("ADV", dgAdv, 1);
  diagPrintVal("STFT", dgStft, 1);
  diagPrintVal("LTFT", dgLtft, 1);
  diagPrintVal("BARO", dgBaro, 0);
  diagPrintVal("RUN", dgRun, 0);
  diagPrintVal("MIL", dgMil, 0);
  diagPrintVal("DTCN", dgDtcN, 0);
  Out.println();
}

void diagStopAll() {
  diagLiveOn = false;
  diagObdRawOn = false;
  diagMonitorOn = false;
}

// Called from loop(). Does nothing while a flash session is active, so
// no frame is read or sent behind the flasher's back.
void diagLoop() {
  // Live data is only used by the phone app: stop it when the phone
  // disconnects, so it does not keep the flash functions locked.
  static bool prevBle = false;
  if (prevBle && !bleClientConnected && diagLiveOn) {
    diagLiveOn = false;
    Out.println("STATUS:LIVE_OFF");
  }
  prevBle = bleClientConnected;

  if (flashSessionActive()) return;
  while (ESP32Can.readFrame(rxFrame, 0)) diagProcessFrame(rxFrame);
  diagPollObd();
  if (diagLiveOn && millis() - dgLastLivePrintMs >= 250) {
    dgLastLivePrintMs = millis();
    diagPrintLive();
  }
}

// Returns true if the line was a diagnostics command (handled here).
bool handleDiagCommand(const String& line) {
  if (line == "MODE") { printMode(); return true; }

  if (line == "ECU_RELEASE") {
    if (reading || writingFull || webOpRunning) { Out.println("ERR:BUSY"); return true; }
    connected = false;
    ecuNeedsReauth = false;
    Out.println("STATUS:ECU_RELEASED");
    printMode();
    return true;
  }

  bool isDiag = line.startsWith("DIAG_LIVE:") || line == "INFO" || line == "DTC_READ" ||
                line.startsWith("DTC_CLEAR:") || line.startsWith("OBD:") ||
                line.startsWith("MON:") || line.startsWith("SEND:");
  if (!isDiag) return false;

  // Stopping is always allowed - it only makes things quieter.
  if (line == "DIAG_LIVE:0") { diagLiveOn = false; Out.println("STATUS:LIVE_OFF"); printMode(); return true; }
  if (line == "OBD:0") { diagObdRawOn = false; Out.println("STATUS:OBD_POLL_OFF"); printMode(); return true; }
  if (line == "MON:0") { diagMonitorOn = false; Out.println("STATUS:MONITOR_OFF"); return true; }

  if (!diagAllowed()) return true;

  if (line == "DIAG_LIVE:1") {
    diagLiveOn = true;
    dgLastLivePrintMs = 0;
    Out.println("STATUS:LIVE_ON");
    printMode();
  }
  else if (line == "OBD:1") { diagObdRawOn = true; Out.println("STATUS:OBD_POLL_ON"); printMode(); }
  else if (line == "MON:1") { diagMonitorOn = true; Out.println("STATUS:MONITOR_ON"); }
  else if (line == "INFO") { diagBusy = true; diagReadInfo(); diagBusy = false; }
  else if (line == "DTC_READ") { diagBusy = true; diagReadDtcs(); diagBusy = false; }
  else if (line.startsWith("DTC_CLEAR:")) { diagBusy = true; diagClearDtcs(line.substring(10)); diagBusy = false; }
  else if (line.startsWith("SEND:")) { diagBusy = true; diagSendRaw(line.substring(5)); diagBusy = false; }
  return true;
}

#endif
