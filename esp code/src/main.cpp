#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <vector>

// ─── Pin Definitions ───────────────────────────────────────────────────────
const int POWER_BUTTON_PIN  = 3;
const int ACTION_BUTTON_PIN = 9;
const int LED1_PIN          = 10;
const int LED2_PIN          = 8;

// ─── BLE UUIDs ─────────────────────────────────────────────────────────────
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define RX_CHAR_UUID        "12345678-1234-1234-1234-000000000001"
#define TX_CHAR_UUID        "12345678-1234-1234-1234-000000000002"

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTxChar = nullptr;
NimBLECharacteristic* pRxChar = nullptr;
bool deviceConnected = false;
std::vector<String> receiveBuffer;
unsigned long bothPressedTime = 0;

// ─── Dynamic Variable System ───────────────────────────────────────────────
enum VarType { VAR_INT, VAR_FLOAT, VAR_BOOL, VAR_STRING, VAR_ARRAY };

struct DynamicVar {
  VarType type;
  
  int i_val;
  float f_val;
  bool b_val;
  String s_val;
  std::vector<DynamicVar> a_val;

  DynamicVar() : type(VAR_INT), i_val(0) {} 
  DynamicVar(int v) : type(VAR_INT), i_val(v) {}
  DynamicVar(long v) : type(VAR_INT), i_val((int)v) {}
  DynamicVar(float v) : type(VAR_FLOAT), f_val(v) {}
  DynamicVar(bool v) : type(VAR_BOOL), b_val(v) {}
  DynamicVar(String v) : type(VAR_STRING), s_val(v) {}
  DynamicVar(const char* v) : type(VAR_STRING), s_val(v) {}
  DynamicVar(std::vector<DynamicVar> v) : type(VAR_ARRAY), a_val(v) {}
};

struct NamedVariable {
  String name;
  DynamicVar value;
};

std::vector<NamedVariable> global_vars;

// Sets or completely overwrites a root variable
void set_var(String var_name, DynamicVar val) {
  for (int i = 0; i < global_vars.size(); i++) {
    if (global_vars[i].name == var_name) {
      global_vars[i].value = val;
      return;
    }
  }
  global_vars.push_back({var_name, val});
}

// Returns a copy of a variable (safest for reading)
DynamicVar get_var(String var_name) {
  for (int i = 0; i < global_vars.size(); i++) {
    if (global_vars[i].name == var_name) {
      return global_vars[i].value;
    }
  }
  return DynamicVar();
}

// Returns a direct pointer to the root variable
DynamicVar* get_var_ptr(String var_name) {
  for (int i = 0; i < global_vars.size(); i++) {
    if (global_vars[i].name == var_name) {
      return &global_vars[i].value;
    }
  }
  return nullptr; 
}

// Digs into nested arrays using a comma-separated path (e.g., "1,0")
DynamicVar* get_nested_ptr(String var_name, String index_path) {
  DynamicVar* current = get_var_ptr(var_name);
  
  int depth = 0;
  while (current != nullptr) {
    // Utility split_str is defined below, but we need it here. 
    // We will inline the logic or use a quick local search for the comma.
    int comma_idx = index_path.indexOf(',');
    String idx_str;
    if (comma_idx == -1) {
      idx_str = index_path;
      index_path = ""; // Clear it so loop breaks next time
    } else {
      idx_str = index_path.substring(0, comma_idx);
      index_path = index_path.substring(comma_idx + 1);
    }
    
    if (idx_str.length() == 0) break;
    
    int idx = idx_str.toInt();
    if (current->type == VAR_ARRAY && idx >= 0 && idx < current->a_val.size()) {
      current = &current->a_val[idx]; 
    } else {
      return nullptr;
    }
    depth++;
  }
  return current;
}

// Deletes a variable completely from memory
void delete_var(String var_name) {
  for (int i = 0; i < global_vars.size(); i++) {
    if (global_vars[i].name == var_name) {
      global_vars.erase(global_vars.begin() + i);
      return;
    }
  }
}

// ─── Interpreter Globals ───────────────────────────────────────────────────
int pc = 0; 
String active_script_code = "";
String active_script_name = "";

// ─── Forward Declarations ──────────────────────────────────────────────────
void interpreter(const char* scriptName); 

// ─── Utility Functions ─────────────────────────────────────────────────────
void ble_send(String msg){
  if(pTxChar != nullptr) {
    pTxChar->setValue(msg.c_str());
    pTxChar->notify();
  }
}

String split_str(String data, char separator, int index) {
  int found = 0;
  int startIndex = 0;
  int endIndex = data.indexOf(separator);
  
  while (endIndex != -1) {
    if (found == index) {
      return data.substring(startIndex, endIndex);
    }
    found++;
    startIndex = endIndex + 1;
    endIndex = data.indexOf(separator, startIndex);
  }
  
  if (found == index) {
    return data.substring(startIndex);
  }
  return "";
}

// ─── NVS Code Upload ───────────────────────────────────────────────────────
void code_upload(String code) {
  Preferences prefs;
  int numTokens = 1;
  for (int i = 0; i < code.length(); i++) {
    if (code[i] == '\n') numTokens++;
  }

  for (int i = 1; i < numTokens; i++) {
    String part = split_str(code, '\n', i);
    if (part.length() < 1) continue;

    char type = part[0];
    String rest = part.substring(1); 

    int sep = rest.indexOf('\r');
    String key, val;
    if (sep >= 0) {
      key = rest.substring(0, sep);
      val = rest.substring(sep + 1);
    } else {
      key = ""; 
      val = rest;
    }

    if (key.length() == 0) key = "default";
    if (key.length() > 15) key = key.substring(0, 15);

    if (type == 'c') {
      prefs.begin("codes", false);
      bool ok = prefs.putBytes(key.c_str(), val.c_str(), val.length());
      prefs.end();
      Serial.printf("[NVS] code '%s' saved (%u bytes) ok=%d\n", key.c_str(), val.length(), ok);
      ble_send("ok:c:" + key);
    } else if (type == 'i') {
      prefs.begin("imgs", false);
      bool ok = prefs.putBytes(key.c_str(), val.c_str(), val.length());
      prefs.end();
      Serial.printf("[NVS] img  '%s' saved (%u bytes) ok=%d\n", key.c_str(), val.length(), ok);
      ble_send("ok:i:" + key);
    }
  }
}

// ─── BLE Callbacks ─────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("BLE Client connected");
  }
  void onDisconnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("BLE Client disconnected.");
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string val = pChar->getValue();
    if (val.length() > 0) {
      receiveBuffer.push_back(String(val.c_str()));
    }
  }
};

// ─── Setup & Loop ──────────────────────────────────────────────────────────
void setup() {
  setCpuFrequencyMhz(80);
  pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ACTION_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);
  Serial.begin(115200);

  NimBLEDevice::init("S-Brush Link");
  NimBLEDevice::setPower(ESP_PWR_LVL_N6);
  NimBLEDevice::setSecurityAuth(false, false, true);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  pRxChar = pService->createCharacteristic(
    RX_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());

  pTxChar = pService->createCharacteristic(
    TX_CHAR_UUID,
    NIMBLE_PROPERTY::NOTIFY
  );

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->enableScanResponse(true);
  pAdv->setMinInterval(200);
  pAdv->setMaxInterval(300);

  NimBLEDevice::startAdvertising();
}

void loop() {
  if (!receiveBuffer.empty()) {
    String pending = receiveBuffer.front();
    receiveBuffer.erase(receiveBuffer.begin());
    if (pending.length() > 0 && pending[0] == 'a') {
      code_upload(pending);
    }
  }

  bool powerPressed  = (digitalRead(POWER_BUTTON_PIN) == LOW);
  bool actionPressed = (digitalRead(ACTION_BUTTON_PIN) == LOW);
  digitalWrite(LED1_PIN, powerPressed  ? HIGH : LOW);
  digitalWrite(LED2_PIN, actionPressed ? HIGH : LOW);

  if (powerPressed && actionPressed) {
    if (bothPressedTime == 0) {
      bothPressedTime = millis();
    } else if (millis() - bothPressedTime > 3000) {
      if (pServer != nullptr) {
        std::vector<uint16_t> peerHandles = pServer->getPeerDevices();
        for (uint16_t connHandle : peerHandles) {
          pServer->disconnect(connHandle);
        }
        delay(100);
      }
      NimBLEDevice::deinit(true);
      delay(500);
      ESP.restart();
    }
  } else {
    bothPressedTime = 0;
  }

  interpreter("main");
  delay(10);
}

// ─── Non-Blocking Interpreter ──────────────────────────────────────────────
void interpreter(const char* scriptName) {
  if (active_script_name != String(scriptName)) {
    Preferences prefs;
    prefs.begin("codes", true); 

    size_t codeLen = prefs.getBytesLength(scriptName);
    if (codeLen == 0) {
      prefs.end();
      return; 
    }

    char* buf = (char*)malloc(codeLen + 1);
    if (!buf) {
      prefs.end();
      return; 
    }
    
    prefs.getBytes(scriptName, buf, codeLen);
    prefs.end();
    buf[codeLen] = '\0';

    active_script_code = String(buf);
    active_script_name = String(scriptName);
    free(buf);
    
    pc = 0; 
    global_vars.clear(); 
  }

  String line = split_str(active_script_code, '\n', pc);

  if (line.length() == 0) {
    pc = 0; 
    return;
  }

  String cmd = split_str(line, '\r', 0);

  // ─── Commands ───
  if (cmd == "goto") {
    // Format: goto\r<line_number>
    pc = split_str(line, '\r', 1).toInt();
    return; 
  }
  
  else if (cmd == "set") {
    // Format: set\r<var_name>\r<index_path>\r<value>
    // Overwrite root var:  set\rhealth\r\r100
    // Edit array index:    set\rinventory\r1,0\rSword
    String var_name   = split_str(line, '\r', 1);
    String index_path = split_str(line, '\r', 2);
    String value_str  = split_str(line, '\r', 3);
    
    // Automatically detect if the value is an int or string
    DynamicVar new_val;
    if (value_str.toInt() != 0 || value_str == "0") {
      new_val = DynamicVar(value_str.toInt());
    } else {
      new_val = DynamicVar(value_str);
    }

    if (index_path == "") {
      // Empty path means we overwrite or create the root variable
      set_var(var_name, new_val);
    } else {
      // Path provided, dig into the array and overwrite the specific index
      DynamicVar* target = get_nested_ptr(var_name, index_path);
      if (target != nullptr) {
        *target = new_val; 
      } else {
        Serial.println("[Interpreter] set failed: Invalid path or out of bounds!");
      }
    }
  }

  
  else if (cmd == "create_array") {
    // Format: create_array\r<var_name>
    String var_name = split_str(line, '\r', 1);
    
    // Creates a brand new, empty vector
    std::vector<DynamicVar> empty_array;
    set_var(var_name, DynamicVar(empty_array));
  }

  else if (cmd == "push") {
    // Format: push\r<array_name>\r<index_path>\r<value>
    // This adds a new item to the END of an array
    String var_name   = split_str(line, '\r', 1);
    String index_path = split_str(line, '\r', 2);
    String value_str  = split_str(line, '\r', 3);

    // Get the array pointer
    DynamicVar* target;
    if (index_path == "") {
      target = get_var_ptr(var_name);
    } else {
      target = get_nested_ptr(var_name, index_path);
    }

    if (target != nullptr && target->type == VAR_ARRAY) {
      if (value_str.toInt() != 0 || value_str == "0") {
        target->a_val.push_back(DynamicVar(value_str.toInt()));
      } else {
        target->a_val.push_back(DynamicVar(value_str));
      }
    }
  }


  else if (cmd == "del") {
    // Format: del\r<var_name>
    String var_name = split_str(line, '\r', 1);
    delete_var(var_name);
  }

  else if (cmd == "print") {
    // Format: print\r<var_name>
    String var_name = split_str(line, '\r', 1);
    DynamicVar target = get_var(var_name);
    
    String out_msg = "";
    if (target.type == VAR_INT) {
      out_msg = String(target.i_val);
    } else if (target.type == VAR_STRING) {
      out_msg = target.s_val;
    } else if (target.type == VAR_ARRAY) {
      out_msg = "[Array]";
    } else {
      out_msg = "null";
    }
    
    ble_send(out_msg);
  }

  pc++;
}