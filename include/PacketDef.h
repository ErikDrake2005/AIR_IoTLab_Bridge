#pragma once
#include <Arduino.h>
#include <ArduinoJson.h> 

// Header gói tin LoRa (5 bytes): ID + Counter
struct PacketHeader { uint8_t nodeId; uint32_t counter; };

// Enum kiểu dữ liệu nén
enum DataType : uint8_t { 
    DT_END=0, DT_KEY_TOKEN=1, DT_VAL_TOKEN=2, DT_VAL_INT8=3, DT_VAL_INT16=4, 
    DT_VAL_INT32=5, DT_VAL_FLOAT=6, DT_VAL_RAW_STR=7, DT_OBJ_START=8, DT_OBJ_END=9, DT_NULL=10        
};

// [TỪ ĐIỂN] PHẢI ĐỒNG BỘ 100% VỚI BRIDGE VÀ NODE
const char* const DICTIONARY[] = {
    // Basic Keys
    "type", "NID", "en", "req", "set", "cmd", "do", "content", "node", "id",
    "device_ID", "device", "target_id", "node_id", "timestamp", "pin", "rssi", "batt", "bridge_volt", "Pin",
    
    // Values & Status
    "AUTO", "MANUAL", "TIMESTAMP", "SLEEP", "ack", "error", "msg", "value", "info", "status",
    "awake", "sleep", "running", "poll", "connected", "disconnected", "open", "close", "on", "off",
    "start", "stop", "trigger_measure", "stop_measure", "set_state", "set_door", "set_fans", 
    "WakeUp", "MEASURE_STARTED", "ack_rec",
    
    // Sensors
    "temp", "hum", "ch4", "co", "nh3", "h2", "alc", "c2h5oh", "measuring", "measure_status",
    
    // NEW KEYS (V2 PROTOCOL)
    "transmissionIntervalMinutes", 
    "measurementCount", 
    "startTime", 
    "cycle_manual", 
    "measures_per_day", 
    "schedules",
    "chamberStatus", 
    "doorStatus", 
    "fanStatus", 
    "saved_manual_cycle", 
    "saved_daily_meansure", 
    "Sleep_Mode", 
    "machine_status",
    "time_req", 
    "time_res", 
    "data", 
    "gw_rssi", 
    "gw_snr",
    
    NULL // Kết thúc
};

class PacketUtils {
public:
    static const char* getString(uint8_t token) {
        int count = 0; while(DICTIONARY[count] != NULL) count++;
        if (token >= count) return nullptr;
        return DICTIONARY[token];
    }
    // Encode JSON -> Binary (Hỗ trợ Nested Object)
    static int encodeJsonToBinary(JsonDocument& doc, uint8_t* buffer, int maxLen) {
        int idx = 0;
        JsonObject root = doc.as<JsonObject>();
        serializeObject(root, buffer, idx, maxLen);
        buffer[idx++] = DT_END; return idx;
    }
    // Decode Binary -> JSON
    static void decodeBinaryToJson(uint8_t* buffer, int len, JsonDocument& doc) {
        int idx = 0;
        JsonObject root = doc.to<JsonObject>();
        deserializeObject(root, buffer, idx, len);
    }
private:
    static void serializeObject(JsonObject obj, uint8_t* buffer, int& idx, int maxLen) {
        for (JsonPair kv : obj) {
            if (idx >= maxLen - 10) return;
            const char* key = kv.key().c_str();
            int keyToken = -1;
            for (int i = 0; DICTIONARY[i] != NULL; i++) {
                if (strcmp(key, DICTIONARY[i]) == 0) { keyToken = i; break; }
            }
            if (keyToken != -1) { buffer[idx++] = DT_KEY_TOKEN; buffer[idx++] = (uint8_t)keyToken; } 
            else continue; // Bỏ qua nếu key không có trong từ điển
            
            JsonVariant v = kv.value();
            if (v.is<JsonObject>()) { 
                buffer[idx++] = DT_OBJ_START; 
                serializeObject(v.as<JsonObject>(), buffer, idx, maxLen); 
                buffer[idx++] = DT_OBJ_END; 
            } 
            else if (v.isNull()) { buffer[idx++] = DT_NULL; }
            else if (v.is<int>()) {
                int32_t val = v.as<int32_t>();
                if (val >= -128 && val <= 127) { buffer[idx++] = DT_VAL_INT8; buffer[idx++] = (int8_t)val; } 
                else if (val >= -32768 && val <= 32767) { buffer[idx++] = DT_VAL_INT16; int16_t x = (int16_t)val; memcpy(buffer+idx, &x, 2); idx+=2; } 
                else { buffer[idx++] = DT_VAL_INT32; memcpy(buffer+idx, &val, 4); idx+=4; }
            } else if (v.is<float>()) { buffer[idx++] = DT_VAL_FLOAT; float f = v.as<float>(); memcpy(buffer+idx, &f, 4); idx+=4; }
            else if (v.is<const char*>()) {
                const char* s = v.as<const char*>();
                int valToken = -1;
                for (int i = 0; DICTIONARY[i] != NULL; i++) { if (strcmp(s, DICTIONARY[i]) == 0) { valToken = i; break; } }
                if (valToken != -1) { buffer[idx++] = DT_VAL_TOKEN; buffer[idx++] = (uint8_t)valToken; } 
                else { buffer[idx++] = DT_VAL_RAW_STR; uint8_t l = strlen(s); if(l>200) l=200; buffer[idx++] = l; memcpy(buffer+idx, s, l); idx+=l; }
            }
        }
    }
    static void deserializeObject(JsonObject obj, uint8_t* buffer, int& idx, int len) {
        const char* currentKey = nullptr;
        while (idx < len) {
            uint8_t type = buffer[idx++];
            if (type == DT_END) return;
            if (type == DT_OBJ_END) return;
            if (type == DT_KEY_TOKEN) { uint8_t k = buffer[idx++]; currentKey = getString(k); continue; }
            if (!currentKey) continue;
            if (type == DT_OBJ_START) { JsonObject nested = obj[currentKey].to<JsonObject>(); deserializeObject(nested, buffer, idx, len); }
            else if (type == DT_NULL) { obj[currentKey] = (char*)nullptr; }
            else if (type == DT_VAL_TOKEN) { uint8_t v = buffer[idx++]; obj[currentKey] = getString(v); }
            else if (type == DT_VAL_INT8) { obj[currentKey] = (int8_t)buffer[idx++]; }
            else if (type == DT_VAL_INT16) { int16_t v; memcpy(&v, buffer+idx, 2); idx+=2; obj[currentKey] = v; }
            else if (type == DT_VAL_INT32) { int32_t v; memcpy(&v, buffer+idx, 4); idx+=4; obj[currentKey] = v; }
            else if (type == DT_VAL_FLOAT) { float v; memcpy(&v, buffer+idx, 4); idx+=4; obj[currentKey] = v; }
            else if (type == DT_VAL_RAW_STR) { uint8_t l = buffer[idx++]; char t[256]; memcpy(t, buffer+idx, l); t[l]=0; idx+=l; obj[currentKey] = t; }
        }
    }
};