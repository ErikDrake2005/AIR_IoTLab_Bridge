#include "StateMachine.h"
#include "CRC32.h" // [QUAN TRỌNG] Phải include file này để tính CRC32 cho UART

HardwareSerial nodeSerial(1);

StateMachine::StateMachine() {
    _state = ST_BOOT;
    _uart = &nodeSerial;
    _lastKnownTimestamp = "0";
}

void StateMachine::begin() {
    _pwr.begin();
    
    pinMode(PIN_NODE_STATUS, INPUT);
    pinMode(PIN_NODE_TRIGGER, OUTPUT); 
    digitalWrite(PIN_NODE_TRIGGER, LOW);

    // [QUAY VỀ CŨ] UART Config chuẩn cho String JSON
    _uart->begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    _uart->setRxBufferSize(2048); 
    _uart->setTimeout(10); 

    // Khởi tạo LoRa (Giữ nguyên)
    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    if (!LoRa.begin(LORA_FREQ)) { Serial.println("LoRa Fail"); while(1); }
    
    LoRa.setSyncWord(LORA_SYNC_WORD); 
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setTxPower(LORA_TX_POWER); 
    LoRa.enableCrc();

    Serial.println("=== BRIDGE STARTED (AES-LORA <-> JSON-UART) ===");
    
    // Logic khởi động (Giữ nguyên)
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        if (digitalRead(PIN_NODE_STATUS) == HIGH) {
            _switchState(ST_NORMAL);
        } else {
            JsonDocument doc; 
            doc["type"] = "poll"; 
            doc["id"] = MY_DEVICE_NAME;
            doc["batt"] = _pwr.getVoltage();
            _sendLoRaPacket(doc); 
            _switchState(ST_POLLING);
        }
    } else {
        _switchState(ST_NORMAL);
    }
}

void StateMachine::loop() {
    _handleLoRaRx();
    _handleUartRx();

    // Máy trạng thái (Logic giữ nguyên)
    switch (_state) {
        case ST_POLLING:
            if (millis() - _timerState > POLL_TIMEOUT_MS) _switchState(ST_DEEP_SLEEP);
            break;

        case ST_NORMAL:
            if (millis() - _timerState > 10000) {
                if (_pwr.isBatteryLow()) _switchState(ST_LOW_BAT_WAIT_OFF);
                _timerState = millis();
            }
            break;

        case ST_WAKE_PULSE_HIGH:
            if (millis() - _timerState > 200) {
                digitalWrite(PIN_NODE_TRIGGER, LOW);
                _switchState(ST_WAKE_WAIT_GPIO);
            }
            break;

        case ST_WAKE_WAIT_GPIO:
            if (digitalRead(PIN_NODE_STATUS) == HIGH) _switchState(ST_WAKE_SEND_REPORT);
            if (millis() - _timerState > 5000) _switchState(ST_DEEP_SLEEP);
            break;

        case ST_WAKE_SEND_REPORT: {
            JsonDocument doc; 
            doc["type"]="info"; doc["status"]="awake"; doc["id"]=MY_DEVICE_NAME;
            _sendLoRaPacket(doc); 
            _switchState(ST_NORMAL);
            break;
        }

        case ST_WAIT_NODE_SLEEP:
            if (digitalRead(PIN_NODE_STATUS) == LOW) _switchState(ST_SEND_ACK_SLEEP);
            if (millis() - _timerState > 10000) _switchState(ST_SEND_ACK_SLEEP);
            break;

        case ST_SEND_ACK_SLEEP: {
            JsonDocument doc; 
            doc["type"]="ack"; doc["cmd"]="EN:0";
            doc["id"]=MY_DEVICE_NAME; doc["timestamp"]=_lastKnownTimestamp;
            _sendLoRaPacket(doc); 
            _switchState(ST_WAIT_GW_CONFIRM);
            break;
        }

        case ST_WAIT_GW_CONFIRM:
            if (millis() - _timerState > GW_CONFIRM_MS) _switchState(ST_SEND_ACK_SLEEP);
            break;

        case ST_DEEP_SLEEP:
            _pwr.sleepForSeconds(POLL_INTERVAL_SEC);
            break;
            
        case ST_LOW_BAT_WAIT_OFF:
             // [SỬA LẠI] Gửi JSON Text xuống UART
             if (millis() - _timerState < 1000 && (millis()%200)==0) {
                 JsonDocument doc; doc["EN"] = 0;
                 _sendUartWithCRC32(doc); 
             }
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
// LORA: GIỮ NGUYÊN AES + BINARY (ĐỂ GATEWAY HIỂU)
// =============================================================================

void StateMachine::_handleLoRaRx() {
    int packetSize = LoRa.parsePacket();
    if (!packetSize) return;

    uint8_t buf[256]; 
    int idx = 0;
    while (LoRa.available() && idx < 256) buf[idx++] = LoRa.read();
    
    if (buf[0] != MY_NODE_INDEX) return;

    PacketHeader head; 
    uint8_t decrypted[240];
    
    // 1. Giải mã AES
    int len = Security::decryptBinary(buf, packetSize, head, decrypted, MY_AES_KEY);
    
    if (len > 0) {
        JsonDocument doc; 
        // 2. Bung nén Binary -> JSON
        PacketUtils::decodeBinaryToJson(decrypted, len, doc);
        _processGatewayCmd(doc);
    }
}

void StateMachine::_sendLoRaPacket(JsonDocument& doc) {
    String jsonStr; 
    serializeJson(doc, jsonStr);

    // 1. Nén JSON -> Binary
    uint8_t plain[250];
    plain[0] = DT_VAL_RAW_STR; 
    plain[1] = jsonStr.length();
    memcpy(plain + 2, jsonStr.c_str(), jsonStr.length());
    plain[2 + jsonStr.length()] = DT_END;
    int plainLen = 3 + jsonStr.length();

    // 2. Mã hóa AES
    PacketHeader head; 
    head.nodeId = MY_NODE_INDEX; 
    head.counter = millis(); 
    
    uint8_t encrypted[256];
    int sendLen = Security::encryptBinary(head, plain, plainLen, encrypted, MY_AES_KEY);

    // 3. Gửi LoRa
    LoRa.beginPacket(); 
    LoRa.write(encrypted, sendLen); 
    LoRa.endPacket();
    
    Serial.print("[LORA TX] "); Serial.println(jsonStr);
}

// =============================================================================
// UART: QUAY VỀ JSON TEXT + CRC32 (ĐÚNG NHƯ BẠN MUỐN)
// =============================================================================

void StateMachine::_handleUartRx() {
    while (_uart->available()) {
        // 1. Đọc chuỗi text: {"..."}|CRC_HEX\n
        String line = _uart->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        // 2. Tách CRC
        int separatorIdx = line.lastIndexOf('|');
        if (separatorIdx == -1) continue; // Sai format

        String jsonPart = line.substring(0, separatorIdx);
        String crcPart = line.substring(separatorIdx + 1);

        // 3. Kiểm tra CRC32
        unsigned long calcCRC = CRC32::calculate(jsonPart);
        unsigned long recvCRC = strtoul(crcPart.c_str(), NULL, 16);

        if (calcCRC == recvCRC) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, jsonPart);
            if (!err) {
                // Sniff Timestamp
                if (doc["timestamp"]) _lastKnownTimestamp = doc["timestamp"].as<String>();
                
                // Nếu Normal mode -> Đóng gói AES gửi LoRa
                if (_state == ST_NORMAL) {
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
// LOGIC (Đã sửa hàm gửi)
// =============================================================================

void StateMachine::_processGatewayCmd(JsonDocument& doc) {
    String type = doc["type"] | "";
    if (_state == ST_POLLING) {
        if (doc["EN"] == 1) { 
            digitalWrite(PIN_NODE_TRIGGER, HIGH);
            _switchState(ST_WAKE_PULSE_HIGH);
        } else _switchState(ST_DEEP_SLEEP);
        return;
    }
    if (_state == ST_WAIT_GW_CONFIRM) {
        if (type == "ack" || doc["ack_rec"]) _switchState(ST_DEEP_SLEEP);
        return;
    }
    if (_state == ST_NORMAL) {
        if (doc["EN"] == 0) {
            JsonDocument stopDoc; stopDoc["EN"] = 0;
            _sendUartWithCRC32(stopDoc); // [SỬA] Gửi Text
            _switchState(ST_WAIT_NODE_SLEEP);
        } else {
            // Forward lệnh xuống Node
            _sendUartWithCRC32(doc); // [SỬA] Gửi Text
        }
    }
}

void StateMachine::_switchState(BridgeState newState) {
    _state = newState; _timerState = millis();
}