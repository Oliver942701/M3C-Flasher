#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

// =====================================================================
// DIAGNOSTICS MODULE - DTC (trouble codes) and Live Data
//
// WARNING: these commands are a BEST GUESS based on standard KWP2000
// (ISO 14230) services, NOT confirmed from a real CAN log (unlike the
// 0x52 read and 0x45/0x37 write commands, which are 100% verified).
// This ECU uses custom/non-standard commands in many places (0x7D
// security, 0x52/0x45 read/write) - so it is very possible that these
// DTC/Live Data commands DO NOT work as-is.
//
// Once a CAN log from Flex (or another tool) is captured while reading/
// clearing DTCs and viewing Live Data, these commands can be replaced
// here with the confirmed correct ones - exactly as was done for the
// 0x52 (read) and 0x45/0x37 (write) commands.
// =====================================================================

// ---- DTC read (GUESS: KWP2000 service 0x18 ReadDTCByStatus) ----
void readDTCs() {
  Out.println("STEP:DTC_READ_START");

  uint8_t resp[8];
  // 18 00 FF 00 - "read all DTCs, any status" (guessed format)
  send8(0x03, 0x18, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00);
  if (!waitResponse(resp, 1000)) {
    Out.println("ERR:DTC_NO_RESP");
    Out.println("DTC_READ_DONE:0");
    return;
  }

  // If the response is an ISO-TP First Frame (multi-frame), read it in full
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
    // DTC records are usually 3 bytes (2 bytes code + 1 byte status)
    for (int i = 3; i + 2 < idx; i += 3) {  // the first bytes are usually a header
      uint16_t code = (buf[i] << 8) | buf[i + 1];
      uint8_t status = buf[i + 2];
      if (code == 0x0000 || code == 0xFFFF) continue;
      Out.print("DTC:0x");
      if (code < 0x1000) Out.print("0");
      Out.print(code, HEX);
      Out.print(":0x");
      if (status < 0x10) Out.print("0");
      Out.println(status, HEX);
      count++;
    }
  } else if (resp[1] != 0x00 && resp[0] != 0x7F) {
    // Short response, try to interpret it as a 2-3 byte code
    Out.print("DTC:0x");
    Out.print(resp[1], HEX);
    Out.print(resp[2], HEX);
    Out.println(":0x00");
    count++;
  }

  Out.print("DTC_READ_DONE:");
  Out.println(count);
}

// ---- DTC clear (GUESS: KWP2000 service 0x14 ClearDiagnosticInformation) ----
void clearDTCs() {
  Out.println("STEP:DTC_CLEAR_START");

  uint8_t resp[8];
  // 14 FF 00 - "clear all DTCs" (guessed format)
  send8(0x03, 0x14, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00);
  if (!waitResponse(resp, 1000)) {
    Out.println("ERR:DTC_CLEAR_NO_RESP");
    return;
  }
  if (resp[0] == 0x7F) {
    Out.print("ERR:DTC_CLEAR_REJECTED:0x");
    Out.println(resp[2], HEX);
    return;
  }
  Out.println("STATUS:DTC_CLEARED");
}

// ---- Live Data (GUESS: KWP2000 service 0x21 ReadDataByLocalIdentifier) ----
// The local identifiers (LIDs) are COMPLETELY GUESSED - the real values
// depend on the ECU software and have to be identified from a CAN log.
#define LID_RPM  0x01
#define LID_TEMP 0x02
#define LID_MAP  0x03

bool readLiveValue(uint8_t lid, uint16_t* outValue) {
  uint8_t resp[8];
  send8(0x02, 0x21, lid, 0x00, 0x00, 0x00, 0x00, 0x00);
  if (!waitResponse(resp, 300)) return false;
  if (resp[0] == 0x7F) return false;
  // Assume the response holds a 2-byte value at positions [2],[3]
  *outValue = (resp[2] << 8) | resp[3];
  return true;
}

void readLiveData() {
  uint16_t rpm = 0, temp = 0, map_ = 0;
  bool okRpm = readLiveValue(LID_RPM, &rpm);
  bool okTemp = readLiveValue(LID_TEMP, &temp);
  bool okMap = readLiveValue(LID_MAP, &map_);

  Out.print("LIVE:RPM:");
  Out.print(okRpm ? String(rpm) : String("ERR"));
  Out.print(":TEMP:");
  Out.print(okTemp ? String(temp) : String("ERR"));
  Out.print(":MAP:");
  Out.println(okMap ? String(map_) : String("ERR"));
}

#endif
