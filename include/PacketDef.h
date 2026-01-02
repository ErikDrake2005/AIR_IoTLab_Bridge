#pragma once
#include <Arduino.h>
#include <ArduinoJson.h> 

// --- ĐỊNH NGHĨA STRUCT QUEUE (Dùng chung) ---
typedef struct {
    uint8_t payload[256];
    size_t length;
} LoraQueueMsg;

#define ID_MAX_LEN 16

// --- HEADER (5 Bytes) ---
struct PacketHeader {
    uint8_t nodeId;    // Index trong KeyStore
    uint32_t counter;  // Chống Replay Attack
};

// --- DATA TYPE TAGS ---
enum DataType : uint8_t {
    DT_END        = 0x00,
    DT_KEY_TOKEN  = 0x01,
    DT_VAL_TOKEN  = 0x02,
    DT_VAL_INT8   = 0x03,
    DT_VAL_INT16  = 0x04,
    DT_VAL_INT32  = 0x05,
    DT_VAL_FLOAT  = 0x06,
    DT_VAL_RAW_STR= 0x07
};

// --- SIÊU TỪ ĐIỂN (SUPER DICTIONARY) ---
// Đã cập nhật đầy đủ các từ khóa mới cho Node
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h> 

// ... (Giữ nguyên các Struct Header, LoraQueueMsg...)

// --- SIÊU TỪ ĐIỂN (SUPER DICTIONARY) ---
const char* const DICTIONARY[] = {
    // --- [KEYS] TỪ KHÓA LỆNH ---
    "type", "cmd", "status", "val", "error", "timestamp", "unknown",
    
    // [MỚI] Key định dạng mới (Key-Value)
    "set_state",        // Thay cho trigger/stop
    "set_door",         // Thay cho open_door/close_door
    "set_fans",         // Thay cho fans_on/fans_off
    "set_time",         // Vừa là lệnh, vừa chứa giá trị chuỗi
    "mode",             // set_mode
    "cycle_manual",     // Tách biệt với cycle cũ
    "measures_per_day", // Auto config
    "device",           // Device ID
    
    // [CŨ] Key cũ (Giữ để tương thích)
    "cycle", "msg", 
    
    // --- [OTA KEYS] ---
    "ota_start", "ota_chunk", "ota_finish", "total_size", "data",

    // --- [VALUES] GIÁ TRỊ ---
    // [MỚI] Value cho các Key mới
    "measure", "stop", "idle",  // Value của set_state
    "open", "close",            // Value của set_door
    "on", "off",                // Value của set_fans
    "manual", "auto",           // Value của mode

    // [CŨ] Value cũ (Giữ để tương thích)
    "trigger_measure", "stop_measure",
    "open_door", "close_door", 
    "fans_on", "fans_off",
    "ok", "fail", "busy", "done",
    
    // --- [SENSOR DATA KEYS] ---
    "sensor_data", "temp", "hum", "co", "nh3", "h2", "alc", "ch4"
};
const int DICT_SIZE = sizeof(DICTIONARY) / sizeof(DICTIONARY[0]);

class PacketUtils {
public:
    static uint8_t getTokenId(const char* str) {
        for (uint8_t i = 0; i < DICT_SIZE; i++) {
            if (strcmp(DICTIONARY[i], str) == 0) return i;
        }
        return 0xFF;
    }

    static const char* getString(uint8_t id) {
        if (id < DICT_SIZE) return DICTIONARY[id];
        return "unknown";
    }

    // --- ENCODER: JSON -> BINARY ---
    static int encodeJsonToBinary(const JsonDocument& doc, uint8_t* buffer, int maxLen) {
        int idx = 0;
        JsonObjectConst root = doc.as<JsonObjectConst>();

        for (JsonPairConst kv : root) {
            const char* keyStr = kv.key().c_str();
            
            // Bỏ qua ID/Device để tiết kiệm
            if (strcmp(keyStr, "device_id") == 0 || strcmp(keyStr, "device") == 0) continue;

            uint8_t keyToken = getTokenId(keyStr);
            if (keyToken == 0xFF) {
                // [FIX QUAN TRỌNG] Nếu key không có trong từ điển, thay vì bỏ qua,
                // ta có thể chọn bỏ qua (như hiện tại) HOẶC in ra debug để biết.
                // Với file mới này đã đủ key, nên giữ nguyên logic bỏ qua là an toàn.
                continue; 
            }

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
            else if (val.is<float>() || val.is<int>()) { // Support cả int
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

    // --- DECODER: BINARY -> JSON ---
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
                        char tmp[256]; memcpy(tmp, buffer+idx, lenStr); tmp[lenStr] = 0;
                        doc[currentKey] = String(tmp);
                        idx += lenStr;
                        break;
                    }
                }
                currentKey = nullptr; 
            }
        }
    }
};