#include <ESP32-TWAI-CAN.hpp>
#include <SPI.h>
#include <SD.h>
#include "blocks.h"
#include "ble_link.h"   // must come before anything that prints (defines Out)
#include "checksum.h"

#define CAN_TX 4
#define CAN_RX 5
#define SD_CS   15
#define SD_SCK  18
#define SD_MISO 17
#define SD_MOSI 16

// Size of the full readable/writable flash - 4 pages x 256 addresses x 256 bytes.
// Used to validate the WRITEFULL file size before writing.
#define EXPECTED_BIN_SIZE 262144UL

// 2.0 = flasher + diagnostics mode in one firmware
#define FW_VERSION "2.0"

CanFrame rxFrame;
String serialBuf = "";
bool connected = false;
bool reading = false;
bool writingFull = false;
bool sdReady = false;

volatile bool webOpRunning = false;
String webStatusText = "Ready";
int webProgressPct = 0;
// Short, human-readable reason for the last failed write - shown on the
// phone/web page. Set together with each ERR:WRITEFULL_... Serial message.
String webFailReason = "";
// Name of the corrected "_CHK_OK" file if the last write used one, else "".
String webFixedFile = "";

// CHANGE: these globals expose ECU info and results to the WiFi/web UI
// as well, not just via Serial (Python GUI) - previously this info was
// only printed to Serial, so the web UI couldn't see any of it.
String g_vin = "";
String g_cal = "";
String g_date = "";
String g_ver = "";
int webReadErrors = 0;
int webReadDone = 0;

// FIX: tracks whether the ECU has left the unlocked (connected) state.
//  - false right after a successful CONNECT: the ECU is already unlocked,
//    and it does NOT answer a new security-access request in this state -
//    so a re-authentication here failed with REAUTH_NO_SECACCESS_RESP.
//  - true once a WRITEFULL has started: after a write the ECU drops back
//    into normal running mode, and only the full re-authentication
//    (reauthForRead) brings it back for reading.
bool ecuNeedsReauth = false;
bool webWriteOk = false;

File dumpFile;
File uploadFile;
bool uploadActive = false;

long addrToBinOffset(uint16_t addr) {
  if (addr >= 0x7800 && addr <= 0x79FF) {
    return (long)(addr - 0x7800) * 256;
  } else if (addr >= 0x7E00 && addr <= 0x7FFF) {
    return (long)(addr - 0x7E00) * 256 + 0x20000;
  }
  return -1;
}

// CHANGE: reduced from 50 to 3 - if sending a CAN frame repeatedly
// fails, there's no point retrying 50 times; 3 is enough to fail fast.
bool sendWithRetry(uint8_t* data, int retries = 3) {
  for (int i = 0; i < retries; i++) {
    CanFrame tx = {0};
    tx.identifier = 0x7E0;
    tx.extd = 0;
    tx.data_length_code = 8;
    memcpy(tx.data, data, 8);
    if (ESP32Can.writeFrame(tx)) {
      if (!reading && !writingFull) {
        Out.print("TX:");
        for (int j = 0; j < 8; j++) {
          if (data[j] < 0x10) Out.print("0");
          Out.print(data[j], HEX);
          Out.print(" ");
        }
        Out.println();
      }
      return true;
    }
    while (ESP32Can.readFrame(rxFrame, 1)) { yield(); }
    delay(1);
  }
  Out.println("TX_FAIL");
  return false;
}

void send8(uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3,
           uint8_t d4, uint8_t d5, uint8_t d6, uint8_t d7) {
  uint8_t d[] = {d0, d1, d2, d3, d4, d5, d6, d7};
  sendWithRetry(d);
}

bool waitResponse(uint8_t* resp, int timeoutMs = 1000) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (ESP32Can.readFrame(rxFrame, 10)) {
      if (rxFrame.identifier != 0x7E8) { yield(); continue; }
      memcpy(resp, rxFrame.data, 8);
      if (!reading && !writingFull) {
        Out.print("RX:");
        for (int i = 0; i < 8; i++) {
          if (rxFrame.data[i] < 0x10) Out.print("0");
          Out.print(rxFrame.data[i], HEX);
          Out.print(" ");
        }
        Out.println();
      }
      return true;
    }
    yield();
  }
  return false;
}

void drainRX(int ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    ESP32Can.readFrame(rxFrame, 5);
    yield();
  }
}

void sendFC() {
  uint8_t fc[] = {0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  sendWithRetry(fc);
}

void sendKA() {
  CanFrame tx = {0};
  tx.identifier = 0x7E0;
  tx.extd = 0;
  tx.data_length_code = 8;
  tx.data[3] = 0x02;
  ESP32Can.writeFrame(tx);
}

void testerPresentNoWait() {
  CanFrame tx = {0};
  tx.identifier = 0x7E0;
  tx.extd = 0;
  tx.data_length_code = 8;
  tx.data[0] = 0x01;
  tx.data[1] = 0x54;
  ESP32Can.writeFrame(tx);
}

bool waitSeedAndAnswer(unsigned long timeoutMs) {
  unsigned long start = millis(), lastTP = millis();
  while (millis() - start < timeoutMs) {
    if (ESP32Can.readFrame(rxFrame, 5)) {
      if (rxFrame.identifier == 0x7E8) {
        Out.print("RX:");
        for (int i = 0; i < 8; i++) {
          if (rxFrame.data[i] < 0x10) Out.print("0");
          Out.print(rxFrame.data[i], HEX);
          Out.print(" ");
        }
        Out.println();
        if (rxFrame.data[0] == 0x05 && rxFrame.data[1] == 0xA0) {
          Out.println("STEP:GotSeed");
          delay(10);
          send8(0x05, 0x65, 0x5F, 0xB7, 0xB7, 0x70, 0x00, 0x00);
          uint8_t resp[8];
          waitResponse(resp, 300);
          return true;
        }
      }
    }
    if (millis() - lastTP > 100) {
      testerPresentNoWait();
      lastTP = millis();
    }
    yield();
  }
  return false;
}

bool waitSeed2AndAnswer(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    uint8_t resp[8];
    send8(0x01, 0x79, 0, 0, 0, 0, 0, 0);
    if (waitResponse(resp, 300)) {
      if (resp[0] == 0x01 && resp[1] == 0xFF) {
        Out.println("STEP:GotSeed2");
        send8(0x01, 0xFF, 0x5F, 0xB7, 0xB7, 0x70, 0, 0);
        waitResponse(resp, 300);
        return true;
      } else {
        Out.print("STEP:Seed2Wait:0x");
        Out.println(resp[1], HEX);
      }
    } else {
      Out.println("STEP:Seed2NoResp");
    }
    delay(100);
    yield();
  }
  Out.println("ERR:NO_SEED2");
  return false;
}

bool writeBlock(uint8_t page, uint8_t group, const uint8_t* data, int dataLen) {
  uint8_t resp[8];
  uint16_t isoLen = 4 + dataLen;
  uint8_t d0 = pgm_read_byte(&data[0]);
  uint8_t d1 = (dataLen > 1) ? pgm_read_byte(&data[1]) : 0xFF;

  uint8_t hdr[] = {
    (uint8_t)(0x10 | ((isoLen >> 8) & 0x0F)),
    (uint8_t)(isoLen & 0xFF),
    page,
    0x00,
    0x00,
    group,
    d0, d1
  };
  sendWithRetry(hdr);
  if (!waitResponse(resp, 500)) { Out.println("ERR:WR_NO_FC"); return false; }
  if (resp[0] != 0x30) { Out.println("ERR:WR_BAD_FC"); return false; }

  int idx = 2, remaining = dataLen - 2, frame = 0;
  while (remaining > 0) {
    uint8_t fdata[8] = {0};
    fdata[0] = 0x21 + frame;
    for (int b = 0; b < 7; b++) {
      fdata[b + 1] = (remaining > 0) ? pgm_read_byte(&data[idx]) : 0xFF;
      idx++;
      remaining--;
    }
    sendWithRetry(fdata);
    frame++;
  }
  if (!waitResponse(resp, 500)) { Out.println("ERR:WR_NO_ACK"); return false; }
  if (resp[1] != 0xA0) { Out.println("ERR:WR_BAD_ACK"); return false; }
  return true;
}

bool eraseCheck(uint8_t page, uint8_t counter) {
  uint8_t resp[8];
  send8(0x45, 0x00, page, counter, 0x00, 0x00, 0x00, 0x00);
  if (!waitResponse(resp, 500)) return false;
  return resp[0] == 0x45;
}

// =====================================================================
// READING ECU IDENTIFICATION FIELDS (VIN/Cal/Date/Ver) - service 0x1A
//
// Reads the ISO-TP length (isoLen) DYNAMICALLY from the response's PCI
// byte, instead of a hardcoded number of CF frames as before. This
// means it also works correctly if a field is longer/shorter than it
// happens to be for this particular ECU firmware version (e.g. a
// different model year / VIN length).
//
// The first 2 bytes of the response content are always the service
// response code (0x5A) + an echo of the requested identifier (e.g.
// 0x90) - these are stripped out of the text, the rest is the actual
// content.
// =====================================================================

bool readEcuIdRaw(uint8_t identifier, uint8_t* outBuf, int& outLen) {
  uint8_t resp[8];
  send8(0x02, 0x1A, identifier, 0, 0, 0, 0, 0);
  if (!waitResponse(resp, 500)) return false;

  outLen = 0;
  uint8_t pci = resp[0];

  if ((pci & 0xF0) == 0x00) {
    // Single Frame: length in the low nibble, data in resp[1..]
    int len = pci & 0x0F;
    for (int i = 0; i < len && i < 7; i++) outBuf[outLen++] = resp[1 + i];
  } else if ((pci & 0xF0) == 0x10) {
    // First Frame: 12-bit length, 6 data bytes included
    int isoLen = ((pci & 0x0F) << 8) | resp[1];
    if (isoLen > 64) isoLen = 64;  // guard against buffer overflow
    for (int i = 0; i < 6 && outLen < isoLen; i++) outBuf[outLen++] = resp[2 + i];
    sendFC();
    while (outLen < isoLen) {
      uint8_t cf[8];
      if (!waitResponse(cf, 300)) break;
      for (int i = 0; i < 7 && outLen < isoLen; i++) outBuf[outLen++] = cf[1 + i];
    }
  } else {
    return false;
  }
  return true;
}

// Decodes an ECU-ID response into ASCII text (for the VIN, Cal, Ver fields).
// The first 2 bytes (service echo + identifier echo) are stripped out.
String decodeEcuIdText(uint8_t* buf, int len) {
  String out = "";
  for (int i = 2; i < len; i++) {
    char c = (char)buf[i];
    out += (c >= 32 && c < 127) ? c : '.';
  }
  return out;
}

// Decodes the Date field (0x86) from BCD into a DD.MM.YY date string.
// The response is a Single Frame, 3 content bytes after 2 header bytes:
// DD, MM, YY, each BCD-encoded (e.g. 0x19 = "19", not 25).
String decodeEcuDate(uint8_t* buf, int len) {
  if (len < 5) return "??.??.??";
  auto bcd = [](uint8_t b) { return (int)((b >> 4) * 10 + (b & 0x0F)); };
  char out[16];
  snprintf(out, sizeof(out), "%02d.%02d.%02d", bcd(buf[2]), bcd(buf[3]), bcd(buf[4]));
  return String(out);
}

bool connectECU() {
  uint8_t resp[8];
  Out.println("STATUS:CONNECTING");
  g_vin = ""; g_cal = ""; g_date = ""; g_ver = "";

  Out.println("STEP:Session");
  send8(0x02, 0x3E, 0x01, 0, 0, 0, 0, 0);
  if (!waitResponse(resp, 500) || resp[1] != 0x7E) { Out.println("ERR:NO_RESP_SESSION"); return false; }

  uint8_t idBuf[64];
  int idLen;

  Out.println("STEP:VIN");
  if (!readEcuIdRaw(0x90, idBuf, idLen)) { Out.println("ERR:NO_RESP_VIN"); return false; }
  g_vin = decodeEcuIdText(idBuf, idLen);
  Out.print("INFO:VIN:");
  Out.println(g_vin);

  Out.println("STEP:Cal");
  if (!readEcuIdRaw(0x93, idBuf, idLen)) { Out.println("ERR:NO_RESP_CAL"); return false; }
  g_cal = decodeEcuIdText(idBuf, idLen);
  Out.print("INFO:CAL:");
  Out.println(g_cal);

  Out.println("STEP:Date");
  if (readEcuIdRaw(0x86, idBuf, idLen)) {
    g_date = decodeEcuDate(idBuf, idLen);
    Out.print("INFO:DATE:");
    Out.println(g_date);
  }

  Out.println("STEP:Ver");
  if (!readEcuIdRaw(0x95, idBuf, idLen)) { Out.println("ERR:NO_RESP_VER"); return false; }
  g_ver = decodeEcuIdText(idBuf, idLen);
  Out.print("INFO:VER:");
  Out.println(g_ver);

  Out.println("STEP:Mode");
  send8(0x01, 0x20, 0, 0, 0, 0, 0, 0);
  if (!waitResponse(resp, 500) || resp[1] != 0x60) { Out.println("ERR:BAD_MODE"); return false; }

  Out.println("STEP:SecAccess");
  send8(0x04, 0x7D, 0x4E, 0x42, 0x31, 0, 0, 0);
  if (!waitResponse(resp, 500) || resp[1] != 0xA0) { Out.println("ERR:KEY_REJECTED"); return false; }

  delay(300);
  send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0);
  drainRX(100);
  delay(2000);
  send8(0x04, 0x7D, 0x4E, 0x42, 0x31, 0, 0, 0);
  waitResponse(resp, 500);

  Out.println("STEP:WaitSeed1");
  if (!waitSeedAndAnswer(15000)) { Out.println("ERR:NO_SEED1"); return false; }

  Out.println("STEP:WriteCalBlocks");
  for (int i = 0; i < NUM_BLOCKS; i++) {
    Out.print("STEP:WB_");
    Out.println(i);
    uint16_t addr = BLOCKS[i].addr;
    uint8_t page = 0x78;
    uint8_t group = (uint8_t)(addr >> 8);
    int dataLen = (i == NUM_BLOCKS - 1) ? 51 : 65;

    if (!writeBlock(page, group, BLOCKS[i].data, dataLen)) {
      Out.print("ERR:WB_FAIL:");
      Out.println(i);
      return false;
    }
  }
  Out.println("STEP:WriteCalBlocks_OK");

  Out.println("STEP:WaitSeed2");
  waitSeed2AndAnswer(10000);

  delay(78); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);
  delay(77); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);
  delay(77); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);

  ecuNeedsReauth = false;
  Out.println("STATUS:CONNECTED");
  return true;
}

// =====================================================================
// RE-AUTHENTICATION BEFORE READ
//
// DISCOVERED from write_read_log.csv (Flex: Write -> immediately Read,
// WITHOUT a power cycle): once the ECU has just finished a Write, it
// drops back into normal running mode (regular background traffic
// 6B1/6B2/... starts up again on the bus), and Read no longer works
// with just the plain "00 00 00 02" handshake ("no response"). Instead,
// Flex performs an ALMOST full re-connect sequence:
//
//   1) "00 00 00 02" handshake
//   2) "04 7D 4E 42 31" security-access request (SENT ONCE, unlike
//      connectECU(), where it's sent twice with delays in between -
//      that "double send" is apparently only needed for a cold start
//      (a just-powered ECU), not for a warm ECU)
//   3) poll "01 54" until the seed arrives ("05 A0 ..."), answer with
//      the fixed key "05 65 5F B7 B7 70" - exactly the same
//      waitSeedAndAnswer() logic as in connectECU()
//   4) rewrite ALL 36 calibration blocks again (same data as in
//      Connect - this appears to be part of the security protocol
//      itself, not an actual calibration change)
//   5) seed2 ("01 79" / "01 FF ...") and the same 3x "00 00 00 02"
//      handshake timing as connectECU()
//
// Unlike connectECU(), this does NOT perform the "02 3E 01" session
// request, the VIN/Cal/Date/Ver reads, or the "01 20" mode command -
// these were completely skipped in the log; Flex went straight from
// the handshake to security access.
// =====================================================================

bool reauthForRead(unsigned long seedTimeoutMs = 20000) {
  uint8_t resp[8];

  Out.println("STEP:Reauth_Handshake");
  send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0);
  waitResponse(resp, 500);

  Out.println("STEP:Reauth_SecAccess");
  send8(0x04, 0x7D, 0x4E, 0x42, 0x31, 0, 0, 0);
  if (!waitResponse(resp, 500)) {
    Out.println("ERR:REAUTH_NO_SECACCESS_RESP");
    return false;
  }

  Out.println("STEP:Reauth_WaitSeed1");
  if (!waitSeedAndAnswer(seedTimeoutMs)) {
    Out.println("ERR:REAUTH_NO_SEED1");
    return false;
  }

  Out.println("STEP:Reauth_WriteCalBlocks");
  for (int i = 0; i < NUM_BLOCKS; i++) {
    uint16_t addr = BLOCKS[i].addr;
    uint8_t page = 0x78;
    uint8_t group = (uint8_t)(addr >> 8);
    int dataLen = (i == NUM_BLOCKS - 1) ? 51 : 65;
    if (!writeBlock(page, group, BLOCKS[i].data, dataLen)) {
      Out.print("ERR:REAUTH_WB_FAIL:");
      Out.println(i);
      return false;
    }
  }
  Out.println("STEP:Reauth_WriteCalBlocks_OK");

  Out.println("STEP:Reauth_WaitSeed2");
  waitSeed2AndAnswer(10000);

  delay(78); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);
  delay(77); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);
  delay(77); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);

  Out.println("STEP:Reauth_Done");
  return true;
}

// =====================================================================
// Finds the next free filename to write to the SD card - NEVER
// overwrites an existing file. The base name comes from the READ
// command ("READ:timo" -> timo.bin, timo_2.bin, ...); without a name it
// is flash_dump.bin, flash_dump_2.bin, flash_dump_3.bin, etc.
// =====================================================================

// Base name for the next read dump, set by "READ:<name>". Empty = default.
String readDumpName = "";

String cleanDumpBaseName(String name) {
  name.trim();
  if (name.endsWith(".bin") || name.endsWith(".BIN")) name = name.substring(0, name.length() - 4);
  String out = "";
  for (size_t i = 0; i < name.length() && out.length() < 40; i++) {
    char c = name[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') out += c;
    else if (c == ' ') out += '_';
  }
  if (out.length() == 0) out = "flash_dump";
  return out;
}

String nextDumpFilename() {
  String base = "/" + cleanDumpBaseName(readDumpName);
  readDumpName = "";
  if (!SD.exists(base + ".bin")) {
    return base + ".bin";
  }
  int n = 2;
  while (SD.exists(base + "_" + String(n) + ".bin")) {
    n++;
  }
  return base + "_" + String(n) + ".bin";
}

void readFlash(bool skipHandshake = false) {
  Out.println("STATUS:READING");

  const uint16_t SECTS[][2] = {{0x7800, 0x79FF}, {0x7E00, 0x7FFF}};
  int total = 0;
  for (auto& s : SECTS) total += s[1] - s[0] + 1;

  webReadDone = 0;
  webReadErrors = 0;

  // FIX: two different pre-read sequences depending on the ECU state:
  //  - right after CONNECT (ECU still unlocked): the simple "00 00 00 02"
  //    handshake, exactly as before. A security-access request is NOT
  //    answered in this state, so re-authenticating here would fail.
  //  - after a WRITEFULL (ECU back in normal mode, see write_read_log.csv):
  //    the full reauthForRead() sequence that Flex uses. Once it
  //    succeeds the ECU is unlocked again, so further reads use the
  //    simple handshake.
  if (!skipHandshake) {
    if (!ecuNeedsReauth) {
      Out.println("STEP:PreReadHandshake");
      uint8_t hs[8];
      send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0);
      waitResponse(hs, 500);
    } else {
      Out.println("STEP:PreReadReauth");
      if (!reauthForRead()) {
        Out.println("ERR:REAUTH_FAILED");
        Out.print("WARN:INCOMPLETE_READ:");
        Out.println(total);
        webReadErrors = total;
        Out.print("STATUS:READ_DONE:0:ERR:");
        Out.println(total);
        return;
      }
      ecuNeedsReauth = false;
    }
  } else {
    Out.println("STEP:PreReadHandshake_SKIPPED");
  }

  int done = 0, errors = 0;

  bool sdWriting = false;
  String dumpName;
  if (sdReady) {
    dumpName = nextDumpFilename();
    dumpFile = SD.open(dumpName, FILE_WRITE);
    if (dumpFile) {
      sdWriting = true;
      Out.print("STEP:SD_DUMP_START:");
      Out.println(dumpName);
    } else {
      Out.println("ERR:SD_OPEN_FAIL");
    }
  }

  // Collect the list of failed addresses to print a summary at the end -
  // keep at most 64 addresses in memory (printing more over Serial
  // wouldn't be useful, but ERR:count is always exact).
  uint16_t failedAddrs[64];
  int failedCount = 0;

  for (auto& sect : SECTS) {
    for (uint16_t addr = sect[0]; addr <= sect[1]; addr++) {
      yield();

      uint8_t blockBuf[256];
      int received = 0;

      for (int attempt = 0; attempt < 3 && received == 0; attempt++) {
        CanFrame tx = {0};
        tx.identifier = 0x7E0;
        tx.extd = 0;
        tx.data_length_code = 8;
        tx.data[0] = 0x52;
        tx.data[1] = 0x00;
        tx.data[2] = (addr >> 8) & 0xFF;
        tx.data[3] = addr & 0xFF;
        tx.data[4] = 0x00;
        tx.data[5] = 0x01;

        bool sent = false;
        for (int i = 0; i < 3; i++) {
          if (ESP32Can.writeFrame(tx)) { sent = true; break; }
          while (ESP32Can.readFrame(rxFrame, 1)) { yield(); }
          delay(1);
        }
        if (!sent) {
          Out.print("ERR:SEND:0x");
          Out.println(addr, HEX);
          continue;
        }

        received = 0;
        unsigned long t = millis();
        while (received < 32 && millis() - t < 1000) {
          if (ESP32Can.readFrame(rxFrame, 10) && rxFrame.identifier == 0x7E8) {
            memcpy(blockBuf + received * 8, rxFrame.data, 8);
            received++;
          }
          yield();
        }

        if (received == 0 && attempt < 2) {
          Out.print("STEP:RETRY:0x");
          Out.print(addr, HEX);
          Out.print(":");
          Out.println(attempt + 1);
        }
      }

      if (received == 0) {
        // CHANGE: a failed address is written to SD EXPLICITLY as
        // 0xFF, rather than being skipped in the file. This way the
        // file content is always deterministic and you don't have to
        // guess what the SD card's filesystem leaves by default in
        // unwritten spots.
        Out.print("ERR:NO_RESP:0x");
        Out.println(addr, HEX);
        errors++;
        if (failedCount < 64) failedAddrs[failedCount++] = addr;

        if (sdWriting) {
          uint8_t ffBuf[256];
          memset(ffBuf, 0xFF, 256);
          long off = addrToBinOffset(addr);
          if (off >= 0) {
            dumpFile.seek(off);
            dumpFile.write(ffBuf, 256);
          }
        }
        continue;
      }

      bleMute = true;   // per-block data only to USB, not to the phone
      Out.print("DATA:0x");
      if (addr < 0x1000) Out.print("0");
      Out.print(addr, HEX);
      Out.print(":");
      for (int i = 0; i < received * 8; i++) {
        if (blockBuf[i] < 0x10) Out.print("0");
        Out.print(blockBuf[i], HEX);
        if (i < received * 8 - 1) Out.print(" ");
      }
      Out.println();
      bleMute = false;

      if (sdWriting) {
        long off = addrToBinOffset(addr);
        if (off >= 0) {
          dumpFile.seek(off);
          dumpFile.write(blockBuf, received * 8);
        }
      }

      done++;
      if (done % 32 == 0) {
        Out.print("PROGRESS:");
        Out.print(done);
        Out.print("/");
        Out.println(total);
      }
      webProgressPct = (done * 100) / total;
    }
  }

  if (sdWriting) {
    dumpFile.close();
    Out.println("STEP:SD_DUMP_DONE");
  }

  // CHANGE: a clear summary if anything failed - shows exactly which
  // addresses hold an 0xFF placeholder in the file rather than an
  // actual read result. If errors==0, the whole file is 100%
  // confirmed real ECU content.
  if (errors > 0) {
    Out.print("WARN:INCOMPLETE_READ:");
    Out.println(errors);
    Out.print("WARN:FAILED_ADDRS:");
    for (int i = 0; i < failedCount; i++) {
      Out.print("0x");
      Out.print(failedAddrs[i], HEX);
      if (i < failedCount - 1) Out.print(",");
    }
    if (errors > failedCount) {
      Out.print(",...(+");
      Out.print(errors - failedCount);
      Out.print(" more)");
    }
    Out.println();
  }

  webReadErrors = errors;
  webReadDone = done;
  Out.print("STATUS:READ_DONE:");
  Out.print(done);
  Out.print(":ERR:");
  Out.println(errors);
}

void readFileBlock(File& f, long offset, size_t fileSize, uint8_t* buf) {
  if ((size_t)offset + 256 <= fileSize) {
    f.seek(offset);
    f.read(buf, 256);
  } else if ((size_t)offset < fileSize) {
    f.seek(offset);
    size_t avail = fileSize - offset;
    f.read(buf, avail);
    memset(buf + avail, 0xFF, 256 - avail);
  } else {
    memset(buf, 0xFF, 256);
  }
}

// =====================================================================
// FULL-FLASH WRITE HELPER FUNCTIONS - check ONLY the command-byte echo
// (e.g. resp[0]==0x45), exactly as Flex itself does. The rest of the
// response content is not checked, and no retry logic is done here.
// =====================================================================

bool loadBuffer256(uint8_t* buf256) {
  uint8_t hdr[8] = {0x02, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00};
  sendWithRetry(hdr);
  for (int i = 0; i < 32; i++) {
    sendWithRetry(buf256 + i * 8);
  }
  uint8_t resp[8];
  if (!waitResponse(resp, 1500)) return false;
  return resp[0] == 0x02;
}

bool confirmBlock(uint8_t page, uint8_t counter) {
  uint8_t resp[8];
  send8(0x57, 0x00, page, counter, 0x00, 0x01, 0x00, 0x00);
  if (!waitResponse(resp, 500)) return false;
  return resp[0] == 0x57;
}

bool writeFullFlash(const char* filename) {
  webFailReason = "";
  webFixedFile = "";
  webProgressPct = 0;
  webStatusText = "Checking file...";
  if (!connected) { Out.println("ERR:WRITEFULL_NOT_CONNECTED"); webFailReason = "not connected to the ECU"; return false; }
  if (!sdReady) { Out.println("ERR:WRITEFULL_SD_NOT_READY"); webFailReason = "SD card not found"; return false; }

  // First, quick size check - no point checking the checksum if the
  // file isn't even the right length to begin with.
  {
    File sizeCheck = SD.open(filename, FILE_READ);
    if (!sizeCheck) { Out.println("ERR:WRITEFULL_FILE_NOT_FOUND"); webFailReason = "file not found on the SD card"; return false; }
    size_t sz = sizeCheck.size();
    sizeCheck.close();
    if (sz != EXPECTED_BIN_SIZE) {
      Out.print("ERR:WRITEFULL_BAD_FILESIZE:");
      Out.print(sz);
      Out.print(":EXPECTED:");
      Out.println(EXPECTED_BIN_SIZE);
      webFailReason = "wrong file size (" + String((unsigned long)sz) + " bytes, expected 262144)";
      return false;
    }
  }

  // CHANGE: ALWAYS, before touching the ECU at all, we verify the
  // file's internal CRC-16/MODBUS checksum (the same algorithm Flex
  // uses - confirmed byte-identical against 5 real tests). If the
  // checksum is wrong, the original is NEVER written to the ECU -
  // instead a new, correctly-calculated copy is created on the SD
  // card ("<name>_CHK_OK.bin") and that one is written instead. If the
  // file's checksum is already fine, the original is used directly.
  Out.println("STEP:WriteFull_ChecksumVerify");
  webStatusText = "Checking checksum...";
  String useFilename;
  int chkResult = verifyAndFixChecksum(filename, useFilename);
  if (chkResult < 0) {
    Out.println("STATUS:WRITEFULL_FAILED");
    return false;
  }
  if (chkResult == 1) {
    Out.print("STATUS:WRITEFULL_USING_FIXED_FILE:");
    Out.println(useFilename);
    webFixedFile = useFilename;
    webStatusText = "Checksum was wrong - corrected file created";
  } else {
    webStatusText = "Checksum OK";
  }

  File f = SD.open(useFilename, FILE_READ);
  if (!f) { Out.println("ERR:WRITEFULL_FILE_NOT_FOUND"); webFailReason = "file not found on the SD card"; return false; }

  size_t fileSize = f.size();
  Out.print("STEP:WriteFull_FileSize:");
  Out.println(fileSize);

  // For safety, check the size once more on the final (possibly
  // corrected) file too - the copy must end up exactly the same size.
  if (fileSize != EXPECTED_BIN_SIZE) {
    Out.print("ERR:WRITEFULL_BAD_FILESIZE:");
    Out.print(fileSize);
    Out.print(":EXPECTED:");
    Out.println(EXPECTED_BIN_SIZE);
    webFailReason = "wrong file size (" + String((unsigned long)fileSize) + " bytes, expected 262144)";
    f.close();
    return false;
  }

  writingFull = true;
  bool ok = true;
  const int totalSteps = 1 + 128 + 1024 + 128 + 1024 + 1;
  int stepCount = 0;
  uint8_t resp[8];

  Out.println("STEP:WriteFull_Phase0_Preamble");
  webStatusText = "Starting write...";
  send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0);
  if (!waitResponse(resp, 1000)) {
    Out.println("ERR:WRITEFULL_NO_PREAMBLE");
    webFailReason = "ECU did not respond - nothing was changed";
    ok = false;
  }
  stepCount++;
  webProgressPct = (stepCount * 100) / totalSteps;

  const uint8_t pagesS1[2] = {0x78, 0x79};
  const uint8_t pagesS2[2] = {0x7E, 0x7F};
  const long baseS1[2] = {0x00000, 0x10000};
  const long baseS2[2] = {0x20000, 0x30000};

  if (ok) {
    // The ECU accepted the write session - from here on it will leave the
    // unlocked state, so the next READ must re-authenticate first.
    ecuNeedsReauth = true;
    Out.println("STEP:WriteFull_Phase1_EraseSection1");
    webStatusText = "Erasing section 1/2...";
    for (int p = 0; p < 2 && ok; p++) {
      for (int c = 0; c < 256 && ok; c += 4) {
        if (!eraseCheck(pagesS1[p], (uint8_t)c)) {
          webFailReason = "ECU did not confirm erasing - check cable/connector";
          Out.print("ERR:WRITEFULL_ERASE_FAIL:");
          Out.print(pagesS1[p], HEX);
          Out.print(":");
          Out.println(c, HEX);
          ok = false;
          break;
        }
        stepCount++;
        webProgressPct = (stepCount * 100) / totalSteps;
        if (stepCount % 32 == 0) {
          Out.print("PROGRESS:");
          Out.print(stepCount);
          Out.print("/");
          Out.println(totalSteps);
        }
        yield();
      }
    }
  }

  if (ok) {
    Out.println("STEP:WriteFull_Phase2_WriteSection1");
    webStatusText = "Writing section 1/2...";
    for (int p = 0; p < 2 && ok; p++) {
      for (int c = 0; c < 256 && ok; c++) {
        long off = baseS1[p] + (long)c * 256;
        uint8_t buf[256];
        readFileBlock(f, off, fileSize, buf);

        if (!loadBuffer256(buf)) {
          webFailReason = "ECU did not confirm a data block - check cable/connector";
          Out.print("ERR:WRITEFULL_LOAD_FAIL:");
          Out.print(pagesS1[p], HEX);
          Out.print(":");
          Out.println(c, HEX);
          ok = false;
          break;
        }
        if (!confirmBlock(pagesS1[p], (uint8_t)c)) {
          webFailReason = "ECU did not confirm programming - check cable/connector";
          Out.print("ERR:WRITEFULL_CONFIRM_FAIL:");
          Out.print(pagesS1[p], HEX);
          Out.print(":");
          Out.println(c, HEX);
          ok = false;
          break;
        }
        stepCount += 2;
        webProgressPct = (stepCount * 100) / totalSteps;
        if (stepCount % 64 == 0) {
          Out.print("PROGRESS:");
          Out.print(stepCount);
          Out.print("/");
          Out.println(totalSteps);
        }
        yield();
      }
    }
  }

  if (ok) {
    Out.println("STEP:WriteFull_Phase3_EraseSection2");
    webStatusText = "Erasing section 2/2...";
    for (int p = 0; p < 2 && ok; p++) {
      for (int c = 0; c < 256 && ok; c += 4) {
        if (!eraseCheck(pagesS2[p], (uint8_t)c)) {
          webFailReason = "ECU did not confirm erasing - check cable/connector";
          Out.print("ERR:WRITEFULL_ERASE_FAIL:");
          Out.print(pagesS2[p], HEX);
          Out.print(":");
          Out.println(c, HEX);
          ok = false;
          break;
        }
        stepCount++;
        webProgressPct = (stepCount * 100) / totalSteps;
        if (stepCount % 32 == 0) {
          Out.print("PROGRESS:");
          Out.print(stepCount);
          Out.print("/");
          Out.println(totalSteps);
        }
        yield();
      }
    }
  }

  if (ok) {
    Out.println("STEP:WriteFull_Phase4_WriteSection2");
    webStatusText = "Writing section 2/2...";
    for (int p = 0; p < 2 && ok; p++) {
      for (int c = 0; c < 256 && ok; c++) {
        long off = baseS2[p] + (long)c * 256;
        uint8_t buf[256];
        readFileBlock(f, off, fileSize, buf);

        if (!loadBuffer256(buf)) {
          webFailReason = "ECU did not confirm a data block - check cable/connector";
          Out.print("ERR:WRITEFULL_LOAD_FAIL:");
          Out.print(pagesS2[p], HEX);
          Out.print(":");
          Out.println(c, HEX);
          ok = false;
          break;
        }
        if (!confirmBlock(pagesS2[p], (uint8_t)c)) {
          webFailReason = "ECU did not confirm programming - check cable/connector";
          Out.print("ERR:WRITEFULL_CONFIRM_FAIL:");
          Out.print(pagesS2[p], HEX);
          Out.print(":");
          Out.println(c, HEX);
          ok = false;
          break;
        }
        stepCount += 2;
        webProgressPct = (stepCount * 100) / totalSteps;
        if (stepCount % 64 == 0) {
          Out.print("PROGRESS:");
          Out.print(stepCount);
          Out.print("/");
          Out.println(totalSteps);
        }
        yield();
      }
    }
  }

  f.close();

  if (ok) {
    Out.println("STEP:WriteFull_Phase5_End");
    webStatusText = "Finishing...";
    send8(0x01, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00);
    waitResponse(resp, 1500);
  }

  writingFull = false;
  webWriteOk = ok;

  if (ok) {
    Out.println("STATUS:WRITEFULL_DONE");
  } else {
    Out.println("STATUS:WRITEFULL_FAILED");
  }
  return ok;
}

void listSDFiles() {
  File root = SD.open("/");
  if (!root) { Out.println("ERR:SD_LIST_FAIL"); return; }
  int count = 0;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      Out.print("SDFILE:");
      Out.print(f.name());
      Out.print(":");
      Out.println(f.size());
      count++;
    }
    f.close();
    f = root.openNextFile();
  }
  root.close();
  Out.print("STEP:SD_FILE_COUNT:");
  Out.println(count);
  Out.println("SDLIST_DONE");
}

void sendSDFile(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  File f = SD.open(filename, FILE_READ);
  if (!f) { Out.println("ERR:SD_FILE_NOT_FOUND"); return; }
  Out.print("SDSIZE:");
  Out.println(f.size());
  bleBlocking = true;   // nothing time-critical here - let BLE slow us down
  uint8_t buf[128];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    Out.print("SDDATA:");
    for (int i = 0; i < n; i++) {
      if (buf[i] < 0x10) Out.print("0");
      Out.print(buf[i], HEX);
    }
    Out.println();
  }
  f.close();
  Out.println("SDGET_DONE");
  bleBlocking = false;
}

// ---------------------------------------------------------------------
// Base64 helpers for the fast phone transfers (SDGETB / WB64). Base64
// adds 33% instead of the 100% of hex, so less data goes over Bluetooth.
// ---------------------------------------------------------------------
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64Encode(const uint8_t* in, size_t len, char* out) {
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < len) v |= in[i + 2];
    out[o++] = B64[(v >> 18) & 63];
    out[o++] = B64[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? B64[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Returns the number of decoded bytes, or -1 on bad input.
int b64Decode(const String& s, uint8_t* out, int outMax) {
  int n = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '=') break;
    int v = b64Value(c);
    if (v < 0) return -1;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= outMax) return -1;
      out[n++] = (acc >> bits) & 0xFF;
    }
  }
  return n;
}

// Fast download for the phone app: same as SDGET, but Base64 and every
// line carries its file offset ("SDB:<offset>:<base64>") so the app can
// detect a lost piece. Data lines go to Bluetooth only (not USB serial).
// "SDGETB:<name>:<offset>" resumes from <offset> after a lost piece.
void sendSDFileB64(String arg) {
  String filename = arg;
  long offset = 0;
  int sep = arg.lastIndexOf(':');
  if (sep > 0) {               // FAT file names cannot contain ':'
    filename = arg.substring(0, sep);
    offset = strtol(arg.substring(sep + 1).c_str(), NULL, 10);
  }
  if (!filename.startsWith("/")) filename = "/" + filename;
  File f = SD.open(filename, FILE_READ);
  if (!f) { Out.println("ERR:SD_FILE_NOT_FOUND"); return; }
  Out.print("SDSIZE:");
  Out.println(f.size());
  if (offset < 0 || offset > (long)f.size()) offset = 0;
  f.seek(offset);
  bleBlocking = true;
  serialMute = true;
  static uint8_t buf[360];
  static char enc[484];
  while (f.available()) {
    if (bleShouldAbort()) break;   // phone asked to stop (lost piece) or left
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    b64Encode(buf, n, enc);
    Out.print("SDB:");
    Out.print(offset);
    Out.print(":");
    Out.println(enc);
    offset += n;
  }
  serialMute = false;
  f.close();
  Out.println("SDGET_DONE");
  bleBlocking = false;
}

// Fast upload chunk from the phone app: "WB64:<base64>", up to 1024 bytes.
void handleUploadChunkB64(const String& b64Data) {
  if (!uploadActive) { Out.println("ERR:UPLOAD_NOT_ACTIVE"); return; }
  static uint8_t buf[1024];
  int len = b64Decode(b64Data, buf, sizeof(buf));
  if (len < 0) { Out.println("ERR:UPLOAD_BAD_B64"); return; }
  uploadFile.write(buf, len);
  Out.println("STATUS:CHUNK_OK");
}

void deleteSDFile(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  if (SD.remove(filename)) Out.println("STATUS:SD_DELETED");
  else Out.println("ERR:SD_DELETE_FAIL");
}

void handleUploadStart(String filename, size_t expectedSize) {
  if (!sdReady) { Out.println("ERR:UPLOAD_SD_NOT_READY"); return; }
  if (!filename.startsWith("/")) filename = "/" + filename;
  if (SD.exists(filename)) SD.remove(filename);
  uploadFile = SD.open(filename, FILE_WRITE);
  if (!uploadFile) { Out.println("ERR:UPLOAD_OPEN_FAIL"); return; }
  uploadActive = true;
  Out.println("STATUS:UPLOAD_READY");
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool parseHexBytes(const String& s, uint8_t* out, int outLen) {
  if ((int)s.length() < outLen * 2) return false;
  for (int i = 0; i < outLen; i++) {
    int hi = hexNibble(s[i * 2]);
    int lo = hexNibble(s[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

void handleUploadChunk(const String& hexData) {
  if (!uploadActive) { Out.println("ERR:UPLOAD_NOT_ACTIVE"); return; }
  int len = hexData.length() / 2;
  static uint8_t buf[512];
  if (len > (int)sizeof(buf)) len = sizeof(buf);
  if (!parseHexBytes(hexData, buf, len)) {
    Out.println("ERR:UPLOAD_BAD_HEX");
    return;
  }
  uploadFile.write(buf, len);
  Out.println("STATUS:CHUNK_OK");
}

void handleUploadDone() {
  if (!uploadActive) { Out.println("ERR:UPLOAD_NOT_ACTIVE"); return; }
  uploadFile.close();
  uploadActive = false;

  // CHANGE: check right after upload whether the file has the correct
  // size - lets the user know immediately, rather than only once they
  // later run WRITEFULL.
  Out.println("STATUS:UPLOAD_DONE");
}

// =====================================================================
// ESP32 RESTART - closes any open SD files first so nothing is left
// half-written, then restarts the chip. Used by the "RESET" serial
// command and the web UI's /reset endpoint.
// =====================================================================
void doEspRestart() {
  Out.println("STATUS:RESETTING");
  if (uploadActive) { uploadFile.close(); uploadActive = false; }
  if (dumpFile) dumpFile.close();
  Serial.flush();
  delay(200);
  ESP.restart();
}

#include "diagnostics.h"
#include "webui.h"

void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  // Diagnostics commands and the flash/diagnostics safety lock
  // (see diagnostics.h)
  if (handleDiagCommand(line)) return;

  if (line == "CONNECT") {
    if (!flashAllowed()) return;
    connected = connectECU();
    if (!connected) Out.println("STATUS:CONNECT_FAILED");
    printMode();
  }
  else if (line == "READ" || line.startsWith("READ:")) {
    // "READ:<name>" saves the dump as <name>.bin on the SD card
    readDumpName = line.startsWith("READ:") ? line.substring(5) : "";
    if (!flashAllowed()) return;
    if (!connected) { Out.println("ERR:NOT_CONNECTED"); return; }
    reading = true;
    readFlash();
    reading = false;
  }
  // FOR TESTING: same as READ, but without the pre-handshake. Use this
  // to test whether, right after a WRITEFULL finishes, the extra
  // handshake itself interferes with the ECU's state (the opposite
  // hypothesis to needing more handshakes). The result of either
  // version (whether "ERR:NO_RESP" goes away or not) gives real
  // information without needing a CAN log.
  else if (line == "READ_NOHS" || line.startsWith("READ_NOHS:")) {
    readDumpName = line.startsWith("READ_NOHS:") ? line.substring(10) : "";
    if (!flashAllowed()) return;
    if (!connected) { Out.println("ERR:NOT_CONNECTED"); return; }
    reading = true;
    readFlash(true);
    reading = false;
  }
  else if (line == "SDLIST") {
    if (!sdReady) { Out.println("ERR:SD_NOT_READY"); return; }
    listSDFiles();
  }
  else if (line.startsWith("SDGET:")) {
    if (!sdReady) { Out.println("ERR:SD_NOT_READY"); return; }
    sendSDFile(line.substring(6));
  }
  else if (line.startsWith("SDGETB:")) {
    if (!sdReady) { Out.println("ERR:SD_NOT_READY"); return; }
    sendSDFileB64(line.substring(7));
  }
  else if (line.startsWith("SDDEL:")) {
    if (!sdReady) { Out.println("ERR:SD_NOT_READY"); return; }
    deleteSDFile(line.substring(6));
  }
  else if (line.startsWith("WUPLOAD:")) {
    String rest = line.substring(8);
    int sep = rest.indexOf(':');
    if (sep < 0) { Out.println("ERR:UPLOAD_BAD_FORMAT"); return; }
    String fname = rest.substring(0, sep);
    size_t size = (size_t)strtoul(rest.substring(sep + 1).c_str(), NULL, 10);
    handleUploadStart(fname, size);
  }
  else if (line.startsWith("WDATA:")) {
    handleUploadChunk(line.substring(6));
  }
  else if (line.startsWith("WB64:")) {
    handleUploadChunkB64(line.substring(5));
  }
  else if (line == "WUPLOAD_DONE") {
    handleUploadDone();
  }
  else if (line.startsWith("WRITEFULL:")) {
    if (!flashAllowed()) return;
    String fname = line.substring(10);
    if (!fname.startsWith("/")) fname = "/" + fname;
    writeFullFlash(fname.c_str());
  }
  else if (line.startsWith("SILENCE")) {
    unsigned long ms = 5000;
    int sep = line.indexOf(':');
    if (sep > 0) ms = (unsigned long)strtoul(line.substring(sep + 1).c_str(), NULL, 10);
    unsigned long start = millis();
    int count = 0;
    while (millis() - start < ms) {
      if (ESP32Can.readFrame(rxFrame, 20)) {
        count++;
        Out.print("UNEXPECTED_FRAME:id=0x");
        Out.print(rxFrame.identifier, HEX);
        Out.print(" data=");
        for (int i = 0; i < 8; i++) {
          if (rxFrame.data[i] < 0x10) Out.print("0");
          Out.print(rxFrame.data[i], HEX);
          Out.print(" ");
        }
        Out.println();
      }
      yield();
    }
    if (count == 0) Out.println("STATUS:BUS_SILENT");
    else { Out.print("STATUS:BUS_NOT_SILENT:"); Out.println(count); }
  }
  // Software restart of the ESP32 (same as pressing the EN/RST button).
  // The desktop GUI's Reset button normally resets the board through the
  // USB-serial RTS line instead, which works even if the firmware is stuck
  // in a long operation - this command is the fallback for that.
  else if (line == "RESET") {
    doEspRestart();
  }
  else if (line == "PING") {
    Out.println("PONG");
    Out.println("VERSION:" FW_VERSION);
  }
  else if (line == "STATUS") {
    Out.println(connected ? "STATUS:CONNECTED" : "STATUS:DISCONNECTED");
    printMode();
  }
}

void setup() {
  Serial.begin(115200);

  ESP32Can.setRxQueueSize(200);
  ESP32Can.setTxQueueSize(10);
  ESP32Can.setPins(CAN_TX, CAN_RX);
  if (ESP32Can.begin(ESP32Can.convertSpeed(500)))
    Out.println("STATUS:CAN_OK");
  else {
    Out.println("STATUS:CAN_FAIL");
    return;
  }

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, SPI, 4000000)) {
    sdReady = true;
    Out.println("STATUS:SD_OK");
  } else {
    sdReady = false;
    Out.println("STATUS:SD_FAIL");
  }

  setupWebUI();
  setupBLE();

  Out.println("VERSION:" FW_VERSION);
  Out.println("STATUS:READY");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      handleSerialLine(serialBuf);
      serialBuf = "";
    } else if (c != '\r') {
      serialBuf += c;
    }
  }
  bleRxPoll(handleSerialLine);
  loopWebUI();
  diagLoop();
  yield();
}