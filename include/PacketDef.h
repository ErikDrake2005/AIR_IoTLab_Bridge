#pragma once
#include <Arduino.h>
#include <ArduinoJson.h> 

// --- 1. CẤU TRÚC GÓI TIN & HÀNG ĐỢI ---
typedef struct {
    uint8_t payload[256];
    size_t length;
} LoraQueueMsg;

#define ID_MAX_LEN 16

struct PacketHeader {
    uint8_t nodeId;    // Index định danh thiết bị
    uint8_t type;      // [QUAN TRỌNG] Loại gói tin (0: Data, 1: Cmd, 2: Ack...)
    uint32_t counter;  // Chống Replay Attack
};

// --- 2. CÁC LOẠI DỮ LIỆU (TAGS) ---
enum DataType : uint8_t {
    DT_END         = 0x00,
    DT_KEY_TOKEN   = 0x01,
    DT_VAL_TOKEN   = 0x02,
    DT_VAL_INT8    = 0x03,
    DT_VAL_INT16   = 0x04,
    DT_VAL_INT32   = 0x05,
    DT_VAL_FLOAT   = 0x06,
    DT_VAL_RAW_STR = 0x07
};

// --- 3. SIÊU TỪ ĐIỂN (SUPER DICTIONARY) ---
// Đã bổ sung đầy đủ các lệnh hệ thống (EN, pin...) và cảm biến
const char* const DICTIONARY[] = {
    // --- [GROUP A] CÁC KEY HỆ THỐNG (Dùng cho StateMachine) ---
    "type", "id", "cmd", "timestamp", "status", "info", "error",
    "batt", "bridge_volt", "EN", "pin", "ack", "msg", "value",
    
    // --- [GROUP B] CÁC KEY CẤU HÌNH NODE ---
    "set_state", "set_door", "set_fans", "set_time", 
    "mode", "cycle_manual", "measures_per_day", "device", "device_id",
    
    // --- [GROUP C] DỮ LIỆU CẢM BIẾN ---
    "temp", "hum", "ch4", "co", "nh3", "h2", "alc", "rssi", "node_id", "mics",

    // --- [GROUP D] GIÁ TRỊ (VALUES) ---
    "data", "manual", "auto", "measure", "stop", 
    "trigger_measure", "stop_measure", "open", "close", "on", "off",
    
    // --- [GROUP E] THÔNG BÁO TRẠNG THÁI ---
    "MEASURE_STARTED", "MEASURE_STOPPED",
    "DOOR_OPENED", "DOOR_CLOSED",
    "FANS_ON", "FANS_OFF",
    "CONFIG_OK", "ERR_IN_AUTO", "BUSY", "JSON_ERR",
    "do_full_measure", "time_req",

    // --- [GROUP F] MÃ LỖI CẢM BIẾN ---
    "dht", "sht", "soil_ss", "bh1750", "pzem", "ds18b20", "slave",

    // --- [GROUP G] DỰ PHÒNG (DEF) ---
    "DeFire0", "DeFire1", "DeFire2", "DeFire3", "DeFire4",
    "DeFire5", "DeFire6", "DeFire7", "DeFire8", "DeFire9"
};

// Tính toán kích thước
const int DICT_SIZE = sizeof(DICTIONARY) / sizeof(DICTIONARY[0]);

// --- 4. LỚP TIỆN ÍCH NÉN/GIẢI NÉN & CRC ---
class PacketUtils {
public:
    // [HÀM 1] Lấy ID của chuỗi
    static uint8_t getTokenId(const char* str) {
        for (uint8_t i = 0; i < DICT_SIZE; i++) {
            if (strcmp(DICTIONARY[i], str) == 0) return i;
        }
        return 0xFF; // Không tìm thấy
    }

    // [HÀM 2] Lấy chuỗi từ ID
    static const char* getString(uint8_t id) {
        if (id < DICT_SIZE) return DICTIONARY[id];
        return "unknown";
    }

    // [HÀM 3] TÍNH CRC-16 (Modbus Poly 0xA001) - Dùng cho UART
    static uint16_t calculateCRC(const uint8_t *data, size_t len) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (int j = 0; j < 8; j++) {
                if (crc & 1) crc = (crc >> 1) ^ 0xA001;
                else crc >>= 1;
            }
        }
        return crc;
    }

    // [HÀM 4] KIỂM TRA CRC GÓI TIN NHẬN ĐƯỢC
    static bool checkCRC(const uint8_t *data, size_t totalLen) {
        if (totalLen < 3) return false;
        size_t payloadLen = totalLen - 2;
        uint16_t calc = calculateCRC(data, payloadLen);
        // UART gửi Little Endian (Low trước, High sau)
        uint16_t received = data[payloadLen] | (data[payloadLen+1] << 8);
        return calc == received;
    }

    // [HÀM 5] ENCODER: JSON -> BINARY
    static int encodeJsonToBinary(const JsonDocument& doc, uint8_t* buffer, int maxLen) {
        int idx = 0;
        JsonObjectConst root = doc.as<JsonObjectConst>();

        for (JsonPairConst kv : root) {
            const char* keyStr = kv.key().c_str();
            if (strcmp(keyStr, "device_id") == 0 || strcmp(keyStr, "device") == 0) continue;

            uint8_t keyToken = getTokenId(keyStr);
            if (keyToken == 0xFF) continue; // Key lạ bỏ qua

            if (idx + 10 >= maxLen) break;

            buffer[idx++] = DT_KEY_TOKEN;
            buffer[idx++] = keyToken;

            JsonVariantConst val = kv.value();
            
            if (val.is<const char*>()) {
                const char* s = val.as<const char*>();
                uint8_t valToken = getTokenId(s);
                if (valToken != 0xFF) {
                    buffer[idx++] = DT_VAL_TOKEN;
                    buffer[idx++] = valToken;
                } else {
                    int slen = strlen(s);
                    if (idx + 2 + slen < maxLen) {
                        buffer[idx++] = DT_VAL_RAW_STR;
                        buffer[idx++] = (uint8_t)slen;
                        memcpy(buffer + idx, s, slen);
                        idx += slen;
                    }
                }
            }
            else if (val.is<float>() || val.is<int>()) { 
                 float f = val.as<float>();
                 if (f == (int32_t)f) {
                     int32_t i32 = (int32_t)f;
                     if (i32 >= -128 && i32 <= 127) {
                         buffer[idx++] = DT_VAL_INT8;
                         buffer[idx++] = (int8_t)i32;
                     } else if (i32 >= -32768 && i32 <= 32767) {
                         buffer[idx++] = DT_VAL_INT16;
                         int16_t temp = (int16_t)i32;
                         memcpy(buffer+idx, &temp, 2); idx+=2;
                     } else {
                         buffer[idx++] = DT_VAL_INT32;
                         memcpy(buffer+idx, &i32, 4); idx+=4;
                     }
                 } else {
                     buffer[idx++] = DT_VAL_FLOAT;
                     memcpy(buffer+idx, &f, 4); idx+=4;
                 }
            }
        }
        buffer[idx++] = DT_END;
        return idx;
    }

    // [HÀM 6] DECODER: BINARY -> JSON
    static void decodeBinaryToJson(const uint8_t* buffer, int len, JsonDocument& doc) {
        int idx = 0;
        const char* currentKey = nullptr;

        while (idx < len) {
            uint8_t type = buffer[idx++];
            if (type == DT_END) break;

            if (type == DT_KEY_TOKEN) {
                uint8_t id = buffer[idx++];
                currentKey = getString(id);
            }
            else if (currentKey != nullptr) {
                switch (type) {
                    case DT_VAL_TOKEN:
                        doc[currentKey] = getString(buffer[idx++]);
                        break;
                    case DT_VAL_INT8:
                        doc[currentKey] = (int8_t)buffer[idx++];
                        break;
                    case DT_VAL_INT16: {
                        int16_t v; memcpy(&v, buffer+idx, 2); idx+=2;
                        doc[currentKey] = v;
                        break;
                    }
                    case DT_VAL_INT32: {
                        int32_t v; memcpy(&v, buffer+idx, 4); idx+=4;
                        doc[currentKey] = v;
                        break;
                    }
                    case DT_VAL_FLOAT: {
                        float v; memcpy(&v, buffer+idx, 4); idx+=4;
                        doc[currentKey] = (float)((int)(v * 100 + 0.5)) / 100.0;
                        break;
                    }
                    case DT_VAL_RAW_STR: {
                        uint8_t lenStr = buffer[idx++];
                        char tmp[256]; 
                        if (idx + lenStr <= len) {
                            memcpy(tmp, buffer+idx, lenStr); 
                            tmp[lenStr] = 0;
                            doc[currentKey] = String(tmp);
                            idx += lenStr;
                        }
                        break;
                    }
                }
                currentKey = nullptr; 
            }
        }
    }
};