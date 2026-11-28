package com.oliver.m3cflasher

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import java.util.Locale
import kotlin.math.roundToInt

private val Orange = Color(0xFFFF6A13)
private val Green = Color(0xFF4EC9B0)
private val Red = Color(0xFFF44747)
private val Grey = Color(0xFF888888)
private val CardBg = Color(0xFF252526)

class FlasherActivity : ComponentActivity() {

    private lateinit var model: FlasherModel

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        model = FlasherModel.get(this)

        setContent {
            MaterialTheme(
                colorScheme = darkColorScheme(
                    primary = Orange,
                    background = Color(0xFF1E1E1E),
                    surface = Color(0xFF1E1E1E),
                )
            ) {
                Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    // Keep the screen on while something runs (a write takes minutes).
                    LaunchedEffect(model.busy, model.liveOn) {
                        if (model.busy || model.liveOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    }
                    FlasherScreen(model)
                }
            }
        }
    }
}

// ======================================================================
// Permissions
// ======================================================================
private fun blePermissions(): Array<String> =
    if (Build.VERSION.SDK_INT >= 31)
        arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    else
        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

// ======================================================================
// Screens
// ======================================================================
private enum class Screen { START, FLASHER, DIAG }
private enum class DiagTab { LIVE, FAULTS, INFO }

@Composable
fun FlasherScreen(m: FlasherModel) {
    val context = androidx.compose.ui.platform.LocalContext.current

    var screen by rememberSaveable { mutableStateOf(Screen.START) }
    var diagTab by rememberSaveable { mutableStateOf(DiagTab.LIVE) }

    var confirmWrite by remember { mutableStateOf<String?>(null) }
    var confirmDelete by remember { mutableStateOf<String?>(null) }
    var confirmReset by remember { mutableStateOf(false) }
    var confirmClear by remember { mutableStateOf(false) }
    var confirmEndSession by remember { mutableStateOf(false) }
    var showWiring by remember { mutableStateOf(false) }
    var showLog by remember { mutableStateOf(false) }
    var showCodeList by remember { mutableStateOf(false) }
    var askReadName by remember { mutableStateOf(false) }
    var readName by remember { mutableStateOf("") }
    var downloadName by remember { mutableStateOf("") }

    // --- Bluetooth on + permissions, then connect ---
    val enableBt = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (m.ble.isBluetoothOn()) m.connectBle()
    }
    val permLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) {
            if (m.ble.isBluetoothOn()) m.connectBle()
            else enableBt.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        }
    }
    fun startBle() {
        val missing = blePermissions().any {
            ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing) permLauncher.launch(blePermissions())
        else if (!m.ble.isBluetoothOn()) enableBt.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        else m.connectBle()
    }

    // --- File pickers ---
    val pickUpload = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) m.uploadFile(uri)
    }
    val pickDownload = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri ->
        if (uri != null && downloadName.isNotEmpty()) m.downloadFile(downloadName, uri)
    }

    // Try to connect once when the app opens.
    LaunchedEffect(Unit) {
        if (m.bleState == BleLink.State.DISCONNECTED) startBle()
    }

    // Leaving diagnostics always stops live data, so the flasher is free.
    fun goTo(s: Screen) {
        if (s != Screen.DIAG && m.liveOn) m.stopLive()
        screen = s
    }
    BackHandler(enabled = screen != Screen.START) { goTo(Screen.START) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .safeDrawingPadding()
            .verticalScroll(rememberScrollState())
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        when (screen) {
            Screen.START -> StartContent(
                m,
                onBle = { startBle() },
                onFlasher = { goTo(Screen.FLASHER) },
                onDiag = { goTo(Screen.DIAG) },
                onWiring = { showWiring = true },
                onReset = { confirmReset = true },
                onLog = { showLog = !showLog },
                showLog = showLog,
            )
            Screen.FLASHER -> {
                ModeHeader("Flasher", m, onBack = { goTo(Screen.START) })
                StatusCard(m)
                FlasherContent(
                    m,
                    onRead = { readName = ""; askReadName = true },
                    onUpload = { pickUpload.launch(arrayOf("*/*")) },
                    onDownload = { name -> downloadName = name; pickDownload.launch(name) },
                    onDelete = { confirmDelete = it },
                    onWrite = { confirmWrite = it },
                )
            }
            Screen.DIAG -> {
                ModeHeader("Diagnostics", m, onBack = { goTo(Screen.START) })
                StatusCard(m)
                DiagContent(
                    m, diagTab,
                    onTab = { diagTab = it },
                    onClear = { confirmClear = true },
                    onEndSession = { confirmEndSession = true },
                    onCodeList = { showCodeList = true },
                )
            }
        }
        Spacer(Modifier.height(24.dp))
    }

    // ---------------- Dialogs ----------------
    confirmWrite?.let { name ->
        AlertDialog(
            onDismissRequest = { confirmWrite = null },
            title = { Text("Write to ECU?") },
            text = {
                Text(
                    "File: $name\n\n" +
                        "• Battery / power supply must be stable\n" +
                        "• Do not touch the OBD connector or cables\n" +
                        "• Do not switch off the ignition until it says \"Write complete\"\n\n" +
                        "The checksum is checked and corrected automatically before writing."
                )
            },
            confirmButton = {
                TextButton(onClick = { confirmWrite = null; m.writeEcu(name) }) {
                    Text("WRITE", color = Red, fontWeight = FontWeight.Bold)
                }
            },
            dismissButton = { TextButton(onClick = { confirmWrite = null }) { Text("Cancel") } }
        )
    }
    if (askReadName) {
        AlertDialog(
            onDismissRequest = { askReadName = false },
            title = { Text("Read flash") },
            text = {
                Column {
                    Text("File name for the read, e.g. the owner's name. " +
                        "Leave empty for flash_dump. \".bin\" is added automatically.")
                    Spacer(Modifier.height(10.dp))
                    OutlinedTextField(
                        value = readName,
                        onValueChange = { readName = it.take(40) },
                        singleLine = true,
                        placeholder = { Text("flash_dump") },
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = { askReadName = false; m.readFlash(readName) }) {
                    Text("READ", fontWeight = FontWeight.Bold)
                }
            },
            dismissButton = { TextButton(onClick = { askReadName = false }) { Text("Cancel") } }
        )
    }
    confirmDelete?.let { name ->
        AlertDialog(
            onDismissRequest = { confirmDelete = null },
            title = { Text("Delete file?") },
            text = { Text("$name will be deleted from the SD card.") },
            confirmButton = {
                TextButton(onClick = { confirmDelete = null; m.deleteFile(name) }) { Text("Delete", color = Red) }
            },
            dismissButton = { TextButton(onClick = { confirmDelete = null }) { Text("Cancel") } }
        )
    }
    if (confirmReset) {
        AlertDialog(
            onDismissRequest = { confirmReset = false },
            title = { Text("Restart ESP32?") },
            text = { Text("The app reconnects automatically after the restart. Press CONNECT again afterwards.") },
            confirmButton = {
                TextButton(onClick = { confirmReset = false; m.resetEsp() }) { Text("Restart") }
            },
            dismissButton = { TextButton(onClick = { confirmReset = false }) { Text("Cancel") } }
        )
    }
    if (confirmClear) {
        AlertDialog(
            onDismissRequest = { confirmClear = false },
            title = { Text("Clear fault codes?") },
            text = {
                Text("All stored fault codes are deleted from the ECU memory. " +
                    "A fault that is still present comes back right away.")
            },
            confirmButton = {
                TextButton(onClick = { confirmClear = false; m.clearDtcs() }) {
                    Text("CLEAR", color = Red, fontWeight = FontWeight.Bold)
                }
            },
            dismissButton = { TextButton(onClick = { confirmClear = false }) { Text("Cancel") } }
        )
    }
    if (confirmEndSession) {
        AlertDialog(
            onDismissRequest = { confirmEndSession = false },
            title = { Text("End flash session?") },
            text = {
                Text("Only do this when no read or write is running.\n\n" +
                    "After a write: switch the ignition OFF, wait 10 seconds and switch it ON again " +
                    "before using diagnostics.")
            },
            confirmButton = {
                TextButton(onClick = { confirmEndSession = false; m.endFlashSession() }) {
                    Text("END SESSION", fontWeight = FontWeight.Bold)
                }
            },
            dismissButton = { TextButton(onClick = { confirmEndSession = false }) { Text("Cancel") } }
        )
    }
    if (showCodeList) CodeListDialog(onClose = { showCodeList = false })
    if (showWiring) {
        AlertDialog(
            onDismissRequest = { showWiring = false },
            title = { Text("Wiring") },
            text = {
                Box(Modifier.verticalScroll(rememberScrollState())) {
                    Image(
                        painter = painterResource(R.drawable.wiring),
                        contentDescription = "Wiring diagram",
                        contentScale = ContentScale.FillWidth,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            },
            confirmButton = { TextButton(onClick = { showWiring = false }) { Text("Close") } }
        )
    }
}

// ======================================================================
// Start screen
// ======================================================================
@Composable
private fun StartContent(
    m: FlasherModel,
    onBle: () -> Unit,
    onFlasher: () -> Unit,
    onDiag: () -> Unit,
    onWiring: () -> Unit,
    onReset: () -> Unit,
    onLog: () -> Unit,
    showLog: Boolean,
) {
    val bleReady = m.bleState == BleLink.State.READY
    Text("KTM · HUSQVARNA · GASGAS", color = Orange, fontSize = 13.sp, fontWeight = FontWeight.Bold)
    Text("M3C Tool", fontSize = 28.sp, fontWeight = FontWeight.Bold, color = Color.White)

    Section {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Dot(bleColor(m.bleState))
            Spacer(Modifier.width(10.dp))
            Column(Modifier.weight(1f)) {
                Text(bleText(m.bleState), fontWeight = FontWeight.SemiBold)
                if (bleReady) {
                    Text(
                        (if (m.fwVersion.isNotEmpty()) "Firmware ${m.fwVersion}" else "Old firmware - no diagnostics") +
                            " · " + (m.sdStatus.ifEmpty { "SD card ?" }),
                        color = Grey, fontSize = 12.sp
                    )
                }
            }
            if (m.bleState == BleLink.State.DISCONNECTED) {
                Button(onClick = onBle) { Text("Bluetooth") }
            } else if (bleReady && !m.busy) {
                OutlinedButton(onClick = { m.disconnectBle() }) { Text("Disconnect") }
            }
        }
    }
    if (m.statusIsError) StatusCard(m)

    Text("Choose mode", color = Grey, fontSize = 13.sp)
    ModeCard(
        title = "Flasher",
        text = "Read the ECU flash to the SD card, write a tuned file, manage SD card files.",
        note = "Needs: engine off · ignition on · battery charged",
        color = Orange,
        onClick = onFlasher,
    )
    ModeCard(
        title = "Diagnostics",
        text = "Live data with the engine running, read and clear fault codes, ECU info.",
        note = "Safe - only reads (except Clear codes)",
        color = Green,
        onClick = onDiag,
    )
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(onClick = onWiring) { Text("Wiring") }
        OutlinedButton(onClick = onReset, enabled = bleReady && !m.busy) { Text("Reset ESP32") }
        OutlinedButton(onClick = onLog) { Text(if (showLog) "Hide log" else "Log") }
    }
    if (showLog) {
        Section {
            m.log.takeLast(120).forEach {
                Text(it, fontFamily = FontFamily.Monospace, fontSize = 11.sp, color = logColor(it))
            }
        }
    }
}

@Composable
private fun ModeCard(title: String, text: String, note: String, color: Color, onClick: () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = CardBg),
        shape = RoundedCornerShape(16.dp),
        modifier = Modifier
            .fillMaxWidth()
            .border(2.dp, color, RoundedCornerShape(16.dp))
            .clickable(onClick = onClick)
    ) {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, fontSize = 22.sp, fontWeight = FontWeight.Bold, color = Color.White)
            Text(text, color = Color(0xFFBDBDBD), fontSize = 14.sp)
            Text(note, color = color, fontSize = 12.sp, fontWeight = FontWeight.Medium)
        }
    }
}

// ======================================================================
// Shared header + status
// ======================================================================
// The mode is only chosen on the start screen: to switch between Flasher
// and Diagnostics, go back first.
@Composable
private fun ModeHeader(title: String, m: FlasherModel, onBack: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        TextButton(onClick = onBack) { Text("‹ Back", color = Color.White, fontSize = 16.sp) }
        Text(title, fontSize = 20.sp, fontWeight = FontWeight.Bold, color = Color.White, modifier = Modifier.weight(1f))
        Dot(bleColor(m.bleState))
        Spacer(Modifier.width(6.dp))
        Text("BLE", color = bleColor(m.bleState), fontSize = 12.sp)
    }
}

@Composable
private fun StatusCard(m: FlasherModel) {
    Section {
        Text(
            m.statusText,
            color = if (m.statusIsError) Red else Color.White,
            fontWeight = FontWeight.SemiBold
        )
        m.progress?.let { p ->
            Spacer(Modifier.height(8.dp))
            LinearProgressIndicator(
                progress = { p.coerceIn(0f, 1f) },
                modifier = Modifier.fillMaxWidth().height(8.dp),
                color = Orange,
            )
            Text("${(p * 100).toInt().coerceIn(0, 100)} %", color = Grey, fontSize = 13.sp)
        }
    }
}

// ======================================================================
// Flasher mode
// ======================================================================
@Composable
private fun FlasherContent(
    m: FlasherModel,
    onRead: () -> Unit,
    onUpload: () -> Unit,
    onDownload: (String) -> Unit,
    onDelete: (String) -> Unit,
    onWrite: (String?) -> Unit,
) {
    val bleReady = m.bleState == BleLink.State.READY
    val canAct = bleReady && !m.busy
    val canFlash = canAct && !m.flashLocked

    if (m.flashLocked) {
        LockCard(
            color = Green,
            title = "Flasher locked",
            text = "Diagnostics live data is running. Stop it before connecting, reading or writing.",
            button = "STOP LIVE DATA",
            enabled = m.liveOn,
            onClick = { m.stopLive() },
        )
    }

    Section(title = "ECU") {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Dot(if (m.ecuConnected) Green else Grey)
            Spacer(Modifier.width(8.dp))
            Text(if (m.ecuConnected) "Connected (flash mode)" else "Not connected", modifier = Modifier.weight(1f))
            Button(onClick = { m.connectEcu() }, enabled = canFlash) { Text("CONNECT") }
        }
        if (m.vin.isNotEmpty() || m.cal.isNotEmpty()) {
            Spacer(Modifier.height(6.dp))
            InfoRow("VIN", m.vin)
            InfoRow("Calibration", m.cal)
            InfoRow("Date", m.date)
            InfoRow("Version", m.ver)
        }
        Spacer(Modifier.height(6.dp))
        OutlinedButton(
            onClick = onRead,
            enabled = canFlash && m.ecuConnected,
            modifier = Modifier.fillMaxWidth()
        ) { Text("READ FLASH (save to SD card)") }
    }

    Section(title = "SD card") {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { m.refreshSd() }, enabled = canAct) { Text("Refresh") }
            OutlinedButton(onClick = onUpload, enabled = canAct) { Text("Upload from phone") }
        }
        if (m.sdStatus.isNotEmpty()) Text(m.sdStatus, color = Grey, fontSize = 13.sp)
        Spacer(Modifier.height(4.dp))
        m.sdFiles.forEach { f ->
            val sel = f.name == m.selectedFile
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 2.dp)
                    .background(if (sel) Orange.copy(alpha = 0.18f) else Color.Transparent, RoundedCornerShape(6.dp))
                    .border(1.dp, if (sel) Orange else Color(0xFF3C3C3C), RoundedCornerShape(6.dp))
                    .clickable(enabled = !m.busy) { m.selectedFile = if (sel) null else f.name }
                    .padding(10.dp)
            ) {
                Text(f.name, fontWeight = if (sel) FontWeight.Bold else FontWeight.Normal)
                val sizeOk = f.size == FlasherModel.EXPECTED_BIN_SIZE
                Text(
                    "${f.size} B" + if (sizeOk) "  ✓ full flash size" else "",
                    color = if (sizeOk) Green else Grey, fontSize = 12.sp
                )
            }
        }
        m.selectedFile?.let { name ->
            Spacer(Modifier.height(4.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { onDownload(name) }, enabled = canAct) { Text("Save to phone") }
                OutlinedButton(onClick = { onDelete(name) }, enabled = canAct) { Text("Delete", color = Red) }
            }
        }
    }

    Button(
        onClick = { onWrite(m.selectedFile) },
        enabled = canFlash && m.ecuConnected && m.selectedFile != null,
        colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFC62828), contentColor = Color.White),
        modifier = Modifier.fillMaxWidth().height(56.dp)
    ) {
        Text(
            if (m.selectedFile == null) "WRITE TO ECU (select a file)" else "WRITE TO ECU",
            fontWeight = FontWeight.Bold
        )
    }
}

@Composable
private fun LockCard(color: Color, title: String, text: String, button: String?, enabled: Boolean, onClick: () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = Color(0xFF2E2A20)),
        modifier = Modifier.fillMaxWidth().border(1.dp, color, RoundedCornerShape(12.dp)),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("🔒 $title", fontWeight = FontWeight.Bold, color = color)
            Text(text, fontSize = 14.sp)
            if (button != null) {
                OutlinedButton(onClick = onClick, enabled = enabled) { Text(button) }
            }
        }
    }
}

// ======================================================================
// Diagnostics mode
// ======================================================================
@Composable
private fun DiagContent(
    m: FlasherModel,
    tab: DiagTab,
    onTab: (DiagTab) -> Unit,
    onClear: () -> Unit,
    onEndSession: () -> Unit,
    onCodeList: () -> Unit,
) {
    val bleReady = m.bleState == BleLink.State.READY
    if (!bleReady) {
        Section { Text("Connect to the ESP32 first (Bluetooth on the start screen).") }
        return
    }
    if (!m.diagSupported) {
        Section(title = "Firmware update needed") {
            Text("The ESP32 runs an older firmware without diagnostics. " +
                "Upload ktm_m3c_flasher (version 2.0 or newer) with the Arduino IDE.")
        }
        return
    }
    if (m.diagLocked) {
        LockCard(
            color = Orange,
            title = "Diagnostics locked",
            text = if (m.busy) "A flash operation is running. Wait until it has finished."
            else "The ECU is in flash mode (CONNECT was done). Diagnostics is blocked so it can never " +
                "disturb a read or write. When the flash work is finished, end the flash session.",
            button = if (m.busy) null else "END FLASH SESSION",
            enabled = !m.busy,
            onClick = onEndSession,
        )
        return
    }

    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(18.dp)) {
        TabText("Live data", tab == DiagTab.LIVE) { onTab(DiagTab.LIVE) }
        TabText("Fault codes" + if (m.dtcs.isNotEmpty()) " (${m.dtcs.size})" else "", tab == DiagTab.FAULTS) { onTab(DiagTab.FAULTS) }
        TabText("ECU info", tab == DiagTab.INFO) { onTab(DiagTab.INFO) }
    }
    when (tab) {
        DiagTab.LIVE -> LiveTab(m)
        DiagTab.FAULTS -> FaultsTab(m, onClear, onCodeList)
        DiagTab.INFO -> InfoTab(m)
    }
}

@Composable
private fun TabText(text: String, selected: Boolean, onClick: () -> Unit) {
    Column(Modifier.clickable(onClick = onClick).padding(vertical = 8.dp)) {
        Text(text, color = if (selected) Green else Color(0xFFBDBDBD),
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal, fontSize = 15.sp)
        Spacer(Modifier.height(4.dp))
        Box(Modifier.height(2.dp).width(if (selected) 40.dp else 0.dp).background(Green))
    }
}

private fun fmt(v: Float?, decimals: Int = 0): String =
    if (v == null) "–" else if (decimals == 0) v.roundToInt().toString() else String.format(Locale.US, "%.${decimals}f", v)

@Composable
private fun LiveTab(m: FlasherModel) {
    val d = m.live
    val busyOther = m.busy
    Section {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Engine speed", color = Grey, fontSize = 12.sp, modifier = Modifier.weight(1f))
            Text(
                when {
                    !m.liveOn -> "stopped"
                    m.ecuSilent -> "ECU silent - ignition on?"
                    else -> "● live"
                },
                color = if (m.liveOn && !m.ecuSilent) Green else Grey, fontSize = 12.sp
            )
        }
        Row(verticalAlignment = Alignment.Bottom) {
            Text(if (m.liveOn) fmt(d.rpm) else "–", fontSize = 44.sp, fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace, color = Color.White)
            Text(" rpm", color = Grey, modifier = Modifier.padding(bottom = 8.dp))
        }
        RpmGraph(m.rpmHistory.toList())
    }
    val tiles = listOf(
        Triple("Throttle", fmt(d.throttle), "%"),
        Triple("Coolant", fmt(d.coolant), "°C"),
        Triple("Engine load", fmt(d.load), "%"),
        Triple("Ignition advance", fmt(d.advance, 1), "°"),
        Triple("Intake air", fmt(d.intakeTemp), "°C"),
        Triple("Intake pressure", fmt(d.intakePressure), "kPa"),
    )
    tiles.chunked(2).forEach { row ->
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            row.forEach { (label, value, unit) ->
                Card(colors = CardDefaults.cardColors(containerColor = CardBg), modifier = Modifier.weight(1f)) {
                    Column(Modifier.padding(12.dp)) {
                        Text(label, color = Grey, fontSize = 12.sp)
                        Row(verticalAlignment = Alignment.Bottom) {
                            Text(if (m.liveOn) value else "–", fontSize = 24.sp, fontWeight = FontWeight.Bold,
                                fontFamily = FontFamily.Monospace, color = Color.White)
                            Text(" $unit", color = Grey, fontSize = 13.sp, modifier = Modifier.padding(bottom = 3.dp))
                        }
                    }
                }
            }
        }
    }
    Section {
        val mil = d.mil
        Text(
            "Check-engine light (MIL): " + when {
                !m.liveOn || mil == null -> "–"
                mil -> "ON" + (d.dtcCount?.let { " · $it code(s) stored" } ?: "")
                else -> "OFF"
            },
            color = if (m.liveOn && mil == true) Red else Color.White
        )
    }
    Button(
        onClick = { if (m.liveOn) m.stopLive() else m.startLive() },
        enabled = !busyOther,
        colors = ButtonDefaults.buttonColors(containerColor = Green, contentColor = Color(0xFF1E1E1E)),
        modifier = Modifier.fillMaxWidth().height(52.dp)
    ) { Text(if (m.liveOn) "STOP LIVE DATA" else "START LIVE DATA", fontWeight = FontWeight.Bold) }
}

@Composable
private fun RpmGraph(values: List<Float>) {
    Canvas(Modifier.fillMaxWidth().height(48.dp)) {
        if (values.size < 2) return@Canvas
        val maxV = maxOf(values.maxOrNull() ?: 1f, 3000f)
        val stepX = size.width / (values.size - 1)
        val path = Path()
        values.forEachIndexed { i, v ->
            val x = i * stepX
            val y = size.height - (v / maxV) * size.height
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        drawPath(path, Orange, style = Stroke(width = 2.dp.toPx()))
    }
}

@Composable
private fun FaultsTab(m: FlasherModel, onClear: () -> Unit, onCodeList: () -> Unit) {
    val canAct = m.bleState == BleLink.State.READY && !m.busy
    Section {
        Text(m.dtcStatus, fontWeight = FontWeight.SemiBold,
            color = if (m.dtcs.isNotEmpty()) Color(0xFFFF8A80) else Color.White)
        val active = if (m.liveOn) m.activeFaults() else emptyList()
        active.forEach { Text("Active now (live): $it", color = Color(0xFFFF8A80), fontSize = 13.sp) }
    }
    m.dtcs.forEach { e ->
        Section {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(e.code, fontFamily = FontFamily.Monospace, fontSize = 18.sp,
                    fontWeight = FontWeight.Bold, color = Color.White, modifier = Modifier.weight(1f))
                Text(
                    e.state, fontSize = 11.sp, fontWeight = FontWeight.Bold, color = Color(0xFF1E1E1E),
                    modifier = Modifier
                        .background(if (e.state == "PENDING") Color(0xFFFFC46B) else Color(0xFFFF8A80), RoundedCornerShape(10.dp))
                        .padding(horizontal = 8.dp, vertical = 2.dp)
                )
            }
            Spacer(Modifier.height(4.dp))
            Text(e.description, fontSize = 14.sp)
            if (e.blink.isNotEmpty()) Text("Blink code ${e.blink}", color = Grey, fontSize = 12.sp)
        }
    }
    Text("STORED = saved in the ECU memory. PENDING = seen once, not confirmed yet.",
        color = Grey, fontSize = 12.sp)
    OutlinedButton(onClick = onCodeList, modifier = Modifier.fillMaxWidth()) { Text("Code list – search all codes") }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Button(
            onClick = { m.readDtcs() }, enabled = canAct,
            colors = ButtonDefaults.buttonColors(containerColor = Green, contentColor = Color(0xFF1E1E1E)),
            modifier = Modifier.weight(1f).height(52.dp)
        ) { Text("READ CODES", fontWeight = FontWeight.Bold) }
        OutlinedButton(onClick = onClear, enabled = canAct, modifier = Modifier.weight(1f).height(52.dp)) {
            Text("CLEAR CODES", color = Color(0xFFFF8A80), fontWeight = FontWeight.Bold)
        }
    }
}

@Composable
private fun InfoTab(m: FlasherModel) {
    val canAct = m.bleState == BleLink.State.READY && !m.busy
    val d = m.live
    Section(title = "ECU") {
        InfoLine("VIN", m.vin)
        InfoLine("Calibration ID", m.cal)
        InfoLine("Software version", m.ver)
        InfoLine("Programming date", m.date)
        InfoLine("ECU", "Continental M3C")
    }
    Section(title = "Standard OBD-II (from live data)") {
        if (!m.liveOn) Text("Start live data to see these values.", color = Grey, fontSize = 13.sp)
        InfoLine("Short fuel trim", if (m.liveOn) fmt(d.stft, 1) + " %" else "–")
        InfoLine("Long fuel trim", if (m.liveOn) fmt(d.ltft, 1) + " %" else "–")
        InfoLine("Baro pressure", if (m.liveOn) fmt(d.baro) + " kPa" else "–")
        InfoLine("Engine run time", if (m.liveOn && d.runTime != null) {
            val s = d.runTime.roundToInt(); "%d:%02d".format(s / 60, s % 60) + " min"
        } else "–")
    }
    Button(
        onClick = { m.readEcuInfo() }, enabled = canAct,
        colors = ButtonDefaults.buttonColors(containerColor = Green, contentColor = Color(0xFF1E1E1E)),
        modifier = Modifier.fillMaxWidth().height(52.dp)
    ) { Text("READ ECU INFO", fontWeight = FontWeight.Bold) }
}

@Composable
private fun InfoLine(label: String, value: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
        Text(label, color = Grey, fontSize = 13.sp, modifier = Modifier.weight(1f))
        Text(value.ifEmpty { "–" }, fontFamily = FontFamily.Monospace, fontSize = 13.sp, color = Color.White)
    }
}

@Composable
private fun CodeListDialog(onClose: () -> Unit) {
    var query by remember { mutableStateOf("") }
    val q = query.trim().lowercase()
    val list = DtcCodes.all.filter {
        q.isEmpty() || it.first.lowercase().contains(q) || it.second.description.lowercase().contains(q) ||
            it.second.blink.lowercase().contains(q)
    }
    AlertDialog(
        onDismissRequest = onClose,
        title = { Text("Fault code list") },
        text = {
            Column {
                OutlinedTextField(
                    value = query, onValueChange = { query = it.take(30) }, singleLine = true,
                    placeholder = { Text("Search: P0118, coolant, 12...") }, modifier = Modifier.fillMaxWidth()
                )
                Spacer(Modifier.height(8.dp))
                Column(Modifier.height(360.dp).verticalScroll(rememberScrollState())) {
                    list.forEach { (code, info) ->
                        Column(Modifier.padding(vertical = 5.dp)) {
                            Text(code, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold)
                            Text(info.description, fontSize = 13.sp)
                            if (info.blink.isNotEmpty()) Text("Blink ${info.blink}", color = Grey, fontSize = 12.sp)
                        }
                    }
                    if (list.isEmpty()) Text("No match", color = Grey)
                }
            }
        },
        confirmButton = { TextButton(onClick = onClose) { Text("Close") } }
    )
}

private fun bleColor(s: BleLink.State): Color = when (s) {
    BleLink.State.READY -> Green
    BleLink.State.DISCONNECTED -> Grey
    else -> Orange
}

private fun bleText(s: BleLink.State): String = when (s) {
    BleLink.State.READY -> "ESP32 connected"
    BleLink.State.SCANNING -> "Searching for ESP32..."
    BleLink.State.CONNECTING -> "Connecting..."
    BleLink.State.DISCONNECTED -> "ESP32 not connected"
}

private fun logColor(line: String): Color = when {
    line.startsWith("ERR:") -> Red
    line.startsWith("WARN:") -> Orange
    line.startsWith("STATUS:") -> Green
    else -> Color(0xFFCCCCCC)
}

@Composable
private fun Section(title: String? = null, content: @Composable () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = CardBg),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(Modifier.padding(12.dp)) {
            if (title != null) {
                Text(title, fontWeight = FontWeight.Bold, color = Orange)
                Spacer(Modifier.height(6.dp))
            }
            content()
        }
    }
}

@Composable
private fun Dot(color: Color) {
    Box(Modifier.size(12.dp).background(color, CircleShape))
}

@Composable
private fun InfoRow(label: String, value: String) {
    if (value.isEmpty()) return
    Row {
        Text(label, color = Grey, modifier = Modifier.width(100.dp), fontSize = 14.sp)
        Text(value, fontFamily = FontFamily.Monospace, fontSize = 14.sp)
    }
}
