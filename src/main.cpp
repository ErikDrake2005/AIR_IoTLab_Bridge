#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include "config.h"
#include "PacketDef.h"
#include "Security.h"
#include "KeyConfig.h"

// --- GLOBALS ---
HardwareSerial uart(1);
uint32_t msgCounter = 0; // Bộ đếm gói tin

// --- PROTOTYPES ---
void onLoRaReceive(int packetSize);

void setup() {
    Serial.begin(115200);
    
    // 1. Init UART Bridge (Kết nối với Main MCU)
    uart.setRxBufferSize(2048);
    uart.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.printf("--- BRIDGE STARTED: %s (ID: %d) ---\n", MY_DEVICE_NAME, MY_NODE_INDEX);

    // 2. Init LoRa
    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("[LoRa] Init Failed!");
        while (1);
    }
    
    // Cài đặt LoRa giống hệt Gateway
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.enableCrc();
    
    Serial.println("[LoRa] Ready.");
}

void loop() {
    // 1. Kiểm tra LoRa (Nhận dữ liệu từ Gateway)
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
        onLoRaReceive(packetSize);
    }

    // 2. Kiểm tra UART (Nhận dữ liệu từ Main MCU để gửi đi)
    if (uart.available()) {
        String input = uart.readStringUntil('\n');
        input.trim();
        if (input.length() > 0) {
            Serial.println("[UART] Received: " + input);

            // Parse JSON
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, input);

            if (!error) {
                // --- BƯỚC 1: Chuẩn bị Header ---
                PacketHeader head;
                head.nodeId = MY_NODE_INDEX; // ID số của mình
                head.counter = msgCounter++;

                // --- BƯỚC 2: Nén JSON sang Binary ---
                uint8_t payloadBuf[200];
                // Dùng PacketUtils để nén tự động dựa trên Từ điển
                int payloadLen = PacketUtils::encodeJsonToBinary(doc, payloadBuf, sizeof(payloadBuf));

                // --- BƯỚC 3: Mã hóa ---
                uint8_t encryptedBuf[256];
                int finalLen = Security::encryptBinary(head, payloadBuf, payloadLen, encryptedBuf, MY_AES_KEY);

                // --- BƯỚC 4: Gửi LoRa ---
                LoRa.beginPacket();
                LoRa.write(encryptedBuf, finalLen);
                LoRa.endPacket();
                
                Serial.printf("[LoRa TX] Sent %d bytes (Counter: %d)\n", finalLen, head.counter);
            } else {
                Serial.println("[UART] JSON Error!");
            }
        }
    }
}

// --- XỬ LÝ NHẬN LORA (Gateway -> Node) ---
void onLoRaReceive(int packetSize) {
    if (packetSize > 256) return;
    
    uint8_t buf[256];
    int idx = 0;
    while (LoRa.available()) {
        buf[idx++] = LoRa.read();
    }

    // 1. Giải mã (Decrypt)
    PacketHeader head;
    uint8_t decryptedPayload[200];
    // Hàm decryptBinary sẽ tự tách Header và Payload
    int payloadLen = Security::decryptBinary(buf, packetSize, head, decryptedPayload, MY_AES_KEY);

    if (payloadLen >= 0) {
        // Kiểm tra xem gói tin có phải gửi cho mình không (nếu Gateway gửi Broadcast hoặc Unicast)
        // Hiện tại Gateway gửi xuống thì Node nào nhận được cứ giải mã, 
        // sai Key thì payloadLen sẽ ra rác hoặc checksum fail (nếu có).
        
        Serial.printf("[LoRa RX] From Gateway, Counter: %d\n", head.counter);

        // 2. Giải nén (Binary -> JSON)
        JsonDocument doc;
        PacketUtils::decodeBinaryToJson(decryptedPayload, payloadLen, doc);

        // 3. Đẩy ra UART cho Main MCU xử lý
        String jsonOutput;
        serializeJson(doc, jsonOutput);
        uart.println(jsonOutput); // Gửi: {"cmd":"open_door","val":1}
        
        Serial.println("[UART TX] Forwarded: " + jsonOutput);
    } else {
        Serial.println("[LoRa RX] Decrypt Failed (Wrong Key?)");
    }
}