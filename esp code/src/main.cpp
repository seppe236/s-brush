#include <Arduino.h>
#include <NimBLEDevice.h>

// ─── Pin Definitions ───────────────────────────────────────────────────────
const int POWER_BUTTON_PIN  = 3;   // Toggles System On/Off (RTC-safe)
const int ACTION_BUTTON_PIN = 9;   // Cycles LED modes (safe BOOT pin)
const int LED1_PIN          = 10;  // LED 1 (Active HIGH)
const int LED2_PIN          = 8;   // LED 2 (Active HIGH)

// ─── BLE UUIDs ─────────────────────────────────────────────────────────────
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define POWER_CHAR_UUID     "12345678-1234-1234-1234-000000000001"
#define MODE_CHAR_UUID      "12345678-1234-1234-1234-000000000002"
#define STATUS_CHAR_UUID    "12345678-1234-1234-1234-000000000003"

// ─── System State ──────────────────────────────────────────────────────────
bool isSystemOn  = false;  // Starts OFF
int  activeMode  = 0;      // 0=LED1, 1=LED2, 2=Both

// ─── Button Debounce ───────────────────────────────────────────────────────
bool         lastPowerButtonState  = HIGH;
bool         lastActionButtonState = HIGH;
unsigned long lastPowerPressTime   = 0;
unsigned long lastActionPressTime  = 0;
const unsigned long DEBOUNCE_DELAY = 200;

// ─── BLE Objects ───────────────────────────────────────────────────────────
NimBLEServer*         pServer       = nullptr;
NimBLECharacteristic* pPowerChar    = nullptr;
NimBLECharacteristic* pModeChar     = nullptr;
NimBLECharacteristic* pStatusChar   = nullptr;
bool deviceConnected = false;

// ─── Forward Declarations ──────────────────────────────────────────────────
void updateHardware();
void notifyStatus();

// ─── BLE Server Callbacks ──────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("BLE: Client connected");
    // Send current state to the newly connected client
    notifyStatus();
  }
  void onDisconnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("BLE: Client disconnected – restarting advertising");
    NimBLEDevice::startAdvertising();
  }
};

// ─── Power Characteristic Callback ─────────────────────────────────────────
class PowerCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string val = pChar->getValue();
    if (val.length() > 0) {
      isSystemOn = (val[0] == 0x01);
      Serial.print("BLE Power write → System: ");
      Serial.println(isSystemOn ? "ON" : "OFF");
      updateHardware();
      notifyStatus();
    }
  }
};

// ─── Mode Characteristic Callback ──────────────────────────────────────────
class ModeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string val = pChar->getValue();
    if (val.length() > 0) {
      int requested = (int)val[0];
      if (requested >= 0 && requested <= 2) {
        activeMode = requested;
        Serial.print("BLE Mode write → Mode: ");
        Serial.println(activeMode);
        updateHardware();
        notifyStatus();
      }
    }
  }
};

// ─── Notify Status Char (power + mode packed as 2 bytes) ───────────────────
void notifyStatus() {
  uint8_t payload[2] = {
    (uint8_t)(isSystemOn ? 0x01 : 0x00),
    (uint8_t)activeMode
  };
  pStatusChar->setValue(payload, 2);
  if (deviceConnected) {
    pStatusChar->notify();
  }
  // Keep power & mode chars in sync so they're readable too
  uint8_t pwrVal = isSystemOn ? 0x01 : 0x00;
  uint8_t modeVal = (uint8_t)activeMode;
  pPowerChar->setValue(&pwrVal, 1);
  pModeChar->setValue(&modeVal, 1);
}

// ─── Hardware Update ───────────────────────────────────────────────────────
void updateHardware() {
  if (!isSystemOn) {
    digitalWrite(LED1_PIN, LOW);
    digitalWrite(LED2_PIN, LOW);
    return;
  }
  switch (activeMode) {
    case 0:
      digitalWrite(LED1_PIN, HIGH);
      digitalWrite(LED2_PIN, LOW);
      break;
    case 1:
      digitalWrite(LED1_PIN, LOW);
      digitalWrite(LED2_PIN, HIGH);
      break;
    case 2:
      digitalWrite(LED1_PIN, HIGH);
      digitalWrite(LED2_PIN, HIGH);
      break;
  }
}

// ─── Setup ─────────────────────────────────────────────────────────────────
  void setup() {
  // Serial.begin(115200); // optional for debugging
  // Serial.println("S-Brush ESP32-C3 starting...");

  // GPIO
  pinMode(POWER_BUTTON_PIN,  INPUT_PULLUP);
  pinMode(ACTION_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED1_PIN,          OUTPUT);
  pinMode(LED2_PIN,          OUTPUT);
  updateHardware();

// ─── BLE Init ───────────────────────────────────────────────────────
  setCpuFrequencyMhz(80);
  NimBLEDevice::init("S-Brush");
  // Set device name for advertising (optional)
  NimBLEDevice::setDeviceName("S-Brush");
  // Use lowest TX power for reduced heating (ESP32-C3 specific enum)
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  // Disable security authentication for Android compatibility
  NimBLEDevice::setSecurityAuth(false, false, true);
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  // Power characteristic – read + write
  pPowerChar = pService->createCharacteristic(
    POWER_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
  );
  pPowerChar->setCallbacks(new PowerCallbacks());

  // ─── Advertising — include service UUID and device name, enable scan response
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  // Advertise device name (already set via NimBLEDevice::setDeviceName)

  // Use longer advertising intervals to lower CPU load
  pAdv->setMinInterval(200); // 125 ms
  pAdv->setMaxInterval(300); // 187.5 ms

  NimBLEDevice::startAdvertising();

  // Mode characteristic – read + write
  pModeChar = pService->createCharacteristic(
    MODE_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
  );
  pModeChar->setCallbacks(new ModeCallbacks());

  // Status characteristic – read + notify
  pStatusChar = pService->createCharacteristic(
    STATUS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );



  // Removed Serial output to reduce CPU usage
  // Serial.println("S-Brush BLE started – advertising as 'S-Brush'");
}

// ─── Loop ──────────────────────────────────────────────────────────────────
void loop() {
  unsigned long currentTime = millis();
  // Small delay to lower CPU load and heat
  delay(5);

  // --- 1. POWER BUTTON ---
  bool powerReading = digitalRead(POWER_BUTTON_PIN);
  if (powerReading == LOW && lastPowerButtonState == HIGH &&
      (currentTime - lastPowerPressTime > DEBOUNCE_DELAY)) {
    lastPowerPressTime = currentTime;
    isSystemOn = !isSystemOn;
    Serial.print("Power Button → System: ");
    Serial.println(isSystemOn ? "ON" : "OFF");
    updateHardware();
    notifyStatus();
  }
  lastPowerButtonState = powerReading;

  // --- 2. ACTION BUTTON (only when system ON) ---
  bool actionReading = digitalRead(ACTION_BUTTON_PIN);
  if (isSystemOn) {
    if (actionReading == LOW && lastActionButtonState == HIGH &&
        (currentTime - lastActionPressTime > DEBOUNCE_DELAY)) {
      lastActionPressTime = currentTime;
      activeMode = (activeMode + 1) % 3;
      Serial.print("Action Button → Mode: ");
      Serial.println(activeMode);
      updateHardware();
      notifyStatus();
    }
  }
  lastActionButtonState = actionReading;
}