package com.oliver.m3cflasher

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeoutOrNull
import java.util.UUID

/**
 * Bluetooth Low Energy link to the ESP32 ("M3C_Flasher").
 *
 * Uses the Nordic UART Service - a text "serial port over BLE". The app
 * sends the same command lines the desktop GUI sends over USB
 * ("CONNECT", "SDLIST", ...) and receives the same reply lines.
 *
 * Received lines arrive in [lines]; the connection state is in [state].
 */
@SuppressLint("MissingPermission") // permissions are requested by the activity
class BleLink(private val context: Context) {

    enum class State { DISCONNECTED, SCANNING, CONNECTING, READY }

    companion object {
        const val DEVICE_NAME = "M3C_Flasher"
        val SERVICE: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val RX: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E") // phone -> ESP32
        val TX: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E") // ESP32 -> phone
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val SCAN_TIMEOUT_MS = 12_000L
    }

    val state = MutableStateFlow(State.DISCONNECTED)

    /** Every complete text line received from the ESP32. */
    val lines = Channel<String>(Channel.UNLIMITED)

    /** Short human-readable reason for the last failure, or "". */
    val lastError = MutableStateFlow("")

    private val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val handler = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var mtu = 23
    private val lineBuf = StringBuilder()
    private val writeMutex = Mutex()
    @Volatile private var pendingWrite: CompletableDeferred<Boolean>? = null

    fun isBluetoothOn(): Boolean = manager.adapter?.isEnabled == true

    // ------------------------------------------------------------------
    // Scanning
    // ------------------------------------------------------------------
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (state.value != State.SCANNING) return
            val name = result.scanRecord?.deviceName ?: result.device.name
            val uuids = result.scanRecord?.serviceUuids?.map { it.uuid } ?: emptyList()
            if (name == DEVICE_NAME || uuids.contains(SERVICE)) {
                stopScan()
                connectTo(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            lastError.value = "Bluetooth scan failed (code $errorCode)"
            state.value = State.DISCONNECTED
        }
    }

    /** Finds the ESP32 and connects to it. */
    fun connect() {
        if (state.value != State.DISCONNECTED) return
        val adapter = manager.adapter
        if (adapter == null || !adapter.isEnabled) {
            lastError.value = "Bluetooth is off"
            return
        }
        val scanner = adapter.bluetoothLeScanner ?: run {
            lastError.value = "Bluetooth scanner not available"
            return
        }
        lastError.value = ""
        state.value = State.SCANNING
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner.startScan(null, settings, scanCallback)
        handler.postDelayed({
            if (state.value == State.SCANNING) {
                stopScan()
                state.value = State.DISCONNECTED
                lastError.value = "ESP32 not found - is it powered on and nearby?"
            }
        }, SCAN_TIMEOUT_MS)
    }

    private fun stopScan() {
        try {
            manager.adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
    }

    fun disconnect() {
        stopScan()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        rxChar = null
        pendingWrite?.complete(false)
        state.value = State.DISCONNECTED
    }

    // ------------------------------------------------------------------
    // Connection
    // ------------------------------------------------------------------
    private fun connectTo(device: BluetoothDevice) {
        state.value = State.CONNECTING
        lineBuf.clear()
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                // Larger packets -> much faster transfers. Continue in onMtuChanged.
                if (!g.requestMtu(247)) g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                g.close()
                if (gatt == g) gatt = null
                rxChar = null
                pendingWrite?.complete(false)
                if (state.value != State.DISCONNECTED && status != BluetoothGatt.GATT_SUCCESS) {
                    lastError.value = "Bluetooth connection lost"
                }
                state.value = State.DISCONNECTED
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(SERVICE)
            val tx = service?.getCharacteristic(TX)
            val rx = service?.getCharacteristic(RX)
            if (tx == null || rx == null) {
                lastError.value = "This device is not an M3C Flasher (update the ESP32 firmware)"
                disconnect()
                return
            }
            rxChar = rx
            g.setCharacteristicNotification(tx, true)
            val cccd = tx.getDescriptor(CCCD)
            if (cccd == null) {
                state.value = State.READY
                return
            }
            val enable = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeDescriptor(cccd, enable)
            } else {
                @Suppress("DEPRECATION")
                cccd.value = enable
                @Suppress("DEPRECATION")
                g.writeDescriptor(cccd)
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            state.value = State.READY
        }

        // Android 13+
        override fun onCharacteristicChanged(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray
        ) {
            onBytes(value)
        }

        // Android 8-12
        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
            onBytes(c.value ?: return)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int
        ) {
            pendingWrite?.complete(status == BluetoothGatt.GATT_SUCCESS)
        }
    }

    /** Splits incoming bytes into lines. Runs on the Bluetooth thread. */
    private fun onBytes(bytes: ByteArray) {
        synchronized(lineBuf) {
            for (b in bytes) {
                val c = (b.toInt() and 0xFF).toChar()
                when (c) {
                    '\n' -> {
                        lines.trySend(lineBuf.toString())
                        lineBuf.clear()
                    }
                    '\r' -> {}
                    else -> lineBuf.append(c)
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Sending
    // ------------------------------------------------------------------

    /** Sends one command line (a newline is added). Returns false on failure. */
    suspend fun sendLine(line: String): Boolean = writeMutex.withLock {
        val ch = rxChar ?: return false
        val g = gatt ?: return false
        val data = (line + "\n").toByteArray(Charsets.ISO_8859_1)
        val chunk = (mtu - 3).coerceIn(20, 240)
        var pos = 0
        while (pos < data.size) {
            val end = minOf(pos + chunk, data.size)
            val part = data.copyOfRange(pos, end)
            val done = CompletableDeferred<Boolean>()
            pendingWrite = done
            val type = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            var started = false
            // The Bluetooth stack can be briefly busy - retry a few times.
            for (attempt in 0 until 10) {
                started = if (Build.VERSION.SDK_INT >= 33) {
                    g.writeCharacteristic(ch, part, type) == BluetoothGatt.GATT_SUCCESS
                } else {
                    @Suppress("DEPRECATION")
                    ch.writeType = type
                    @Suppress("DEPRECATION")
                    ch.value = part
                    @Suppress("DEPRECATION")
                    g.writeCharacteristic(ch)
                }
                if (started) break
                kotlinx.coroutines.delay(30)
            }
            if (!started) {
                pendingWrite = null
                return false
            }
            val ok = withTimeoutOrNull(3000) { done.await() } ?: false
            pendingWrite = null
            if (!ok) return false
            pos = end
        }
        true
    }
}
