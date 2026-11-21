#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

// =====================================================================
// DIAGNOSTICS MODULE — DTC (fault codes) and Live Data
//
// WARNING: these commands are a BEST GUESS based on standard KWP2000 (ISO 14230)
// services, NOT confirmed from a real CAN log (unlike 0x52
// read and 0x45/0x02/0x57 write, which are 100% verified).
// This ECU uses custom/non-standard commands in many places -
// so it's quite possible that these same DTC/Live Data commands
// will NOT work as-is.
//
// If you get a CAN log from the commercial tool (or another tool) during DTC
// read/clear and Live Data viewing, these commands can be replaced here
// with confirmed, correct ones - exactly the way the Read/Write
// protocols in this project were confirmed.
// =====================================================================

// ---- DTC read (ASSUMPTION: KWP2000 service 0x18 ReadDTCByStatus) ----
void readDTCs() {
  Serial.println("STEP:DTC_READ_START");

  uint8_t resp[8];
  // 18 00 FF 00 - "read all DTCs, any status" (assumed format)
  send8(0x03, 0x18, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00);
  if (!waitResponse(resp, 1000)) {
    Serial.println("ERR:DTC_NO_RESP");
    Serial.println("DTC_READ_DONE:0");
    return;
  }

  // If the response is a multi-frame ISO-TP FF, read it in full
  int count = 0;
  if (resp[0] == 0x10) {
    uint16_t len = ((resp[0] & 0x0F) << 8) | resp[1];
    sendFC();
    uint8_t buf[256];
    int idx = 0;
    for (int i = 2; i < 8 && idx < len; i++) buf[idx++] = resp[i];
    while (idx < len) {
      uint8_t cf[8];
      if (!waitResponse(cf, 500)) break;
      for (int i = 1; i < 8 && idx < len; i++) buf[idx++] = cf[i];
    }
    // DTC entries are usually 3 bytes (2 bytes code + 1 byte status)
    for (int i = 3; i + 2 < idx; i += 3) {  // the first bytes are usually a header
      uint16_t code = (buf[i] << 8) | buf[i + 1];
      uint8_t status = buf[i + 2];
      if (code == 0x0000 || code == 0xFFFF) continue;
      Serial.print("DTC:0x");
      if (code < 0x1000) Serial.print("0");
      Serial.print(code, HEX);
      Serial.print(":0x");
      if (status < 0x10) Serial.print("0");
      Serial.println(status, HEX);
      count++;
    }
  } else if (resp[1] != 0x00 && resp[0] != 0x7F) {
    // Short response, try to interpret it as a 2-3 byte code
    Serial.print("DTC:0x");
    Serial.print(resp[1], HEX);
    Serial.print(resp[2], HEX);
    Serial.println(":0x00");
    count++;
  }

  Serial.print("DTC_READ_DONE:");
  Serial.println(count);
}

// ---- DTC clear (ASSUMPTION: KWP2000 service 0x14 ClearDiagnosticInformation) ----
void clearDTCs() {
  Serial.println("STEP:DTC_CLEAR_START");

  uint8_t resp[8];
  // 14 FF 00 - "clear all DTCs" (assumed format)
  send8(0x03, 0x14, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00);
  if (!waitResponse(resp, 1000)) {
    Serial.println("ERR:DTC_CLEAR_NO_RESP");
    return;
  }
  if (resp[0] == 0x7F) {
    Serial.print("ERR:DTC_CLEAR_REJECTED:0x");
    Serial.println(resp[2], HEX);
    return;
  }
  Serial.println("STATUS:DTC_CLEARED");
}

// ---- Live Data (ASSUMPTION: KWP2000 service 0x21 ReadDataByLocalIdentifier) ----
// Local identifiers (LIDs) are COMPLETELY ASSUMED - the real
// values depend on the ECU software and must be identified from a CAN log.
#define LID_RPM  0x01
#define LID_TEMP 0x02
#define LID_MAP  0x03

bool readLiveValue(uint8_t lid, uint16_t* outValue) {
  uint8_t resp[8];
  send8(0x02, 0x21, lid, 0x00, 0x00, 0x00, 0x00, 0x00);
  if (!waitResponse(resp, 300)) return false;
  if (resp[0] == 0x7F) return false;
  // We assume a 2-byte value is at position [2],[3] in the response
  *outValue = (resp[2] << 8) | resp[3];
  return true;
}

void readLiveData() {
  uint16_t rpm = 0, temp = 0, map_ = 0;
  bool okRpm = readLiveValue(LID_RPM, &rpm);
  bool okTemp = readLiveValue(LID_TEMP, &temp);
  bool okMap = readLiveValue(LID_MAP, &map_);

  Serial.print("LIVE:RPM:");
  Serial.print(okRpm ? String(rpm) : String("ERR"));
  Serial.print(":TEMP:");
  Serial.print(okTemp ? String(temp) : String("ERR"));
  Serial.print(":MAP:");
  Serial.println(okMap ? String(map_) : String("ERR"));
}

#endif
