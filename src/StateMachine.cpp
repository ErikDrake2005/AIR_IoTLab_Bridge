#include "StateMachine.h"

// Helper: Lấy ID số từ chuỗi config (VD: "BRIDGE_01" -> 1)
uint8_t getByteId(String idStr) {
    int lastIndex = idStr.length() - 1;
    while(lastIndex >= 0 && isDigit(idStr[lastIndex])) lastIndex--;
    String numStr = idStr.substring(lastIndex + 1);
    if (numStr.length() > 0) return (uint8_t)numStr.toInt();
    return 1; 
}

StateMachine::StateMachine() {
    _state = ST_BOOT;
    _uart = &Serial2; // Bridge dùng Serial2 nối với Node
    _packetCounter = 0;
}

void StateMachine::begin() {
    // 1. Init UART (Tốc độ cao để truyền JSON nhanh)
    _uart->begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    _uart->setRxBufferSize(4096);
    
    // 2. Init GPIO Handshake
    pinMode(PIN_NODE_TRIGGER, OUTPUT); // GPIO 26: Gọi Node dậy
    digitalWrite(PIN_NODE_TRIGGER, LOW);
    pinMode(PIN_NODE_STATUS, INPUT_PULLDOWN); // GPIO 25: Xem Node thức chưa
    
    // 3. Init Power & LoRa
    _pwr.begin();
    _tryLoRaInit();
    
    _state = ST_NORMAL;
    Serial.println("[BRIDGE] Ready. Listening...");
}

void StateMachine::loop() {
    _handleLoRa();
    _handleUart();
}

// =================================================================
// 1. XỬ LÝ DOWNLINK (LORA -> BRIDGE -> NODE)
// =================================================================
void StateMachine::_handleLoRa() {
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
        Serial.printf("[LORA RX] Size: %d, RSSI: %d\n", packetSize, LoRa.packetRssi());
        
        uint8_t buf[256];
        int idx = 0;
        while (LoRa.available() && idx < 256) buf[idx++] = LoRa.read();
        
        // A. Giải mã AES
        PacketHeader header;
        uint8_t decrypted[256];
        int len = Security::decryptBinary(buf, idx, header, decrypted, MY_AES_KEY);
        
        if (len < 0) { Serial.println("[ERR] Decrypt Fail"); return; }

        // B. Giải nén Binary -> JSON
        JsonDocument doc;
        PacketUtils::decodeBinaryToJson(decrypted, len, doc);
        
        // Debug
        String dbg; serializeJson(doc, dbg);
        Serial.println("[LORA->JSON] " + dbg);

        // C. Xử lý Logic (Lọc ID, En, Req)
        _processDownlinkJson(doc);
    }
}

void StateMachine::_processDownlinkJson(JsonDocument& doc) {
    // 1. Lọc ID (NID)
    // Cấu trúc: {"NID":"...", "en":..., "req":{...}}
    if (doc["NID"].isNull()) return;
    
    String targetID = doc["NID"].as<String>();
    // Chỉ xử lý nếu ID là của mình hoặc "ALL"
    if (targetID != BRIDGE_DEVICE_ID && targetID != "ALL") {
        return; 
    }

    // 2. Kiểm tra Enable (en)
    int en = doc["en"].as<int>(); // 0 hoặc 1

    // TRƯỜNG HỢP 1: Yêu cầu ngủ (en = 0)
    if (en == 0) {
        // Tự tạo lệnh SLEEP gửi xuống Node
        // Không cần đánh thức nếu nó đang ngủ, nhưng để chắc chắn lệnh đến được -> Vẫn handshake
        if (_wakeUpNode()) {
            // Gửi JSON: {"set":"SLEEP"} (Node sẽ tự hiểu là cmd.enable=false)
            // Hoặc chuẩn hơn theo protocol: {"en":0, "req":{"set":"SLEEP"}}
            // Tuy nhiên Node V2 đã xử lý được lớp "req", ta gửi format chuẩn để Node dễ parse
            
            JsonDocument sleepDoc;
            sleepDoc["en"] = 0;
            sleepDoc["req"]["set"] = "SLEEP";
            
            String jsonStr;
            serializeJson(sleepDoc, jsonStr);
            _sendUart(jsonStr);
            Serial.println("[DOWN] Sent SLEEP command to Node");
        }
        return;
    }

    // TRƯỜNG HỢP 2: Yêu cầu hoạt động/TimeSync (en = 1)
    if (en == 1) {
        JsonObject req = doc["req"];
        if (req.isNull()) return;

        // Đánh thức Node
        if (_wakeUpNode()) {
            // Gửi phần nội dung "req" xuống, bọc lại để Node hiểu
            // Node mong đợi: {"en":1, "req":{...}} hoặc {"set":...}
            // Để đơn giản và đúng logic Node parse: Ta gửi nguyên cấu trúc đã nhận (đã giải mã)
            // Vì Node cũng có logic check "en" và "req".
            
            String jsonToSend;
            serializeJson(doc, jsonToSend); // Gửi nguyên cục {"NID":..., "en":1, "req":...}
            _sendUart(jsonToSend);
            Serial.println("[DOWN] Sent command to Node");
        } else {
            Serial.println("[ERR] Node did not wake up!");
        }
    }
}

// =================================================================
// 2. XỬ LÝ UPLINK (NODE -> BRIDGE -> LORA)
// =================================================================
void StateMachine::_handleUart() {
    while (_uart->available()) {
        String raw = _uart->readStringUntil('\n');
        raw.trim();
        if (raw.length() == 0) continue;
        
        // Format: JSON|CRC
        int sep = raw.lastIndexOf('|');
        if (sep == -1) continue;
        
        String jsonPart = raw.substring(0, sep);
        String crcPart = raw.substring(sep + 1);
        
        // 1. Verify CRC
        uint32_t cal = CRC32::calculate(jsonPart);
        uint32_t rec = strtoul(crcPart.c_str(), NULL, 16);
        
        if (cal == rec) {
            Serial.print("[UART->BRIDGE] "); Serial.println(jsonPart);
            
            // 2. Parse JSON từ Node
            JsonDocument nodeDoc;
            DeserializationError err = deserializeJson(nodeDoc, jsonPart);
            
            if (!err) {
                // 3. Đóng gói lại theo yêu cầu Uplink mới
                _wrapAndSendUplink(nodeDoc);
            }
        } else {
            Serial.printf("[ERR] CRC Fail. Cal:%lX Rec:%lX\n", cal, rec);
        }
    }
}

void StateMachine::_wrapAndSendUplink(JsonDocument& nodeDoc) {
    // Yêu cầu: #{ "device_ID":..., "pin":..., "node": { ... } }
    // Lưu ý: Dấu # ở đây là quy ước logic, khi nén Binary ta dùng cấu trúc JSON chuẩn.
    
    JsonDocument bridgeDoc;
    
    // Thêm thông tin Bridge
    bridgeDoc["device_ID"] = BRIDGE_DEVICE_ID; // Từ config.h
    
    float volt = _pwr.getVoltage();
    bridgeDoc["pin"] = (volt > 0) ? volt : 3.6; // Giả lập nếu không có pin
    
    // Nhúng toàn bộ JSON của Node vào key "node"
    bridgeDoc["node"] = nodeDoc;
    
    // 4. Gửi LoRa (Mã hóa Binary)
    _sendLoRa(bridgeDoc);
}

// =================================================================
// 3. CÁC HÀM GIAO TIẾP & UTILS
// =================================================================

void StateMachine::_sendLoRa(JsonDocument& doc) {
    // Encode JSON -> Binary
    uint8_t plainBuf[512]; // Buffer lớn hơn chút cho an toàn
    int plainLen = PacketUtils::encodeJsonToBinary(doc, plainBuf, 512);
    
    if (plainLen <= 0) {
        Serial.println("[ERR] JSON Encode Failed (Too large?)");
        return;
    }

    // Encrypt AES
    PacketHeader header;
    header.nodeId = getByteId(BRIDGE_DEVICE_ID);
    header.counter = _packetCounter++;
    
    uint8_t cipherBuf[512];
    int cipherLen = Security::encryptBinary(header, plainBuf, plainLen, cipherBuf, MY_AES_KEY);
    
    // Send
    LoRa.beginPacket();
    LoRa.write(cipherBuf, cipherLen);
    LoRa.endPacket();
    LoRa.receive(); // Quay lại chế độ nhận ngay
    
    Serial.printf("[LORA TX] Sent %d bytes uplink\n", cipherLen);
}

void StateMachine::_sendUart(String jsonString) {
    uint32_t crc = CRC32::calculate(jsonString);
    _uart->print(jsonString);
    _uart->print("|");
    _uart->println(String(crc, HEX));
}

bool StateMachine::_wakeUpNode() {
    // 1. Kiểm tra xem Node có đang thức không? (GPIO 25)
    if (digitalRead(PIN_NODE_STATUS) == HIGH) {
        return true; // Node đang thức, gửi luôn
    }
    
    Serial.println("[WAKE] Triggering Node...");
    
    // 2. Kích xung đánh thức (GPIO 26)
    digitalWrite(PIN_NODE_TRIGGER, HIGH);
    delay(50); // Giữ 50ms
    digitalWrite(PIN_NODE_TRIGGER, LOW);
    
    // 3. Chờ Node phản hồi (Timeout 500ms)
    unsigned long start = millis();
    while (millis() - start < 500) {
        if (digitalRead(PIN_NODE_STATUS) == HIGH) {
            Serial.println("[WAKE] Node Awake!");
            delay(50); // Chờ thêm xíu cho Node ổn định UART
            return true;
        }
        delay(5);
    }
    
    return false; // Node không phản hồi
}

void StateMachine::_tryLoRaInit() {
    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("[LORA] Init Failed!");
        return;
    }
    
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.enableCrc();
    
    Serial.println("[LORA] Init Success");
    LoRa.receive();
}