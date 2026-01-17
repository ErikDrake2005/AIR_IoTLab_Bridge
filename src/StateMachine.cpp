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
        
        uint8_t buf[512]; // Tăng buffer lên 512 cho an toàn
        int idx = 0;
        while (LoRa.available() && idx < 512) buf[idx++] = LoRa.read();
        
        // A. Giải mã AES
        PacketHeader header;
        uint8_t decrypted[512];
        int len = Security::decryptBinary(buf, idx, header, decrypted, MY_AES_KEY);
        
        if (len < 0) { Serial.println("[ERR] Decrypt Fail"); return; }

        // B. Giải nén Binary -> JSON
        JsonDocument doc;
        PacketUtils::decodeBinaryToJson(decrypted, len, doc);
        
        // Debug (Chỉ bật khi cần thiết để tránh spam)
        // String dbg; serializeJson(doc, dbg); Serial.println("[LORA->JSON] " + dbg);

        // C. Xử lý Logic (Lọc ID, En, Req)
        _processDownlinkJson(doc);
    }
}

void StateMachine::_processDownlinkJson(JsonDocument& doc) {
    // 1. Lọc ID (NID)
    if (doc["NID"].isNull()) return;
    
    String targetID = doc["NID"].as<String>();
    // Chỉ xử lý nếu ID là của mình hoặc "ALL"
    if (targetID != BRIDGE_DEVICE_ID && targetID != "ALL") {
        return; 
    }

    // 2. Kiểm tra Enable (en)
    int en = doc["en"].as<int>(); 

    // TRƯỜNG HỢP 1: Yêu cầu ngủ (en = 0)
    if (en == 0) {
        // Gửi lệnh ngủ trực tiếp, không cần đánh thức
        JsonDocument sleepDoc;
        sleepDoc["set"] = "SLEEP";
        
        String jsonStr;
        serializeJson(sleepDoc, jsonStr);
        _sendUart(jsonStr);
        Serial.println("[DOWN] Sent SLEEP command to Node");
        return;
    }

    // TRƯỜNG HỢP 2: Yêu cầu hoạt động/TimeSync (en = 1)
    if (en == 1) {
        JsonObject req = doc["req"];
        if (req.isNull()) return;

        // Đánh thức Node trước khi gửi lệnh
        if (_wakeUpNode()) {
            // Chỉ gửi nội dung của "req" xuống Node
            String jsonToSend;
            serializeJson(req, jsonToSend);  // ← Chỉ "req"
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
    // [FIX WDT] Thay while bằng if để tránh treo nếu nhiễu liên tục
    if (_uart->available()) {
        String raw = _uart->readStringUntil('\n');
        raw.trim();
        
        if (raw.length() == 0) return;
        
        // Format: JSON|CRC
        int sep = raw.lastIndexOf('|');
        if (sep == -1) return;
        
        String jsonPart = raw.substring(0, sep);
        String crcPart = raw.substring(sep + 1);
        
        // 1. Verify CRC
        uint32_t cal = CRC32::calculate(jsonPart);
        uint32_t rec = strtoul(crcPart.c_str(), NULL, 16);
        
        if (cal == rec) {
            Serial.print("[UART RX] "); Serial.println(jsonPart);
            
            JsonDocument nodeDoc;
            DeserializationError err = deserializeJson(nodeDoc, jsonPart);
            
            if (!err) {
                // ========== WRAP & GỬI LÊN GATEWAY ==========
                // Tất cả gói tin từ Node (time_req, machine_status, data,...)
                // đều được wrap và gửi LoRa lên Gateway để xử lý
                _wrapAndSendUplink(nodeDoc);
            } else {
                Serial.println("[ERR] JSON Parse Fail");
            }
        } else {
            Serial.printf("[ERR] CRC Fail. Cal:%lX Rec:%lX\n", cal, rec);
        }
    }
}

void StateMachine::_wrapAndSendUplink(JsonDocument& nodeDoc) {
    JsonDocument bridgeDoc;
    
    // Thêm thông tin Bridge
    bridgeDoc["device_ID"] = BRIDGE_DEVICE_ID; 
    
    float volt = _pwr.getVoltage();
    bridgeDoc["pin"] = (volt > 0) ? volt : 3.6; 
    
    // Nhúng toàn bộ JSON của Node vào key "node"
    bridgeDoc["node"] = nodeDoc;
    
    // Gửi LoRa (tag "#" sẽ được thêm trong _sendLoRa)
    _sendLoRa(bridgeDoc);
}

// =================================================================
// 3. CÁC HÀM GIAO TIẾP & UTILS
// =================================================================

void StateMachine::_sendLoRa(JsonDocument& doc) {
    // Encode JSON -> Binary
    uint8_t plainBuf[512]; 
    int plainLen = PacketUtils::encodeJsonToBinary(doc, plainBuf, 512);
    
    if (plainLen <= 0) {
        Serial.println("[ERR] JSON Encode Failed");
        return;
    }

    // Encrypt AES
    PacketHeader header;
    header.nodeId = getByteId(BRIDGE_DEVICE_ID);
    header.counter = _packetCounter++;
    
    uint8_t cipherBuf[512];
    int cipherLen = Security::encryptBinary(header, plainBuf, plainLen, cipherBuf, MY_AES_KEY);
    LoRa.beginPacket();
    LoRa.write('#');  // ← Tag "#" để Gateway biết đó là Uplink từ Node
    LoRa.write(cipherBuf, cipherLen);
    LoRa.endPacket();
    LoRa.receive(); // Quay lại chế độ nhận ngay
    
    Serial.printf("[LORA TX] Sent %d bytes uplink with tag '#'\n", cipherLen + 1);
}

void StateMachine::_sendUart(String jsonString) {
    uint32_t crc = CRC32::calculate(jsonString);
    _uart->print(jsonString);
    _uart->print("|");
    _uart->println(String(crc, HEX));
    
    Serial.printf("[UART TX] %s\n", jsonString.c_str());
}

bool StateMachine::_wakeUpNode() {
    if (digitalRead(PIN_NODE_STATUS) == HIGH) {
        Serial.println("[WAKE] Node already awake!");
        return true; 
    }
    
    // Node đang ngủ → cần đánh thức
    Serial.println("[WAKE] Node sleeping, triggering wakeup...");
    
    // Kích GPIO 26 của Bridge vào GPIO 33 của Node
    digitalWrite(PIN_NODE_TRIGGER, HIGH);
    delay(50); 
    digitalWrite(PIN_NODE_TRIGGER, LOW);
    
    // Chờ Node phản hồi (GPIO 25 lên HIGH) - Timeout 1 giây
    unsigned long start = millis();
    unsigned long timeout = 1000;  // 1 giây
    int attempts = 0;
    
    while (millis() - start < timeout) {
        if (digitalRead(PIN_NODE_STATUS) == HIGH) {
            Serial.println("[WAKE] Node Awake! (GPIO 25 HIGH)");
            delay(50); // Chờ ổn định
            return true;
        }
        delay(10);
    }
    
    // Timeout lần 1 → đánh thức lại
    Serial.println("[WAKE] First timeout, attempting again...");
    digitalWrite(PIN_NODE_TRIGGER, HIGH);
    delay(50); 
    digitalWrite(PIN_NODE_TRIGGER, LOW);
    
    // Chờ lần 2 - Timeout 500ms
    start = millis();
    timeout = 500;
    
    while (millis() - start < timeout) {
        if (digitalRead(PIN_NODE_STATUS) == HIGH) {
            Serial.println("[WAKE] Node Awake on retry!");
            delay(50);
            return true;
        }
        delay(10);
    }
    
    Serial.println("[WAKE] Node failed to wake up!");
    return false; 
}

void StateMachine::_tryLoRaInit() {
    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("[LORA] Init Failed!");
        return;
    }
    
    // CẤU HÌNH KHỚP VỚI GATEWAY
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setSyncWord(LORA_SYNC_WORD); // <--- QUAN TRỌNG: 0xF3
    LoRa.enableCrc();
    
    Serial.println("[LORA] Init Success");
    LoRa.receive();
}