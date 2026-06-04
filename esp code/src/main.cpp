#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>

// ─── BLE UUIDs ─────────────────────────────────────────────────────────────
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define RX_CHAR_UUID        "12345678-1234-1234-1234-000000000001" // Client writes to this
#define TX_CHAR_UUID        "12345678-1234-1234-1234-000000000002" // Client receives from this

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTxChar = nullptr;
NimBLECharacteristic* pRxChar = nullptr;
bool deviceConnected = false;
unsigned long lastMsgTime = 0;


void ble_send(String msg){
  pTxChar->setValue(msg.c_str());
  pTxChar->notify();
}



std::vector<String> split_str(String str, char delimiter) {
  std::vector<String> result;
  int startIdx = 0;
  int endIdx = str.indexOf(delimiter);
  
  while (endIdx != -1) {
    result.push_back(str.substring(startIdx, endIdx));
    startIdx = endIdx + 1;
    endIdx = str.indexOf(delimiter, startIdx);
  }
  
  if (startIdx < str.length()) {
    result.push_back(str.substring(startIdx));
  }
  
  return result;
}

void code_upload(String code) {
  std::vector<String> parts = split_str(code, '\n');
  for(size_t i = 0; i < parts.size(); i++) {
    ble_send(parts[i]+'\n');
  }
}

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
      if(val[0] == 'a'){
        code_upload(String(val.c_str()));
      }
    }
  }
};

void setup() {
  setCpuFrequencyMhz(80); // Lower CPU frequency to reduce heat
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

  // pService->start(); // Deprecated in newer NimBLE versions

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
      ble_send(msg);
    }
  }
  
  delay(10);
}