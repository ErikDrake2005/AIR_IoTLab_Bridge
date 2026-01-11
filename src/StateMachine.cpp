#include "StateMachine.h"
#include "CRC32.h" // [BẮT BUỘC] Tính CRC32 cho UART

HardwareSerial nodeSerial(1);

StateMachine::StateMachine() {
    _state = ST_BOOT;
    _uart = &nodeSerial;
    _lastKnownTimestamp = "0";
    _pollSent = false;
}

void StateMachine::begin() {
    _pwr.begin();
    
    // Config GPIO
    pinMode(PIN_NODE_STATUS, INPUT);
    pinMode(PIN_NODE_TRIGGER, OUTPUT); 
    digitalWrite(PIN_NODE_TRIGGER, LOW);

    // [UART] Config chuẩn cho JSON Text
    _uart->begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    _uart->setRxBufferSize(2048); 
    _uart->setTimeout(10); 

    // [LORA] Config chuẩn
    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    
    if (!LoRa.begin(LORA_FREQ)) { 
        Serial.println("[ERR] LoRa Init Fail!"); 
        while(1); 
    }
    
    LoRa.setSyncWord(LORA_SYNC_WORD); 
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setTxPower(LORA_TX_POWER); 
    LoRa.enableCrc();
    
    // Mặc định luôn ở chế độ nhận
    LoRa.receive();

    Serial.println("=== BRIDGE STARTED (AES-LORA <-> JSON-UART) ===");
    
    // LOGIC KHỞI ĐỘNG
    // Nếu bị đánh thức bởi Timer (hết giờ ngủ)
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        // Kiểm tra xem Node có đang thức không (qua chân Status)
        if (digitalRead(PIN_NODE_STATUS) == HIGH) {
            _switchState(ST_NORMAL);
        } else {
            // Node vẫn ngủ -> Vào chế độ Poll để hỏi Gateway
            _switchState(ST_POLLING);
            _pollSent = false; // Đặt cờ để gửi Poll trong vòng lặp
        }
    } else {
        // Khởi động lạnh hoặc Reset nút -> Vào chế độ thường
        _switchState(ST_NORMAL);
    }
}

void StateMachine::loop() {
    // 1. Luôn ưu tiên xử lý gói tin LoRa đến (Lệnh từ Gateway)
    _handleLoRaRx();
    
    // 2. Xử lý gói tin UART từ Node
    _handleUartRx();

    // 3. Máy trạng thái
    switch (_state) {
        case ST_POLLING:
            // Gửi Poll 1 lần duy nhất khi vào trạng thái này
            if (!_pollSent) {
                JsonDocument doc; 
                doc["type"] = "poll"; 
                doc["id"] = MY_DEVICE_NAME;
                // Gửi điện áp Bridge để Gateway giám sát
                doc["bridge_volt"] = _pwr.getVoltage(); 
                _sendLoRaPacket(doc); 
                
                _pollSent = true; 
                _timerState = millis(); // Bắt đầu đếm timeout
                
                // Quan trọng: Chuyển sang nghe ngay để bắt EN:1
                LoRa.receive(); 
            }
            
            // Nếu chờ quá lâu không thấy Gateway trả lời -> Ngủ tiếp
            if (millis() - _timerState > POLL_TIMEOUT_MS) {
                Serial.println("[POLL] Timeout. No Gateway. Sleep.");
                _switchState(ST_DEEP_SLEEP);
            }
            break;

        case ST_NORMAL:
            // Kiểm tra pin yếu định kỳ (10s/lần)
            if (millis() - _timerState > 10000) {
                if (_pwr.isBatteryLow()) _switchState(ST_LOW_BAT_WAIT_OFF);
                _timerState = millis();
            }
            break;

        case ST_WAKE_PULSE_HIGH:
            // Giữ chân Trigger mức cao trong 200ms để đánh thức Node
            if (millis() - _timerState > 200) {
                digitalWrite(PIN_NODE_TRIGGER, LOW);
                _switchState(ST_WAKE_WAIT_GPIO);
            }
            break;

        case ST_WAKE_WAIT_GPIO:
            // Chờ Node phản hồi bằng cách kéo chân Status lên High
            if (digitalRead(PIN_NODE_STATUS) == HIGH) _switchState(ST_WAKE_SEND_REPORT);
            // Timeout 5s nếu Node hỏng không dậy được
            if (millis() - _timerState > 5000) _switchState(ST_DEEP_SLEEP);
            break;

        case ST_WAKE_SEND_REPORT: {
            // Báo cáo Gateway là "Tôi đã dậy rồi"
            JsonDocument doc; 
            doc["type"]="info"; doc["status"]="awake"; doc["id"]=MY_DEVICE_NAME;
            _sendLoRaPacket(doc); 
            _switchState(ST_NORMAL);
            break;
        }

        case ST_WAIT_NODE_SLEEP:
            // Chờ Node tắt chân Status (xác nhận Node đã tắt)
            if (digitalRead(PIN_NODE_STATUS) == LOW) _switchState(ST_SEND_ACK_SLEEP);
            // Timeout 10s
            if (millis() - _timerState > 10000) _switchState(ST_SEND_ACK_SLEEP);
            break;

        case ST_SEND_ACK_SLEEP: {
            // Gửi yêu cầu "Xin ngủ" lên Gateway
            JsonDocument doc; 
            doc["type"]="ack"; doc["cmd"]="EN:0";
            doc["id"]=MY_DEVICE_NAME; 
            _sendLoRaPacket(doc); 
            _switchState(ST_WAIT_GW_CONFIRM);
            break;
        }

        case ST_WAIT_GW_CONFIRM:
            // Chờ Gateway gửi {"ack_rec": 1}
            // Nếu quá hạn mà không thấy Gateway rep -> Ngủ đại (Force Sleep)
            if (millis() - _timerState > GW_CONFIRM_MS) {
                Serial.println("[SLEEP] GW No Confirm. Force Sleep.");
                _switchState(ST_DEEP_SLEEP);
            }
            break;

        case ST_DEEP_SLEEP:
            _pwr.sleepForSeconds(POLL_INTERVAL_SEC);
            break;
            
        case ST_LOW_BAT_WAIT_OFF:
             // Gửi liên tục lệnh tắt xuống Node qua UART
             if (millis() - _timerState < 1000 && (millis()%200)==0) {
                 JsonDocument doc; doc["EN"] = 0;
                 _sendUartWithCRC32(doc); 
             }
             // Sau đó báo Gateway là pin yếu rồi ngủ lâu
             if (digitalRead(PIN_NODE_STATUS) == LOW || (millis() - _timerState > 2000)) {
                 JsonDocument doc; doc["pin"] = 0; doc["id"] = MY_DEVICE_NAME;
                 _sendLoRaPacket(doc); 
                 delay(500);
                 _pwr.sleepForSeconds(DEEP_SLEEP_BAT_SEC);
             }
             break;
         default: break;
    }
}

// =============================================================================
// LORA HANDLER: AES + BINARY + PACKETDEF
// =============================================================================

void StateMachine::_handleLoRaRx() {
    int packetSize = LoRa.parsePacket();
    if (!packetSize) return;

    uint8_t buf[256]; 
    int idx = 0;
    while (LoRa.available() && idx < 256) buf[idx++] = LoRa.read();
    
    // Kiểm tra ID: Byte đầu tiên phải khớp ID của Bridge này
    if (buf[0] != MY_NODE_INDEX) {
        LoRa.receive(); // Không phải của mình, nghe tiếp
        return;
    }

    PacketHeader head; 
    uint8_t decrypted[240];
    
    // 1. Giải mã AES
    int len = Security::decryptBinary(buf, packetSize, head, decrypted, MY_AES_KEY);
    
    if (len > 0) {
        JsonDocument doc; 
        // 2. Bung nén Binary -> JSON (Dùng PacketUtils)
        PacketUtils::decodeBinaryToJson(decrypted, len, doc);
        
        // 3. Xử lý lệnh
        _processGatewayCmd(doc);
    }
    
    // [QUAN TRỌNG] Luôn tái kích hoạt chế độ nhận
    LoRa.receive(); 
}

void StateMachine::_sendLoRaPacket(JsonDocument& doc) {
    // 1. Nén JSON -> Binary (Dùng PacketUtils để nén token)
    // Cách này tốt hơn gửi raw string vì tiết kiệm băng thông
    uint8_t plain[250];
    int plainLen = PacketUtils::encodeJsonToBinary(doc, plain, 240);

    // Nếu nén thất bại (dữ liệu rỗng), fallback về raw string
    if (plainLen == 0) {
        String jsonStr; serializeJson(doc, jsonStr);
        plain[0] = DT_VAL_RAW_STR; 
        plain[1] = jsonStr.length();
        memcpy(plain + 2, jsonStr.c_str(), jsonStr.length());
        plain[2 + jsonStr.length()] = DT_END;
        plainLen = 3 + jsonStr.length();
        Serial.print("[LORA RAW] "); Serial.println(jsonStr);
    } 

    // 2. Mã hóa AES
    PacketHeader head; 
    head.nodeId = MY_NODE_INDEX; 
    head.counter = millis(); 
    
    uint8_t encrypted[256];
    int sendLen = Security::encryptBinary(head, plain, plainLen, encrypted, MY_AES_KEY);

    // 3. Gửi LoRa
    // Chuyển LoRa về standby trước khi gửi để an toàn
    LoRa.idle(); 
    LoRa.beginPacket(); 
    LoRa.write(encrypted, sendLen); 
    LoRa.endPacket(true); // true = Blocking wait (Chờ gửi xong)
    
    // 4. [QUAN TRỌNG] Chuyển ngay về Receive để chờ phản hồi (nếu có)
    LoRa.receive();
    
    Serial.printf("[LORA TX] Sent %d bytes (Encrypted)\n", sendLen);
}

// =============================================================================
// UART HANDLER: JSON TEXT + CRC32
// =============================================================================

void StateMachine::_handleUartRx() {
    while (_uart->available()) {
        // 1. Đọc chuỗi text: {"..."}|CRC_HEX\n
        String line = _uart->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        // 2. Tách CRC
        int separatorIdx = line.lastIndexOf('|');
        if (separatorIdx == -1) continue; 

        String jsonPart = line.substring(0, separatorIdx);
        String crcPart = line.substring(separatorIdx + 1);

        // 3. Kiểm tra CRC32
        unsigned long calcCRC = CRC32::calculate(jsonPart);
        unsigned long recvCRC = strtoul(crcPart.c_str(), NULL, 16);

        if (calcCRC == recvCRC) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, jsonPart);
            if (!err) {
                // Lấy Timestamp nếu có
                if (doc["timestamp"]) _lastKnownTimestamp = doc["timestamp"].as<String>();
                
                // Nếu đang ở chế độ Normal -> Forward lên Gateway
                if (_state == ST_NORMAL) {
                    // Chèn thêm thông tin pin của Bridge
                    doc["bridge_volt"] = _pwr.getVoltage();
                    _sendLoRaPacket(doc); 
                }
            }
        } else {
            Serial.println("[UART ERR] CRC Mismatch!");
        }
    }
}

void StateMachine::_sendUartWithCRC32(JsonDocument& doc) {
    // 1. Serialize JSON
    String jsonStr; 
    serializeJson(doc, jsonStr);

    // 2. Tính CRC32
    unsigned long crc = CRC32::calculate(jsonStr);

    // 3. Gửi Text format: JSON|CRC_HEX
    _uart->print(jsonStr);
    _uart->print("|");
    _uart->println(String(crc, HEX));
    
    Serial.print("[UART TX] "); Serial.println(jsonStr);
}

// =============================================================================
// LOGIC XỬ LÝ LỆNH TỪ GATEWAY
// =============================================================================

void StateMachine::_processGatewayCmd(JsonDocument& doc) {
    String type = doc["type"] | "";
    
    // Trường hợp 1: Đang Poll (Ngủ dậy hỏi Gateway)
    if (_state == ST_POLLING) {
        // Gateway trả lời {"EN": 1} -> Đánh thức Node
        if (doc["EN"] == 1) { 
            Serial.println("[CMD] Gateway says WAKEUP!");
            digitalWrite(PIN_NODE_TRIGGER, HIGH);
            _switchState(ST_WAKE_PULSE_HIGH);
        } else {
            // Nhận rác hoặc không có lệnh -> Ngủ tiếp
            _switchState(ST_DEEP_SLEEP);
        }
        return;
    }

    // Trường hợp 2: Đang chờ xác nhận ngủ
    if (_state == ST_WAIT_GW_CONFIRM) {
        // Gateway trả lời {"ack_rec": 1} -> Cho phép ngủ
        if (doc["ack_rec"] == 1) {
            Serial.println("[CMD] Gateway confirmed Sleep.");
            _switchState(ST_DEEP_SLEEP);
        }
        return;
    }

    // Trường hợp 3: Đang hoạt động bình thường (Forwarding)
    if (_state == ST_NORMAL) {
        // Lệnh cưỡng chế ngủ từ Gateway {"EN": 0}
        if (doc["EN"] == 0) {
            JsonDocument stopDoc; stopDoc["EN"] = 0;
            _sendUartWithCRC32(stopDoc); 
            _switchState(ST_WAIT_NODE_SLEEP);
        } 
        else {
            // Các lệnh khác (Mở cửa, Bật quạt...) -> Chuyển xuống Node
            _sendUartWithCRC32(doc); 
        }
    }
}

void StateMachine::_switchState(BridgeState newState) {
    _state = newState; 
    _timerState = millis();
    Serial.printf("[STATE] -> %d\n", newState);
}