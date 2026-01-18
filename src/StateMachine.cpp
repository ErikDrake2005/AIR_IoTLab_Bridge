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
        // Có thể là dữ liệu rác từ bootloader, bỏ qua không log lỗi
        // Serial.println("[UART-DBG] No CRC separator (boot garbage?)");
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
        Serial.printf("[UART-ERR] CRC Mismatch! Calc: %lX, Recv: %lX\n", calcCRC, recvCRC);
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

            Serial.printf("[RX] %d bytes, RSSI:%d SNR:%.1f\n", idx, rssi, snr);

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

                // DEBUG: In ra raw JSON từ Node
                Serial.printf("[UART-RAW] %s\n", (char*)msg.buf);

                // 1. Parse JSON từ Node trước
                JsonDocument nodeDoc;
                DeserializationError err = deserializeJson(nodeDoc, (char*)msg.buf);
                
                if (err) {
                    Serial.printf("[PROC-ERR] Parse Node JSON failed: %s\n", err.c_str());
                    return; // Bỏ qua gói lỗi
                }

                // 2. Tạo gói tin Uplink
                JsonDocument doc;
                doc["device_ID"] = BRIDGE_DEVICE_ID;
                doc["pin"] = _pwr.getVoltage();
                doc["rssi"] = _lastRssi;
                doc["snr"] = _lastSnr;
                
                // 3. Copy toàn bộ nội dung từ nodeDoc vào doc["node"]
                doc["node"] = nodeDoc;
                
                // DEBUG: In ra JSON sau khi build
                char debugBuf[512];
                serializeJson(doc, debugBuf, sizeof(debugBuf));
                Serial.printf("[UPLINK-JSON] %s\n", debugBuf);
                
                _sendToGateway(doc);
            }
            
            // --- CASE 2: DOWNLINK (GATEWAY -> BRIDGE -> NODE) ---
            else if (msg.source == 1) {
                
                // 1. Giải mã AES
                PacketHeader header;
                uint8_t decryptedBuf[512];
                int decLen = Security::decryptBinary(msg.buf, msg.len, header, decryptedBuf, MY_AES_KEY);
                
                if (decLen > 0) {
                    // 2. Parse Binary -> JSON
                    JsonDocument doc;
                    PacketUtils::decodeBinaryToJson(decryptedBuf, decLen, doc);
                    
                    const char* nid = doc["NID"] | "";
                    
                    if (strcmp(nid, BRIDGE_DEVICE_ID) == 0 || strcmp(nid, "ALL") == 0) {
                        int en = doc["en"];
                        
                        // --- EN = 0: YÊU CẦU NGỦ ---
                        if (en == 0) {
                            if (digitalRead(PIN_NODE_STATUS) == HIGH) {
                                _sendToNode("{\"set\":\"SLEEP\"}");
                            }
                        } 
                        // --- EN = 1: YÊU CẦU THỰC THI ---
                        else if (en == 1) {
                            if (_wakeUpNode()) {
                                char reqBuf[256];
                                serializeJson(doc["req"], reqBuf, sizeof(reqBuf));
                                _sendToNode(reqBuf);
                            } else {
                                Serial.println("[ERR] Node did not wake up (Handshake Timeout)!");
                            }
                        }
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
        LoRa.receive();
        xSemaphoreGive(_loraMutex);
        Serial.printf("[TX] %d bytes\n", encLen);
    }
}

// ================= GỬI UART =================
void StateMachine::_sendToNode(const char* jsonCmd) {
    if (!jsonCmd || jsonCmd[0] == '\0') return;
    
    unsigned long crc = CRC32::calculate(jsonCmd);
    _uart->printf("%s|%lX\n", jsonCmd, crc);
}

// ================= HANDSHAKE =================
bool StateMachine::_wakeUpNode() {
    if (digitalRead(PIN_NODE_STATUS) == HIGH) return true;
    
    digitalWrite(PIN_NODE_WAKEUP, HIGH);
    vTaskDelay(50 / portTICK_PERIOD_MS);
    digitalWrite(PIN_NODE_WAKEUP, LOW);
    
    unsigned long start = millis();
    while (millis() - start < 2000) {
        if (digitalRead(PIN_NODE_STATUS) == HIGH) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            return true;
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    
    Serial.println("[ERR] Wake timeout");
    return false;
}