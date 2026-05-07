#include <Arduino.h>
#include <NimBLEDevice.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <esp_mac.h>
#include <mbedtls/aes.h>
#include <vector>

// ===== Display pins from your ESPHome config =====
static constexpr int PIN_TFT_RST = 6;
static constexpr int PIN_TFT_CS  = 7;
static constexpr int PIN_TFT_DC  = 8;

// ===== Optional SPI pins for ESP32-C3 (adjust if needed) =====
static constexpr int PIN_SPI_SCLK = 10;
static constexpr int PIN_SPI_MOSI = 9;
static constexpr int PIN_SPI_MISO = -1;
static constexpr int PIN_BTN_POLL = 20;
static constexpr int PIN_BTN_LIGHT = 21;

// ===== BLE NUS UUIDs =====
static NimBLEUUID NUS_SERVICE_UUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static NimBLEUUID NUS_RX_UUID("6e400002-b5a3-f393-e0a9-e50e24dcca9e");   // write
static NimBLEUUID NUS_TX_UUID("6e400003-b5a3-f393-e0a9-e50e24dcca9e");   // notify

// ===== Bridge config =====
static const char *ADV_NAME = "EKD01-TB ";   // note trailing space (seen on real device)
// Put your real display MAC here to avoid connecting to ourselves.
static const char *SPOOF_BLE_MAC = "70:DE:F9:D3:B2:4E";
// Manufacturer data to clone (edit as needed). First 2 bytes are Company ID (little-endian).
static const uint8_t ADV_MFG_DATA[] = {0x59, 0x00, 0x01, 0x02, 0x03, 0x04};

// ===== Globals =====
Adafruit_GC9A01A tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

NimBLEServer *g_server = nullptr;
NimBLECharacteristic *g_charPhoneWrite = nullptr;
NimBLECharacteristic *g_charPhoneNotify = nullptr;
NimBLEAdvertising *g_adv = nullptr;

bool g_phoneConnected = false;
bool g_displayConnected = true; // mock always "has display"
bool g_phoneConnecting = false;
uint32_t g_txPhoneToDisplay = 0;
uint32_t g_txDisplayToPhone = 0;
uint32_t g_errors = 0;
String g_lastTx = "-";
String g_lastRx = "-";
unsigned long g_lastUiMs = 0;
uint32_t g_uptimeSec = 0;
bool g_beat = false;
bool g_logEnabled = true;
static constexpr bool ENABLE_MOCK_HANDSHAKE = true;
unsigned long g_lastStatsMs = 0;
bool g_pollMode = false;
int g_lastPollBtn = HIGH;
unsigned long g_lastPollBtnMs = 0;
unsigned long g_lastPollTxMs = 0;
int g_lastLightBtn = HIGH;
unsigned long g_lastLightBtnMs = 0;
unsigned long g_initStepMs = 0;
static const uint8_t BIKEGO_AES_KEY[16] = {
  0x32, 0x43, 0x54, 0x44, 0x55, 0x34, 0x30, 0x71,
  0x4E, 0x79, 0x43, 0x67, 0x54, 0x6A, 0x62, 0x31
};
uint8_t g_assist = 1;
uint8_t g_light = 0;
uint8_t g_brightness = 4; // 1..4
uint8_t g_unitMph = 0;    // 0 kmh, 1 mph
uint8_t g_battery = 85;
uint16_t g_speedRaw = 0;  // speed*10
unsigned long g_lastNotifyMs = 0;
uint8_t g_seq10 = 0xC3;
uint8_t g_challenge[4] = {0x47, 0x5A, 0x61, 0x13};
unsigned long g_lastBatteryTickMs = 0;
int g_batteryDir = -1; // drain then charge to look alive

enum SoloInitState : uint8_t {
  SOLO_IDLE = 0,
  SOLO_WAIT_CHALLENGE,
  SOLO_WAIT_AUTH_ACK,
  SOLO_POST_INIT,
  SOLO_RUN_POLL,
  SOLO_FAILED
};
SoloInitState g_soloState = SOLO_IDLE;
uint8_t g_postInitStep = 0;
uint8_t g_lightStep = 0;
unsigned long g_lightStepMs = 0;

static String hexOf(const uint8_t *data, size_t len, size_t maxLen = 24);

static uint16_t bikegoCrc16Like(const uint8_t *data, size_t len) {
  if (len < 4) return 0;
  uint32_t sum = 0;
  for (size_t i = 2; i < len - 2; i++) sum += data[i];
  return static_cast<uint16_t>(0xFFFFu ^ (sum & 0xFFFFu));
}

static bool writeDisplayFrame(const uint8_t *buf, size_t len) {
  (void)buf;
  (void)len;
  return false;
}

static void startSoloProcess() {
  if (!g_displayConnected) {
    Serial.println("[SOLO] Cannot start: display not connected");
    g_soloState = SOLO_FAILED;
    return;
  }
  // 1) Challenge read.
  static const uint8_t fChallenge[] = {0x55, 0xAA, 0x01, 0x11, 0x10, 0x01, 0x00, 0x04, 0xD8, 0xFF};
  writeDisplayFrame(fChallenge, sizeof(fChallenge));
  g_soloState = SOLO_WAIT_CHALLENGE;
  g_initStepMs = millis();
  g_postInitStep = 0;
  Serial.println("[SOLO] Started: waiting challenge");
}

static bool sendSoloAuthFromChallenge(const uint8_t ch4[4]) {
  uint8_t plain[16] = {0};
  plain[0] = ch4[0];
  plain[1] = ch4[1];
  plain[2] = ch4[2];
  plain[3] = ch4[3];
  uint8_t cipher[16] = {0};

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, BIKEGO_AES_KEY, 128) != 0) {
    mbedtls_aes_free(&ctx);
    Serial.println("[SOLO] AES setkey failed");
    return false;
  }
  if (mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, plain, cipher) != 0) {
    mbedtls_aes_free(&ctx);
    Serial.println("[SOLO] AES encrypt failed");
    return false;
  }
  mbedtls_aes_free(&ctx);

  uint8_t auth[25] = {0};
  auth[0] = 0x55; auth[1] = 0xAA; auth[2] = 0x10; auth[3] = 0x11;
  auth[4] = 0x10; auth[5] = 0x20; auth[6] = 0x00;
  for (int i = 0; i < 16; i++) auth[7 + i] = cipher[i];
  uint16_t crc = bikegoCrc16Like(auth, sizeof(auth));
  auth[23] = (uint8_t)(crc & 0xFF);
  auth[24] = (uint8_t)((crc >> 8) & 0xFF);
  return writeDisplayFrame(auth, sizeof(auth));
}

static void pollDisplayOnce() {
  // Frames observed from Bikego session (request/response polling loop).
  static const uint8_t f42[] = {0x55, 0xAA, 0x04, 0x11, 0x10, 0x02, 0x42, 0x20, 0x1C, 0x00, 0x00, 0x5A, 0xFF};
  static const uint8_t f46[] = {0x55, 0xAA, 0x04, 0x11, 0x10, 0x02, 0x46, 0xFB, 0x58, 0xFB, 0x69, 0xDB, 0xFC};
  writeDisplayFrame(f42, sizeof(f42));
  writeDisplayFrame(f46, sizeof(f46));
}

static bool writeA5Value(uint8_t offset, uint8_t value) {
  uint8_t fr[11] = {0x55, 0xAA, 0x02, 0x11, 0xA5, 0x02, offset, value, 0x00, 0x00, 0x00};
  uint16_t crc = bikegoCrc16Like(fr, sizeof(fr));
  fr[9] = (uint8_t)(crc & 0xFF);
  fr[10] = (uint8_t)((crc >> 8) & 0xFF);
  return writeDisplayFrame(fr, sizeof(fr));
}

static bool isBikegoAuthCmd20(const uint8_t *data, size_t len) {
  if (!data || len < 9) return false;
  if (data[0] != 0x55 || data[1] != 0xAA) return false;
  if (data[3] != 0x11 || data[4] != 0x10) return false;
  return data[5] == 0x20;
}

static bool isBikegoReadChallengeCmd01(const uint8_t *data, size_t len) {
  if (!data || len < 10) return false;
  if (data[0] != 0x55 || data[1] != 0xAA) return false;
  if (data[3] != 0x11 || data[4] != 0x10) return false;
  // read cmd, offset 0, expected size 4
  return data[5] == 0x01 && data[6] == 0x00 && data[7] == 0x04;
}

static void sendMockChallengeResp() {
  if (!g_phoneConnected || !g_charPhoneNotify) return;
  // 0x04 means read response in Bikego parser; 4-byte payload at bytes 7..10.
  uint8_t resp[13] = {0x55, 0xAA, 0x04, 0x10, 0x11, 0x04, 0x00, g_challenge[0], g_challenge[1], g_challenge[2], g_challenge[3], 0x00, 0x00};
  uint16_t crc = bikegoCrc16Like(resp, sizeof(resp));
  resp[11] = static_cast<uint8_t>(crc & 0xFF);
  resp[12] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  g_charPhoneNotify->setValue(resp, sizeof(resp));
  g_charPhoneNotify->notify();
  g_txDisplayToPhone++;
  g_lastRx = hexOf(resp, sizeof(resp));
  Serial.printf("[MOCK RX->P] %s\n", g_lastRx.c_str());
}

static void sendMockAuthAckSuccess() {
  if (!g_phoneConnected || !g_charPhoneNotify) return;
  uint8_t ack[10] = {0x55, 0xAA, 0x01, 0x10, 0x11, 0x20, 0x00, 0x00, 0x00, 0x00};
  uint16_t crc = bikegoCrc16Like(ack, sizeof(ack));
  ack[8] = static_cast<uint8_t>(crc & 0xFF);
  ack[9] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  g_charPhoneNotify->setValue(ack, sizeof(ack));
  g_charPhoneNotify->notify();
  g_txDisplayToPhone++;
  g_lastRx = hexOf(ack, sizeof(ack));
  Serial.printf("[MOCK RX->P] %s\n", g_lastRx.c_str());
}

static void sendNotifyFrame(const uint8_t *buf, size_t len) {
  if (!g_phoneConnected || !g_charPhoneNotify) return;
  g_charPhoneNotify->setValue(buf, len);
  g_charPhoneNotify->notify();
  g_txDisplayToPhone++;
  g_lastRx = hexOf(buf, len, len);
  if (g_logEnabled) Serial.printf("[MOCK RX->P] %s\n", g_lastRx.c_str());
}

static void sendA5ReadResp88() {
  uint8_t r[32] = {0x55, 0xAA, 0x18, 0xA5, 0x11, 0x04, 0x88};
  const char *name = "EKD01-TB ";
  for (int i = 0; i < 24; i++) r[7 + i] = (i < (int)strlen(name)) ? (uint8_t)name[i] : 0x00;
  uint16_t crc = bikegoCrc16Like(r, sizeof(r));
  r[30] = (uint8_t)(crc & 0xFF);
  r[31] = (uint8_t)(crc >> 8);
  sendNotifyFrame(r, sizeof(r));
}

static void sendA5ReadResp18() {
  uint8_t r[32] = {0x55, 0xAA, 0x18, 0xA5, 0x11, 0x04, 0x18};
  const char *name = "EKD01_TB_N22";
  for (int i = 0; i < 24; i++) r[7 + i] = (i < (int)strlen(name)) ? (uint8_t)name[i] : 0x00;
  uint16_t crc = bikegoCrc16Like(r, sizeof(r));
  r[30] = (uint8_t)(crc & 0xFF);
  r[31] = (uint8_t)(crc >> 8);
  sendNotifyFrame(r, sizeof(r));
}

static void sendA5WriteAck(uint8_t off) {
  uint8_t r[10] = {0x55, 0xAA, 0x01, 0xA5, 0x11, 0x05, off, 0x00, 0x00, 0x00};
  uint16_t crc = bikegoCrc16Like(r, sizeof(r));
  r[8] = (uint8_t)(crc & 0xFF);
  r[9] = (uint8_t)(crc >> 8);
  sendNotifyFrame(r, sizeof(r));
}

static void sendPollAck(uint8_t off, uint8_t val) {
  uint8_t r[10] = {0x55, 0xAA, 0x01, 0x10, 0x11, 0x05, off, val, 0x00, 0x00};
  uint16_t crc = bikegoCrc16Like(r, sizeof(r));
  r[8] = (uint8_t)(crc & 0xFF);
  r[9] = (uint8_t)(crc >> 8);
  sendNotifyFrame(r, sizeof(r));
}

static void sendF101Resp() {
  uint8_t r[34] = {
      0x55, 0xAA, 0x1A, 0xF1, 0x11, 0x04, 0x01, 0x00, 0x00, 0x56, 0x07, 0x00,
      0x00, 0x0A, 0xA8, 0x89, 0x8B, 0x07, 0x0A, 0x14, 0x64, 0x00, 0x2C, 0x01,
      0x01, 0x02, 0x32, 0x00, 0xF4, 0x01, 0x01, 0x09, 0x00, 0x00};
  uint16_t crc = bikegoCrc16Like(r, sizeof(r));
  r[32] = (uint8_t)(crc & 0xFF);
  r[33] = (uint8_t)(crc >> 8);
  sendNotifyFrame(r, sizeof(r));
}

static void sendState15() {
  uint8_t r[30] = {0x55, 0xAA, 0x15, 0x10, 0x11, 0x06, 0x01, 0x00, 0x00, 0x00, 0x01,
                   g_light, g_assist, 0x05, g_battery, 0x01, (uint8_t)(g_speedRaw & 0xFF), (uint8_t)(g_speedRaw >> 8),
                   0x8C, 0x06, 0x00, 0x00, 0xF3, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint16_t crc = bikegoCrc16Like(r, sizeof(r));
  r[28] = (uint8_t)(crc & 0xFF);
  r[29] = (uint8_t)(crc >> 8);
  sendNotifyFrame(r, sizeof(r));
}

static void sendState10() {
  uint8_t r[24] = {0x55, 0xAA, 0x10, 0x10, 0x11, 0x06, 0x09, g_seq10, 0x0E, 0x00, 0x00, 0x3D,
                   0x06, 0x88, 0x01, 0x18, 0x01, 0x1E, 0x00, (uint8_t)(0x40 + g_brightness), g_unitMph, 0x01, 0x00, 0x00};
  g_seq10++;
  uint16_t crc = bikegoCrc16Like(r, sizeof(r));
  r[22] = (uint8_t)(crc & 0xFF);
  r[23] = (uint8_t)(crc >> 8);
  sendNotifyFrame(r, sizeof(r));
}

static String hexOf(const uint8_t *data, size_t len, size_t maxLen) {
  String out;
  size_t n = len < maxLen ? len : maxLen;
  for (size_t i = 0; i < n; i++) {
    if (i) out += ' ';
    char b[4];
    snprintf(b, sizeof(b), "%02X", data[i]);
    out += b;
  }
  if (len > n) out += " ...";
  return out;
}

static String hexOfVec(const std::vector<uint8_t> &v) {
  if (v.empty()) return String("-");
  return hexOf(v.data(), v.size(), v.size());
}

static bool parseMac(const char *s, uint8_t out[6]) {
  unsigned int v[6];
  if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(v[i] & 0xFF);
  return true;
}

static void spoofBleMacIfPossible() {
  uint8_t mac[6];
  if (!parseMac(SPOOF_BLE_MAC, mac)) {
    Serial.println("[MAC] Invalid SPOOF_BLE_MAC format");
    g_errors++;
    return;
  }
  // Set base MAC before BLE init (affects derived interface MACs).
  esp_err_t err = esp_base_mac_addr_set(mac);
  if (err == ESP_OK) {
    Serial.printf("[MAC] BLE MAC spoof set to %s\n", SPOOF_BLE_MAC);
  } else {
    Serial.printf("[MAC] Spoof failed err=%d\n", static_cast<int>(err));
    g_errors++;
  }
}

static void drawUi() {
  tft.fillScreen(0x0000);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(2);
  tft.setCursor(42, 14);
  tft.print("EKD01 MOCK");

  tft.setTextSize(1);
  const int x = 26; // shift content towards center for circular display
  tft.setCursor(x, 42);
  tft.print("ADV: ");
  tft.print(ADV_NAME);

  tft.setCursor(x, 58);
  tft.print("PHONE: ");
  if (g_phoneConnected) {
    tft.print("CONNECTED");
  } else if (g_phoneConnecting) {
    tft.print("CONNECTING");
  } else {
    tft.print("WAIT");
  }

  tft.setCursor(x, 74);
  tft.print("DISPLAY: MOCK");

  tft.setCursor(x, 90);
  tft.print("TX P->D: ");
  tft.print(g_txPhoneToDisplay);

  tft.setCursor(x, 106);
  tft.print("TX D->P: ");
  tft.print(g_txDisplayToPhone);

  tft.setCursor(x, 122);
  tft.print("ERRORS: ");
  tft.print(g_errors);

  tft.setCursor(x, 136);
  tft.print("UPTIME: ");
  tft.print(g_uptimeSec);
  tft.print("s");

  tft.setCursor(x, 146);
  tft.print("LOG: ");
  tft.print(g_logEnabled ? "ON" : "OFF");

  tft.setCursor(x, 154);
  tft.print("ASSIST:");
  tft.print(g_assist);
  tft.print(" L:");
  tft.print(g_light ? "ON" : "OFF");

  tft.setCursor(x, 166);
  tft.print("UNIT:");
  tft.print(g_unitMph ? "MPH" : "KMH");
  tft.print(" B:");
  tft.print(g_brightness);

  tft.setCursor(x, 178);
  tft.print("BATT:");
  tft.print(g_battery);
  tft.print("% SPD:");
  tft.print(((float)g_speedRaw) / 10.0f, 1);

  tft.setCursor(x, 188);
  tft.print("BTN20:+5 BTN21:-5");

  tft.setCursor(x, 192);
  tft.print("Last TX:");
  tft.setCursor(x, 174);
  tft.print(g_lastTx);

  tft.setCursor(x, 206);
  tft.print("Last RX:");
  tft.setCursor(x, 218);
  tft.print(g_lastRx);

  // Heartbeat indicator: green/red dot toggles every second.
  uint16_t color = g_beat ? 0x07E0 : 0xF800;
  tft.fillCircle(225, 15, 8, color);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, ble_gap_conn_desc *desc) override {
    (void)pServer;
    g_phoneConnecting = true;
    Serial.printf("[PHONE] Connect attempt from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  desc->peer_id_addr.val[5], desc->peer_id_addr.val[4], desc->peer_id_addr.val[3],
                  desc->peer_id_addr.val[2], desc->peer_id_addr.val[1], desc->peer_id_addr.val[0]);
  }

  void onConnect(NimBLEServer *pServer) override {
    (void)pServer;
    g_phoneConnecting = false;
    g_phoneConnected = true;
    Serial.println("[PHONE] Connected");
  }

  void onDisconnect(NimBLEServer *pServer) override {
    g_phoneConnecting = false;
    Serial.println("[PHONE] Disconnected");
    g_phoneConnected = false;
    NimBLEDevice::startAdvertising();
  }
};

class PhoneWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar) override {
    std::string v = pChar->getValue();
    if (v.empty()) return;
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(v.data());
    const size_t rawLen = v.size();

    g_lastTx = hexOf(raw, rawLen, rawLen);
    if (g_logEnabled) {
      Serial.printf("[TX P->D] %s\n", g_lastTx.c_str());
    }

    if (ENABLE_MOCK_HANDSHAKE && isBikegoAuthCmd20(raw, rawLen)) {
      Serial.println("[MOCK] Intercepted cmd 0x20 auth; sending success ACK");
      sendMockAuthAckSuccess();
      return;
    }
    if (ENABLE_MOCK_HANDSHAKE && isBikegoReadChallengeCmd01(raw, rawLen)) {
      Serial.println("[MOCK] Intercepted cmd 0x01 challenge read; sending 4-byte challenge");
      sendMockChallengeResp();
      return;
    }
    // A5 read
    if (rawLen >= 10 && raw[0] == 0x55 && raw[1] == 0xAA && raw[3] == 0x11 && raw[4] == 0xA5 && raw[5] == 0x01) {
      if (raw[6] == 0x88) sendA5ReadResp88();
      if (raw[6] == 0x18) sendA5ReadResp18();
      if (raw[6] == 0xE0) sendA5WriteAck(0xE0);
      return;
    }
    // A5 write
    if (rawLen >= 11 && raw[0] == 0x55 && raw[1] == 0xAA && raw[3] == 0x11 && raw[4] == 0xA5 && raw[5] == 0x02) {
      uint8_t off = raw[6];
      uint8_t val = raw[7];
      if (off == 0xA4) g_assist = val;
      if (off == 0xA6) g_light = (val ? 1 : 0);
      if (off == 0xA7) g_brightness = (val >= 1 && val <= 4) ? val : g_brightness;
      if (off == 0xE0) g_unitMph = (val ? 1 : 0);
      sendA5WriteAck(off);
      return;
    }
    // F1 query
    if (rawLen >= 10 && raw[0] == 0x55 && raw[1] == 0xAA && raw[3] == 0x11 && raw[4] == 0xF1 && raw[5] == 0x01) {
      sendF101Resp();
      return;
    }
    // Poll requests 42/46
    if (rawLen >= 13 && raw[0] == 0x55 && raw[1] == 0xAA && raw[3] == 0x11 && raw[4] == 0x10 && raw[5] == 0x02) {
      if (raw[6] == 0x42) sendPollAck(0x42, 0x01);
      if (raw[6] == 0x46) sendPollAck(0x46, 0x00);
      return;
    }
  }
};


static void setupBleServer() {
  NimBLEDevice::init(ADV_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(new ServerCallbacks());

  NimBLEService *svc = g_server->createService(NUS_SERVICE_UUID);

  g_charPhoneWrite = svc->createCharacteristic(
      NUS_RX_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
  g_charPhoneWrite->setCallbacks(new PhoneWriteCallbacks());

  g_charPhoneNotify = svc->createCharacteristic(
      NUS_TX_UUID,
      NIMBLE_PROPERTY::NOTIFY);

  svc->start();

  g_adv = NimBLEDevice::getAdvertising();

  // Build full advertising payload explicitly so we can clone/tune byte by byte.
  NimBLEAdvertisementData advData;
  NimBLEAdvertisementData scanData;

  // Flags: LE General Discoverable + BR/EDR not supported
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  // Include HID UUID in primary ADV (common in EKD01 scans).
  std::vector<NimBLEUUID> uu16 = {NimBLEUUID((uint16_t)0x1812)};
  advData.setCompleteServices16(uu16);
  // Manufacturer data (clonable)
  std::vector<uint8_t> mfg(ADV_MFG_DATA, ADV_MFG_DATA + sizeof(ADV_MFG_DATA));
  advData.setManufacturerData(mfg);

  // Put local name + NUS UUID on scan response
  scanData.setName(ADV_NAME);
  scanData.setCompleteServices(NUS_SERVICE_UUID);

  g_adv->setAdvertisementData(advData);
  g_adv->setScanResponseData(scanData);
  g_adv->setScanResponse(true);
  g_adv->start();

  Serial.println("[BLE] Advertising started");
  std::string advPayload = advData.getPayload();
  std::string scanPayload = scanData.getPayload();
  std::vector<uint8_t> advVec(advPayload.begin(), advPayload.end());
  std::vector<uint8_t> scanVec(scanPayload.begin(), scanPayload.end());
  Serial.printf("[BLE] ADV NAME: '%s'\n", ADV_NAME);
  Serial.printf("[BLE] ADV RAW (%d): %s\n", (int)advVec.size(), hexOfVec(advVec).c_str());
  Serial.printf("[BLE] SCAN_RSP RAW (%d): %s\n", (int)scanVec.size(), hexOfVec(scanVec).c_str());
  Serial.printf("[BLE] MFG RAW (%d): %s\n", (int)sizeof(ADV_MFG_DATA), hexOf(ADV_MFG_DATA, sizeof(ADV_MFG_DATA), sizeof(ADV_MFG_DATA)).c_str());
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32-C3 BLE Bridge EKD01 ===");

  SPI.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_TFT_CS);
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(0x001F);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(2);
  tft.setCursor(20, 100);
  tft.print("ESP32-C3 OK");
  delay(1200);
  drawUi();
  pinMode(PIN_BTN_POLL, INPUT_PULLUP);
  pinMode(PIN_BTN_LIGHT, INPUT_PULLUP);

  spoofBleMacIfPossible();
  setupBleServer();

  drawUi();
}

void loop() {
  int btn = digitalRead(PIN_BTN_POLL);
  if (btn == LOW && g_lastPollBtn == HIGH && (millis() - g_lastPollBtnMs) > 220) {
    // GPIO20: increase speed by +5 raw (0.5 units shown by display app)
    if (g_speedRaw <= 995) g_speedRaw += 5;
    Serial.printf("[BTN20] speed_raw=%u\n", (unsigned)g_speedRaw);
    g_lastPollBtnMs = millis();
  }
  g_lastPollBtn = btn;

  int btnLight = digitalRead(PIN_BTN_LIGHT);
  if (btnLight == LOW && g_lastLightBtn == HIGH && (millis() - g_lastLightBtnMs) > 220) {
    g_lastLightBtnMs = millis();
    // GPIO21: decrease speed by -5 raw, floor at 0
    if (g_speedRaw >= 5) g_speedRaw -= 5;
    else g_speedRaw = 0;
    Serial.printf("[BTN21] speed_raw=%u\n", (unsigned)g_speedRaw);
  }
  g_lastLightBtn = btnLight;

  if (g_phoneConnected && (millis() - g_lastNotifyMs > 400)) {
    g_lastNotifyMs = millis();
    sendState15();
    sendState10();
  }

  // Fake battery animation (slow drift)
  if (millis() - g_lastBatteryTickMs > 3000) {
    g_lastBatteryTickMs = millis();
    int next = (int)g_battery + g_batteryDir;
    if (next <= 20) {
      next = 20;
      g_batteryDir = +1;
    } else if (next >= 95) {
      next = 95;
      g_batteryDir = -1;
    }
    g_battery = (uint8_t)next;
  }

  static unsigned long lastSec = 0;
  if (millis() - lastSec > 1000) {
    lastSec = millis();
    g_uptimeSec++;
    g_beat = !g_beat;
  }

  if (millis() - g_lastUiMs > 300) {
    g_lastUiMs = millis();
    drawUi();
  }

  if (millis() - g_lastStatsMs > 3000) {
    g_lastStatsMs = millis();
    Serial.printf("[STATS] up=%lus phone=%d display=%d poll=%d solo=%u tx_pd=%lu tx_dp=%lu err=%lu\n",
                  g_uptimeSec, g_phoneConnected ? 1 : 0, g_displayConnected ? 1 : 0,
                  g_pollMode ? 1 : 0,
                  (unsigned)g_soloState,
                  (unsigned long)g_txPhoneToDisplay, (unsigned long)g_txDisplayToPhone,
                  (unsigned long)g_errors);
  }

  delay(10);
}
