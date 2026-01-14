#include "StateMachine.h"
#include "CRC32.h"
#include "PacketDef.h"

// Timing constants (ms)
#define T_BATTERY_CHECK    30000   // Check battery every 30s
#define VOLTAGE_THRESHOLD   3.6    // Voltage threshold (V)

HardwareSerial ns(1);

String getStateName(BridgeState s) {
    switch(s) {
        case ST_BOOT:              return "ST_BOOT";
        case ST_NORMAL:            return "ST_NORMAL (Always-On)";
        default: return "UNKNOWN";
    }
}

StateMachine::StateMachine() {
    _state = ST_BOOT;
    _uart = &ns;
    _nodeState = NodeState();
    _nodeState.device_ID = MY_DEVICE_NAME;
    _lastHeartbeat = 0;
}

void StateMachine::begin() {
    _pwr.begin();
    pinMode(PIN_NODE_TRIGGER, OUTPUT); 
    digitalWrite(PIN_NODE_TRIGGER, LOW);
    pinMode(PIN_NODE_STATUS, INPUT);

    // UART 921600, Buffer lớn
    _uart->begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    _uart->setRxBufferSize(4096);
    _uart->setTimeout(10);

    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    
    // ✅ FIX: Don't hang on LoRa init failure - allow recovery
    int loraRetries = 3;
    while (loraRetries-- > 0) {
        if (LoRa.begin(LORA_FREQ)) {
            Serial.println("[LORA] Initialized successfully");
            break;
        }
        Serial.printf("[WARN] LoRa init failed, retries left: %d\n", loraRetries);
        delay(500);
    }
    
    if (loraRetries <= 0) {
        Serial.println("[ERR] LoRa failed after 3 retries - continuing anyway");
        delay(2000);  // Brief delay before continuing
    }
    
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.enableCrc();
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.receive();  // LoRa luôn ở trạng thái nhận 24/7

    _lastHeartbeat = millis();
    Serial.println("=== BRIDGE STARTED - Always-On Mode (No Sleep) ===");
    Serial.println("[INFO] Bridge stays on 24/7, LoRa RX listening");
    Serial.println("[INFO] Node handles deep_sleep via EN:0 command");
    _setState(ST_NORMAL);
}

void StateMachine::_setState(BridgeState s) {
    if (_state != s) {
        Serial.printf("[STATE] %s -> %s\n", getStateName(_state).c_str(), getStateName(s).c_str());
        _state = s;
        _lastHeartbeat = millis();
    }
}

void StateMachine::loop() {
    _handleLoRa();    // Always listen LoRa 24/7
    _handleUart();    // Always receive from Node
    _checkBatteryPeriodic();

    switch (_state) {
        case ST_NORMAL:
            // Bridge always on, acts as transparent gateway
            // - MQTT commands via LoRa → Forward to Node via UART (EN commands)
            // - Node data via UART → Forward to Gateway via LoRa
            // - If battery low: Send EN:0 to Node (Node handles deep_sleep)
            if (_pwr.isBatteryLow()) {
                Serial.println("[PWR] Low Battery -> Send EN:0 to Node for deep_sleep");
                JsonDocument doc;
                doc["device_ID"] = MY_DEVICE_NAME;
                doc["EN"] = 0;
                doc["type"] = "command";
                _sendUart(doc);  // Node will handle EN:0 -> esp_deep_sleep_start()
            }
            break;

        case ST_BOOT:
            _setState(ST_NORMAL);
            break;

        default:
            break;
    }
}

// --- DEVICE_ID FILTERING ---
bool StateMachine::_isCommandForMe(const JsonDocument& doc) {
    // Check if command has device_ID field
    if (doc["device_ID"].isNull()) {
        // No device_ID specified - accept it (legacy support)
        Serial.println("[CMD] No device_ID in command (legacy) - accepting");
        return true;
    }
    
    // Extract device_ID from command
    String cmdDeviceId = doc["device_ID"].is<String>() ? doc["device_ID"].as<String>() : "";
    
    Serial.printf("[CMD] Checking device_ID: command=%s, mine=%s\n", cmdDeviceId.c_str(), BRIDGE_DEVICE_ID);
    
    // Check if it matches this Bridge's device_ID
    if (cmdDeviceId == BRIDGE_DEVICE_ID) {
        Serial.printf("[CMD] ✓ MATCH! Command for me: device_ID=%s\n", cmdDeviceId.c_str());
        return true;
    }
    
    // Command is for another Bridge - ignore it
    Serial.printf("[CMD] ✗ MISMATCH! Command for other Bridge: cmd=%s vs mine=%s - IGNORING\n", 
                  cmdDeviceId.c_str(), BRIDGE_DEVICE_ID);
    return false;
}

void StateMachine::_addDeviceIdToResponse(JsonDocument& doc) {
    // Add this Bridge's device_ID to all responses
    if (doc["device_ID"].isNull()) {
        doc["device_ID"] = BRIDGE_DEVICE_ID;
    }
}

void StateMachine::_handleLoRa() {
    int size = LoRa.parsePacket();
    if (!size) return;
    
    Serial.printf("[LORA] Packet received: %d bytes\n", size);

    uint8_t buf[256];
    int len = 0;
    while(LoRa.available() && len < 256) {
        buf[len++] = LoRa.read();
    }
    
    Serial.printf("[LORA] Read: %d bytes from FIFO\n", len);

    PacketHeader h;
    uint8_t decrypted[256];
    int dLen = Security::decryptBinary(buf, len, h, decrypted, MY_AES_KEY);
    if (dLen < 0) {
        Serial.printf("[LORA] Decrypt fail (error code: %d)\n", dLen);
        LoRa.receive();
        return;
    }

    Serial.printf("[LORA] Decrypted: %d bytes\n", dLen);

    JsonDocument doc;
    PacketUtils::decodeBinaryToJson(decrypted, dLen, doc);
    
    const char* cmdType = doc["type"].is<const char*>() ? doc["type"].as<const char*>() : "UNKNOWN";
    const char* cmdDevId = doc["device_ID"].is<const char*>() ? doc["device_ID"].as<const char*>() : "NONE";
    
    Serial.printf("[LORA RX] type=%s, device_ID=%s\n", cmdType, cmdDevId);
    
    // Log all fields in the received command
    JsonObject obj = doc.as<JsonObject>();
    Serial.print("[LORA RX] Fields: ");
    for (JsonPair p : obj) {
        Serial.printf("%s ", p.key().c_str());
    }
    Serial.println();
    
    _procGwCmd(doc);
    LoRa.receive();  // Quay lại nhận ngay
}

// Translate Node-RED command format to internal format
void StateMachine::_translateNodeRedCmd(JsonDocument& doc) {
    // Command 1: set_state -> type (measure or stop)
    if (doc["set_state"].is<const char*>()) {
        const char* state = doc["set_state"];
        doc["type"] = state;  // "measure" or "stop"
        
        // If measure, rename cycle_manual to cycle
        if (String(state) == "measure" && doc["cycle_manual"].is<int>()) {
            doc["cycle"] = doc["cycle_manual"];
            Serial.printf("[TRANS] set_state:measure cycle_manual=%d -> cycle=%d\n", 
                         doc["cycle_manual"].as<int>(), doc["cycle"].as<int>());
        }
        return;
    }
    
    // Command 2: set_door -> type
    if (doc["set_door"].is<const char*>()) {
        const char* action = doc["set_door"];
        doc["type"] = "door";
        doc["action"] = action;  // "open" or "close"
        Serial.printf("[TRANS] set_door:%s -> type:door action:%s\n", action, action);
        return;
    }
    
    // Command 3: set_fans -> type
    if (doc["set_fans"].is<const char*>()) {
        const char* action = doc["set_fans"];
        doc["type"] = "fan";
        doc["action"] = action;  // "on" or "off"
        Serial.printf("[TRANS] set_fans:%s -> type:fan action:%s\n", action, action);
        return;
    }
    
    // Command 4: measures_per_day (auto config)
    if (doc["measures_per_day"].is<int>()) {
        doc["type"] = "config";
        int count = doc["measures_per_day"];
        Serial.printf("[TRANS] measures_per_day:%d -> type:config\n", count);
        return;
    }
    
    // If no translation needed (EN field, type field already present, etc.)
    // Just use as-is
}

void StateMachine::_procGwCmd(JsonDocument& doc) {
    // Always-On Bridge Mode: Transparent gateway - no sleep logic
    // All commands from Gateway → forward to Node via LoRa
    
    _translateNodeRedCmd(doc);
    
    String type = doc["type"].is<String>() ? doc["type"].as<String>() : "";
    String deviceId = doc["device_ID"].is<String>() ? doc["device_ID"].as<String>() : "";
    
    Serial.printf("[GW_CMD] type=%s, device_ID=%s\n", type.c_str(), deviceId.c_str());
    
    // TIME_REQ and TIME_RES: Bridge forwards time sync requests/responses to Node
    if (type == "time_req" || type == "time_res") {
        Serial.printf("[GW_CMD] %s -> Forward to Node\n", type.c_str());
        _sendUart(doc);
        return;
    }
    
    // STATUS_REQ: Bridge responds with current machine status
    if (type == "status_req") {
        Serial.println("[GW_CMD] status_req -> Send Bridge status");
        _sendReport(0);  // Always-on mode, sleep_mode=0
        return;
    }
    
    // Device ID filtering: Only process commands for this Bridge
    if (!deviceId.isEmpty() && !_isCommandForMe(doc)) {
        return;
    }
    
    // All other commands (EN:0, EN:1, door, fan, cycle, etc.) → Forward to Node
    // Node handles EN:0 → deep_sleep directly via esp_deep_sleep_start()
    if (!type.isEmpty() && type != "ack_status") {
        Serial.printf("[GW_CMD] Forward command to Node: %s\n", type.c_str());
        _sendUart(doc);
        return;
    }
}

void StateMachine::_handleUart() {
    while (_uart->available()) {
        String s = _uart->readStringUntil('\n');
        s.trim();
        if (s.length() == 0) continue;
        
        // Tìm CRC (ngăn cách bằng |)
        int split = s.lastIndexOf('|');
        if (split == -1) {
            // Log từ Node
            Serial.print("[NODE LOG] ");
            Serial.println(s);
            continue;
        }
        
        String json = s.substring(0, split);
        String crcS = s.substring(split + 1);
        
        // Kiểm tra CRC
        uint32_t expectedCrc = strtoul(crcS.c_str(), NULL, 16);
        uint32_t actualCrc = CRC32::calculate(json);
        
        if (actualCrc != expectedCrc) {
            Serial.printf("[ERR] CRC Fail: %lX vs %lX\n", expectedCrc, actualCrc);
            continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, json) != DeserializationError::Ok) {
            Serial.printf("[ERR] JSON parse fail: %s\n", json.c_str());
            continue;
        }
        
        Serial.printf("[UART RX] type=%s\n", doc["type"].as<const char*>());
        _procNodeJson(doc);
    }
}

void StateMachine::_procNodeJson(JsonDocument& doc) {
    // Always-On Bridge Mode: Simply forward/relay Node data to Gateway
    String type = doc["type"].as<String>();
    
    // Data and time requests: Forward directly to Gateway via LoRa
    if (type == "data" || type == "time_req") {
        Serial.printf("[NODE->GW] Forward %s to Gateway\n", type.c_str());
        
        if (type == "time_req" && doc["device_ID"].isNull()) {
            doc["device_ID"] = MY_DEVICE_NAME;
        }
        
        _sendLoRa(doc);
        return;
    }
    
    // Machine status: Cache Node state and relay to Gateway
    if (type == "machine_status") {
        Serial.println("[NODE] Received machine_status - caching and forwarding");
        
        // Update local cache of Node state
        if (!doc["door_status"].isNull()) {
            _nodeState.door_status = doc["door_status"].as<String>();
        }
        if (!doc["fan_status"].isNull()) {
            _nodeState.fan_status = doc["fan_status"].as<String>();
        }
        if (!doc["mode"].isNull()) {
            _nodeState.mode = doc["mode"].as<String>();
        }
        if (!doc["measuring"].isNull()) {
            _nodeState.measuring = doc["measuring"].as<String>();
        }
        if (!doc["timestamp"].isNull()) {
            _nodeState.timestamp = doc["timestamp"].as<String>();
        }
        
        // Relay to Gateway
        _sendReport(0);  // Always-on mode: sleep=0
        return;
    }
    
    // ACK: Log for debugging (no action needed in always-on mode)
    if (type == "ack") {
        Serial.println("[NODE] ACK received");
        return;
    }
}

void StateMachine::_sendReport(bool sleep) {
    JsonDocument doc;
    doc["type"] = "machine_status";
    
    // Use Node's device_ID (this is Node data being relayed)
    doc["device_ID"] = _nodeState.device_ID.isEmpty() ? NODE_DEVICE_ID : _nodeState.device_ID;
    
    // ALL 9 fields always sent
    doc["door_status"] = _nodeState.door_status.isEmpty() ? "unknown" : _nodeState.door_status;
    doc["fan_status"] = _nodeState.fan_status.isEmpty() ? "unknown" : _nodeState.fan_status;
    doc["mode"] = _nodeState.mode.isEmpty() ? "manual" : _nodeState.mode;
    doc["measuring"] = _nodeState.measuring.isEmpty() ? "NO" : _nodeState.measuring;
    doc["timestamp"] = _nodeState.timestamp.isEmpty() ? "0" : _nodeState.timestamp;
    
    // Bridge measures Pin voltage
    float voltage = _pwr.getVoltage();
    doc["Pin"] = (voltage > 0) ? voltage : 3.6;
    
    // Sleep status
    doc["Sleep_Mode"] = sleep ? 1 : 0;
    
    // DEBUG: Log JSON being sent
    String jsonStr;
    serializeJson(doc, jsonStr);
    Serial.printf("[SEND] Machine Status from Bridge %s: %s\n", BRIDGE_DEVICE_ID, jsonStr.c_str());
    
    _sendLoRa(doc);
}

void StateMachine::_sendLoRa(JsonDocument& doc) {
    // Thêm device_ID nếu chưa có
    if (doc["device_ID"].isNull()) {
        doc["device_ID"] = MY_DEVICE_NAME;
    }
    
    // Encode JSON -> Binary
    uint8_t buf[256];
    int len = PacketUtils::encodeJsonToBinary(doc, buf, 256);
    if (len <= 0) {
        // ✅ FALLBACK: Send as plain JSON with CRC (same as _sendUart)
        // Use static buffer to avoid stack overflow
        static char jsonBuf[512];
        serializeJson(doc, jsonBuf, sizeof(jsonBuf));
        
        uint32_t crc = CRC32::calculate(jsonBuf);
        
        // Send: #<json>|<crc>
        LoRa.beginPacket();
        LoRa.write((uint8_t)'#');  // JSON marker
        LoRa.write((uint8_t*)jsonBuf, strlen(jsonBuf));
        LoRa.write((uint8_t)'|');
        
        // Write CRC as hex string
        char crcStr[10];
        snprintf(crcStr, sizeof(crcStr), "%lX", crc);
        LoRa.write((uint8_t*)crcStr, strlen(crcStr));
        
        LoRa.endPacket();
        
        Serial.printf("[LORA TX JSON] #%s|%s\n", jsonBuf, crcStr);
        LoRa.receive();
        return;
    }
    
    // Tạo header và mã hóa
    PacketHeader h;
    h.nodeId = MY_NODE_INDEX;
    h.counter = millis();
    
    uint8_t enc[256];
    int eLen = Security::encryptBinary(h, buf, len, enc, MY_AES_KEY);
    if (eLen <= 0) {
        Serial.println("[ERR] Encrypt failed");
        LoRa.receive();
        return;
    }
    
    // Gửi qua LoRa
    LoRa.beginPacket();
    LoRa.write(enc, eLen);
    LoRa.endPacket();
    
    Serial.printf("[LORA TX BIN] %d bytes sent\n", eLen);
    
    // Quay lại chế độ nhận
    LoRa.receive();
}

void StateMachine::_sendUart(JsonDocument& doc) {
    // Use binary encoding for efficiency (PacketDef)
    uint8_t buf[256];
    int len = PacketUtils::encodeJsonToBinary(doc, buf, 256);
    
    if (len <= 0) {
        // Fallback to JSON if binary encoding fails
        String s;
        serializeJson(doc, s);
        uint32_t crc = CRC32::calculate(s);
        _uart->printf("%s|%lX\n", s.c_str(), crc);
        Serial.printf("[UART TX JSON] %s|%lX\n", s.c_str(), crc);
        return;
    }
    
    // Send binary format
    _uart->write(buf, len);
    _uart->write('\n');  // Terminator
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    Serial.printf("[UART TX BIN] %d bytes: %s\n", len, jsonStr.c_str());
}

void StateMachine::_checkBatteryPeriodic() {
    static unsigned long lastCheck = 0;
    static bool noBatteryWarning = false;
    
    if (millis() - lastCheck > T_BATTERY_CHECK) {
        lastCheck = millis();
        
        float voltage = _pwr.getVoltage();
        
        // No battery connected (voltage = 0)
        if (voltage <= 0) {
            if (!noBatteryWarning) {
                Serial.println("[PWR] No battery detected - running on external power only");
                noBatteryWarning = true;
            }
            return;  // Silent mode - don't check further without battery
        }
        
        noBatteryWarning = false;  // Reset flag
        
        // Detect power source
        if (_pwr.isAdapterConnected()) {
            // Battery not connected, using external power (adapter)
            Serial.printf("[PWR] External power detected (V=%.2fV < 0.3V)\n", voltage);
            Serial.println("[PWR] Running on adapter power - battery assumed fully charged");
        } else {
            // Battery connected
            Serial.printf("[PWR] Battery voltage: %.2fV\n", voltage);
            
            // Always-On Mode: If battery critically low, send EN:0 to Node for deep_sleep
            // Bridge itself stays on - Node handles its own deep_sleep
            if (voltage < VOLTAGE_THRESHOLD) {
                Serial.printf("[PWR] CRITICAL - Battery %.2fV < %.1fV threshold\n", voltage, VOLTAGE_THRESHOLD);
                Serial.println("[PWR] Sending EN:0 to Node for deep_sleep");
                JsonDocument doc;
                doc["device_ID"] = MY_DEVICE_NAME;
                doc["EN"] = 0;
                doc["type"] = "command";
                _sendUart(doc);  // Node handles EN:0 → esp_deep_sleep_start()
            }
        }
    }
}

void StateMachine::_wakeNodeWithPulse() {
    // Phát pulse qua PIN_NODE_TRIGGER
    digitalWrite(PIN_NODE_TRIGGER, HIGH);
    delay(50);
    digitalWrite(PIN_NODE_TRIGGER, LOW);
    delay(200);
    Serial.println("[PULSE] Wake pulse sent to Node");
}