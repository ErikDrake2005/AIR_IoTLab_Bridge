#pragma once
#include <Arduino.h>
#include "mbedtls/aes.h"
#include "PacketDef.h"

class Security {
public:
    static int encryptBinary(PacketHeader header, void* dataPtr, size_t dataLen, uint8_t *outputBuffer, const char* key) {
        mbedtls_aes_context aes; mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, (const unsigned char*) key, 128);
        outputBuffer[0] = header.nodeId; 
        size_t rawLen = sizeof(uint32_t) + dataLen;
        size_t paddedLen = (rawLen % 16 == 0) ? rawLen : ((rawLen / 16) + 1) * 16;
        uint8_t* tempInput = new uint8_t[paddedLen]; memset(tempInput, 0, paddedLen);
        memcpy(tempInput, &header.counter, 4); 
        if (dataPtr && dataLen > 0) memcpy(tempInput + 4, dataPtr, dataLen);
        for (int i = 0; i < paddedLen; i += 16) 
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, tempInput + i, outputBuffer + 1 + i);
        delete[] tempInput; mbedtls_aes_free(&aes);
        return 1 + paddedLen;
    }
    static int decryptBinary(uint8_t *inputBuffer, int len, PacketHeader &header, uint8_t *outputPayload, const char* key) {
        if (len < 17) return -1;
        int encryptedLen = len - 1;
        header.nodeId = inputBuffer[0];
        mbedtls_aes_context aes; mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_dec(&aes, (const unsigned char*) key, 128);
        uint8_t* tempDecrypted = new uint8_t[encryptedLen];
        for (int i = 0; i < encryptedLen; i += 16) 
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, inputBuffer + 1 + i, tempDecrypted + i);
        memcpy(&header.counter, tempDecrypted, 4); 
        int dataLen = encryptedLen - 4;
        memcpy(outputPayload, tempDecrypted + 4, dataLen);
        delete[] tempDecrypted; mbedtls_aes_free(&aes);
        return dataLen;
    }
};