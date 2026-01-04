#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include "config.h"
#include "PacketDef.h"
#include "Security.h"
#include "KeyConfig.h" // Chứa #define MY_NODE_INDEX và MY_AES_KEY

// --- GLOBALS ---
HardwareSerial uart(1);  // UART nối với Node chính
uint32_t msgCounter = 0; // Bộ đếm gói tin chống trùng lặp

// --- PROTOTYPES ---
void onLoRaReceive(int packetSize);

void setup() {
    // 1. Debug Serial (USB)
    Serial.begin(115200);
    
    // 2. Init UART Bridge (Kết nối với Main Node)
    // Lưu ý: RX_PIN và TX_PIN phải đấu chéo với Node (TX Bridge -> RX Node)
    uart.setRxBufferSize(2048); // Tăng buffer để chứa JSON dài
    uart.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.printf("\n\n--- BRIDGE STARTED: %s (ID: %d) ---\n", MY_DEVICE_NAME, MY_NODE_INDEX);

    // 3. Init LoRa
    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("[LoRa] Init Failed! Check wiring.");
        while (1);
    }
    
    // Cài đặt LoRa khớp hoàn toàn với Gateway
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.enableCrc();
    
    // Bắt đầu lắng nghe ngay lập tức
    LoRa.receive();
    Serial.println("[LoRa] Ready & Listening...");
}

void loop() {
    // ============================================================
    // 1. CHIỀU VỀ: Gateway -> Bridge -> Node (LoRa RX)
    // ============================================================
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
        onLoRaReceive(packetSize);
    }

    // ============================================================
    // 2. CHIỀU ĐI: Node -> Bridge -> Gateway (UART RX)
    // ============================================================
    if (uart.available()) {
        // Đọc chuỗi từ Node gửi sang (Format: {"..."}|CRC_HEX\n)
        String input = uart.readStringUntil('\n');
        input.trim(); // Xóa khoảng trắng thừa đầu đuôi

        if (input.length() > 0 && input.startsWith("{")) {
            Serial.println("[UART RAW] " + input);

            // --- QUAN TRỌNG: CẮT BỎ ĐUÔI CRC (|CRC) ---
            int separatorIdx = input.lastIndexOf('|');
            if (separatorIdx != -1) {
                input = input.substring(0, separatorIdx); // Lấy phần JSON phía trước dấu |
            }

            // --- Parse JSON ---
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, input);

            if (!error) {
                // --- BƯỚC A: Chuẩn bị Header ---
                PacketHeader head;
                head.nodeId = MY_NODE_INDEX; // <--- QUAN TRỌNG: Bridge tự đóng dấu ID của mình
                head.counter = msgCounter++;

                // --- BƯỚC B: Nén JSON sang Binary (Tokenization) ---
                uint8_t payloadBuf[200];
                int payloadLen = PacketUtils::encodeJsonToBinary(doc, payloadBuf, sizeof(payloadBuf));

                if (payloadLen > 0) {
                    // --- BƯỚC C: Mã hóa bảo mật (AES-128) ---
                    uint8_t encryptedBuf[256];
                    
                    // Security::encryptBinary sẽ tự đặt head.nodeId vào byte[0] (công khai)
                    // và mã hóa phần còn lại.
                    int finalLen = Security::encryptBinary(head, payloadBuf, payloadLen, encryptedBuf, MY_AES_KEY);

                    // --- BƯỚC D: Gửi LoRa ---
                    LoRa.beginPacket();
                    LoRa.write(encryptedBuf, finalLen);
                    LoRa.endPacket();
                    
                    // QUAN TRỌNG: Sau khi gửi xong phải gọi receive() để nghe lại ngay
                    LoRa.receive(); 
                    
                    Serial.printf("[LoRa TX] Sent %d bytes (MsgCount: %d, ID: %d)\n", finalLen, head.counter, head.nodeId);
                } else {
                    Serial.println("[Bridge] Encode Failed! (Key not in Dictionary?)");
                }
            } else {
                Serial.print("[Bridge] JSON Parse Error: ");
                Serial.println(error.c_str());
            }
        }
    }
}

// --- HÀM XỬ LÝ NHẬN LORA ---
void onLoRaReceive(int packetSize) {
    if (packetSize < 2) return; // Quá ngắn (phải có ít nhất 1 byte ID + 1 block AES)
    
    uint8_t buf[256];
    int idx = 0;
    while (LoRa.available()) {
        buf[idx++] = LoRa.read();
    }

    // ============================================================
    // [BỘ LỌC CỨNG] KIỂM TRA ID CÔNG KHAI (BYTE 0)
    // ============================================================
    uint8_t targetID = buf[0];
    if (targetID != MY_NODE_INDEX) {
        // Gói tin này không phải gửi cho mình -> Bỏ qua
        // Serial.printf("[Filter] Ignored packet for Node %d\n", targetID); 
        return; 
    }

    Serial.println("[LoRa RX] Valid Packet for ME! Decrypting...");

    // 1. Giải mã (Decrypt)
    PacketHeader head;
    uint8_t decryptedPayload[200];
    
    // Dùng Key của chính mình để giải mã
    int payloadLen = Security::decryptBinary(buf, packetSize, head, decryptedPayload, MY_AES_KEY);

    if (payloadLen >= 0) {
        // 2. Giải nén (Binary -> JSON)
        JsonDocument doc;
        PacketUtils::decodeBinaryToJson(decryptedPayload, payloadLen, doc);

        // 3. Đẩy ra UART xuống Node Main
        String jsonOutput;
        serializeJson(doc, jsonOutput);
        
        uart.println(jsonOutput); 
        
        Serial.println("[UART TX] Forwarded to Node: " + jsonOutput);
    } else {
        // Nếu decrypt thất bại (Key đúng nhưng dữ liệu sai/nhiễu)
        Serial.println("[LoRa RX] Decrypt Failed (Corrupted Data?)");
    }
}