#pragma once
#include <Arduino.h>
#include "mbedtls/aes.h"
#include "PacketDef.h"

class Security {
public:
    // --- MÃ HÓA (ENCRYPT) ---
    // Output: [Byte 0: NodeID] + [Byte 1..N: Encrypted Data]
    static int encryptBinary(PacketHeader header, void* dataPtr, size_t dataLen, uint8_t *outputBuffer, const char* key) {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, (const unsigned char*) key, 128);

        // 1. Ghi NodeID (Cleartext) vào đầu gói tin
        outputBuffer[0] = header.nodeId;

        // 2. Chuẩn bị dữ liệu cần mã hóa (Gồm Counter + Payload)
        size_t rawLen = sizeof(uint32_t) + dataLen;
        size_t paddedLen = (rawLen % 16 == 0) ? rawLen : ((rawLen / 16) + 1) * 16;
        
        uint8_t tempInput[paddedLen];
        memset(tempInput, 0, paddedLen); // Fill 0 padding

        // Đóng gói: [Counter] + [Payload]
        memcpy(tempInput, &header.counter, sizeof(uint32_t));
        if (dataPtr != nullptr && dataLen > 0) {
            memcpy(tempInput + sizeof(uint32_t), dataPtr, dataLen);
        }

        // 3. Mã hóa và ghi vào buffer từ vị trí số 1 trở đi
        for (int i = 0; i < paddedLen; i += 16) {
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, tempInput + i, outputBuffer + 1 + i);
        }
        
        mbedtls_aes_free(&aes);
        return 1 + paddedLen; // Trả về độ dài tổng (1 byte ID + Encrypted part)
    }

    // --- GIẢI MÃ (DECRYPT) ---
    static int decryptBinary(uint8_t *inputBuffer, int len, PacketHeader &header, uint8_t *outputPayload, const char* key) {
        if (len < 17) return -1; // Tối thiểu 1 byte ID + 16 byte block
        int encryptedLen = len - 1;
        if (encryptedLen % 16 != 0) return -1;

        // 1. Lấy NodeID (để tham khảo)
        header.nodeId = inputBuffer[0];

        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_dec(&aes, (const unsigned char*) key, 128);

        uint8_t plainBuffer[encryptedLen];
        
        // 2. Giải mã phần thân (Bỏ qua byte đầu tiên)
        for (int i = 0; i < encryptedLen; i += 16) {
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, inputBuffer + 1 + i, plainBuffer + i);
        }
        mbedtls_aes_free(&aes);

        // 3. Tách Counter và Payload
        memcpy(&header.counter, plainBuffer, sizeof(uint32_t));
        int payloadLen = encryptedLen - sizeof(uint32_t);
        memcpy(outputPayload, plainBuffer + sizeof(uint32_t), payloadLen);

        return payloadLen;
    }
};