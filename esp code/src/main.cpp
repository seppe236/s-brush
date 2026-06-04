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

NimBLEServer*           pServer        = nullptr;
NimBLECharacteristic*   pTxChar        = nullptr;
NimBLECharacteristic*   pRxChar        = nullptr;
bool                    deviceConnected = false;
unsigned long           bothPressedTime = 0;

// ─── BLE Chunk Reassembly ──────────────────────────────────────────────────
// Messages are framed as:  L<length>|<payload>
// e.g.  L47|a\ncmain\rdigital_write\r10\r1|wait\r500|goto\r0
// If no length header is present, falls back to a 500 ms idle timeout.
String        bleAssemblyBuffer = "";
unsigned long lastChunkTime     = 0;
const unsigned long CHUNK_TIMEOUT_MS = 500;

// ─── Dynamic Variable System ───────────────────────────────────────────────
enum VarType { VAR_INT, VAR_FLOAT, VAR_BOOL, VAR_STRING, VAR_ARRAY };

struct DynamicVar {
  VarType type;
  int     i_val;
  float   f_val;
  bool    b_val;
  String  s_val;
  std::vector<DynamicVar> a_val;

  DynamicVar()                          : type(VAR_INT),    i_val(0)      {}
  DynamicVar(int v)                     : type(VAR_INT),    i_val(v)      {}
  DynamicVar(long v)                    : type(VAR_INT),    i_val((int)v) {}
  DynamicVar(float v)                   : type(VAR_FLOAT),  f_val(v)      {}
  DynamicVar(bool v)                    : type(VAR_BOOL),   b_val(v)      {}
  DynamicVar(String v)                  : type(VAR_STRING), s_val(v)      {}
  DynamicVar(const char* v)             : type(VAR_STRING), s_val(v)      {}
  DynamicVar(std::vector<DynamicVar> v) : type(VAR_ARRAY),  a_val(v)      {}
};

struct NamedVariable {
  String     name;
  DynamicVar value;
};

std::vector<NamedVariable> global_vars;

void set_var(String var_name, DynamicVar val) {
  for (int i = 0; i < (int)global_vars.size(); i++) {
    if (global_vars[i].name == var_name) {
      global_vars[i].value = val;
      return;
    }
  }
  global_vars.push_back({var_name, val});
}

DynamicVar get_var(String var_name) {
  for (int i = 0; i < (int)global_vars.size(); i++) {
    if (global_vars[i].name == var_name) return global_vars[i].value;
  }
  return DynamicVar();
}

DynamicVar* get_var_ptr(String var_name) {
  for (int i = 0; i < (int)global_vars.size(); i++) {
    if (global_vars[i].name == var_name) return &global_vars[i].value;
  }
  return nullptr;
}

DynamicVar* get_nested_ptr(String var_name, String index_path) {
  DynamicVar* current = get_var_ptr(var_name);
  while (current != nullptr) {
    int    comma_idx = index_path.indexOf(',');
    String idx_str;
    if (comma_idx == -1) {
      idx_str    = index_path;
      index_path = "";
    } else {
      idx_str    = index_path.substring(0, comma_idx);
      index_path = index_path.substring(comma_idx + 1);
    }
    if (idx_str.length() == 0) break;
    int idx = idx_str.toInt();
    if (current->type == VAR_ARRAY && idx >= 0 && idx < (int)current->a_val.size()) {
      current = &current->a_val[idx];
    } else {
      return nullptr;
    }
  }
  return current;
}

void delete_var(String var_name) {
  for (int i = 0; i < (int)global_vars.size(); i++) {
    if (global_vars[i].name == var_name) {
      global_vars.erase(global_vars.begin() + i);
      return;
    }
  }
}

// ─── Interpreter Globals ───────────────────────────────────────────────────
int    pc                 = 0;
String active_script_code = "";
String active_script_name = "";

unsigned long wait_start_time = 0;
unsigned long wait_duration   = 0;
bool          is_waiting      = false;

bool check_delay() {
  if (!is_waiting) return true;
  if (millis() - wait_start_time >= wait_duration) {
    is_waiting = false;
    return true;
  }
  return false;
}

// ─── Forward Declarations ──────────────────────────────────────────────────
void interpreter(const char* scriptName);

// ─── Utility Functions ─────────────────────────────────────────────────────
void ble_send(String msg) {
  if (pTxChar != nullptr) {
    pTxChar->setValue(msg.c_str());
    pTxChar->notify();
  }
}

String split_str(String data, char separator, int index) {
  int found      = 0;
  int startIndex = 0;
  int endIndex   = data.indexOf(separator);
  while (endIndex != -1) {
    if (found == index) return data.substring(startIndex, endIndex);
    found++;
    startIndex = endIndex + 1;
    endIndex   = data.indexOf(separator, startIndex);
  }
  if (found == index) return data.substring(startIndex);
  return "";
}

// ─── NVS Code Upload ───────────────────────────────────────────────────────
void code_upload(String code) {
  Preferences prefs;
  int numTokens = 1;
  for (int i = 0; i < (int)code.length(); i++) {
    if (code[i] == '\n') numTokens++;
  }

  for (int i = 1; i < numTokens; i++) {
    String part = split_str(code, '\n', i);
    if (part.length() < 1) continue;

    char   type = part[0];
    String rest = part.substring(1);

    int    sep = rest.indexOf('\r');
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
      prefs.putBytes(key.c_str(), val.c_str(), val.length());
      prefs.end();
      ble_send("ok:c:" + key);

      // Force reload if this script is currently running
      if (active_script_name == key) {
        active_script_name = "";
      }
    } else if (type == 'i') {
      prefs.begin("imgs", false);
      prefs.putBytes(key.c_str(), val.c_str(), val.length());
      prefs.end();
      ble_send("ok:i:" + key);
    }
  }
}

// ─── Commit a fully-assembled BLE message ─────────────────────────────────
void process_ble_message(String msg) {
  ble_send("[RX] " + String(msg.length()) + " bytes");
  if (msg.length() > 0 && msg[0] == 'a') {
    code_upload(msg);
  }
}

// ─── BLE Callbacks ─────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo) override {
    deviceConnected     = true;
    bleAssemblyBuffer   = "";
    // Request larger MTU so fewer chunks are needed
    pSvr->setDataLen(connInfo.getConnHandle(), 251);
  }
  void onDisconnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected   = false;
    bleAssemblyBuffer = "";
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string val = pChar->getValue();
    if (val.length() > 0) {
      bleAssemblyBuffer += String(val.c_str());
      lastChunkTime      = millis();
    }
  }
};

// ─── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  setCpuFrequencyMhz(80);
  pinMode(POWER_BUTTON_PIN,  INPUT_PULLUP);
  pinMode(ACTION_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED1_PIN,          OUTPUT);
  pinMode(LED2_PIN,          OUTPUT);
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
    NIMBLE_PROPERTY::WRITE   // acknowledged write — reliable delivery
  );
  pRxChar->setCallbacks(new RxCallbacks());

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
}

// ─── Loop ──────────────────────────────────────────────────────────────────
void loop() {

  // ── Framed message reassembly ─────────────────────────────────────────
  // Framed format:  L<payloadLength>|<payload>
  //   e.g.  L1178|a\ncmain\r...
  // Unframed fallback: wait CHUNK_TIMEOUT_MS after last chunk.
  if (bleAssemblyBuffer.length() > 0) {

    if (bleAssemblyBuffer[0] == 'L') {
      // ── Framed path: wait until we have the declared number of bytes ──
      int sep = bleAssemblyBuffer.indexOf('|');
      if (sep > 0) {
        int    expected = bleAssemblyBuffer.substring(1, sep).toInt();
        String payload  = bleAssemblyBuffer.substring(sep + 1);
        if ((int)payload.length() >= expected) {
          bleAssemblyBuffer = "";
          process_ble_message(payload.substring(0, expected));
        }
        // else: keep buffering — more chunks still in flight
      }
    } else {
      // ── Unframed fallback: commit after 500 ms idle ───────────────────
      if (millis() - lastChunkTime > CHUNK_TIMEOUT_MS) {
        String complete   = bleAssemblyBuffer;
        bleAssemblyBuffer = "";
        process_ble_message(complete);
      }
    }
  }

  // ── Buttons: hold both 3 s to reboot ──────────────────────────────────
  bool powerPressed  = (digitalRead(POWER_BUTTON_PIN)  == LOW);
  bool actionPressed = (digitalRead(ACTION_BUTTON_PIN) == LOW);

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
  yield();
}

// ─── Interpreter ───────────────────────────────────────────────────────────
void interpreter(const char* scriptName) {
  if (!check_delay()) return;

  // ── Load script from NVS if not already active ───────────────────────
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
    buf[codeLen]      = '\0';
    active_script_code = String(buf);
    active_script_name = String(scriptName);
    free(buf);
    pc = 0;
    global_vars.clear();
    ble_send("[interp] loaded '" + String(scriptName) + "' " + String(active_script_code.length()) + " bytes");
  }

  // ── Fetch current line ───────────────────────────────────────────────
  String line = split_str(active_script_code, '|', pc);

  if (line.length() == 0) {
    // Past end of script — loop back
    pc = 0;
    return;
  }

  String cmd = split_str(line, '\r', 0);

  // ── Commands ─────────────────────────────────────────────────────────

  if (cmd == "goto") {
    pc = split_str(line, '\r', 1).toInt();
    return;  // don't fall through to pc++
  }

  else if (cmd == "set") {
    String var_name   = split_str(line, '\r', 1);
    String index_path = split_str(line, '\r', 2);
    String value_str  = split_str(line, '\r', 3);
    DynamicVar new_val;
    if (value_str.toInt() != 0 || value_str == "0") {
      new_val = DynamicVar(value_str.toInt());
    } else {
      new_val = DynamicVar(value_str);
    }
    if (index_path == "") {
      set_var(var_name, new_val);
    } else {
      DynamicVar* target = get_nested_ptr(var_name, index_path);
      if (target != nullptr) *target = new_val;
    }
  }

  else if (cmd == "create_array") {
    String var_name = split_str(line, '\r', 1);
    std::vector<DynamicVar> empty_array;
    set_var(var_name, DynamicVar(empty_array));
  }

  else if (cmd == "push") {
    String var_name   = split_str(line, '\r', 1);
    String index_path = split_str(line, '\r', 2);
    String value_str  = split_str(line, '\r', 3);
    DynamicVar* target = (index_path == "")
      ? get_var_ptr(var_name)
      : get_nested_ptr(var_name, index_path);
    if (target != nullptr && target->type == VAR_ARRAY) {
      if (value_str.toInt() != 0 || value_str == "0") {
        target->a_val.push_back(DynamicVar(value_str.toInt()));
      } else {
        target->a_val.push_back(DynamicVar(value_str));
      }
    }
  }

  else if (cmd == "del") {
    delete_var(split_str(line, '\r', 1));
  }

  else if (cmd == "wait") {
    wait_duration    = split_str(line, '\r', 1).toInt();
    wait_start_time  = millis();
    is_waiting       = true;
    pc++;            // advance NOW so we don't re-trigger the wait
    return;
  }

  else if (cmd == "digital_write") {
    int pin   = split_str(line, '\r', 1).toInt();
    int state = split_str(line, '\r', 2).toInt();
    pinMode(pin, OUTPUT);
    digitalWrite(pin, state);
  }

  else if (cmd == "digital_write_all") {
    // Format: digital_write_all\r<pin1>\r<state1>\r<pin2>\r<state2>...
    // Sets multiple pins in the same interpreter tick — no flicker between them.
    int i = 1;
    while (true) {
      String pin_str   = split_str(line, '\r', i);
      String state_str = split_str(line, '\r', i + 1);
      if (pin_str.length() == 0) break;
      pinMode(pin_str.toInt(), OUTPUT);
      digitalWrite(pin_str.toInt(), state_str.toInt());
      i += 2;
    }
  }

  else if (cmd == "print") {
    String     var_name = split_str(line, '\r', 1);
    DynamicVar target   = get_var(var_name);
    String     out_msg;
    if      (target.type == VAR_INT)    out_msg = String(target.i_val);
    else if (target.type == VAR_FLOAT)  out_msg = String(target.f_val);
    else if (target.type == VAR_STRING) out_msg = target.s_val;
    else if (target.type == VAR_BOOL)   out_msg = target.b_val ? "true" : "false";
    else if (target.type == VAR_ARRAY)  out_msg = "[Array]";
    else                                out_msg = "null";
    ble_send(out_msg);
  }

  pc++;
}