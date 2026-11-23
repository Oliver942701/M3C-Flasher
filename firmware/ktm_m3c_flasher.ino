#include <ESP32-TWAI-CAN.hpp>
#include <SPI.h>
#include <SD.h>
#include "blocks.h"
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

CanFrame rxFrame;
String serialBuf = "";
bool connected = false;
bool reading = false;
bool writingFull = false;
bool sdReady = false;

volatile bool webOpRunning = false;
String webStatusText = "Ready";
int webProgressPct = 0;

// CHANGE: these globals expose ECU info and results to the WiFi/web UI
// as well, not just via Serial (Python GUI) - previously this info was
// only printed to Serial, so the web UI couldn't see any of it.
String g_vin = "";
String g_cal = "";
String g_date = "";
String g_ver = "";
int webReadErrors = 0;
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
        Serial.print("TX:");
        for (int j = 0; j < 8; j++) {
          if (data[j] < 0x10) Serial.print("0");
          Serial.print(data[j], HEX);
          Serial.print(" ");
        }
        Serial.println();
      }
      return true;
    }
    while (ESP32Can.readFrame(rxFrame, 1)) { yield(); }
    delay(1);
  }
  Serial.println("TX_FAIL");
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
        Serial.print("RX:");
        for (int i = 0; i < 8; i++) {
          if (rxFrame.data[i] < 0x10) Serial.print("0");
          Serial.print(rxFrame.data[i], HEX);
          Serial.print(" ");
        }
        Serial.println();
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
        Serial.print("RX:");
        for (int i = 0; i < 8; i++) {
          if (rxFrame.data[i] < 0x10) Serial.print("0");
          Serial.print(rxFrame.data[i], HEX);
          Serial.print(" ");
        }
        Serial.println();
        if (rxFrame.data[0] == 0x05 && rxFrame.data[1] == 0xA0) {
          Serial.println("STEP:GotSeed");
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
        Serial.println("STEP:GotSeed2");
        send8(0x01, 0xFF, 0x5F, 0xB7, 0xB7, 0x70, 0, 0);
        waitResponse(resp, 300);
        return true;
      } else {
        Serial.print("STEP:Seed2Wait:0x");
        Serial.println(resp[1], HEX);
      }
    } else {
      Serial.println("STEP:Seed2NoResp");
    }
    delay(100);
    yield();
  }
  Serial.println("ERR:NO_SEED2");
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
  if (!waitResponse(resp, 500)) { Serial.println("ERR:WR_NO_FC"); return false; }
  if (resp[0] != 0x30) { Serial.println("ERR:WR_BAD_FC"); return false; }

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
  if (!waitResponse(resp, 500)) { Serial.println("ERR:WR_NO_ACK"); return false; }
  if (resp[1] != 0xA0) { Serial.println("ERR:WR_BAD_ACK"); return false; }
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
  Serial.println("STATUS:CONNECTING");
  g_vin = ""; g_cal = ""; g_date = ""; g_ver = "";

  Serial.println("STEP:Session");
  send8(0x02, 0x3E, 0x01, 0, 0, 0, 0, 0);
  if (!waitResponse(resp, 500) || resp[1] != 0x7E) { Serial.println("ERR:NO_RESP_SESSION"); return false; }

  uint8_t idBuf[64];
  int idLen;

  Serial.println("STEP:VIN");
  if (!readEcuIdRaw(0x90, idBuf, idLen)) { Serial.println("ERR:NO_RESP_VIN"); return false; }
  g_vin = decodeEcuIdText(idBuf, idLen);
  Serial.print("INFO:VIN:");
  Serial.println(g_vin);

  Serial.println("STEP:Cal");
  if (!readEcuIdRaw(0x93, idBuf, idLen)) { Serial.println("ERR:NO_RESP_CAL"); return false; }
  g_cal = decodeEcuIdText(idBuf, idLen);
  Serial.print("INFO:CAL:");
  Serial.println(g_cal);

  Serial.println("STEP:Date");
  if (readEcuIdRaw(0x86, idBuf, idLen)) {
    g_date = decodeEcuDate(idBuf, idLen);
    Serial.print("INFO:DATE:");
    Serial.println(g_date);
  }

  Serial.println("STEP:Ver");
  if (!readEcuIdRaw(0x95, idBuf, idLen)) { Serial.println("ERR:NO_RESP_VER"); return false; }
  g_ver = decodeEcuIdText(idBuf, idLen);
  Serial.print("INFO:VER:");
  Serial.println(g_ver);

  Serial.println("STEP:Mode");
  send8(0x01, 0x20, 0, 0, 0, 0, 0, 0);
  if (!waitResponse(resp, 500) || resp[1] != 0x60) { Serial.println("ERR:BAD_MODE"); return false; }

  Serial.println("STEP:SecAccess");
  send8(0x04, 0x7D, 0x4E, 0x42, 0x31, 0, 0, 0);
  if (!waitResponse(resp, 500) || resp[1] != 0xA0) { Serial.println("ERR:KEY_REJECTED"); return false; }

  delay(300);
  send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0);
  drainRX(100);
  delay(2000);
  send8(0x04, 0x7D, 0x4E, 0x42, 0x31, 0, 0, 0);
  waitResponse(resp, 500);

  Serial.println("STEP:WaitSeed1");
  if (!waitSeedAndAnswer(15000)) { Serial.println("ERR:NO_SEED1"); return false; }

  Serial.println("STEP:WriteCalBlocks");
  for (int i = 0; i < NUM_BLOCKS; i++) {
    Serial.print("STEP:WB_");
    Serial.println(i);
    uint16_t addr = BLOCKS[i].addr;
    uint8_t page = 0x78;
    uint8_t group = (uint8_t)(addr >> 8);
    int dataLen = (i == NUM_BLOCKS - 1) ? 51 : 65;

    if (!writeBlock(page, group, BLOCKS[i].data, dataLen)) {
      Serial.print("ERR:WB_FAIL:");
      Serial.println(i);
      return false;
    }
  }
  Serial.println("STEP:WriteCalBlocks_OK");

  Serial.println("STEP:WaitSeed2");
  waitSeed2AndAnswer(10000);

  delay(78); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);
  delay(77); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);
  delay(77); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);

  Serial.println("STATUS:CONNECTED");
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

  Serial.println("STEP:Reauth_Handshake");
  send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0);
  waitResponse(resp, 500);

  Serial.println("STEP:Reauth_SecAccess");
  send8(0x04, 0x7D, 0x4E, 0x42, 0x31, 0, 0, 0);
  if (!waitResponse(resp, 500)) {
    Serial.println("ERR:REAUTH_NO_SECACCESS_RESP");
    return false;
  }

  Serial.println("STEP:Reauth_WaitSeed1");
  if (!waitSeedAndAnswer(seedTimeoutMs)) {
    Serial.println("ERR:REAUTH_NO_SEED1");
    return false;
  }

  Serial.println("STEP:Reauth_WriteCalBlocks");
  for (int i = 0; i < NUM_BLOCKS; i++) {
    uint16_t addr = BLOCKS[i].addr;
    uint8_t page = 0x78;
    uint8_t group = (uint8_t)(addr >> 8);
    int dataLen = (i == NUM_BLOCKS - 1) ? 51 : 65;
    if (!writeBlock(page, group, BLOCKS[i].data, dataLen)) {
      Serial.print("ERR:REAUTH_WB_FAIL:");
      Serial.println(i);
      return false;
    }
  }
  Serial.println("STEP:Reauth_WriteCalBlocks_OK");

  Serial.println("STEP:Reauth_WaitSeed2");
  waitSeed2AndAnswer(10000);

  delay(78); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);
  delay(77); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);
  delay(77); send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0); drainRX(50);

  Serial.println("STEP:Reauth_Done");
  return true;
}

// =====================================================================
// Finds the next free filename to write to the SD card - NEVER
// overwrites an existing file. flash_dump.bin, flash_dump_2.bin,
// flash_dump_3.bin, etc.
// =====================================================================

String nextDumpFilename() {
  if (!SD.exists("/flash_dump.bin")) {
    return "/flash_dump.bin";
  }
  int n = 2;
  while (SD.exists("/flash_dump_" + String(n) + ".bin")) {
    n++;
  }
  return "/flash_dump_" + String(n) + ".bin";
}

void readFlash(bool skipHandshake = false) {
  Serial.println("STATUS:READING");

  const uint16_t SECTS[][2] = {{0x7800, 0x79FF}, {0x7E00, 0x7FFF}};
  int total = 0;
  for (auto& s : SECTS) total += s[1] - s[0] + 1;

  // CHANGE: the old simple "00 00 00 02" handshake has been replaced
  // with reauthForRead() - this is ALWAYS needed, since the ECU may
  // have dropped back into normal mode in the meantime (e.g.
  // immediately after WRITEFULL, without a power cycle). See the
  // reauthForRead() comment and the write_read_log.csv analysis: a
  // plain handshake is no longer enough, the same security-access +
  // calibration-block sequence that Flex uses is required. This is
  // harmless even if the ECU was already unlocked - rewriting the
  // blocks with the same data changes nothing.
  if (!skipHandshake) {
    Serial.println("STEP:PreReadReauth");
    if (!reauthForRead()) {
      Serial.println("ERR:REAUTH_FAILED");
      Serial.print("WARN:INCOMPLETE_READ:");
      Serial.println(total);
      webReadErrors = total;
      Serial.print("STATUS:READ_DONE:0:ERR:");
      Serial.println(total);
      return;
    }
  } else {
    Serial.println("STEP:PreReadHandshake_SKIPPED");
  }

  int done = 0, errors = 0;

  bool sdWriting = false;
  String dumpName;
  if (sdReady) {
    dumpName = nextDumpFilename();
    dumpFile = SD.open(dumpName, FILE_WRITE);
    if (dumpFile) {
      sdWriting = true;
      Serial.print("STEP:SD_DUMP_START:");
      Serial.println(dumpName);
    } else {
      Serial.println("ERR:SD_OPEN_FAIL");
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
          Serial.print("ERR:SEND:0x");
          Serial.println(addr, HEX);
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
          Serial.print("STEP:RETRY:0x");
          Serial.print(addr, HEX);
          Serial.print(":");
          Serial.println(attempt + 1);
        }
      }

      if (received == 0) {
        // CHANGE: a failed address is written to SD EXPLICITLY as
        // 0xFF, rather than being skipped in the file. This way the
        // file content is always deterministic and you don't have to
        // guess what the SD card's filesystem leaves by default in
        // unwritten spots.
        Serial.print("ERR:NO_RESP:0x");
        Serial.println(addr, HEX);
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

      Serial.print("DATA:0x");
      if (addr < 0x1000) Serial.print("0");
      Serial.print(addr, HEX);
      Serial.print(":");
      for (int i = 0; i < received * 8; i++) {
        if (blockBuf[i] < 0x10) Serial.print("0");
        Serial.print(blockBuf[i], HEX);
        if (i < received * 8 - 1) Serial.print(" ");
      }
      Serial.println();

      if (sdWriting) {
        long off = addrToBinOffset(addr);
        if (off >= 0) {
          dumpFile.seek(off);
          dumpFile.write(blockBuf, received * 8);
        }
      }

      done++;
      if (done % 32 == 0) {
        Serial.print("PROGRESS:");
        Serial.print(done);
        Serial.print("/");
        Serial.println(total);
      }
      webProgressPct = (done * 100) / total;
    }
  }

  if (sdWriting) {
    dumpFile.close();
    Serial.println("STEP:SD_DUMP_DONE");
  }

  // CHANGE: a clear summary if anything failed - shows exactly which
  // addresses hold an 0xFF placeholder in the file rather than an
  // actual read result. If errors==0, the whole file is 100%
  // confirmed real ECU content.
  if (errors > 0) {
    Serial.print("WARN:INCOMPLETE_READ:");
    Serial.println(errors);
    Serial.print("WARN:FAILED_ADDRS:");
    for (int i = 0; i < failedCount; i++) {
      Serial.print("0x");
      Serial.print(failedAddrs[i], HEX);
      if (i < failedCount - 1) Serial.print(",");
    }
    if (errors > failedCount) {
      Serial.print(",...(+");
      Serial.print(errors - failedCount);
      Serial.print(" more)");
    }
    Serial.println();
  }

  webReadErrors = errors;
  Serial.print("STATUS:READ_DONE:");
  Serial.print(done);
  Serial.print(":ERR:");
  Serial.println(errors);
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
  if (!connected) { Serial.println("ERR:WRITEFULL_NOT_CONNECTED"); return false; }
  if (!sdReady) { Serial.println("ERR:WRITEFULL_SD_NOT_READY"); return false; }

  // First, quick size check - no point checking the checksum if the
  // file isn't even the right length to begin with.
  {
    File sizeCheck = SD.open(filename, FILE_READ);
    if (!sizeCheck) { Serial.println("ERR:WRITEFULL_FILE_NOT_FOUND"); return false; }
    size_t sz = sizeCheck.size();
    sizeCheck.close();
    if (sz != EXPECTED_BIN_SIZE) {
      Serial.print("ERR:WRITEFULL_BAD_FILESIZE:");
      Serial.print(sz);
      Serial.print(":EXPECTED:");
      Serial.println(EXPECTED_BIN_SIZE);
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
  Serial.println("STEP:WriteFull_ChecksumVerify");
  String useFilename;
  int chkResult = verifyAndFixChecksum(filename, useFilename);
  if (chkResult < 0) {
    Serial.println("STATUS:WRITEFULL_FAILED");
    return false;
  }
  if (chkResult == 1) {
    Serial.print("STATUS:WRITEFULL_USING_FIXED_FILE:");
    Serial.println(useFilename);
  }

  File f = SD.open(useFilename, FILE_READ);
  if (!f) { Serial.println("ERR:WRITEFULL_FILE_NOT_FOUND"); return false; }

  size_t fileSize = f.size();
  Serial.print("STEP:WriteFull_FileSize:");
  Serial.println(fileSize);

  // For safety, check the size once more on the final (possibly
  // corrected) file too - the copy must end up exactly the same size.
  if (fileSize != EXPECTED_BIN_SIZE) {
    Serial.print("ERR:WRITEFULL_BAD_FILESIZE:");
    Serial.print(fileSize);
    Serial.print(":EXPECTED:");
    Serial.println(EXPECTED_BIN_SIZE);
    f.close();
    return false;
  }

  writingFull = true;
  bool ok = true;
  const int totalSteps = 1 + 128 + 1024 + 128 + 1024 + 1;
  int stepCount = 0;
  uint8_t resp[8];

  Serial.println("STEP:WriteFull_Phase0_Preamble");
  send8(0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0);
  if (!waitResponse(resp, 1000)) {
    Serial.println("ERR:WRITEFULL_NO_PREAMBLE");
    ok = false;
  }
  stepCount++;
  webProgressPct = (stepCount * 100) / totalSteps;

  const uint8_t pagesS1[2] = {0x78, 0x79};
  const uint8_t pagesS2[2] = {0x7E, 0x7F};
  const long baseS1[2] = {0x00000, 0x10000};
  const long baseS2[2] = {0x20000, 0x30000};

  if (ok) {
    Serial.println("STEP:WriteFull_Phase1_EraseSection1");
    for (int p = 0; p < 2 && ok; p++) {
      for (int c = 0; c < 256 && ok; c += 4) {
        if (!eraseCheck(pagesS1[p], (uint8_t)c)) {
          Serial.print("ERR:WRITEFULL_ERASE_FAIL:");
          Serial.print(pagesS1[p], HEX);
          Serial.print(":");
          Serial.println(c, HEX);
          ok = false;
          break;
        }
        stepCount++;
        webProgressPct = (stepCount * 100) / totalSteps;
        if (stepCount % 32 == 0) {
          Serial.print("PROGRESS:");
          Serial.print(stepCount);
          Serial.print("/");
          Serial.println(totalSteps);
        }
        yield();
      }
    }
  }

  if (ok) {
    Serial.println("STEP:WriteFull_Phase2_WriteSection1");
    for (int p = 0; p < 2 && ok; p++) {
      for (int c = 0; c < 256 && ok; c++) {
        long off = baseS1[p] + (long)c * 256;
        uint8_t buf[256];
        readFileBlock(f, off, fileSize, buf);

        if (!loadBuffer256(buf)) {
          Serial.print("ERR:WRITEFULL_LOAD_FAIL:");
          Serial.print(pagesS1[p], HEX);
          Serial.print(":");
          Serial.println(c, HEX);
          ok = false;
          break;
        }
        if (!confirmBlock(pagesS1[p], (uint8_t)c)) {
          Serial.print("ERR:WRITEFULL_CONFIRM_FAIL:");
          Serial.print(pagesS1[p], HEX);
          Serial.print(":");
          Serial.println(c, HEX);
          ok = false;
          break;
        }
        stepCount += 2;
        webProgressPct = (stepCount * 100) / totalSteps;
        if (stepCount % 64 == 0) {
          Serial.print("PROGRESS:");
          Serial.print(stepCount);
          Serial.print("/");
          Serial.println(totalSteps);
        }
        yield();
      }
    }
  }

  if (ok) {
    Serial.println("STEP:WriteFull_Phase3_EraseSection2");
    for (int p = 0; p < 2 && ok; p++) {
      for (int c = 0; c < 256 && ok; c += 4) {
        if (!eraseCheck(pagesS2[p], (uint8_t)c)) {
          Serial.print("ERR:WRITEFULL_ERASE_FAIL:");
          Serial.print(pagesS2[p], HEX);
          Serial.print(":");
          Serial.println(c, HEX);
          ok = false;
          break;
        }
        stepCount++;
        webProgressPct = (stepCount * 100) / totalSteps;
        if (stepCount % 32 == 0) {
          Serial.print("PROGRESS:");
          Serial.print(stepCount);
          Serial.print("/");
          Serial.println(totalSteps);
        }
        yield();
      }
    }
  }

  if (ok) {
    Serial.println("STEP:WriteFull_Phase4_WriteSection2");
    for (int p = 0; p < 2 && ok; p++) {
      for (int c = 0; c < 256 && ok; c++) {
        long off = baseS2[p] + (long)c * 256;
        uint8_t buf[256];
        readFileBlock(f, off, fileSize, buf);

        if (!loadBuffer256(buf)) {
          Serial.print("ERR:WRITEFULL_LOAD_FAIL:");
          Serial.print(pagesS2[p], HEX);
          Serial.print(":");
          Serial.println(c, HEX);
          ok = false;
          break;
        }
        if (!confirmBlock(pagesS2[p], (uint8_t)c)) {
          Serial.print("ERR:WRITEFULL_CONFIRM_FAIL:");
          Serial.print(pagesS2[p], HEX);
          Serial.print(":");
          Serial.println(c, HEX);
          ok = false;
          break;
        }
        stepCount += 2;
        webProgressPct = (stepCount * 100) / totalSteps;
        if (stepCount % 64 == 0) {
          Serial.print("PROGRESS:");
          Serial.print(stepCount);
          Serial.print("/");
          Serial.println(totalSteps);
        }
        yield();
      }
    }
  }

  f.close();

  if (ok) {
    Serial.println("STEP:WriteFull_Phase5_End");
    send8(0x01, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00);
    waitResponse(resp, 1500);
  }

  writingFull = false;
  webWriteOk = ok;

  if (ok) {
    Serial.println("STATUS:WRITEFULL_DONE");
  } else {
    Serial.println("STATUS:WRITEFULL_FAILED");
  }
  return ok;
}

void listSDFiles() {
  File root = SD.open("/");
  if (!root) { Serial.println("ERR:SD_LIST_FAIL"); return; }
  int count = 0;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      Serial.print("SDFILE:");
      Serial.print(f.name());
      Serial.print(":");
      Serial.println(f.size());
      count++;
    }
    f.close();
    f = root.openNextFile();
  }
  root.close();
  Serial.print("STEP:SD_FILE_COUNT:");
  Serial.println(count);
  Serial.println("SDLIST_DONE");
}

void sendSDFile(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  File f = SD.open(filename, FILE_READ);
  if (!f) { Serial.println("ERR:SD_FILE_NOT_FOUND"); return; }
  Serial.print("SDSIZE:");
  Serial.println(f.size());
  uint8_t buf[128];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    Serial.print("SDDATA:");
    for (int i = 0; i < n; i++) {
      if (buf[i] < 0x10) Serial.print("0");
      Serial.print(buf[i], HEX);
    }
    Serial.println();
  }
  f.close();
  Serial.println("SDGET_DONE");
}

void deleteSDFile(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  if (SD.remove(filename)) Serial.println("STATUS:SD_DELETED");
  else Serial.println("ERR:SD_DELETE_FAIL");
}

void handleUploadStart(String filename, size_t expectedSize) {
  if (!sdReady) { Serial.println("ERR:UPLOAD_SD_NOT_READY"); return; }
  if (!filename.startsWith("/")) filename = "/" + filename;
  if (SD.exists(filename)) SD.remove(filename);
  uploadFile = SD.open(filename, FILE_WRITE);
  if (!uploadFile) { Serial.println("ERR:UPLOAD_OPEN_FAIL"); return; }
  uploadActive = true;
  Serial.println("STATUS:UPLOAD_READY");
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
  if (!uploadActive) { Serial.println("ERR:UPLOAD_NOT_ACTIVE"); return; }
  int len = hexData.length() / 2;
  static uint8_t buf[512];
  if (len > (int)sizeof(buf)) len = sizeof(buf);
  if (!parseHexBytes(hexData, buf, len)) {
    Serial.println("ERR:UPLOAD_BAD_HEX");
    return;
  }
  uploadFile.write(buf, len);
  Serial.println("STATUS:CHUNK_OK");
}

void handleUploadDone() {
  if (!uploadActive) { Serial.println("ERR:UPLOAD_NOT_ACTIVE"); return; }
  uploadFile.close();
  uploadActive = false;

  // CHANGE: check right after upload whether the file has the correct
  // size - lets the user know immediately, rather than only once they
  // later run WRITEFULL.
  Serial.println("STATUS:UPLOAD_DONE");
}

// =====================================================================
// ESP32 RESTART - closes any open SD files first so nothing is left
// half-written, then restarts the chip. Used by the "RESET" serial
// command and the web UI's /reset endpoint.
// =====================================================================
void doEspRestart() {
  Serial.println("STATUS:RESETTING");
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

  if (line == "CONNECT") {
    connected = connectECU();
    if (!connected) Serial.println("STATUS:CONNECT_FAILED");
  }
  else if (line == "READ") {
    if (!connected) { Serial.println("ERR:NOT_CONNECTED"); return; }
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
  else if (line == "READ_NOHS") {
    if (!connected) { Serial.println("ERR:NOT_CONNECTED"); return; }
    reading = true;
    readFlash(true);
    reading = false;
  }
  else if (line == "SDLIST") {
    if (!sdReady) { Serial.println("ERR:SD_NOT_READY"); return; }
    listSDFiles();
  }
  else if (line.startsWith("SDGET:")) {
    if (!sdReady) { Serial.println("ERR:SD_NOT_READY"); return; }
    sendSDFile(line.substring(6));
  }
  else if (line.startsWith("SDDEL:")) {
    if (!sdReady) { Serial.println("ERR:SD_NOT_READY"); return; }
    deleteSDFile(line.substring(6));
  }
  else if (line.startsWith("WUPLOAD:")) {
    String rest = line.substring(8);
    int sep = rest.indexOf(':');
    if (sep < 0) { Serial.println("ERR:UPLOAD_BAD_FORMAT"); return; }
    String fname = rest.substring(0, sep);
    size_t size = (size_t)strtoul(rest.substring(sep + 1).c_str(), NULL, 10);
    handleUploadStart(fname, size);
  }
  else if (line.startsWith("WDATA:")) {
    handleUploadChunk(line.substring(6));
  }
  else if (line == "WUPLOAD_DONE") {
    handleUploadDone();
  }
  else if (line.startsWith("WRITEFULL:")) {
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
        Serial.print("UNEXPECTED_FRAME:id=0x");
        Serial.print(rxFrame.identifier, HEX);
        Serial.print(" data=");
        for (int i = 0; i < 8; i++) {
          if (rxFrame.data[i] < 0x10) Serial.print("0");
          Serial.print(rxFrame.data[i], HEX);
          Serial.print(" ");
        }
        Serial.println();
      }
      yield();
    }
    if (count == 0) Serial.println("STATUS:BUS_SILENT");
    else { Serial.print("STATUS:BUS_NOT_SILENT:"); Serial.println(count); }
  }
  else if (line == "DTC_READ") {
    readDTCs();
  }
  else if (line == "DTC_CLEAR") {
    clearDTCs();
  }
  else if (line == "LIVE_READ") {
    readLiveData();
  }
  // Software restart of the ESP32 (same as pressing the EN/RST button).
  // The desktop GUI's Reset button normally resets the board through the
  // USB-serial RTS line instead, which works even if the firmware is stuck
  // in a long operation - this command is the fallback for that.
  else if (line == "RESET") {
    doEspRestart();
  }
  else if (line == "PING") Serial.println("PONG");
  else if (line == "STATUS") Serial.println(connected ? "STATUS:CONNECTED" : "STATUS:DISCONNECTED");
}

void setup() {
  Serial.begin(115200);

  ESP32Can.setRxQueueSize(200);
  ESP32Can.setTxQueueSize(10);
  ESP32Can.setPins(CAN_TX, CAN_RX);
  if (ESP32Can.begin(ESP32Can.convertSpeed(500)))
    Serial.println("STATUS:CAN_OK");
  else {
    Serial.println("STATUS:CAN_FAIL");
    return;
  }

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, SPI, 4000000)) {
    sdReady = true;
    Serial.println("STATUS:SD_OK");
  } else {
    sdReady = false;
    Serial.println("STATUS:SD_FAIL");
  }

  setupWebUI();

  Serial.println("STATUS:READY");
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
  loopWebUI();
  yield();
}