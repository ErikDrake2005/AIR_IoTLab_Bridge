#pragma once
#include <Arduino.h>
#include <ArduinoJson.h> 

// Cấu trúc hàng đợi
typedef struct { uint8_t payload[512]; size_t length; uint8_t nodeId; } LoraQueueMsg;
struct PacketHeader { uint8_t nodeId; uint32_t counter; };

// Enum định nghĩa kiểu dữ liệu nén
enum DataType : uint8_t { 
    DT_END=0, DT_KEY_TOKEN=1, DT_VAL_TOKEN=2, 
    DT_VAL_INT8=3, DT_VAL_INT16=4, DT_VAL_INT32=5, 
    DT_VAL_FLOAT=6, DT_VAL_RAW_STR=7 
};

// [TỪ ĐIỂN] PHẢI GIỐNG HỆT NHAU 100% Ở CẢ 3 THIẾT BỊ
const char* const DICTIONARY[] = {
    // 0-6
    "type", "cmd", "msg", "value", "error", "timestamp", "id",
    // 7-16
    "EN", "ack_rec", "batt", "bridge_volt", "poll", "awake", "sleep", "running", "status", "info",
    // 17-20
    "set_state", "set_door", "set_fans", "set_time", 
    // 21-26
    "mode", "cycle_manual", "measures_per_day", "device", "device_id", "target_id", 
    // 27-35
    "temp", "hum", "ch4", "co", "nh3", "h2", "alc", "rssi", "node_id", "mics",
    // 36-43
    "ack", "data", "manual", "auto", "measure", "stop", "trigger_measure", "stop_measure",
    // 44-47
    "open", "close", "on", "off",
    // 48-52 (Các lệnh đặc biệt mới thêm)
    "MEASURE_STARTED", "time_req", "time_res", "machine_status", "ack_status",
    // 53-56 (Trạng thái Node)
    "door_status", "fan_status", "measuring", "Sleep_Mode", "Pin",
    NULL // <--- BẮT BUỘC PHẢI CÓ
};

class PacketUtils {
public:
    static const char* getString(uint8_t token) {
        int count = 0; while(DICTIONARY[count] != NULL) count++;
        if (token >= count) return nullptr;
        return DICTIONARY[token];
    }

    // --- ENCODER (JSON -> BINARY) ---
    static int encodeJsonToBinary(JsonDocument& doc, uint8_t* buffer, int maxLen) {
        int idx = 0;
        JsonObject root = doc.as<JsonObject>();
        for (JsonPair kv : root) {
            if (idx >= maxLen - 10) break;
            const char* key = kv.key().c_str();
            int keyToken = -1;
            for (int i = 0; DICTIONARY[i] != NULL; i++) {
                if (strcmp(key, DICTIONARY[i]) == 0) { keyToken = i; break; }
            }
            if (keyToken != -1) { buffer[idx++] = DT_KEY_TOKEN; buffer[idx++] = (uint8_t)keyToken; }
            else continue; // Bỏ qua key lạ

            if (kv.value().is<int>()) {
                int32_t val = kv.value().as<int32_t>();
                if (val >= -128 && val <= 127) { buffer[idx++] = DT_VAL_INT8; buffer[idx++] = (int8_t)val; }
                else if (val >= -32768 && val <= 32767) { buffer[idx++] = DT_VAL_INT16; int16_t v = (int16_t)val; memcpy(buffer+idx, &v, 2); idx+=2; }
                else { buffer[idx++] = DT_VAL_INT32; memcpy(buffer+idx, &val, 4); idx+=4; }
            } 
            else if (kv.value().is<float>()) { buffer[idx++] = DT_VAL_FLOAT; float f = kv.value().as<float>(); memcpy(buffer+idx, &f, 4); idx+=4; }
            else if (kv.value().is<const char*>()) {
                const char* s = kv.value().as<const char*>();
                int valToken = -1;
                for (int i = 0; DICTIONARY[i] != NULL; i++) { if (strcmp(s, DICTIONARY[i]) == 0) { valToken = i; break; } }
                if (valToken != -1) { buffer[idx++] = DT_VAL_TOKEN; buffer[idx++] = (uint8_t)valToken; }
                else { buffer[idx++] = DT_VAL_RAW_STR; uint8_t l = strlen(s); if(l>200) l=200; buffer[idx++] = l; memcpy(buffer+idx, s, l); idx+=l; }
            }
        }
        buffer[idx++] = DT_END; return idx;
    }

    // --- DECODER (BINARY -> JSON) ---
    static void decodeBinaryToJson(uint8_t* buffer, int len, JsonDocument& doc) {
        int idx = 0; const char* currentKey = nullptr;
        while (idx < len) {
            uint8_t type = buffer[idx++];
            if (type == DT_END) break;
            if (type == DT_KEY_TOKEN) { uint8_t k = buffer[idx++]; currentKey = getString(k); continue; }
            if (!currentKey) continue;
            
            if (type == DT_VAL_TOKEN) { uint8_t v = buffer[idx++]; doc[currentKey] = getString(v); }
            else if (type == DT_VAL_INT8) doc[currentKey] = (int8_t)buffer[idx++];
            else if (type == DT_VAL_INT16) { int16_t v; memcpy(&v, buffer+idx, 2); idx+=2; doc[currentKey] = v; }
            else if (type == DT_VAL_INT32) { int32_t v; memcpy(&v, buffer+idx, 4); idx+=4; doc[currentKey] = v; }
            else if (type == DT_VAL_FLOAT) { float v; memcpy(&v, buffer+idx, 4); idx+=4; doc[currentKey] = v; }
            else if (type == DT_VAL_RAW_STR) { uint8_t l = buffer[idx++]; char t[256]; memcpy(t, buffer+idx, l); t[l]=0; idx+=l; doc[currentKey] = t; }
        }
    }
};