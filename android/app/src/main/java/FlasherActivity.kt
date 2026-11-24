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
                    LaunchedEffect(model.busy) {
                        if (model.busy) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
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
// Screen
// ======================================================================
@Composable
fun FlasherScreen(m: FlasherModel) {
    val context = androidx.compose.ui.platform.LocalContext.current

    var confirmWrite by remember { mutableStateOf<String?>(null) }
    var confirmDelete by remember { mutableStateOf<String?>(null) }
    var confirmReset by remember { mutableStateOf(false) }
    var showWiring by remember { mutableStateOf(false) }
    var showLog by remember { mutableStateOf(false) }
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

    val bleReady = m.bleState == BleLink.State.READY
    val canAct = bleReady && !m.busy

    Column(
        modifier = Modifier
            .fillMaxSize()
            .safeDrawingPadding()
            .verticalScroll(rememberScrollState())
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Text(
            "KTM Husqvarna GasGas M3C Flasher",
            fontSize = 18.sp, fontWeight = FontWeight.Bold, color = Orange
        )

        // ---------------- Bluetooth ----------------
        Section {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Dot(
                    when (m.bleState) {
                        BleLink.State.READY -> Green
                        BleLink.State.DISCONNECTED -> Grey
                        else -> Orange
                    }
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    when (m.bleState) {
                        BleLink.State.READY -> "ESP32 connected"
                        BleLink.State.SCANNING -> "Searching for ESP32..."
                        BleLink.State.CONNECTING -> "Connecting..."
                        BleLink.State.DISCONNECTED -> "ESP32 not connected"
                    },
                    modifier = Modifier.weight(1f)
                )
                if (m.bleState == BleLink.State.DISCONNECTED) {
                    Button(onClick = { startBle() }) { Text("Bluetooth") }
                } else if (bleReady && !m.busy) {
                    OutlinedButton(onClick = { m.disconnectBle() }) { Text("Disconnect") }
                }
            }
        }

        // ---------------- Status ----------------
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

        // ---------------- ECU ----------------
        Section(title = "ECU") {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Dot(if (m.ecuConnected) Green else Grey)
                Spacer(Modifier.width(8.dp))
                Text(if (m.ecuConnected) "Connected" else "Not connected", modifier = Modifier.weight(1f))
                Button(onClick = { m.connectEcu() }, enabled = canAct) { Text("CONNECT") }
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
                onClick = { m.readFlash() },
                enabled = canAct && m.ecuConnected,
                modifier = Modifier.fillMaxWidth()
            ) { Text("READ FLASH (save to SD card)") }
        }

        // ---------------- SD card ----------------
        Section(title = "SD card") {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { m.refreshSd() }, enabled = canAct) { Text("Refresh") }
                OutlinedButton(onClick = { pickUpload.launch(arrayOf("*/*")) }, enabled = canAct) {
                    Text("Upload from phone")
                }
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
                    OutlinedButton(
                        onClick = { downloadName = name; pickDownload.launch(name) },
                        enabled = canAct
                    ) { Text("Save to phone") }
                    OutlinedButton(onClick = { confirmDelete = name }, enabled = canAct) {
                        Text("Delete", color = Red)
                    }
                }
            }
        }

        // ---------------- Write ----------------
        Button(
            onClick = { confirmWrite = m.selectedFile },
            enabled = canAct && m.ecuConnected && m.selectedFile != null,
            colors = ButtonDefaults.buttonColors(containerColor = Red, contentColor = Color.White),
            modifier = Modifier.fillMaxWidth().height(56.dp)
        ) {
            Text(
                if (m.selectedFile == null) "WRITE TO ECU (select a file)" else "WRITE TO ECU",
                fontWeight = FontWeight.Bold
            )
        }

        // ---------------- Tools ----------------
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { showWiring = true }) { Text("Wiring") }
            OutlinedButton(onClick = { confirmReset = true }, enabled = canAct) { Text("Reset ESP32") }
            OutlinedButton(onClick = { showLog = !showLog }) { Text(if (showLog) "Hide log" else "Log") }
        }

        if (showLog) {
            Section {
                m.log.takeLast(120).forEach {
                    Text(it, fontFamily = FontFamily.Monospace, fontSize = 11.sp, color = logColor(it))
                }
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
