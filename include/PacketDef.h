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
    uint32_t counter;  // Chống tấn công phát lại (Replay Attack)
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
    DT_VAL_RAW_STR = 0x07  // Dùng khi chuỗi không có trong từ điển
};

// --- 3. SIÊU TỪ ĐIỂN (SUPER DICTIONARY) ---
// QUAN TRỌNG: Thứ tự chuỗi trong mảng này phải GIỐNG HỆT nhau 
// ở cả Gateway và Bridge/Node. Không được lệch index.

const char* const DICTIONARY[] = {
    // --- [KEYS] TỪ KHÓA CỦA NODE ---
    "type", "cmd", "msg", "value", "error", "timestamp",
    "set_state", "set_door", "set_fans", "set_time", 
    "mode", "cycle_manual", "measures_per_day", "device", "device_id",
    
    // --- [SENSOR DATA] ---
    "temp", "hum", "ch4", "co", "nh3", "h2", "alc", "rssi", "node_id",

    // --- [VALUES] GIÁ TRỊ LỆNH ---
    "ack", "data",                 // Response Type
    "manual", "auto",              // Mode
    "measure", "stop",             // State
    "trigger_measure", "stop_measure", // Cmd
    "open", "close",               // Door val
    "on", "off",                   // Fan val
    
    // --- [MỚI - TỐI ƯU] CÁC CÂU PHẢN HỒI THƯỜNG GẶP (Tiết kiệm 10-15 bytes/gói) ---
    "MEASURE_STARTED",
    "MEASURE_STOPPED",
    "DOOR_OPENED",
    "DOOR_CLOSED",
    "FANS_ON",
    "FANS_OFF",
    "CONFIG_OK",
    "ERR_IN_AUTO",
    "BUSY",
    "JSON_ERR",
    "do_full_measure"
};

const int DICT_SIZE = sizeof(DICTIONARY) / sizeof(DICTIONARY[0]);

// --- 4. LỚP TIỆN ÍCH NÉN/GIẢI NÉN ---
class PacketUtils {
public:
    // Tìm ID của chuỗi trong từ điển
    static uint8_t getTokenId(const char* str) {
        for (uint8_t i = 0; i < DICT_SIZE; i++) {
            if (strcmp(DICTIONARY[i], str) == 0) return i;
        }
        return 0xFF; // Không tìm thấy
    }

    // Lấy chuỗi từ ID
    static const char* getString(uint8_t id) {
        if (id < DICT_SIZE) return DICTIONARY[id];
        return "unknown";
    }

    // --- ENCODER: JSON -> BINARY ---
    // Nén JsonDocument thành mảng byte để gửi LoRa
    static int encodeJsonToBinary(const JsonDocument& doc, uint8_t* buffer, int maxLen) {
        int idx = 0;
        JsonObjectConst root = doc.as<JsonObjectConst>();

        for (JsonPairConst kv : root) {
            const char* keyStr = kv.key().c_str();
            
            // Bỏ qua các key không cần thiết gửi qua LoRa để tiết kiệm
            if (strcmp(keyStr, "device_id") == 0 || strcmp(keyStr, "device") == 0) continue;

            uint8_t keyToken = getTokenId(keyStr);
            if (keyToken == 0xFF) {
                // Key lạ => Bỏ qua (hoặc có thể gửi dạng RAW nếu muốn, nhưng ở đây ta bỏ qua cho gọn)
                continue; 
            }

            if (idx + 10 >= maxLen) break; // Tràn buffer

            buffer[idx++] = DT_KEY_TOKEN;
            buffer[idx++] = keyToken;

            JsonVariantConst val = kv.value();
            
            // 1. Xử lý chuỗi (String)
            if (val.is<const char*>()) {
                const char* s = val.as<const char*>();
                uint8_t valToken = getTokenId(s);
                
                if (valToken != 0xFF) {
                    // Chuỗi có trong từ điển -> Nén còn 2 bytes
                    buffer[idx++] = DT_VAL_TOKEN;
                    buffer[idx++] = valToken;
                } else {
                    // Chuỗi lạ -> Gửi nguyên bản (Tốn dung lượng nhưng an toàn)
                    int slen = strlen(s);
                    if (idx + 2 + slen < maxLen) {
                        buffer[idx++] = DT_VAL_RAW_STR;
                        buffer[idx++] = (uint8_t)slen;
                        memcpy(buffer + idx, s, slen);
                        idx += slen;
                    }
                }
            }
            // 2. Xử lý số (Float/Int)
            else if (val.is<float>() || val.is<int>()) { 
                 float f = val.as<float>();
                 
                 // Kiểm tra xem có phải số nguyên không để nén nhỏ hơn
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
                     // Số thực (Float) -> 5 bytes
                     buffer[idx++] = DT_VAL_FLOAT;
                     memcpy(buffer+idx, &f, 4); idx+=4;
                 }
            }
        }
        buffer[idx++] = DT_END; // Kết thúc gói
        return idx;
    }

    // --- DECODER: BINARY -> JSON ---
    // Giải mã mảng byte LoRa thành JsonDocument
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
                        // Làm tròn 2 số lẻ cho đẹp (VD: 30.550000 -> 30.55)
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
                currentKey = nullptr; // Reset sau khi gán value xong
            }
        }
    }
};
