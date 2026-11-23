// =====================================================================
// M3C CRC-16/MODBUS CHECKSUM VERIFICATION AND REPAIR
//
// An exact port of the same algorithm used in m3c_checksum_gui.py (the
// Python checksum tool), confirmed 100% identical against 5 real tests
// to Flex's own checksum correction (same algorithm, same range
// finder, byte-for-byte identical results).
//
// A file can contain several "descriptors" inside it: every time 4
// bytes of FF FF FF FF are found, followed by 2 bytes of stored CRC +
// 2 bytes of range count + N*8 bytes of (start,end) address pairs,
// that's one checksum record. CRC-16/MODBUS (init 0xFFFF, poly 0xA001)
// is computed over all of that record's ranges together (one running
// CRC, not a separate one per range).
//
// writeFullFlash() ALWAYS calls verifyAndFixChecksum() before touching
// the ECU. If any record doesn't match, a new, correctly-calculated
// copy is created on the SD card ("<name>_CHK_OK.bin", replaced on every
// attempt) and THAT ONE IS WRITTEN instead of the original - the original is never written to
// the ECU if its checksum is wrong.
// =====================================================================

#define M3C_PAGE 0x4000UL
#define MAX_CHK_RECORDS 8
#define MAX_RANGES_PER_RECORD 16

static uint16_t g_crc16Table[256];
static bool g_crc16TableReady = false;

void crc16_build_table() {
  for (int n = 0; n < 256; n++) {
    uint16_t c = (uint16_t)n;
    for (int i = 0; i < 8; i++) {
      c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
    }
    g_crc16Table[n] = c;
  }
  g_crc16TableReady = true;
}

struct ChkRecord {
  long offset;
  int rangeCount;
  long rangeStart[MAX_RANGES_PER_RECORD];
  long rangeEnd[MAX_RANGES_PER_RECORD];
};

static bool chk_readU32BE(File& f, long off, uint32_t& out) {
  uint8_t b[4];
  f.seek(off);
  if (f.read(b, 4) != 4) return false;
  out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
  return true;
}

// Port of the Python to_offset(): converts an ECU address (page/window
// format) into a byte offset within the file. Returns -1 if the
// address doesn't match the expected pattern.
static long chk_toOffset(uint32_t addr, long pages) {
  uint32_t page = (addr >> 16) & 0xFF;
  uint32_t win = addr & 0xFFFF;
  if ((addr >> 24) || page < 0xE0 || win < 0x8000 || win > 0xBFFF) return -1;
  if (pages <= 0) return -1;
  return (long)(page % pages) * (long)M3C_PAGE + (long)(win - 0x8000);
}

// Port of the Python parse_descriptor().
static bool chk_parseDescriptor(File& f, long i, long fullSize, ChkRecord& rec) {
  if (i < 4) return false;
  uint8_t marker[4];
  f.seek(i - 4);
  if (f.read(marker, 4) != 4) return false;
  if (!(marker[0] == 0xFF && marker[1] == 0xFF && marker[2] == 0xFF && marker[3] == 0xFF)) return false;

  uint8_t cntB[2];
  f.seek(i + 2);
  if (f.read(cntB, 2) != 2) return false;
  int count = (cntB[0] << 8) | cntB[1];
  if (count < 1 || count > MAX_RANGES_PER_RECORD) return false;
  if (i + 4 + (long)count * 8 > fullSize) return false;

  long pages = fullSize / (long)M3C_PAGE;
  if (pages <= 0) return false;

  rec.offset = i;
  rec.rangeCount = count;
  for (int k = 0; k < count; k++) {
    long p = i + 4 + (long)k * 8;
    uint32_t sa, ea;
    if (!chk_readU32BE(f, p, sa)) return false;
    if (!chk_readU32BE(f, p + 4, ea)) return false;
    long s = chk_toOffset(sa, pages);
    long e = chk_toOffset(ea, pages);
    if (s < 0 || e < s || e >= fullSize) return false;
    if ((sa >> 16) != (ea >> 16)) return false;
    if (s <= i + 1 && e >= i) return false;  // a range must not cover its own descriptor
    rec.rangeStart[k] = s;
    rec.rangeEnd[k] = e;
  }
  return true;
}

// Port of the Python find_descriptors(): scans the file in 0x100-byte
// steps, looking for valid descriptors.
static int chk_findRecords(File& f, long fullSize, ChkRecord* out, int maxOut) {
  int found = 0;
  long i = 4;
  while (i + 12 <= fullSize && found < maxOut) {
    ChkRecord rec;
    if (chk_parseDescriptor(f, i, fullSize, rec)) {
      out[found++] = rec;
    }
    i += 0x100;
    yield();
  }
  return found;
}

// Computes CRC-16/MODBUS running over all of a record's ranges (one
// combined CRC, not a separate one per range - exactly like the
// Python crc16_modbus()).
static uint16_t chk_crc16Ranges(File& f, ChkRecord& rec) {
  if (!g_crc16TableReady) crc16_build_table();
  uint16_t crc = 0xFFFF;
  uint8_t buf[256];
  for (int r = 0; r < rec.rangeCount; r++) {
    long pos = rec.rangeStart[r];
    long remaining = rec.rangeEnd[r] - pos + 1;
    f.seek(pos);
    while (remaining > 0) {
      int chunk = remaining > (long)sizeof(buf) ? (int)sizeof(buf) : (int)remaining;
      int got = f.read(buf, chunk);
      if (got <= 0) break;
      for (int b = 0; b < got; b++) {
        crc = (uint16_t)((crc >> 8) ^ g_crc16Table[(crc ^ buf[b]) & 0xFF]);
      }
      remaining -= got;
      yield();
    }
  }
  return crc;
}

// Temporary file used while building the corrected copy. The corrected
// file only gets its final name after it has passed the self-check, so a
// broken/half-written file is never left on the SD card under a name that
// looks valid.
#define CHK_TMP_NAME "/_chk_tmp.bin"

// Builds the name of the corrected file: "<name>_CHK_OK.bin".
// FIX: the same name is reused on every attempt (the old file is replaced),
// instead of creating _CHK_OK_2, _CHK_OK_3, ... on every try. If the source
// itself is already a "_CHK_OK" file, the suffix is not added twice.
static String chk_fixedFilename(const String& baseName) {
  int dot = baseName.lastIndexOf('.');
  String stem = (dot > 0) ? baseName.substring(0, dot) : baseName;
  String ext = (dot > 0) ? baseName.substring(dot) : ".bin";
  // Strip an existing "_CHK_OK" or "_CHK_OK_<n>" suffix
  int k = stem.lastIndexOf("_CHK_OK");
  if (k > 0) {
    String tail = stem.substring(k + 7);
    bool onlyCounter = true;
    if (tail.length() > 0) {
      if (tail[0] != '_') onlyCounter = false;
      for (unsigned int j = 1; j < tail.length() && onlyCounter; j++) {
        if (!isDigit(tail[j])) onlyCounter = false;
      }
    }
    if (onlyCounter) stem = stem.substring(0, k);
  }
  return stem + "_CHK_OK" + ext;
}

// FIX (the actual bug): the old code copied the file and then re-opened the
// copy with FILE_WRITE to patch the 2 checksum bytes. On ESP32, FILE_WRITE
// is mode "w", which TRUNCATES the file to 0 bytes - so after seek(0x6804)
// and writing 2 bytes the file was only 0x6806 = 26630 bytes long.
//
// Now the copy and the checksum patch happen in ONE pass: the file is read
// in blocks, any checksum bytes that fall inside the current block are
// replaced in RAM, and the block is written out. The destination file is
// opened only once and never re-opened for writing.
static bool chk_copyWithPatches(const char* srcName, const char* dstName,
                                const long* patchOffsets, const uint16_t* patchValues,
                                int patchCount, long expectedSize) {
  File src = SD.open(srcName, FILE_READ);
  if (!src) return false;
  if (SD.exists(dstName)) SD.remove(dstName);
  File dst = SD.open(dstName, FILE_WRITE);
  if (!dst) { src.close(); return false; }

  uint8_t buf[512];
  long pos = 0;
  bool ok = true;
  while (pos < expectedSize) {
    int want = (expectedSize - pos) > (long)sizeof(buf) ? (int)sizeof(buf) : (int)(expectedSize - pos);
    int n = src.read(buf, want);
    if (n <= 0) { ok = false; break; }

    // Patch every checksum byte that lies inside [pos, pos + n)
    for (int k = 0; k < patchCount; k++) {
      uint8_t pb[2] = {(uint8_t)(patchValues[k] >> 8), (uint8_t)(patchValues[k] & 0xFF)};
      for (int b = 0; b < 2; b++) {
        long absPos = patchOffsets[k] + b;
        if (absPos >= pos && absPos < pos + n) buf[absPos - pos] = pb[b];
      }
    }

    if (dst.write(buf, n) != (size_t)n) { ok = false; break; }
    pos += n;
    yield();
  }
  src.close();
  dst.flush();
  dst.close();

  if (!ok || pos != expectedSize) return false;

  // Double-check the size of what actually landed on the card
  File chk = SD.open(dstName, FILE_READ);
  if (!chk) return false;
  long written = chk.size();
  chk.close();
  return written == expectedSize;
}

// =====================================================================
// Main function: verifies the file's checksum(s). If everything is
// fine, outName = srcName and it returns 0. If any was wrong, computes
// the correct values, writes them into a NEW copy on the SD card,
// re-verifies that copy from scratch (never blindly trusts its own
// fix), and returns 1, outName = the new file's name. Returns -1 on
// error (including when the file contains no checksum record at all -
// in that case we refuse to write, since the file doesn't look like a
// real M3C calibration).
// =====================================================================
int verifyAndFixChecksum(const char* srcName, String& outName) {
  File f = SD.open(srcName, FILE_READ);
  if (!f) { Serial.println("ERR:WRITEFULL_CHECKSUM_OPEN_FAIL"); return -1; }
  long fullSize = f.size();

  static ChkRecord recs[MAX_CHK_RECORDS];
  int n = chk_findRecords(f, fullSize, recs, MAX_CHK_RECORDS);
  if (n == 0) {
    f.close();
    Serial.println("ERR:WRITEFULL_NO_CHECKSUM_RECORDS");
    return -1;
  }

  uint16_t stored[MAX_CHK_RECORDS], computed[MAX_CHK_RECORDS];
  bool anyBad = false;
  for (int k = 0; k < n; k++) {
    uint8_t sb[2];
    f.seek(recs[k].offset);
    f.read(sb, 2);
    stored[k] = ((uint16_t)sb[0] << 8) | sb[1];
    computed[k] = chk_crc16Ranges(f, recs[k]);
    Serial.print("STEP:Checksum:0x");
    Serial.print(recs[k].offset, HEX);
    Serial.print(":stored=0x");
    Serial.print(stored[k], HEX);
    Serial.print(":computed=0x");
    Serial.print(computed[k], HEX);
    Serial.println(stored[k] == computed[k] ? ":OK" : ":BAD");
    if (stored[k] != computed[k]) anyBad = true;
    yield();
  }
  f.close();

  if (!anyBad) {
    Serial.println("STEP:WriteFull_ChecksumAllOK");
    outName = String(srcName);
    return 0;
  }

  Serial.println("STEP:WriteFull_ChecksumBad_Recalculating");

  long patchOffsets[MAX_CHK_RECORDS];
  uint16_t patchValues[MAX_CHK_RECORDS];
  int patchCount = 0;
  for (int k = 0; k < n; k++) {
    if (stored[k] != computed[k]) {
      patchOffsets[patchCount] = recs[k].offset;
      patchValues[patchCount] = computed[k];
      patchCount++;
    }
  }

  // Build the corrected copy in a temporary file first
  if (!chk_copyWithPatches(srcName, CHK_TMP_NAME, patchOffsets, patchValues,
                           patchCount, fullSize)) {
    SD.remove(CHK_TMP_NAME);
    Serial.println("ERR:WRITEFULL_CHECKSUM_COPY_FAIL");
    return -1;
  }

  // Self-check: read the corrected file back in from scratch and verify
  // its size and every record again before trusting it.
  File ff = SD.open(CHK_TMP_NAME, FILE_READ);
  if (!ff) {
    SD.remove(CHK_TMP_NAME);
    Serial.println("ERR:WRITEFULL_CHECKSUM_REOPEN_FAIL");
    return -1;
  }
  long fixedSize = ff.size();
  bool allGood = (fixedSize == fullSize);
  static ChkRecord recs2[MAX_CHK_RECORDS];
  int n2 = allGood ? chk_findRecords(ff, fixedSize, recs2, MAX_CHK_RECORDS) : 0;
  if (n2 != n) allGood = false;
  for (int k = 0; k < n2 && allGood; k++) {
    uint8_t sb[2];
    ff.seek(recs2[k].offset);
    ff.read(sb, 2);
    uint16_t st = ((uint16_t)sb[0] << 8) | sb[1];
    uint16_t cp = chk_crc16Ranges(ff, recs2[k]);
    if (st != cp) allGood = false;
  }
  ff.close();

  if (!allGood) {
    // Never leave a broken corrected file on the SD card
    SD.remove(CHK_TMP_NAME);
    Serial.print("ERR:WRITEFULL_CHECKSUM_SELFCHECK_FAIL:SIZE:");
    Serial.println(fixedSize);
    return -1;
  }

  // Passed - give it its final name, replacing any older corrected copy
  String fixedName = chk_fixedFilename(String(srcName));
  if (SD.exists(fixedName)) SD.remove(fixedName);
  if (!SD.rename(CHK_TMP_NAME, fixedName)) {
    SD.remove(CHK_TMP_NAME);
    Serial.println("ERR:WRITEFULL_CHECKSUM_RENAME_FAIL");
    return -1;
  }

  Serial.print("STEP:WriteFull_ChecksumFixed:");
  Serial.println(fixedName);
  outName = fixedName;
  return 1;
}
