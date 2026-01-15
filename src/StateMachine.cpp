#include "StateMachine.h"
#include "CRC32.h"
#include "PacketDef.h"
#include "Security.h"

// --- CẤU HÌNH ---
#define T_BATTERY_CHECK     30000   // Kiểm tra pin mỗi 30s
#define VOLTAGE_THRESHOLD   3.6     // Ngưỡng báo pin yếu
#define BRIDGE_NUM_ID       1       // ID số của Bridge (cho Header LoRa)

HardwareSerial ns(1); // UART nối với Node

// Helper: Lấy tên trạng thái
String getStateName(BridgeState s) {
    switch(s) {
        case ST_BOOT:   return "ST_BOOT";
        case ST_NORMAL: return "ST_NORMAL";
        default:        return "UNKNOWN";
    }
}

// Helper: Lấy ID số từ chuỗi (VD: "AIR_VL_01" -> 1)
uint8_t getByteId(String idStr) {
    // Tìm số ở cuối chuỗi
    int lastIndex = idStr.length() - 1;
    while(lastIndex >= 0 && isDigit(idStr[lastIndex])) {
        lastIndex--;
    }
    String numStr = idStr.substring(lastIndex + 1);
    if (numStr.length() > 0) return (uint8_t)numStr.toInt();
    return 1; // Mặc định là 1 nếu lỗi
}

StateMachine::StateMachine() {
    _state = ST_BOOT;
    _uart = &ns;
    
    // Khởi tạo Struct NodeState mặc định
    _nodeState.device_ID = MY_DEVICE_NAME; // VD: "AIR_VL_01"
    _nodeState.door_status = "unknown";
    _nodeState.fan_status = "unknown";
    _nodeState.mode = "manual";
    _nodeState.measuring = "NO";
    
    _lastHeartbeat = 0;
    _loraReady = false;
    _lastLoRaRetry = 0;
    _packetCounter = 0;
}

void StateMachine::begin() {
    Serial.println("\n[BRIDGE] ===== INITIALIZATION START V7.0 =====");
    
    // 1. Nguồn
    _pwr.begin();
    Serial.println("[BRIDGE] PowerManager OK");

    // 2. GPIO
    pinMode(PIN_NODE_TRIGGER, OUTPUT); digitalWrite(PIN_NODE_TRIGGER, LOW);
    pinMode(PIN_NODE_STATUS, INPUT);

    // 3. UART Node (921600)
    _uart->begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    _uart->setRxBufferSize(4096);
    Serial.println("[BRIDGE] UART Node OK");

    // 4. LoRa Init (Có retry)
    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    
    Serial.println("[BRIDGE] LoRa init...");
    bool loraOK = false;
    for (int i = 0; i < 3; i++) {
        if (LoRa.begin(LORA_FREQ)) {
            loraOK = true; break;
        }
        delay(100);
    }

    if (loraOK) {
        LoRa.setSpreadingFactor(LORA_SF);
        LoRa.setSignalBandwidth(LORA_BW);
        LoRa.setCodingRate4(LORA_CR);
        LoRa.setSyncWord(LORA_SYNC_WORD);
        LoRa.enableCrc();
        LoRa.setTxPower(LORA_TX_POWER);
        LoRa.receive(); 
        _loraReady = true;
        Serial.println("[LORA] ✓ READY (RX Mode)");
    } else {
        Serial.println("[LORA] ⚠ FAIL (Will retry in loop)");
        _loraReady = false;
    }

    _setState(ST_NORMAL);
}

void StateMachine::_setState(BridgeState s) {
    if (_state != s) {
        Serial.printf("[STATE] %s -> %s\n", getStateName(_state).c_str(), getStateName(s).c_str());
        _state = s;
    }
}

// Hàm thử khởi động lại LoRa nếu mất kết nối
void StateMachine::_tryLoRaInit() {
    if (_loraReady) return;
    if (millis() - _lastLoRaRetry < 10000) return; // Retry mỗi 10s
    
    _lastLoRaRetry = millis();
    Serial.println("[LORA] Retrying init...");
    if (LoRa.begin(LORA_FREQ)) {
        // Cấu hình lại như cũ...
        LoRa.setSpreadingFactor(LORA_SF);
        LoRa.setSignalBandwidth(LORA_BW);
        LoRa.receive();
        _loraReady = true;
        Serial.println("[LORA] ✓ Reconnected!");
    }
}

void StateMachine::loop() {
    if (!_loraReady) _tryLoRaInit();

    // 1. Xử lý LoRa (Luôn lắng nghe Gateway)
    _handleLoRa();

    // 2. Xử lý UART (Luôn lắng nghe Node)
    _handleUart();

    // 3. Kiểm tra Pin định kỳ
    _checkBatteryPeriodic();
}

// ---------------------------------------------------------
// XỬ LÝ LORA (GATEWAY -> BRIDGE)
// ---------------------------------------------------------
void StateMachine::_handleLoRa() {
    if (!_loraReady) return;
    
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
        uint8_t buf[512]; int len = 0;
        while (LoRa.available() && len < 512) buf[len++] = LoRa.read();
        
        PacketHeader h; 
        uint8_t decrypted[512];
        
        // 1. GIẢI MÃ AES
        // Lưu ý: MY_AES_KEY phải khớp hoàn toàn với Gateway
        int dLen = Security::decryptBinary(buf, len, h, decrypted, MY_AES_KEY);

        if (dLen > 0) {
            // --- DEBUG: IN RA HEX ĐỂ KIỂM TRA DỮ LIỆU ---
            Serial.printf("[DEBUG] Decrypt Success. Len: %d\n", dLen);
            
            // In dạng chuỗi để xem có phải JSON không
            Serial.print("[DEBUG] CHAR: ");
            for(int i=0; i<dLen; i++) Serial.print((char)decrypted[i]);
            Serial.println();
            // ---------------------------------------------

            JsonDocument doc;
            
            // 2. GIẢI NÉN JSON (Sửa lỗi void tại đây)
            // Không gán biến error nữa mà gọi trực tiếp
            PacketUtils::decodeBinaryToJson(decrypted, dLen, doc);
            
            // 3. KIỂM TRA KẾT QUẢ
            if (doc.isNull()) {
                // Nếu doc rỗng -> Giải nén lỗi -> Khả năng cao là sai Key AES
                Serial.println("[ERR] JSON Parse Failed -> Doc is NULL");
                Serial.println("[HINT] Check AES Key matching between Bridge & Gateway!");
            } else {
                Serial.print("[LORA RX] Decrypted: ");
                serializeJson(doc, Serial);
                Serial.println();
                
                _procGwCmd(doc);
            }
        } else {
            Serial.printf("[LORA RX] Decrypt Failed code: %d\n", dLen);
        }
        
        LoRa.receive(); 
    }
}
void StateMachine::_procGwCmd(JsonDocument& doc) {
    String type = doc["type"] | "";
    
    // 1. Lọc ID: Chỉ xử lý nếu đúng ID hoặc là lệnh hệ thống
    String targetDevId = doc["device_ID"] | ""; // ID của Bridge đích
    String targetNodeId = doc["device"] | "";   // ID của Node đích

    // Nếu lệnh có chỉ định device_ID mà không khớp với mình -> Bỏ qua
    // Lưu ý: targetDevId != "" tương đương !targetDevId.isEmpty()
    if (targetDevId != "" && targetDevId != String(MY_DEVICE_NAME)) {
        return; 
    }

    // 2. Xử lý lệnh
    
    // [ACK_SLEEP] Gateway xác nhận cho ngủ
    // SỬA LỖI: Thay containsKey bằng !isNull()
    if (!doc["ack_sleep"].isNull()) {
        int ackVal = doc["ack_sleep"];
        if (ackVal == 1 && _pwr.isBatteryLow()) {
             Serial.println("[CMD] GATEWAY CONFIRMED SLEEP -> DEEP SLEEP NOW");
             esp_deep_sleep_start();
        }
        return;
    }

    // [TIME_RES] Phản hồi thời gian -> Chuyển ngay cho Node
    if (type == "time_res") {
        _sendUart(doc);
        return;
    }

    // [STATUS_REQ] Yêu cầu trạng thái -> Gửi cache
    if (type == "status_req") {
        _sendReport(0);
        return;
    }

    // [LỆNH ĐIỀU KHIỂN] (set_door, set_fans, EN...) -> Chuyển cho Node
    // SỬA LỖI: Thay containsKey bằng !isNull()
    if (type == "set_state" || !doc["set_door"].isNull() || !doc["EN"].isNull()) {
        // Có thể cần chuẩn hóa dữ liệu ở đây nếu Node-RED gửi format lạ
        _sendUart(doc); 
    }
}

// ---------------------------------------------------------
// XỬ LÝ UART (NODE -> BRIDGE)
// ---------------------------------------------------------
void StateMachine::_handleUart() {
    while (_uart->available()) {
        String line = _uart->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        // Bỏ qua debug log thường (nếu Node gửi log không phải JSON)
        if (!line.startsWith("{")) {
            // Serial.print("[NODE LOG] "); Serial.println(line);
            continue;
        }

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, line);
        if (!error) {
            String type = doc["type"] | "";
            _procNodeJson(doc);
        }
    }
}

void StateMachine::_procNodeJson(JsonDocument& doc) {
    String type = doc["type"] | "";

    // 1. DATA hoặc TIME_REQ -> Chuyển tiếp ngay lên Gateway
    if (type == "data" || type == "time_req") {
        if (doc["device_ID"].isNull()) doc["device_ID"] = MY_DEVICE_NAME; // Đảm bảo có ID
        Serial.printf("[UART->LORA] Forward %s\n", type.c_str());
        _sendLoRa(doc);
        return;
    }

    // 2. MACHINE_STATUS -> Cập nhật Cache & Gửi Report chuẩn
    if (type == "machine_status") {
        // Cập nhật Cache
        _nodeState.door_status = doc["door_status"] | "unknown";
        _nodeState.fan_status = doc["fan_status"] | "unknown";
        _nodeState.mode = doc["mode"] | "manual";
        _nodeState.measuring = doc["measuring"] | "NO";
        _nodeState.timestamp = doc["timestamp"] | "0";
        
        Serial.println("[NODE] Status Updated -> Sending Report to GW");
        _sendReport(0); // 0 = Awake
    }
}

// ---------------------------------------------------------
// GỬI DỮ LIỆU ĐI (UART & LORA)
// ---------------------------------------------------------

// Gửi xuống Node qua UART (Dạng JSON Text - Vì Node dùng readStringUntil)
void StateMachine::_sendUart(JsonDocument& doc) {
    String jsonStr;
    serializeJson(doc, jsonStr);
    _uart->println(jsonStr);
    Serial.printf("[TX UART] %s\n", jsonStr.c_str());
}

// Gửi lên Gateway qua LoRa (Dạng Binary Encrypted)
void StateMachine::_sendLoRa(JsonDocument& doc) {
    if (!_loraReady) { _tryLoRaInit(); return; }

    // 1. Chuẩn bị Header
    PacketHeader header;
    header.nodeId = getByteId(String(MY_DEVICE_NAME)); // Lấy ID số (VD: 1)
    header.counter = _packetCounter++;

    // 2. Encode JSON -> Binary
    uint8_t rawBuf[512];
    int rawLen = PacketUtils::encodeJsonToBinary(doc, rawBuf, 512);

    // 3. Encrypt (Header + Data)
    uint8_t encBuf[512];
    int encLen = Security::encryptBinary(header, rawBuf, rawLen, encBuf, MY_AES_KEY);

    // 4. Send
    LoRa.beginPacket();
    LoRa.write(encBuf, encLen);
    LoRa.endPacket();
    
    LoRa.receive(); // Về lại RX ngay
    Serial.printf("[TX LORA] ID:%d Count:%d Len:%d bytes\n", header.nodeId, header.counter, encLen);
}

// Gửi báo cáo tổng hợp (Machine Status + Bridge Info)
void StateMachine::_sendReport(int sleepMode) {
    JsonDocument doc;
    doc["type"] = "machine_status";
    doc["device_ID"] = MY_DEVICE_NAME;
    
    // Thông tin từ Node (Cache)
    doc["door_status"] = _nodeState.door_status;
    doc["fan_status"] = _nodeState.fan_status;
    doc["mode"] = _nodeState.mode;
    doc["measuring"] = _nodeState.measuring;
    doc["timestamp"] = _nodeState.timestamp;

    // Thông tin từ Bridge
    float volt = _pwr.getVoltage();
    doc["Pin"] = (volt > 0) ? volt : 3.6; // Nếu cắm nguồn ngoài thì giả lập 3.6+
    doc["Sleep_Mode"] = sleepMode; // 1: Xin ngủ, 0: Thức

    _sendLoRa(doc);
}

// ---------------------------------------------------------
// TIỆN ÍCH KHÁC
// ---------------------------------------------------------
void StateMachine::_checkBatteryPeriodic() {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > T_BATTERY_CHECK) {
        lastCheck = millis();
        float volt = _pwr.getVoltage();

        // Logic Pin Yếu (Chỉ khi dùng pin thật)
        if (volt > 0 && volt < VOLTAGE_THRESHOLD) {
             Serial.printf("[PWR] LOW BATTERY (%.2fV) -> REQUEST SLEEP\n", volt);
             
             // 1. Gửi lệnh tắt cho Node trước
             JsonDocument stopNode; 
             stopNode["EN"] = 0;
             _sendUart(stopNode);

             // 2. Gửi yêu cầu ngủ lên Gateway
             _sendReport(1); // Sleep_Mode = 1
        }
    }
}