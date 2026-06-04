#include <Arduino.h>
#include <NimBLEDevice.h>

// ─── BLE UUIDs ─────────────────────────────────────────────────────────────
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define RX_CHAR_UUID        "12345678-1234-1234-1234-000000000001" // Client writes to this
#define TX_CHAR_UUID        "12345678-1234-1234-1234-000000000002" // Client receives from this

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTxChar = nullptr;
NimBLECharacteristic* pRxChar = nullptr;
bool deviceConnected = false;
unsigned long lastMsgTime = 0;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("BLE Client connected");
  }
  void onDisconnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("BLE Client disconnected. Restarting advertising...");
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string val = pChar->getValue();
    if (val.length() > 0) {
      Serial.print("Received via BLE: ");
      Serial.println(val.c_str());
    }
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("Starting BLE Basic Communication...");

  NimBLEDevice::init("S-Brush Link");
  NimBLEDevice::setPower(ESP_PWR_LVL_N6);
  NimBLEDevice::setSecurityAuth(false, false, true);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  // RX Characteristic - For receiving data from the client
  pRxChar = pService->createCharacteristic(
    RX_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());

  // TX Characteristic - For sending data to the client
  pTxChar = pService->createCharacteristic(
    TX_CHAR_UUID,
    NIMBLE_PROPERTY::NOTIFY
  );

  pService->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->enableScanResponse(true);
  pAdv->setMinInterval(200);
  pAdv->setMaxInterval(300);

  NimBLEDevice::startAdvertising();
  Serial.println("Advertising as 'S-Brush Link'...");
}

void loop() {
  // Send a ping message to the client every 5 seconds if connected
  if (deviceConnected) {
    if (millis() - lastMsgTime > 5000) {
      lastMsgTime = millis();
      String msg = "Ping from ESP32! Uptime: " + String(millis() / 1000) + "s";
      pTxChar->setValue(msg.c_str());
      pTxChar->notify();
      Serial.println("Sent: " + msg);
    }
  }
  
  delay(10);
}