#pragma once
#include <Arduino.h>
#include "mbedtls/aes.h"
#include "PacketDef.h"

class Security {
public:
    static int encryptBinary(PacketHeader header, void* dataPtr, size_t dataLen, uint8_t *outputBuffer, const char* key) {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, (const unsigned char*) key, 128);
        int inputLen = sizeof(PacketHeader) + dataLen;
        int paddedLen = (inputLen % 16 == 0) ? inputLen : ((inputLen / 16) + 1) * 16;
        uint8_t tempBuffer[paddedLen];
        memset(tempBuffer, 0, paddedLen);
        memcpy(tempBuffer, &header, sizeof(PacketHeader));
        if (dataPtr != nullptr && dataLen > 0) {
            memcpy(tempBuffer + sizeof(PacketHeader), dataPtr, dataLen);
        }
        for (int i = 0; i < paddedLen; i += 16) {
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, tempBuffer + i, outputBuffer + i);
        }
        mbedtls_aes_free(&aes);
        return paddedLen; 
    }
    // GIẢI MÃ (Kèm Key)
    static int decryptBinary(uint8_t *inputBuffer, int len, PacketHeader &header, uint8_t *outputPayload, const char* key) {
        if (len % 16 != 0) return -1; 
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_dec(&aes, (const unsigned char*) key, 128);
        uint8_t plainBuffer[len];
        for (int i = 0; i < len; i += 16) {
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, inputBuffer + i, plainBuffer + i);
        }
        mbedtls_aes_free(&aes);
        memcpy(&header, plainBuffer, sizeof(PacketHeader));
        // Kiểm tra msgType có hợp lệ không (chống key sai)
        uint8_t msgType = plainBuffer[sizeof(PacketHeader)];
        if (msgType < 0x01 || msgType > 0x05) return -1; 

        int payloadLen = len - sizeof(PacketHeader);
        if (payloadLen > 0) {
            memcpy(outputPayload, plainBuffer + sizeof(PacketHeader), payloadLen);
        }
        return payloadLen;
    }
};