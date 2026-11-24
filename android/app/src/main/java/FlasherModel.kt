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
        if (!line.startsWith("SDDATA:") && !line.startsWith("SDB:")) addLog(line)

        when {
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
    private fun runOp(block: suspend () -> Unit) {
        if (busy || bleState != BleLink.State.READY) return
        scope.launch {
            if (!opMutex.tryLock()) return@launch
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
            ble.sendLine("STATUS")
            delay(300)
            refreshSd()
        }
    }

    // ==================================================================
    // ECU
    // ==================================================================
    fun connectEcu() = runOp {
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

    fun readFlash() = runOp {
        setStatus("Reading flash...")
        progress = 0f
        send("READ")
        while (true) {
            val l = next(60_000)
            if (l.startsWith("STATUS:READ_DONE:")) {
                // STATUS:READ_DONE:<done>:ERR:<errors>
                val errors = l.substringAfterLast(":").toIntOrNull() ?: 0
                if (errors == 0) setStatus("Read complete - saved to the SD card")
                else setStatus("Read finished with $errors failed blocks (filled with FF)", error = true)
                break
            }
            if (l == "ERR:NOT_CONNECTED") {
                setStatus("Connect to the ECU first", error = true)
                return@runOp
            }
        }
        listSd()
    }

    /** Writes the selected SD file to the ECU. */
    fun writeEcu(fileName: String) = runOp {
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
        var size = 0L
        // Fast Base64 download - every piece carries its file offset, so a
        // piece lost over Bluetooth is detected instead of corrupting the file.
        send("SDGETB:$name")
        while (true) {
            val l = next(15_000)
            when {
                l.startsWith("SDSIZE:") -> size = l.substringAfter("SDSIZE:").toLongOrNull() ?: 0
                l.startsWith("SDB:") -> {
                    val rest = l.substring(4)
                    val offset = rest.substringBefore(":").toLongOrNull()
                    if (offset != out.size().toLong()) {
                        setStatus("Download error: a piece was lost over Bluetooth - press Save again",
                            error = true)
                        drainUntil("SDGET_DONE")
                        return@runOp
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
        if (size > 0 && out.size().toLong() != size) {
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

    /** Skips lines until [last] arrives (or nothing comes for a while). */
    private suspend fun drainUntil(last: String) {
        while (true) {
            val l = nextOrNull(5_000) ?: return
            if (l == last) return
        }
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
    // ESP32 reset
    // ==================================================================
    fun resetEsp() {
        if (busy || bleState != BleLink.State.READY) return
        scope.launch {
            setStatus("Restarting ESP32...")
            ble.sendLine("RESET")
            ecuConnected = false
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
