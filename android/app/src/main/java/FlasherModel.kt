package com.oliver.m3cflasher

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import android.util.Base64
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.io.ByteArrayOutputStream

data class SdFile(val name: String, val size: Long)

/** One live data sample (LIVE: line). null = not received from the ECU yet. */
data class LiveData(
    val rpm: Float? = null,
    val throttle: Float? = null,
    val coolant: Float? = null,
    val faultBits: Int? = null,
    val load: Float? = null,
    val intakeTemp: Float? = null,
    val intakePressure: Float? = null,
    val advance: Float? = null,
    val stft: Float? = null,
    val ltft: Float? = null,
    val baro: Float? = null,
    val runTime: Float? = null,
    val mil: Boolean? = null,
    val dtcCount: Int? = null,
)

/** One fault code, merged from all the read methods that reported it. */
data class DtcEntry(
    val code: String,
    val description: String,
    val blink: String,
    /** "STORED" or "PENDING" (pending = only reported by OBD mode 07). */
    val state: String,
)

/** What kind of operation is running - used for the flash/diagnostics lock. */
enum class OpKind { NONE, FLASH, DIAG, SD }

/**
 * All app state and the logic of each operation. Talks to the ESP32 with
 * the same text protocol as the desktop GUI (see ktm_m3c_flasher.ino,
 * handleSerialLine()).
 *
 * One instance lives for the whole app process (see [FlasherModel.get]),
 * so a running operation is not lost when the screen rotates.
 */
class FlasherModel private constructor(private val appContext: Context) {

    companion object {
        @Volatile private var instance: FlasherModel? = null
        fun get(context: Context): FlasherModel =
            instance ?: synchronized(this) {
                instance ?: FlasherModel(context.applicationContext).also { instance = it }
            }

        const val EXPECTED_BIN_SIZE = 262144L
        private const val MAX_LOG = 300
        private const val RPM_HISTORY = 60

        /** Bits of the 0x6B3 fault byte. 0x90 is the normal value. */
        private const val FAULT_NORMAL_MASK = 0x90
        private const val FAULT_IGNORE_MASK = 0x01
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    val ble = BleLink(appContext)

    // ---------------- UI state ----------------
    var bleState by mutableStateOf(BleLink.State.DISCONNECTED); private set
    var ecuConnected by mutableStateOf(false); private set
    var vin by mutableStateOf(""); private set
    var cal by mutableStateOf(""); private set
    var date by mutableStateOf(""); private set
    var ver by mutableStateOf(""); private set

    /** True while an operation (connect, read, write, upload...) runs. */
    var busy by mutableStateOf(false); private set
    var writing by mutableStateOf(false); private set
    var statusText by mutableStateOf("Not connected"); private set
    var statusIsError by mutableStateOf(false); private set
    /** 0..1, or null when no progress bar should be shown. */
    var progress by mutableStateOf<Float?>(null); private set

    val sdFiles = mutableStateListOf<SdFile>()
    var sdStatus by mutableStateOf(""); private set
    var selectedFile by mutableStateOf<String?>(null)

    val log = mutableStateListOf<String>()

    /** What runs right now (only meaningful while [busy]). */
    var opKind by mutableStateOf(OpKind.NONE); private set
    /** ESP32 firmware version from the PING answer ("" = old firmware). */
    var fwVersion by mutableStateOf(""); private set
    /** Diagnostics needs firmware 2.0 or newer. */
    val diagSupported: Boolean
        get() = (fwVersion.substringBefore('.').toIntOrNull() ?: 0) >= 2

    // ---------------- diagnostics state ----------------
    var liveOn by mutableStateOf(false); private set
    var live by mutableStateOf(LiveData()); private set
    /** True when live data is on but the ECU sends nothing (ignition off). */
    var ecuSilent by mutableStateOf(false); private set
    val rpmHistory = mutableStateListOf<Float>()
    val dtcs = mutableStateListOf<DtcEntry>()
    var dtcStatus by mutableStateOf("Press READ CODES"); private set

    /**
     * SAFETY LOCK (the ESP32 enforces the same rules, this only greys out
     * the buttons): flashing and diagnostics never run at the same time.
     * Diagnostic traffic on the bus during a read/write could corrupt the
     * transfer and brick the ECU.
     */
    val flashLocked: Boolean get() = liveOn || (busy && opKind == OpKind.DIAG)
    val diagLocked: Boolean get() = ecuConnected || (busy && opKind == OpKind.FLASH)

    // ---------------- internals ----------------
    private val opMutex = Mutex()
    /** Lines are copied here while an operation waits for replies. */
    @Volatile private var opLines: Channel<String>? = null

    init {
        // Connection state
        scope.launch {
            ble.state.collect { s ->
                val was = bleState
                bleState = s
                when (s) {
                    BleLink.State.READY -> {
                        liveOn = false
                        setStatus("ESP32 connected")
                        syncAfterConnect()
                    }
                    BleLink.State.DISCONNECTED -> {
                        if (was == BleLink.State.READY) {
                            if (writing) {
                                setStatus(
                                    "Bluetooth lost during WRITE. The ESP32 keeps writing on its own - " +
                                        "do NOT switch off the ignition. Wait 5 minutes, then reconnect.",
                                    error = true
                                )
                            } else if (ble.lastError.value.isNotEmpty()) {
                                setStatus(ble.lastError.value, error = true)
                            } else {
                                setStatus("Disconnected")
                            }
                        } else if (ble.lastError.value.isNotEmpty()) {
                            setStatus(ble.lastError.value, error = true)
                        }
                        ecuConnected = false
                        liveOn = false
                        fwVersion = ""
                        opLines?.close()
                    }
                    BleLink.State.SCANNING -> setStatus("Searching for ESP32...")
                    BleLink.State.CONNECTING -> setStatus("Connecting to ESP32...")
                }
            }
        }
        // Received lines
        scope.launch {
            for (line in ble.lines) handleLine(line)
        }
    }

    // ==================================================================
    // Line handling (every received line passes through here)
    // ==================================================================
    private fun handleLine(line: String) {
        if (line.isEmpty()) return
        if (!line.startsWith("SDDATA:") && !line.startsWith("SDB:") && !line.startsWith("LIVE:")) addLog(line)

        when {
            line.startsWith("LIVE:") -> handleLive(line)
            line.startsWith("VERSION:") -> fwVersion = line.substringAfter("VERSION:").trim()
            line == "STATUS:LIVE_ON" -> liveOn = true
            line == "STATUS:LIVE_OFF" -> { liveOn = false; ecuSilent = false }
            line == "STATUS:ECU_RELEASED" -> {
                ecuConnected = false
                setStatus("Flash session ended - diagnostics can be used")
            }
            line == "ERR:LOCKED:FLASH" -> setStatus(
                "Diagnostics is locked: the ECU is in flash mode. Finish the flash work and end the flash session first.",
                error = true
            )
            line == "ERR:LOCKED:DIAG" -> setStatus(
                "Flasher is locked: diagnostics is running. Stop live data first.",
                error = true
            )
            line.startsWith("INFO:VIN:") -> vin = line.substringAfter("INFO:VIN:")
            line.startsWith("INFO:CAL:") -> cal = line.substringAfter("INFO:CAL:")
            line.startsWith("INFO:DATE:") -> date = line.substringAfter("INFO:DATE:")
            line.startsWith("INFO:VER:") -> ver = line.substringAfter("INFO:VER:")
            line == "STATUS:CONNECTED" -> ecuConnected = true
            line == "STATUS:DISCONNECTED" || line == "STATUS:CONNECT_FAILED" -> ecuConnected = false
            line.startsWith("PROGRESS:") -> {
                val p = line.substringAfter("PROGRESS:").split("/")
                val done = p.getOrNull(0)?.toFloatOrNull()
                val total = p.getOrNull(1)?.toFloatOrNull()
                if (done != null && total != null && total > 0) progress = done / total
            }
            line.startsWith("STEP:") -> stepText(line.substringAfter("STEP:"))?.let { setStatus(it) }
            line.startsWith("STATUS:WRITEFULL_USING_FIXED_FILE:") ->
                setStatus("Checksum was wrong - writing corrected file " +
                        line.substringAfter("STATUS:WRITEFULL_USING_FIXED_FILE:").trimStart('/'))
        }
        opLines?.trySend(line)
    }

    /** Friendly text for the STEP lines worth showing, null for the rest. */
    private fun stepText(step: String): String? = when {
        step == "Session" -> "Opening diagnostic session..."
        step == "VIN" -> "Reading VIN..."
        step.startsWith("WaitSeed") -> "Security access..."
        step == "WriteCalBlocks" -> "Unlocking ECU..."
        step == "PreReadHandshake" || step == "PreReadReauth" -> "Preparing read..."
        step.startsWith("SD_DUMP_START:") -> "Reading flash -> " + step.substringAfter(":").trimStart('/')
        step == "WriteFull_ChecksumVerify" -> "Checking checksum..."
        step == "WriteFull_ChecksumAllOK" -> "Checksum OK"
        step == "WriteFull_ChecksumBad_Recalculating" -> "Checksum wrong - correcting..."
        step == "WriteFull_Phase0_Preamble" -> "Starting write..."
        step == "WriteFull_Phase1_EraseSection1" -> "Erasing section 1/2..."
        step == "WriteFull_Phase2_WriteSection1" -> "Writing section 1/2..."
        step == "WriteFull_Phase3_EraseSection2" -> "Erasing section 2/2..."
        step == "WriteFull_Phase4_WriteSection2" -> "Writing section 2/2..."
        step == "WriteFull_Phase5_End" -> "Finishing..."
        else -> null
    }

    private fun handleLive(line: String) {
        if (line == "LIVE:NONE") {
            ecuSilent = true
            return
        }
        ecuSilent = false
        val kv = line.substringAfter("LIVE:").split(';').associate {
            it.substringBefore('=') to it.substringAfter('=', "?")
        }
        fun f(k: String) = kv[k]?.toFloatOrNull()
        val d = LiveData(
            rpm = f("RPM"), throttle = f("THR"), coolant = f("CLT"),
            faultBits = kv["FLT"]?.toIntOrNull(16),
            load = f("LOAD"), intakeTemp = f("IAT"), intakePressure = f("MAP"),
            advance = f("ADV"), stft = f("STFT"), ltft = f("LTFT"), baro = f("BARO"),
            runTime = f("RUN"), mil = f("MIL")?.let { it >= 1f }, dtcCount = f("DTCN")?.toInt(),
        )
        live = d
        d.rpm?.let {
            rpmHistory.add(it)
            while (rpmHistory.size > RPM_HISTORY) rpmHistory.removeAt(0)
        }
    }

    /** Faults the ECU reports as present right now (live 0x6B3 bits). */
    fun activeFaults(): List<String> {
        val b = live.faultBits ?: return emptyList()
        val extra = b and FAULT_NORMAL_MASK.inv() and FAULT_IGNORE_MASK.inv()
        val out = mutableListOf<String>()
        if ((extra and 0x06) != 0) out.add("Ambient air pressure / temperature sensor")
        for (bit in listOf(3, 5, 6)) if ((extra and (1 shl bit)) != 0) out.add("Unknown fault (bit $bit)")
        return out
    }

    private fun addLog(line: String) {
        log.add(line)
        while (log.size > MAX_LOG) log.removeAt(0)
    }

    private fun setStatus(text: String, error: Boolean = false) {
        statusText = text
        statusIsError = error
    }

    // ==================================================================
    // Operation helpers
    // ==================================================================

    /**
     * Runs one operation at a time. While it runs, every received line is
     * also delivered to the operation through [next].
     */
    private fun runOp(kind: OpKind = OpKind.SD, block: suspend () -> Unit) {
        if (busy || bleState != BleLink.State.READY) return
        // Safety lock, checked here too so a flash op never starts while
        // diagnostics runs and the other way round.
        if (kind == OpKind.FLASH && flashLocked) {
            setStatus("Flasher is locked: diagnostics is running. Stop live data first.", error = true)
            return
        }
        if (kind == OpKind.DIAG && diagLocked) {
            setStatus("Diagnostics is locked: the ECU is in flash mode.", error = true)
            return
        }
        scope.launch {
            if (!opMutex.tryLock()) return@launch
            opKind = kind
            busy = true
            progress = null
            val ch = Channel<String>(Channel.UNLIMITED)
            opLines = ch
            try {
                block()
            } catch (e: Exception) {
                // If Bluetooth dropped during a write, keep the special
                // "ESP32 keeps writing" message set by the state handler.
                if (!(e is LinkLost && writing)) setStatus("Error: ${e.message}", error = true)
            } finally {
                opLines = null
                ch.close()
                busy = false
                opKind = OpKind.NONE
                writing = false
                progress = null
                opMutex.unlock()
            }
        }
    }

    private class Timeout : Exception("no answer from the ESP32")
    private class LinkLost : Exception("Bluetooth connection lost")

    /** Next received line. Throws if nothing arrives within [idleMs]. */
    private suspend fun next(idleMs: Long): String {
        val ch = opLines ?: throw LinkLost()
        val r = withTimeoutOrNull(idleMs) { ch.receiveCatching() } ?: throw Timeout()
        return r.getOrNull() ?: throw LinkLost()
    }

    /** Like [next], but returns null on timeout instead of throwing. */
    private suspend fun nextOrNull(idleMs: Long): String? =
        try { next(idleMs) } catch (_: Timeout) { null }

    private suspend fun send(line: String) {
        if (!ble.sendLine(line)) throw LinkLost()
    }

    // ==================================================================
    // Bluetooth
    // ==================================================================
    fun connectBle() = ble.connect()
    fun disconnectBle() = ble.disconnect()

    /** Right after Bluetooth connects: ask the ECU state and list SD files. */
    private fun syncAfterConnect() {
        scope.launch {
            delay(300)
            ble.sendLine("PING")          // answer includes VERSION:x.y
            delay(300)
            ble.sendLine("STATUS")
            delay(300)
            refreshSd()
        }
    }

    // ==================================================================
    // ECU
    // ==================================================================
    fun connectEcu() = runOp(OpKind.FLASH) {
        vin = ""; cal = ""; date = ""; ver = ""
        setStatus("Connecting to ECU...")
        send("CONNECT")
        var err: String? = null
        while (true) {
            // After an error, only wait briefly for the final CONNECT_FAILED.
            val l = if (err == null) next(90_000) else (nextOrNull(5_000) ?: break)
            if (l == "STATUS:CONNECTED") {
                setStatus("ECU connected")
                return@runOp
            }
            if (l == "STATUS:CONNECT_FAILED") break
            if (l == "ERR:LOCKED:DIAG") return@runOp
            if (l.startsWith("ERR:") && err == null) err = l
        }
        setStatus(connectError(err ?: "ERR:CONNECT_FAILED"), error = true)
    }

    private fun connectError(l: String): String = when (l) {
        "ERR:NO_RESP_SESSION" -> "ECU not answering - ignition on? OBD cable connected?"
        "ERR:NO_RESP_VIN" -> "ECU answered but did not send the VIN"
        "ERR:NO_SEED1", "ERR:NO_SEED2" -> "ECU security access failed - try again"
        "ERR:KEY_REJECTED" -> "ECU rejected the security key"
        else -> "ECU connect failed (${l.substringAfter(":")})"
    }

    /** Reads the ECU flash to the SD card as <name>.bin (empty name = flash_dump). */
    fun readFlash(name: String = "") = runOp(OpKind.FLASH) {
        setStatus("Reading flash...")
        progress = 0f
        var savedAs = ""
        send(if (name.isBlank()) "READ" else "READ:${name.trim()}")
        while (true) {
            val l = next(60_000)
            if (l.startsWith("STEP:SD_DUMP_START:")) savedAs = l.substringAfter("STEP:SD_DUMP_START:").trimStart('/')
            if (l.startsWith("STATUS:READ_DONE:")) {
                // STATUS:READ_DONE:<done>:ERR:<errors>
                val errors = l.substringAfterLast(":").toIntOrNull() ?: 0
                val file = if (savedAs.isNotEmpty()) savedAs else "the SD card"
                if (errors == 0) setStatus("✅ Read complete - saved as $file")
                else setStatus("Read finished with $errors failed blocks (filled with FF) - $file", error = true)
                if (savedAs.isNotEmpty()) selectedFile = savedAs
                break
            }
            if (l == "ERR:NOT_CONNECTED") {
                setStatus("Connect to the ECU first", error = true)
                return@runOp
            }
            if (l == "ERR:LOCKED:DIAG") return@runOp
        }
        listSd()
    }

    /** Writes the selected SD file to the ECU. */
    fun writeEcu(fileName: String) = runOp(OpKind.FLASH) {
        writing = true
        setStatus("Checking file...")
        progress = 0f
        send("WRITEFULL:$fileName")
        var failed = false
        while (true) {
            // After an error, only wait briefly for the final WRITEFULL_FAILED
            // (some early checks stop without sending it).
            val l = if (!failed) next(120_000) else (nextOrNull(5_000) ?: break)
            when {
                l == "STATUS:WRITEFULL_DONE" -> {
                    setStatus("✅ Write complete")
                    progress = 1f
                    break
                }
                (l.startsWith("ERR:WRITEFULL_") || l.startsWith("ERR:WB_")) && !failed -> {
                    setStatus(writeError(l), error = true)
                    failed = true
                }
                l == "ERR:LOCKED:DIAG" -> {
                    writing = false
                    return@runOp
                }
                l == "STATUS:WRITEFULL_FAILED" -> {
                    if (!failed) setStatus("❌ Write failed", error = true)
                    break
                }
            }
        }
        writing = false
        listSd()
    }

    private fun writeError(l: String): String {
        val code = l.removePrefix("ERR:")
        return when {
            code == "WRITEFULL_NOT_CONNECTED" -> "❌ Connect to the ECU first"
            code == "WRITEFULL_SD_NOT_READY" -> "❌ SD card not found"
            code == "WRITEFULL_FILE_NOT_FOUND" -> "❌ File not found on the SD card"
            code.startsWith("WRITEFULL_BAD_FILESIZE:") -> {
                val sz = code.split(":").getOrNull(1) ?: "?"
                "❌ Wrong file size: $sz B (must be $EXPECTED_BIN_SIZE B). Nothing was written."
            }
            code.startsWith("WRITEFULL_CHECKSUM") || code == "WRITEFULL_NO_CHECKSUM_RECORDS" ->
                "❌ Checksum problem ($code). Nothing was written."
            code == "WRITEFULL_NO_PREAMBLE" -> "❌ ECU did not accept the write start. Nothing was written."
            code.startsWith("WRITEFULL_ERASE_FAIL") || code.startsWith("WRITEFULL_LOAD_FAIL") ||
                code.startsWith("WRITEFULL_CONFIRM_FAIL") || code.startsWith("WB_") ->
                "❌ ECU stopped answering during write ($code). Do NOT switch off - " +
                    "check the OBD cable and write the file again."
            else -> "❌ Write error: $code"
        }
    }

    // ==================================================================
    // SD card
    // ==================================================================
    fun refreshSd() = runOp { listSd() }

    private suspend fun listSd() {
        sdStatus = "Loading files..."
        val files = mutableListOf<SdFile>()
        send("SDLIST")
        while (true) {
            val l = next(10_000)
            when {
                l.startsWith("SDFILE:") -> {
                    // SDFILE:<name>:<size> - the name itself may contain ':'
                    val rest = l.substringAfter("SDFILE:")
                    val name = rest.substringBeforeLast(":").trimStart('/')
                    val size = rest.substringAfterLast(":").toLongOrNull() ?: 0
                    files.add(SdFile(name, size))
                }
                l == "SDLIST_DONE" -> break
                l == "ERR:SD_NOT_READY" || l == "ERR:SD_LIST_FAIL" -> {
                    sdFiles.clear()
                    sdStatus = "SD card not found - insert it and press Reset ESP32"
                    return
                }
            }
        }
        sdFiles.clear()
        sdFiles.addAll(files.sortedBy { it.name.lowercase() })
        if (selectedFile != null && sdFiles.none { it.name == selectedFile }) selectedFile = null
        sdStatus = "${files.size} files"
    }

    fun deleteFile(name: String) = runOp {
        send("SDDEL:$name")
        while (true) {
            val l = next(10_000)
            if (l == "STATUS:SD_DELETED") { setStatus("Deleted $name"); break }
            if (l.startsWith("ERR:")) { setStatus("Could not delete $name", error = true); break }
        }
        if (selectedFile == name) selectedFile = null
        listSd()
    }

    /** Copies a file from the SD card to the phone ([target] chosen by the user). */
    fun downloadFile(name: String, target: Uri) = runOp {
        setStatus("Downloading $name...")
        progress = 0f
        val out = ByteArrayOutputStream()
        var size = -1L
        var resumes = 0
        while (true) {
            // Fast Base64 download. Every piece carries its file offset; if a
            // piece is lost over Bluetooth, the app says STOP and asks again
            // from the first missing byte - nothing is ever saved corrupted.
            send("SDGETB:$name:${out.size()}")
            var lost = false
            while (true) {
                val l = next(15_000)
                when {
                    l.startsWith("SDSIZE:") -> size = l.substringAfter("SDSIZE:").toLongOrNull() ?: -1
                    l.startsWith("SDB:") -> {
                        if (lost) continue
                        val rest = l.substring(4)
                        val offset = rest.substringBefore(":").toLongOrNull()
                        if (offset != out.size().toLong()) {
                            lost = true
                            send("STOP")
                            continue
                        }
                        out.write(Base64.decode(rest.substringAfter(":"), Base64.NO_WRAP))
                        if (size > 0) progress = out.size().toFloat() / size
                    }
                    l == "SDGET_DONE" -> break
                    l.startsWith("ERR:") -> {
                        setStatus("Download failed ($l)", error = true)
                        return@runOp
                    }
                }
            }
            if (!lost && (size < 0 || out.size().toLong() >= size)) break
            resumes++
            if (resumes > 20) {
                setStatus("Download failed: Bluetooth connection too unstable - move the phone closer and try again",
                    error = true)
                return@runOp
            }
            setStatus("Downloading $name... (resuming at ${out.size() / 1024} KB)")
            delay(150)
        }
        if (size >= 0 && out.size().toLong() != size) {
            setStatus("Download incomplete: ${out.size()} of $size B - try again", error = true)
            return@runOp
        }
        withContext(Dispatchers.IO) {
            appContext.contentResolver.openOutputStream(target)?.use { it.write(out.toByteArray()) }
        }
        setStatus("✅ Saved $name to the phone (${out.size()} B)")
    }

    /** Copies a file from the phone to the SD card. */
    fun uploadFile(source: Uri) = runOp {
        val name = displayName(source)?.replace(Regex("[\\\\/:*?\"<>|]"), "_") ?: "upload.bin"
        val data = withContext(Dispatchers.IO) {
            appContext.contentResolver.openInputStream(source)?.use { it.readBytes() }
        }
        if (data == null) {
            setStatus("Could not open the file", error = true)
            return@runOp
        }
        setStatus("Uploading $name...")
        progress = 0f
        send("WUPLOAD:$name:${data.size}")
        expectOk("STATUS:UPLOAD_READY")
        var pos = 0
        while (pos < data.size) {
            val end = minOf(pos + 1024, data.size)
            send("WB64:" + Base64.encodeToString(data, pos, end - pos, Base64.NO_WRAP))
            expectOk("STATUS:CHUNK_OK")
            pos = end
            progress = pos.toFloat() / data.size
        }
        send("WUPLOAD_DONE")
        expectOk("STATUS:UPLOAD_DONE")
        val note = if (data.size.toLong() != EXPECTED_BIN_SIZE)
            " - note: size ${data.size} B, a full flash file is $EXPECTED_BIN_SIZE B" else ""
        setStatus("✅ Uploaded $name$note", error = note.isNotEmpty())
        listSd()
        selectedFile = name
    }

    private suspend fun expectOk(ok: String) {
        while (true) {
            val l = next(10_000)
            if (l == ok) return
            if (l.startsWith("ERR:")) throw Exception(l.removePrefix("ERR:"))
        }
    }

    private fun displayName(uri: Uri): String? =
        appContext.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }

    // ==================================================================
    // Diagnostics
    // ==================================================================
    fun startLive() {
        if (bleState != BleLink.State.READY || diagLocked || !diagSupported) return
        rpmHistory.clear()
        scope.launch { ble.sendLine("DIAG_LIVE:1") }
    }

    fun stopLive() {
        if (bleState != BleLink.State.READY) { liveOn = false; return }
        scope.launch { ble.sendLine("DIAG_LIVE:0") }
    }

    /**
     * Ends a finished flash session on the ESP32 so diagnostics can be
     * used. The ESP32 refuses it while a read or write is running.
     */
    fun endFlashSession() {
        if (busy || bleState != BleLink.State.READY) return
        scope.launch { ble.sendLine("ECU_RELEASE") }
    }

    fun readEcuInfo() = runOp(OpKind.DIAG) {
        setStatus("Reading ECU info...")
        send("INFO")
        var noAnswer = false
        while (true) {
            val l = next(15_000)
            if (l == "ERR:LOCKED:FLASH") return@runOp
            if (l == "ERR:ECU_NO_RESPONSE") noAnswer = true
            if (l == "INFO_DONE") break
        }
        if (noAnswer) setStatus("ECU not answering - ignition on? Cable connected?", error = true)
        else setStatus("ECU info read")
    }

    fun readDtcs() = runOp(OpKind.DIAG) { readDtcsInner() }

    private suspend fun readDtcsInner() {
        setStatus("Reading fault codes...")
        dtcStatus = "Reading..."
        send("DTC_READ")
        // code -> set of methods that reported it
        val found = linkedMapOf<String, MutableSet<String>>()
        var anyAnswer = false
        while (true) {
            val l = next(15_000)
            when {
                l == "ERR:LOCKED:FLASH" -> { dtcStatus = "Locked - ECU is in flash mode"; return }
                // DTC:<method>:<raw hex>:<P-code>:<status>
                l.startsWith("DTC:") -> {
                    val p = l.split(':')
                    val method = p.getOrNull(1) ?: ""
                    val code = p.getOrNull(3) ?: continue
                    found.getOrPut(code) { mutableSetOf() }.add(method)
                }
                l.startsWith("DTCCOUNT:") -> if (!l.endsWith(":?")) anyAnswer = true
                l == "DTC_READ_DONE" -> break
            }
        }
        dtcs.clear()
        found.forEach { (code, methods) ->
            val info = DtcCodes.describe(code)
            val pending = methods.all { it == "OBD_07" }
            dtcs.add(DtcEntry(code, info.description, info.blink, if (pending) "PENDING" else "STORED"))
        }
        dtcStatus = when {
            !anyAnswer && dtcs.isEmpty() -> "ECU did not answer - ignition on?"
            dtcs.isEmpty() -> "No fault codes stored"
            else -> "${dtcs.size} fault code" + (if (dtcs.size == 1) "" else "s") + " found"
        }
        setStatus(dtcStatus, error = !anyAnswer && dtcs.isEmpty())
    }

    /** Clears the stored fault codes (OBD-II mode 04), then reads them again. */
    fun clearDtcs() = runOp(OpKind.DIAG) {
        setStatus("Clearing fault codes...")
        send("DTC_CLEAR:OBD")
        while (true) {
            val l = next(15_000)
            if (l == "ERR:LOCKED:FLASH") return@runOp
            if (l.startsWith("CLEAR:OBD:")) {
                val r = l.substringAfter("CLEAR:OBD:")
                if (r != "OK") {
                    setStatus("Clear failed ($r)", error = true)
                    return@runOp
                }
                break
            }
        }
        delay(500)
        readDtcsInner()
        if (dtcs.isEmpty()) setStatus("✅ Fault codes cleared")
        else setStatus("Codes cleared, but ${dtcs.size} came back - the fault is still present", error = true)
    }

    // ==================================================================
    // ESP32 reset
    // ==================================================================
    fun resetEsp() {
        if (busy || bleState != BleLink.State.READY) return
        scope.launch {
            setStatus("Restarting ESP32...")
            ble.sendLine("RESET")
            ecuConnected = false
            liveOn = false
            vin = ""; cal = ""; date = ""; ver = ""
            // The ESP32 drops the Bluetooth link when it restarts - reconnect.
            withTimeoutOrNull(5000) {
                while (bleState != BleLink.State.DISCONNECTED) delay(100)
            }
            ble.disconnect()
            delay(3000)
            ble.connect()
        }
    }
}
