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
static const char *REAL_DISPLAY_MAC = "70:DE:F9:D3:B2:52";
static const char *SPOOF_BLE_MAC = "70:DE:F9:D3:B2:4E";
// Manufacturer data to clone (edit as needed). First 2 bytes are Company ID (little-endian).
static const uint8_t ADV_MFG_DATA[] = {0x59, 0x00, 0x01, 0x02, 0x03, 0x04};

// ===== Globals =====
Adafruit_GC9A01A tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

NimBLEServer *g_server = nullptr;
NimBLECharacteristic *g_charPhoneWrite = nullptr;
NimBLECharacteristic *g_charPhoneNotify = nullptr;
NimBLEAdvertising *g_adv = nullptr;

NimBLEClient *g_client = nullptr;
NimBLERemoteCharacteristic *g_charDisplayWrite = nullptr;
NimBLERemoteCharacteristic *g_charDisplayNotify = nullptr;

bool g_phoneConnected = false;
bool g_displayConnected = false;
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
static constexpr bool ENABLE_MOCK_HANDSHAKE = false;
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
  if (!g_displayConnected || !g_charDisplayWrite) return false;
  bool ok = g_charDisplayWrite->writeValue(buf, len, false);
  if (ok) {
    g_txPhoneToDisplay++;
    g_lastTx = hexOf(buf, len, len);
    Serial.printf("[POLL TX] %s\n", g_lastTx.c_str());
  } else {
    g_errors++;
    Serial.println("[ERR] Poll write to display failed");
  }
  return ok;
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
  uint8_t resp[13] = {0x55, 0xAA, 0x04, 0x11, 0x10, 0x04, 0x00, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00};
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
  uint8_t ack[10] = {0x55, 0xAA, 0x01, 0x11, 0x10, 0x20, 0x00, 0x00, 0x00, 0x00};
  uint16_t crc = bikegoCrc16Like(ack, sizeof(ack));
  ack[8] = static_cast<uint8_t>(crc & 0xFF);
  ack[9] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  g_charPhoneNotify->setValue(ack, sizeof(ack));
  g_charPhoneNotify->notify();
  g_txDisplayToPhone++;
  g_lastRx = hexOf(ack, sizeof(ack));
  Serial.printf("[MOCK RX->P] %s\n", g_lastRx.c_str());
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
  tft.print("BLE BRIDGE");

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
  tft.print("DISPLAY: ");
  tft.print(g_displayConnected ? "CONNECTED" : "WAIT");

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

  tft.setCursor(x, 162);
  tft.print("Last TX:");
  tft.setCursor(x, 174);
  tft.print(g_lastTx);

  tft.setCursor(x, 198);
  tft.print("Last RX:");
  tft.setCursor(x, 210);
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
    }
    if (ENABLE_MOCK_HANDSHAKE && isBikegoReadChallengeCmd01(raw, rawLen)) {
      Serial.println("[MOCK] Intercepted cmd 0x01 challenge read; sending 4-byte challenge");
      sendMockChallengeResp();
    }

    if (g_displayConnected && g_charDisplayWrite) {
      bool ok = g_charDisplayWrite->writeValue(raw, rawLen, false);
      if (ok) {
        g_txPhoneToDisplay++;
      } else {
        g_errors++;
        Serial.println("[ERR] Write to display failed");
      }
    } else {
      g_errors++;
      Serial.println("[WARN] Display not connected, drop TX");
    }
  }
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *pClient) override {
    Serial.printf("[DISPLAY] Connected: %s\n", pClient->getPeerAddress().toString().c_str());
    g_displayConnected = true;
  }

  void onDisconnect(NimBLEClient *pClient) override {
    (void)pClient;
    Serial.println("[DISPLAY] Disconnected");
    g_displayConnected = false;
    g_charDisplayWrite = nullptr;
    g_charDisplayNotify = nullptr;
  }

  bool onConnParamsUpdateRequest(NimBLEClient *pClient, const ble_gap_upd_params *params) override {
    (void)pClient;
    (void)params;
    return true;
  }
};

static void onDisplayNotify(NimBLERemoteCharacteristic *pRC, uint8_t *pData, size_t length, bool isNotify) {
  (void)pRC;
  g_lastRx = hexOf(pData, length, length);
  if (g_logEnabled) {
    Serial.printf("[RX D->P] %s\n", g_lastRx.c_str());
  }

  if (g_soloState == SOLO_WAIT_CHALLENGE && length >= 13 &&
      pData[0] == 0x55 && pData[1] == 0xAA &&
      pData[2] == 0x04 && pData[3] == 0x10 && pData[4] == 0x11 &&
      pData[5] == 0x04 && pData[6] == 0x00) {
    const uint8_t ch[4] = {pData[7], pData[8], pData[9], pData[10]};
    Serial.printf("[SOLO] Challenge: %02X %02X %02X %02X\n", ch[0], ch[1], ch[2], ch[3]);
    if (sendSoloAuthFromChallenge(ch)) {
      g_soloState = SOLO_WAIT_AUTH_ACK;
      g_initStepMs = millis();
      Serial.println("[SOLO] Auth sent");
    } else {
      Serial.println("[SOLO] Auth send failed");
      g_soloState = SOLO_FAILED;
    }
  } else if (g_soloState == SOLO_WAIT_AUTH_ACK && length >= 10 &&
             pData[0] == 0x55 && pData[1] == 0xAA &&
             pData[3] == 0x10 && pData[4] == 0x11 &&
             pData[5] == 0x20 && pData[7] == 0x00) {
    Serial.println("[SOLO] Auth ACK received");
    g_soloState = SOLO_POST_INIT;
    g_initStepMs = millis();
    g_postInitStep = 0;
  }

  if (g_phoneConnected && g_charPhoneNotify) {
    g_charPhoneNotify->setValue(pData, length);
    g_charPhoneNotify->notify();
    g_txDisplayToPhone++;
  } else {
    // In standalone mode this is expected; avoid noisy logs.
    if (!(g_soloState == SOLO_WAIT_CHALLENGE || g_soloState == SOLO_WAIT_AUTH_ACK ||
          g_soloState == SOLO_POST_INIT || g_soloState == SOLO_RUN_POLL)) {
      g_errors++;
      Serial.println("[WARN] Phone not connected, drop notify");
    }
  }
}

static bool connectRealDisplay() {
  NimBLEAddress addr(REAL_DISPLAY_MAC);

  if (!g_client) {
    g_client = NimBLEDevice::createClient();
    g_client->setClientCallbacks(new ClientCallbacks(), false);
    g_client->setConnectionParams(12, 24, 0, 200);
    g_client->setConnectTimeout(8);
  }

  if (!g_client->connect(addr, false)) {
    Serial.println("[ERR] Cannot connect real display");
    g_errors++;
    return false;
  }

  NimBLERemoteService *svc = g_client->getService(NUS_SERVICE_UUID);
  if (!svc) {
    Serial.println("[ERR] NUS service not found on display");
    g_errors++;
    g_client->disconnect();
    return false;
  }

  g_charDisplayWrite = svc->getCharacteristic(NUS_RX_UUID);
  g_charDisplayNotify = svc->getCharacteristic(NUS_TX_UUID);

  if (!g_charDisplayWrite || !g_charDisplayNotify) {
    Serial.println("[ERR] NUS chars not found on display");
    g_errors++;
    g_client->disconnect();
    return false;
  }

  if (g_charDisplayNotify->canNotify()) {
    if (!g_charDisplayNotify->subscribe(true, onDisplayNotify)) {
      Serial.println("[ERR] subscribe notify failed");
      g_errors++;
      g_client->disconnect();
      return false;
    }
  } else {
    Serial.println("[ERR] display notify char cannot notify");
    g_errors++;
    g_client->disconnect();
    return false;
  }

  Serial.println("[OK] Real display connected + notify subscribed");
  return true;
}

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

  // Connect to real display immediately; reconnect logic in loop.
  connectRealDisplay();
  drawUi();
}

void loop() {
  int btn = digitalRead(PIN_BTN_POLL);
  if (btn == LOW && g_lastPollBtn == HIGH && (millis() - g_lastPollBtnMs) > 220) {
    if (g_soloState == SOLO_IDLE || g_soloState == SOLO_FAILED || g_soloState == SOLO_RUN_POLL) {
      if (g_soloState == SOLO_RUN_POLL) {
        g_pollMode = false;
        g_soloState = SOLO_IDLE;
        Serial.println("[SOLO] STOP");
      } else {
        startSoloProcess();
      }
    } else {
      g_soloState = SOLO_FAILED;
      g_pollMode = false;
      Serial.println("[SOLO] ABORT");
    }
    g_lastPollBtnMs = millis();
  }
  g_lastPollBtn = btn;

  int btnLight = digitalRead(PIN_BTN_LIGHT);
  if (btnLight == LOW && g_lastLightBtn == HIGH && (millis() - g_lastLightBtnMs) > 220) {
    g_lastLightBtnMs = millis();
    if (g_displayConnected) {
      g_lightStep = 1; // start ON->OFF->ON sequence
      g_lightStepMs = 0;
      Serial.println("[LIGHT] Sequence requested via GPIO21");
    } else {
      Serial.println("[LIGHT] Ignored, display not connected");
    }
  }
  g_lastLightBtn = btnLight;

  if (!g_displayConnected) {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 4000) {
      lastRetry = millis();
      Serial.println("[DISPLAY] Reconnect attempt...");
      connectRealDisplay();
    }
  }

  if (g_soloState == SOLO_WAIT_CHALLENGE && (millis() - g_initStepMs > 2000)) {
    Serial.println("[SOLO] Timeout waiting challenge");
    g_soloState = SOLO_FAILED;
  } else if (g_soloState == SOLO_WAIT_AUTH_ACK && (millis() - g_initStepMs > 2000)) {
    Serial.println("[SOLO] Timeout waiting auth ACK");
    g_soloState = SOLO_FAILED;
  } else if (g_soloState == SOLO_POST_INIT && (millis() - g_initStepMs > 180)) {
    // Post-auth sequence observed in Bikego before periodic polling.
    if (g_postInitStep == 0) {
      static const uint8_t fA588[] = {0x55, 0xAA, 0x01, 0x11, 0xA5, 0x01, 0x88, 0x18, 0xA7, 0xFE};
      writeDisplayFrame(fA588, sizeof(fA588));
    } else if (g_postInitStep == 1) {
      static const uint8_t fA518[] = {0x55, 0xAA, 0x01, 0x11, 0xA5, 0x01, 0x18, 0x18, 0x17, 0xFF};
      writeDisplayFrame(fA518, sizeof(fA518));
    } else if (g_postInitStep == 2) {
      static const uint8_t fF101[] = {0x55, 0xAA, 0x01, 0x11, 0xF1, 0x01, 0x01, 0x1A, 0xE0, 0xFE};
      writeDisplayFrame(fF101, sizeof(fF101));
    } else {
      g_pollMode = true;
      g_soloState = SOLO_RUN_POLL;
      g_lastPollTxMs = 0;
      Serial.println("[SOLO] Enter polling");
    }
    g_postInitStep++;
    g_initStepMs = millis();
  }

  if (g_pollMode && g_soloState == SOLO_RUN_POLL && g_displayConnected && (millis() - g_lastPollTxMs > 500)) {
    g_lastPollTxMs = millis();
    pollDisplayOnce();
  }

  if (g_lightStep != 0 && g_displayConnected) {
    unsigned long now = millis();
    if (g_lightStep == 1) {
      if (writeA5Value(0xA6, 0x01)) Serial.println("[LIGHT] ON");
      g_lightStep = 2;
      g_lightStepMs = now;
    } else if (g_lightStep == 2 && (now - g_lightStepMs) > 350) {
      if (writeA5Value(0xA6, 0x00)) Serial.println("[LIGHT] OFF");
      g_lightStep = 3;
      g_lightStepMs = now;
    } else if (g_lightStep == 3 && (now - g_lightStepMs) > 350) {
      if (writeA5Value(0xA6, 0x01)) Serial.println("[LIGHT] ON");
      g_lightStep = 0;
      Serial.println("[LIGHT] Sequence done");
    }
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
