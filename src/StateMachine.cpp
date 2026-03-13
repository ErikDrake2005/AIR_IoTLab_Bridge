#include "StateMachine.h"

void StateMachine::uartRxTaskFn(void* param) { ((StateMachine*)param)->_uartRxLoop(); }
void StateMachine::loraRxTaskFn(void* param) { ((StateMachine*)param)->_loraRxLoop(); }
void StateMachine::processTaskFn(void* param) { ((StateMachine*)param)->_processLoop(); }
void StateMachine::powerMonitorTaskFn(void* param) { ((StateMachine*)param)->_powerMonitorLoop(); }

StateMachine::StateMachine() {
    _uart = &Serial2;
}

void StateMachine::begin() {
    Serial.println("\n[BRIDGE] System Init (V2 + Power Management)...");
    _uart->begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    _uart->setRxBufferSize(4096); 
    _pwr.begin();
    _nodePwr.begin();
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

    _processQueue = xQueueCreate(PROCESS_QUEUE_SIZE, sizeof(PacketMsg));

    xTaskCreatePinnedToCore(uartRxTaskFn, "U_RX", 4096, this, 2, NULL, 1);
    xTaskCreatePinnedToCore(loraRxTaskFn, "L_RX", 4096, this, 2, NULL, 1);
    xTaskCreatePinnedToCore(processTaskFn,"PROC", 8192, this, 1, NULL, 1);
    xTaskCreatePinnedToCore(powerMonitorTaskFn, "PWR", 4096, this, 1, NULL, 0);  // Core 0
}

//RX
void StateMachine::_uartRxLoop() {
    String inputBuf = "";
    inputBuf.reserve(512);

    for (;;) {
        if (_uart->available()) {
            while (_uart->available()) {
                char c = (char)_uart->read();
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

bool StateMachine::_extractJsonFromUart(String raw, String& outJson) {
    int pipeIdx = raw.lastIndexOf('|');
    if (pipeIdx == -1) {
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
//RX Gateway
void StateMachine::_loraRxLoop() {
    for (;;) {
        int packetSize = LoRa.parsePacket();
        if (packetSize > 0) {
            uint8_t buf[256];
            int idx = 0;
            while (LoRa.available() && idx < 256) {
                buf[idx++] = LoRa.read();
            }
            int rssi = LoRa.packetRssi();
            float snr = LoRa.packetSnr();
            _lastRssi = rssi;
            _lastSnr = snr;

            Serial.printf("[RX] %d bytes, RSSI:%d SNR:%.1f\n", idx, rssi, snr);

            PacketMsg msg;
            msg.source = 1;
            msg.len = idx;
            memcpy(msg.buf, buf, idx);
            xQueueSend(_processQueue, &msg, 0);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
//Process Center
void StateMachine::_processLoop() {
    PacketMsg msg;
    for (;;) {
        if (xQueueReceive(_processQueue, &msg, portMAX_DELAY) == pdTRUE) {
            // Uplink
            if (msg.source == 0) {
                Serial.printf("[UART-RAW] %s\n", (char*)msg.buf);
                JsonDocument nodeDoc;
                DeserializationError err = deserializeJson(nodeDoc, (char*)msg.buf);

                if (err) {
                    Serial.printf("[PROC-ERR] Parse Node JSON failed: %s\n", err.c_str());
                    continue;
                }
                // Filter - bỏ qua gói nội bộ không cần forward lên Gateway
                const char* nodeType = nodeDoc["type"] | "";
                if (strlen(nodeType) == 0 || strcmp(nodeType, "ACK") == 0) {
                    Serial.printf("[PROC] Skipping internal packet (type='%s'), not forwarding to Gateway\n",
                                  strlen(nodeType) == 0 ? "(none)" : nodeType);
                    continue;
                }
                uint8_t deviceId = 1;
                const char* lastUnderscore = strrchr(BRIDGE_DEVICE_ID, '_');
                if (lastUnderscore) {
                    deviceId = (uint8_t)atoi(lastUnderscore + 1);
                }
                
                uint16_t pinMv = (uint16_t)(_pwr.getCachedVoltage() * 1000);
                bool nodeSleeping = _nodePwr.isNodeSleeping();
                
                uint8_t fixedBuf[64];
                int fixedLen = 0;
                
                // Using FIXED-SCHEMA ENCODING
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
                if (fixedLen > 0) {
                    _sendFixedToGateway(fixedBuf, fixedLen);
                }
                // default LEGACY ENCODING
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
            
            // DOwnlink
            else if (msg.source == 1) {
                
                // 1. Giải mã AES
                PacketHeader header;
                uint8_t decryptedBuf[512];
                int decLen = Security::decryptBinary(msg.buf, msg.len, header, decryptedBuf, MY_AES_KEY);
                
                if (decLen <= 0) {
                    Serial.println("[ERR] Decrypt Failed");
                    continue;
                }
                
                // TRY FIXED-SCHEMA DOWNLINK 
                if (decLen > 0 && FixedPacket::isFixedDownlink(decryptedBuf[0])) {
                    if (decryptedBuf[0] == PKT_DOWNLINK_TIME) {
                        uint32_t epoch = 0;
                        if (FixedPacket::decodeDownlinkTime(decryptedBuf, decLen, epoch)) {
                            Serial.printf("[RX-FIXED] TIME_SYNC epoch=%lu\n", epoch);
                            char timeBuf[64];
                            snprintf(timeBuf, sizeof(timeBuf), "{\"set\":\"TIMESTAMP\",\"cmd\":%lu}", epoch);
                            if (_nodePwr.isNodeAwake() || _nodePwr.wakeUpNode()) {
                                _sendToNode(timeBuf);
                            }
                        }
                    }
                    else if (decryptedBuf[0] == PKT_DOWNLINK_CMD) {
                        uint8_t targetId = 0;
                        uint8_t en = 0;
                        char jsonBuf[256];
                        Serial.printf("[RX-FIXED-RAW] CMD bytes: ");
                        for (int i = 0; i < decLen && i < 10; i++) {
                            Serial.printf("%02X ", decryptedBuf[i]);
                        }
                        Serial.println();
                        
                        if (FixedPacket::decodeDownlinkCmd(decryptedBuf, decLen, targetId, en, jsonBuf, sizeof(jsonBuf))) {
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
                                        vTaskDelay(500 / portTICK_PERIOD_MS);
                                        _nodePwr.confirmSleep();
                                        _sendSleepReport();
                                    } else {
                                        Serial.println("[CMD] Node already sleeping");
                                        _sendSleepReport();
                                    }
                                }
                                else {
                                    Serial.printf("[CMD] EN=1 -> Waking Node and sending: %s\n", jsonBuf);
                                    _nodePwr.resetCooldown();
                                    _nodePwr.setPendingCommand(jsonBuf);
                                    if (_nodePwr.wakeUpNode()) {
                                        Serial.printf("[TX-NODE] %s\n", jsonBuf);
                                        _sendToNode(jsonBuf);
                                    } else {
                                        Serial.println("[ERR] Node did not wake up after 3 attempts!");
                                        Serial.println("[ERR] Entering 15 minute cooldown");
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
                JsonDocument doc;
                PacketUtils::decodeBinaryToJson(decryptedBuf, decLen, doc);
                Serial.printf("[RX-LEGACY] %d bytes\n", decLen);
                const char* nid = doc["NID"] | "";
                if (strcmp(nid, BRIDGE_DEVICE_ID) == 0 || strcmp(nid, "ALL") == 0) {
                    int en = doc["en"];
                    if (en == 0) {
                        Serial.println("[LEGACY] EN=0 -> Requesting Node to SLEEP");
                        if (_nodePwr.isNodeAwake()) {
                            _sendToNode("{\"set\":\"SLEEP\"}");
                            vTaskDelay(500 / portTICK_PERIOD_MS);
                            _nodePwr.confirmSleep();
                        }
                        _sendSleepReport();
                    } 
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

// Lấy nodeId từ BRIDGE_DEVICE_ID (VD: "NODE_01" -> 1)
static uint8_t getBridgeNodeId() {
    const char* p = strrchr(BRIDGE_DEVICE_ID, '_');
    return p ? (uint8_t)atoi(p + 1) : 1;
}

// LoRa Send to Gateway
void StateMachine::_sendToGateway(JsonDocument& doc) {
    uint8_t binBuf[512];
    int binLen = PacketUtils::encodeJsonToBinary(doc, binBuf, 512);
    if (binLen <= 0) {
        Serial.println("[LORA-TX-ERR] Binary Encode Failed");
        return;
    }

    PacketHeader header;
    header.nodeId = getBridgeNodeId();
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

void StateMachine::_sendFixedToGateway(uint8_t* data, int len) {
    PacketHeader header;
    header.nodeId = getBridgeNodeId();
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

void StateMachine::_sendToNode(const char* jsonCmd) {
    if (!jsonCmd || jsonCmd[0] == '\0') return;
    
    unsigned long crc = CRC32::calculate(jsonCmd);
    _uart->printf("%s|%lX\n", jsonCmd, crc);
}

void StateMachine::_sendSleepReport() {
    Serial.println("[PWR] Sending sleep report with default values");
    JsonDocument nodeDoc;
    nodeDoc["type"] = "machine_status";
    JsonObject content = nodeDoc["content"].to<JsonObject>();
    content["mode"] = "MANUAL";
    content["chamberStatus"] = 0;  // stop
    content["doorStatus"] = 1;     // open
    content["fanStatus"] = 0;      // off
    content["saved_manual_cycle"] = 5;
    content["saved_daily_meansure"] = 4;
    content["timestamp"] = 0;
    
    // Encode and send
    uint8_t deviceId = 1;
    uint16_t pinMv = (uint16_t)(_pwr.getCachedVoltage() * 1000);
    bool nodeSleeping = true;  // Node is sleeping
    
    uint8_t fixedBuf[32];
    int fixedLen = FixedPacket::encodeUplinkStatus(nodeDoc, deviceId, pinMv, nodeSleeping, fixedBuf);
    
    Serial.printf("[TX-SLEEP] %d bytes, isSleeping=1\n", fixedLen);
    _sendFixedToGateway(fixedBuf, fixedLen);
}
void StateMachine::_powerMonitorLoop() {
    for (;;) {
        _nodePwr.update();
        _pwr.update();
        if (_pwr.shouldEnterLowPower()) {
            _pwr.clearLowPowerFlag();
            _enterLowPowerMode();
        }
        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void StateMachine::_enterLowPowerMode() {
    Serial.println("\n[PWR] *** ENTERING LOW-POWER MODE ***");
    Serial.printf("[PWR] Battery: %.2fV (< %.1fV threshold)\n", 
                  _pwr.getCachedVoltage(), VOLT_LOW_LIMIT);
    if (_nodePwr.isNodeAwake()) {
        Serial.println("[PWR] Requesting Node to sleep...");
        _sendToNode("{\"set\":\"SLEEP\"}");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        _nodePwr.confirmSleep();
    }
    _sendSleepReport();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    Serial.println("[PWR] Disabling LoRa...");
    LoRa.sleep();
    Serial.printf("[PWR] Deep sleep for %d seconds...\n", LOW_POWER_SLEEP_SEC);
    Serial.flush();
    _pwr.deepSleep(LOW_POWER_SLEEP_SEC);
}