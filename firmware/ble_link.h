#ifndef BLE_LINK_H
#define BLE_LINK_H

// =====================================================================
// Bluetooth Low Energy link for the phone app.
//
// The ESP32 advertises as "M3C_Flasher" with the Nordic UART Service
// (NUS), the de-facto standard "serial port over BLE":
//   service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX       6E400002-...  phone -> ESP32  (write / write without response)
//   TX       6E400003-...  ESP32 -> phone  (notify)
//
// The protocol is exactly the same text protocol the Python GUI uses
// over USB serial: the phone sends lines like "CONNECT\n" or "SDLIST\n",
// and receives the same "STATUS:...", "STEP:...", "ERR:..." lines.
//
// All firmware output goes through the global `Out` object (instead of
// `Serial`). `Out` writes every byte to USB serial AND, while a phone is
// connected, to BLE. So the GUI and the phone app see the same output.
//
// Received BLE bytes are queued by the BLE stack and processed in loop()
// through bleRxPoll(), using the same handleSerialLine() as USB serial.
//
// Any free BLE app with a UART terminal (e.g. nRF Connect, "Serial
// Bluetooth Terminal") can be used to test this before the real app.
// =====================================================================

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <freertos/ringbuf.h>

#define BLE_DEVICE_NAME   "M3C_Flasher"
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Max payload per notification. The phone app asks for MTU 247
// (payload 244). If the phone negotiates a smaller MTU, chunks are made
// smaller automatically.
#define BLE_MAX_CHUNK      244
// Pause between notifications so the BLE stack's send queue never
// overflows (otherwise notifications are silently dropped).
// Normal output: 244 B every 8 ms. Bulk file download (bleBlocking):
// 244 B every 4 ms, about 50 KB/s. The app checks every downloaded
// piece by its offset, so a lost notification is always detected.
#define BLE_NOTIFY_GAP_MS       8
#define BLE_NOTIFY_GAP_BULK_MS  4

static BLEServer*         bleServer = nullptr;
static BLECharacteristic* bleTx = nullptr;
static RingbufHandle_t    bleTxRing = nullptr;   // firmware output -> phone
static RingbufHandle_t    bleRxRing = nullptr;   // phone -> firmware commands
static volatile bool      bleClientConnected = false;
static volatile uint16_t  bleConnId = 0;
static String             bleLineBuf = "";

// bleMute: while true, output is NOT sent to the phone (used for the
//   long per-block "DATA:" lines during READ - the phone doesn't need
//   them, the dump is saved to the SD card anyway).
// bleBlocking: while true, output may wait for the phone when the
//   queue is full (used only for SD file download, where nothing
//   time-critical is running). Otherwise output never waits, so ECU/CAN
//   timing is never affected by Bluetooth - excess output is dropped.
volatile bool bleMute = false;
volatile bool bleBlocking = false;
// serialMute: while true, output is NOT sent to USB serial. Used for the
//   phone-only fast download (SDGETB) - USB serial at 115200 baud would
//   otherwise limit the speed to ~11 KB/s.
volatile bool serialMute = false;

// ---------------------------------------------------------------------
// Queues output bytes for the phone. Called from any task (main loop,
// web task) - the ESP-IDF ring buffer is thread-safe.
// Never waits unless bleBlocking is set (see above).
// ---------------------------------------------------------------------
static void bleOutWrite(const uint8_t* data, size_t len) {
  if (!bleClientConnected || bleMute || bleTxRing == nullptr || len == 0) return;
  TickType_t wait = bleBlocking ? pdMS_TO_TICKS(1000) : 0;
  while (len > 0) {
    size_t part = len > 512 ? 512 : len;
    if (xRingbufferSend(bleTxRing, data, part, wait) != pdTRUE) {
      return;  // phone not reading (or just disconnected) - drop output
    }
    data += part;
    len -= part;
  }
}

// ---------------------------------------------------------------------
// Print object used everywhere instead of Serial.print/println.
// ---------------------------------------------------------------------
class DualOut : public Print {
public:
  size_t write(uint8_t c) override {
    return write(&c, 1);
  }
  size_t write(const uint8_t* buf, size_t size) override {
    if (!serialMute) Serial.write(buf, size);
    bleOutWrite(buf, size);
    return size;
  }
};
DualOut Out;

// ---------------------------------------------------------------------
// BLE callbacks
// ---------------------------------------------------------------------
class M3CServerCallbacks : public BLEServerCallbacks {
  // Only the plain onConnect(BLEServer*) exists in every ESP32 core
  // version (2.x, 3.x with Bluedroid or NimBLE), so use that one.
  void onConnect(BLEServer* server) override {
    bleConnId = server->getConnId();
    bleClientConnected = true;
  }
  void onDisconnect(BLEServer* server) override {
    bleClientConnected = false;
    // Drop anything still queued for the old connection.
    if (bleTxRing) {
      size_t sz;
      void* item;
      while ((item = xRingbufferReceiveUpTo(bleTxRing, &sz, 0, 4096)) != nullptr) {
        vRingbufferReturnItem(bleTxRing, item);
      }
    }
    // Advertise again so the phone can reconnect.
    delay(100);
    BLEDevice::startAdvertising();
  }
};

class M3CRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    // getValue() is std::string on core 2.x and String on core 3.x -
    // both have c_str() and length().
    auto v = c->getValue();
    if (v.length() > 0 && bleRxRing) {
      xRingbufferSend(bleRxRing, v.c_str(), v.length(), pdMS_TO_TICKS(50));
    }
  }
};

// ---------------------------------------------------------------------
// Sender task: takes queued output and sends it as notifications.
// ---------------------------------------------------------------------
static void bleTxTask(void* arg) {
  for (;;) {
    if (!bleClientConnected) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    size_t chunk = BLE_MAX_CHUNK;
    uint16_t mtu = bleServer->getPeerMTU(bleConnId);
    if (mtu >= 23 && (size_t)(mtu - 3) < chunk) chunk = mtu - 3;

    size_t sz = 0;
    uint8_t* item = (uint8_t*)xRingbufferReceiveUpTo(bleTxRing, &sz,
                                                       pdMS_TO_TICKS(50), chunk);
    if (item == nullptr) continue;
    if (bleClientConnected) {
      bleTx->setValue(item, sz);
      bleTx->notify();
    }
    vRingbufferReturnItem(bleTxRing, item);
    vTaskDelay(pdMS_TO_TICKS(bleBlocking ? BLE_NOTIFY_GAP_BULK_MS : BLE_NOTIFY_GAP_MS));
  }
}

// ---------------------------------------------------------------------
// Call once from setup().
// ---------------------------------------------------------------------
void setupBLE() {
  bleTxRing = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
  bleRxRing = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF);

  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(247);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new M3CServerCallbacks());

  BLEService* svc = bleServer->createService(NUS_SERVICE_UUID);

  bleTx = svc->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleTx->addDescriptor(new BLE2902());

  BLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new M3CRxCallbacks());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  xTaskCreatePinnedToCore(bleTxTask, "bleTx", 4096, NULL, 1, NULL, 0);

  Out.println("STATUS:BLE_OK");
}

// ---------------------------------------------------------------------
// Call from loop(): turns received BLE bytes into command lines and
// hands each complete line to the same handler USB serial uses.
// ---------------------------------------------------------------------
void bleRxPoll(void (*handler)(String)) {
  if (bleRxRing == nullptr) return;
  size_t sz = 0;
  char* item;
  while ((item = (char*)xRingbufferReceiveUpTo(bleRxRing, &sz, 0, 256)) != nullptr) {
    // Copy out and give the ring buffer space back right away - the
    // handler may run for minutes (e.g. a full write).
    char tmp[256];
    memcpy(tmp, item, sz);
    vRingbufferReturnItem(bleRxRing, item);
    for (size_t i = 0; i < sz; i++) {
      char c = tmp[i];
      if (c == '\n') {
        String line = bleLineBuf;
        bleLineBuf = "";
        handler(line);
      } else if (c != '\r') {
        bleLineBuf += c;
      }
    }
  }
}

#endif
