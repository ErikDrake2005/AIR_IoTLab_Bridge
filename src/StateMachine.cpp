#include "StateMachine.h"

// --- TASK WRAPPERS ---
void StateMachine::uartRxTaskFn(void* param) { ((StateMachine*)param)->_uartRxLoop(); }
void StateMachine::loraRxTaskFn(void* param) { ((StateMachine*)param)->_loraRxLoop(); }
void StateMachine::processTaskFn(void* param) { ((StateMachine*)param)->_processLoop(); }
void StateMachine::powerMonitorTaskFn(void* param) { ((StateMachine*)param)->_powerMonitorLoop(); }

StateMachine::StateMachine() {
    _uart = &Serial2; // Bridge kết nối Node qua Serial2
}

void StateMachine::begin() {
    Serial.println("\n[BRIDGE] System Init (V2 + Power Management)...");

    // 1. Setup UART (921600 - Khớp với Node)
    _uart->begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    _uart->setRxBufferSize(4096); 
    
    // 2. Setup GPIO Handshake & Power Managers
    _pwr.begin();
    _nodePwr.begin();  // Initialize Node power management
    
    _loraMutex = xSemaphoreCreateMutex();
    
    // 3. Setup LoRa
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
    xTaskCreatePinnedToCore(powerMonitorTaskFn, "PWR", 4096, this, 1, NULL, 0);  // Core 0
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
                    continue; // Bỏ qua gói lỗi, KHÔNG return
                }

                // 2. Kiểm tra loại gói tin để chọn Fixed-Schema hoặc Legacy
                const char* nodeType = nodeDoc["type"] | "";
                
                // Parse device ID từ BRIDGE_DEVICE_ID (e.g., "NODE_01" -> 1)
                uint8_t deviceId = 1;
                const char* lastUnderscore = strrchr(BRIDGE_DEVICE_ID, '_');
                if (lastUnderscore) {
                    deviceId = (uint8_t)atoi(lastUnderscore + 1);
                }
                
                uint16_t pinMv = (uint16_t)(_pwr.getCachedVoltage() * 1000);
                bool nodeSleeping = _nodePwr.isNodeSleeping();
                
                uint8_t fixedBuf[64];
                int fixedLen = 0;
                
                // ═══ FIXED-SCHEMA ENCODING (Tiết kiệm ~70% băng thông) ═══
                if (strcmp(nodeType, "data") == 0) {
                    fixedLen = FixedPacket::encodeUplinkData(nodeDoc, deviceId, pinMv, nodeSleeping, fixedBuf);
                    Serial.printf("[TX-FIXED] DATA %d bytes (was ~80)\n", fixedLen);
                }
                else if (strcmp(nodeType, "machine_status") == 0) {
                    fixedLen = FixedPacket::encodeUplinkStatus(nodeDoc, deviceId, pinMv, nodeSleeping, fixedBuf);
                    Serial.printf("[TX-FIXED] STATUS %d bytes (was ~60)\n", fixedLen);
                }
                else if (strcmp(nodeType, "time_req") == 0) {
                    fixedLen = FixedPacket::encodeUplinkTimeReq(deviceId, pinMv, nodeSleeping, fixedBuf);
                    Serial.printf("[TX-FIXED] TIME_REQ %d bytes (was ~30)\n", fixedLen);
                }
                
                // Nếu đã encode thành Fixed-Schema → gửi trực tiếp
                if (fixedLen > 0) {
                    _sendFixedToGateway(fixedBuf, fixedLen);
                }
                // ═══ LEGACY ENCODING (Cho các gói tin khác như WakeUp Ack) ═══
                else {
                    JsonDocument doc;
                    doc["device_ID"] = BRIDGE_DEVICE_ID;
                    doc["pin"] = pinMv;
                    doc["isSleeping"] = nodeSleeping ? 1 : 0;
                    doc["node"] = nodeDoc;
                    
                    Serial.printf("[TX-LEGACY] %s\n", nodeType);
                    _sendToGateway(doc);
                }
            }
            
            // --- CASE 2: DOWNLINK (GATEWAY -> BRIDGE -> NODE) ---
            else if (msg.source == 1) {
                
                // 1. Giải mã AES
                PacketHeader header;
                uint8_t decryptedBuf[512];
                int decLen = Security::decryptBinary(msg.buf, msg.len, header, decryptedBuf, MY_AES_KEY);
                
                if (decLen <= 0) {
                    Serial.println("[ERR] Decrypt Failed");
                    continue;
                }
                
                // ═══ TRY FIXED-SCHEMA DOWNLINK FIRST ═══
                if (decLen > 0 && FixedPacket::isFixedDownlink(decryptedBuf[0])) {
                    
                    // ─── Fixed TIME_SYNC packet (0x80) ───
                    if (decryptedBuf[0] == PKT_DOWNLINK_TIME) {
                        uint32_t epoch = 0;
                        if (FixedPacket::decodeDownlinkTime(decryptedBuf, decLen, epoch)) {
                            Serial.printf("[RX-FIXED] TIME_SYNC epoch=%lu\n", epoch);
                            
                            // Forward to Node as JSON (Node expects JSON over UART)
                            char timeBuf[64];
                            snprintf(timeBuf, sizeof(timeBuf), "{\"set\":\"TIMESTAMP\",\"cmd\":%lu}", epoch);
                            
                            // Wake Node nếu cần
                            if (_nodePwr.isNodeAwake() || _nodePwr.wakeUpNode()) {
                                _sendToNode(timeBuf);
                            }
                        }
                    }
                    // ─── Fixed CMD packet (0x81) ───
                    else if (decryptedBuf[0] == PKT_DOWNLINK_CMD) {
                        uint8_t targetId = 0;
                        uint8_t en = 0;
                        char jsonBuf[256];
                        
                        // Debug: Print raw packet bytes
                        Serial.printf("[RX-FIXED-RAW] CMD bytes: ");
                        for (int i = 0; i < decLen && i < 10; i++) {
                            Serial.printf("%02X ", decryptedBuf[i]);
                        }
                        Serial.println();
                        
                        if (FixedPacket::decodeDownlinkCmd(decryptedBuf, decLen, targetId, en, jsonBuf, sizeof(jsonBuf))) {
                            // Check NID: targetId 0 = ALL, hoặc khớp với ID của Bridge
                            // BRIDGE_DEVICE_ID = "NODE_01" -> ID = 1
                            // Parse from BRIDGE_DEVICE_ID
                            uint8_t myId = 1;
                            const char* lastUnderscore = strrchr(BRIDGE_DEVICE_ID, '_');
                            if (lastUnderscore) {
                                myId = (uint8_t)atoi(lastUnderscore + 1);
                            }
                            
                            Serial.printf("[RX-FIXED] CMD targetId=%d myId=%d en=%d\n", targetId, myId, en);
                            
                            if (targetId == 0 || targetId == myId) {
                                // EN = 0: Yêu cầu Node ngủ
                                if (en == 0) {
                                    Serial.println("[CMD] EN=0 -> Requesting Node to SLEEP");
                                    if (_nodePwr.isNodeAwake()) {
                                        _sendToNode("{\"set\":\"SLEEP\"}");
                                        // Đợi Node xác nhận đã ngủ (GPIO LOW)
                                        vTaskDelay(500 / portTICK_PERIOD_MS);
                                        _nodePwr.confirmSleep();
                                        
                                        // Gửi sleep report lên Gateway
                                        _sendSleepReport();
                                    } else {
                                        Serial.println("[CMD] Node already sleeping");
                                        _sendSleepReport();
                                    }
                                }
                                // EN = 1: Thức Node và thực thi lệnh
                                else {
                                    Serial.printf("[CMD] EN=1 -> Waking Node and sending: %s\n", jsonBuf);
                                    
                                    // Reset cooldown nếu có lệnh en=1 mới
                                    _nodePwr.resetCooldown();
                                    
                                    // Lưu lệnh pending
                                    _nodePwr.setPendingCommand(jsonBuf);
                                    
                                    // Wake Node với retry logic (3 lần, 5s apart)
                                    if (_nodePwr.wakeUpNode()) {
                                        Serial.printf("[TX-NODE] %s\n", jsonBuf);
                                        _sendToNode(jsonBuf);
                                    } else {
                                        Serial.println("[ERR] Node did not wake up after 3 attempts!");
                                        Serial.println("[ERR] Entering 15 minute cooldown");
                                        // Gửi sleep report vì không wake được Node
                                        _sendSleepReport();
                                    }
                                }
                            } else {
                                Serial.printf("[RX-FIXED] CMD ignored (target=%d, my=%d)\n", targetId, myId);
                            }
                        } else {
                            Serial.println("[ERR] Failed to decode CMD packet");
                        }
                    }
                    continue;
                }
                
                // ═══ LEGACY DICTIONARY DECODING (Fallback) ═══
                JsonDocument doc;
                PacketUtils::decodeBinaryToJson(decryptedBuf, decLen, doc);
                Serial.printf("[RX-LEGACY] %d bytes\n", decLen);
                
                const char* nid = doc["NID"] | "";
                
                if (strcmp(nid, BRIDGE_DEVICE_ID) == 0 || strcmp(nid, "ALL") == 0) {
                    int en = doc["en"];
                    
                    // --- EN = 0: YÊU CẦU NGỦ ---
                    if (en == 0) {
                        Serial.println("[LEGACY] EN=0 -> Requesting Node to SLEEP");
                        if (_nodePwr.isNodeAwake()) {
                            _sendToNode("{\"set\":\"SLEEP\"}");
                            vTaskDelay(500 / portTICK_PERIOD_MS);
                            _nodePwr.confirmSleep();
                        }
                        _sendSleepReport();
                    } 
                    // --- EN = 1: YÊU CẦU THỰC THI ---
                    else if (en == 1) {
                        _nodePwr.resetCooldown();
                        
                        char reqBuf[256];
                        serializeJson(doc["req"], reqBuf, sizeof(reqBuf));
                        _nodePwr.setPendingCommand(reqBuf);
                        
                        if (_nodePwr.wakeUpNode()) {
                            _sendToNode(reqBuf);
                        } else {
                            Serial.println("[ERR] Node did not wake up after 3 attempts!");
                            _sendSleepReport();
                        }
                    }
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
        Serial.printf("[TX-LEGACY] %d bytes\n", encLen);
    }
}

// ================= GỬI LORA FIXED-SCHEMA (ENCRYPTED) =================
void StateMachine::_sendFixedToGateway(uint8_t* data, int len) {
    PacketHeader header;
    header.nodeId = 1;
    header.counter = millis();
    
    uint8_t encryptedBuf[128];
    int encLen = Security::encryptBinary(header, data, len, encryptedBuf, MY_AES_KEY);
    
    if (xSemaphoreTake(_loraMutex, 1000) == pdTRUE) {
        LoRa.beginPacket();
        LoRa.write(encryptedBuf, encLen);
        LoRa.endPacket();
        LoRa.receive();
        xSemaphoreGive(_loraMutex);
        Serial.printf("[TX-FIXED] %d bytes (payload %d)\n", encLen, len);
    }
}

// ================= GỬI UART =================
void StateMachine::_sendToNode(const char* jsonCmd) {
    if (!jsonCmd || jsonCmd[0] == '\0') return;
    
    unsigned long crc = CRC32::calculate(jsonCmd);
    _uart->printf("%s|%lX\n", jsonCmd, crc);
}

// ================= HANDSHAKE =================
// Removed old _wakeUpNode() - now using NodePowerManager

// ================= SLEEP REPORT (Gửi khi Node ngủ) =================
void StateMachine::_sendSleepReport() {
    Serial.println("[PWR] Sending sleep report with default values");
    
    // Tạo JSON với giá trị mặc định khi Node ngủ
    JsonDocument nodeDoc;
    nodeDoc["type"] = "machine_status";
    
    JsonObject content = nodeDoc["content"].to<JsonObject>();
    content["mode"] = "MANUAL";
    content["chamberStatus"] = 0;  // stop
    content["doorStatus"] = 1;     // open
    content["fanStatus"] = 0;      // off
    content["saved_manual_cycle"] = 5;
    content["saved_daily_meansure"] = 4;
    content["timestamp"] = 0;  // Will be set by Gateway
    
    // Encode và gửi
    uint8_t deviceId = 1;
    uint16_t pinMv = (uint16_t)(_pwr.getCachedVoltage() * 1000);
    bool nodeSleeping = true;  // Node đang ngủ
    
    uint8_t fixedBuf[32];
    int fixedLen = FixedPacket::encodeUplinkStatus(nodeDoc, deviceId, pinMv, nodeSleeping, fixedBuf);
    
    Serial.printf("[TX-SLEEP] %d bytes, isSleeping=1\n", fixedLen);
    _sendFixedToGateway(fixedBuf, fixedLen);
}

// ================= POWER MONITOR LOOP =================
void StateMachine::_powerMonitorLoop() {
    for (;;) {
        // Cập nhật NodePowerManager
        _nodePwr.update();
        
        // Cập nhật PowerManager (kiểm tra pin định kỳ)
        _pwr.update();
        
        // Kiểm tra nếu cần vào low-power mode
        if (_pwr.shouldEnterLowPower()) {
            _pwr.clearLowPowerFlag();
            _enterLowPowerMode();
        }
        
        vTaskDelay(1000 / portTICK_PERIOD_MS);  // Check mỗi giây
    }
}

// ================= LOW-POWER MODE =================
void StateMachine::_enterLowPowerMode() {
    Serial.println("\n[PWR] *** ENTERING LOW-POWER MODE ***");
    Serial.printf("[PWR] Battery: %.2fV (< %.1fV threshold)\n", 
                  _pwr.getCachedVoltage(), VOLT_LOW_LIMIT);
    
    // 1. Gửi lệnh sleep cho Node nếu đang thức
    if (_nodePwr.isNodeAwake()) {
        Serial.println("[PWR] Requesting Node to sleep...");
        _sendToNode("{\"set\":\"SLEEP\"}");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        _nodePwr.confirmSleep();
    }
    
    // 2. Gửi sleep report cuối cùng
    _sendSleepReport();
    vTaskDelay(500 / portTICK_PERIOD_MS);  // Đợi gửi xong
    
    // 3. Tắt LoRa để tiết kiệm pin
    Serial.println("[PWR] Disabling LoRa...");
    LoRa.sleep();
    
    // 4. Vào deep-sleep 15 phút
    Serial.printf("[PWR] Deep sleep for %d seconds...\n", LOW_POWER_SLEEP_SEC);
    Serial.flush();
    
    // Sau khi wake từ deep-sleep, ESP32 sẽ reset
    // Trong setup() sẽ kiểm tra lại pin và quyết định tiếp tục ngủ hay hoạt động
    _pwr.deepSleep(LOW_POWER_SLEEP_SEC);
    
    // Không bao giờ đến đây (deep-sleep = reset)
}