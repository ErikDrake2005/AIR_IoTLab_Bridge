#include "StateMachine.h"

// --- TASK WRAPPERS ---
void StateMachine::uartRxTaskFn(void* param) { ((StateMachine*)param)->_uartRxLoop(); }
void StateMachine::loraRxTaskFn(void* param) { ((StateMachine*)param)->_loraRxLoop(); }
void StateMachine::processTaskFn(void* param) { ((StateMachine*)param)->_processLoop(); }

StateMachine::StateMachine() {
    _uart = &Serial2; // Bridge kết nối Node qua Serial2
}

void StateMachine::begin() {
    Serial.println("\n[BRIDGE] System Init (V2 + RSSI/SNR)...");

    // 1. Setup UART (921600 - Khớp với Node)
    _uart->begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    _uart->setRxBufferSize(4096); 
    
    // 2. Setup GPIO Handshake
    pinMode(PIN_NODE_WAKEUP, OUTPUT);
    digitalWrite(PIN_NODE_WAKEUP, LOW); 
    pinMode(PIN_NODE_STATUS, INPUT_PULLDOWN); 

    // 3. Setup LoRa & Power
    _pwr.begin();
    _loraMutex = xSemaphoreCreateMutex();
    
    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("[ERR] LoRa Init Failed!");
    } else {
        LoRa.setSpreadingFactor(LORA_SF);
        LoRa.setSignalBandwidth(LORA_BW);
        LoRa.setCodingRate4(LORA_CR);
        LoRa.setSyncWord(LORA_SYNC_WORD);
        LoRa.setTxPower(LORA_TX_POWER);
        Serial.println("[LORA] Init OK");
    }

    // 4. Queues & Tasks
    _processQueue = xQueueCreate(PROCESS_QUEUE_SIZE, sizeof(PacketMsg));

    xTaskCreatePinnedToCore(uartRxTaskFn, "U_RX", 4096, this, 2, NULL, 1);
    xTaskCreatePinnedToCore(loraRxTaskFn, "L_RX", 4096, this, 2, NULL, 1);
    xTaskCreatePinnedToCore(processTaskFn,"PROC", 8192, this, 1, NULL, 1);
}

// ================= UART RX (NHẬN TỪ NODE) =================
void StateMachine::_uartRxLoop() {
    String inputBuf = "";
    inputBuf.reserve(512);

    for (;;) {
        if (_uart->available()) {
            while (_uart->available()) {
                char c = (char)_uart->read();
                // Node kết thúc gói tin bằng '\n'
                if (c == '\n') {
                    String cleanJson;
                    // Tách CRC và kiểm tra tính toàn vẹn
                    if (inputBuf.length() > 0 && _extractJsonFromUart(inputBuf, cleanJson)) {
                        Serial.printf("[UART-RX] OK. Size: %d bytes.\n", cleanJson.length());
                        
                        PacketMsg msg;
                        msg.source = 0; // Đánh dấu là tin từ Node
                        msg.len = cleanJson.length();
                        if (msg.len < 512) {
                            strcpy((char*)msg.buf, cleanJson.c_str());
                            xQueueSend(_processQueue, &msg, 0);
                        }
                    }
                    inputBuf = "";
                } else if (c != '\r') {
                    if (inputBuf.length() < 1024) inputBuf += c;
                }
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// Helper: Tách JSON|CRC -> Kiểm tra -> Trả về JSON sạch
bool StateMachine::_extractJsonFromUart(String raw, String& outJson) {
    int pipeIdx = raw.lastIndexOf('|');
    if (pipeIdx == -1) {
        Serial.println("[UART-ERR] No CRC separator found");
        return false;
    }

    String jsonPart = raw.substring(0, pipeIdx);
    String crcPart  = raw.substring(pipeIdx + 1);
    
    unsigned long calcCRC = CRC32::calculate(jsonPart);
    unsigned long recvCRC = strtoul(crcPart.c_str(), NULL, 16);

    if (calcCRC == recvCRC) {
        outJson = jsonPart;
        return true;
    } else {
        Serial.printf("[UART-ERR] CRC Mismatch! Calc: %lX, Recv: %lX. RAW: %s\n", calcCRC, recvCRC, raw.c_str());
        return false;
    }
}

// ================= LORA RX (NHẬN TỪ GATEWAY) =================
void StateMachine::_loraRxLoop() {
    for (;;) {
        int packetSize = LoRa.parsePacket();
        if (packetSize > 0) {
            uint8_t buf[256];
            int idx = 0;
            while (LoRa.available() && idx < 256) {
                buf[idx++] = LoRa.read();
            }

            // --- CẬP NHẬT: LẤY RSSI/SNR ---
            int rssi = LoRa.packetRssi();
            float snr = LoRa.packetSnr();

            // Lưu vào biến private để gửi kèm Uplink sau này
            _lastRssi = rssi;
            _lastSnr = snr;

            Serial.printf("[LORA-RX] Size: %d bytes | RSSI: %d | SNR: %.2f\n", idx, rssi, snr);

            PacketMsg msg;
            msg.source = 1; // Đánh dấu là tin từ Gateway
            msg.len = idx;
            memcpy(msg.buf, buf, idx);
            xQueueSend(_processQueue, &msg, 0);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ================= XỬ LÝ CHÍNH (LOGIC TRUNG TÂM) =================
void StateMachine::_processLoop() {
    PacketMsg msg;
    for (;;) {
        if (xQueueReceive(_processQueue, &msg, portMAX_DELAY) == pdTRUE) {
            
            // --- CASE 1: UPLINK (NODE -> BRIDGE -> GATEWAY) ---
            if (msg.source == 0) { 
                Serial.println("[PROC] Processing Uplink from Node...");

                JsonDocument doc;
                doc["device_ID"] = BRIDGE_DEVICE_ID;
                doc["pin"] = _pwr.getVoltage();
                
                // --- THÊM RSSI & SNR ---
                doc["rssi"] = _lastRssi;
                doc["snr"] = _lastSnr;
                // -----------------------

                // [FIX LỖI QUAN TRỌNG]: Dùng deserializeJson thay vì serialized
                DeserializationError err = deserializeJson(doc["node"], (char*)msg.buf);
                
                if (err) {
                     Serial.println("[PROC-ERR] Parse Node JSON failed, sending as string fallback");
                     doc["node"] = (char*)msg.buf; // Fallback
                }
                
                // Log kiểm tra
                String debugStr;
                serializeJson(doc, debugStr);
                Serial.printf("[LORA-TX] Sending Uplink JSON: %s\n", debugStr.c_str());

                _sendToGateway(doc);
            }
            
            // --- CASE 2: DOWNLINK (GATEWAY -> BRIDGE -> NODE) ---
            else if (msg.source == 1) { 
                Serial.println("[PROC] Processing Downlink from Gateway...");
                
                // 1. Giải mã AES
                PacketHeader header;
                uint8_t decryptedBuf[512];
                int decLen = Security::decryptBinary(msg.buf, msg.len, header, decryptedBuf, MY_AES_KEY);
                
                if (decLen > 0) {
                    // 2. Parse Binary -> JSON
                    JsonDocument doc;
                    PacketUtils::decodeBinaryToJson(decryptedBuf, decLen, doc);
                    
                    String debugStr;
                    serializeJson(doc, debugStr);
                    Serial.printf("[PROC] Decrypted: %s\n", debugStr.c_str());

                    String nid = doc["NID"].as<String>();
                    
                    // 3. Kiểm tra NID
                    if (nid == BRIDGE_DEVICE_ID || nid == "ALL") {
                        int en = doc["en"];
                        
                        // --- EN = 0: YÊU CẦU NGỦ ---
                        if (en == 0) {
                            // Chỉ gửi lệnh ngủ nếu Node đang thức
                            if (digitalRead(PIN_NODE_STATUS) == HIGH) {
                                Serial.println("[PROC] Command: SLEEP -> Sending to Node");
                                _sendToNode("{\"set\":\"SLEEP\"}");
                            } else {
                                Serial.println("[PROC] Command: SLEEP -> Node already sleeping");
                            }
                        } 
                        // --- EN = 1: YÊU CẦU THỰC THI ---
                        else if (en == 1) {
                            Serial.println("[PROC] Command: ACTION (en=1)");
                            
                            // Handshake: Đảm bảo Node thức trước khi gửi
                            if (_wakeUpNode()) {
                                JsonObject reqObj = doc["req"];
                                String reqStr;
                                serializeJson(reqObj, reqStr);
                                _sendToNode(reqStr);
                            } else {
                                Serial.println("[ERR] Node did not wake up (Handshake Timeout)!");
                            }
                        }
                    } else {
                        Serial.printf("[PROC] Ignored (Wrong NID: %s)\n", nid.c_str());
                    }
                } else {
                    Serial.println("[ERR] Decrypt Failed");
                }
            }
        }
    }
}

// ================= GỬI LORA (ENCRYPTED) =================
void StateMachine::_sendToGateway(JsonDocument& doc) {
    uint8_t binBuf[512];
    // Encode JSON object (bao gồm cả nested node object) sang Binary
    int binLen = PacketUtils::encodeJsonToBinary(doc, binBuf, 512);
    
    if (binLen <= 0) {
        Serial.println("[LORA-TX-ERR] Binary Encode Failed");
        return;
    }
    
    PacketHeader header;
    header.nodeId = 1; 
    header.counter = millis();
    
    uint8_t encryptedBuf[512];
    int encLen = Security::encryptBinary(header, binBuf, binLen, encryptedBuf, MY_AES_KEY);
    
    if (xSemaphoreTake(_loraMutex, 1000) == pdTRUE) {
        LoRa.beginPacket();
        LoRa.write(encryptedBuf, encLen);
        LoRa.endPacket();
        xSemaphoreGive(_loraMutex);
        Serial.printf("[LORA-TX] Sent %d bytes (Encrypted)\n", encLen);
    }
}

// ================= GỬI UART (KÈM CRC CHO NODE) =================
void StateMachine::_sendToNode(String jsonCmd) {
    if (jsonCmd.length() == 0) return;
    
    // TÍNH CRC
    unsigned long crc = CRC32::calculate(jsonCmd);
    
    _uart->print(jsonCmd);
    _uart->print("|");
    _uart->println(String(crc, HEX));
    
    Serial.printf("[UART-TX] Payload: %s | CRC: %lX\n", jsonCmd.c_str(), crc);
}

// ================= HANDSHAKE (ĐÁNH THỨC NODE) =================
bool StateMachine::_wakeUpNode() {
    // 1. Kiểm tra nhanh nếu Node đã thức
    if (digitalRead(PIN_NODE_STATUS) == HIGH) {
        Serial.println("[PWR] Node is active.");
        return true; 
    }
    
    Serial.println("[PWR] Waking Node (Pulse Trigger)...");
    
    // 2. Kích xung đánh thức (Pulse)
    // GPIO PIN_NODE_WAKEUP nối với GPIO ExtWakeup của Node (thường là 33 hoặc 26)
    digitalWrite(PIN_NODE_WAKEUP, HIGH);
    vTaskDelay(50 / portTICK_PERIOD_MS); // Giữ 50ms
    digitalWrite(PIN_NODE_WAKEUP, LOW);   // Thả về LOW
    
    // 3. Vòng lặp chờ phản hồi (Timeout 2s)
    unsigned long start = millis();
    while (millis() - start < 2000) { 
        if (digitalRead(PIN_NODE_STATUS) == HIGH) {
            Serial.println("[PWR] Node awake confirmed!");
            
            // Chờ thêm 100ms để UART của Node ổn định hoàn toàn
            vTaskDelay(100 / portTICK_PERIOD_MS); 
            return true;
        }
        vTaskDelay(50 / portTICK_PERIOD_MS); // Check mỗi 50ms
    }
    
    Serial.println("[PWR] Wakeup Timeout!");
    return false; 
}