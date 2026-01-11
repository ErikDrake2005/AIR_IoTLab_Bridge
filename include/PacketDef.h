#pragma once
#include <Arduino.h>
#include <ArduinoJson.h> 

typedef struct {
    uint8_t payload[256];
    size_t length;
    uint8_t nodeId;
} LoraQueueMsg;

struct PacketHeader {
    uint8_t nodeId;    
    uint32_t counter;  
};

enum DataType : uint8_t {
    DT_END = 0x00, DT_KEY_TOKEN = 0x01, DT_VAL_TOKEN = 0x02,
    DT_VAL_INT8 = 0x03, DT_VAL_INT16 = 0x04, DT_VAL_INT32 = 0x05,
    DT_VAL_FLOAT = 0x06, DT_VAL_RAW_STR = 0x07
};

const char* const DICTIONARY[] = {
    "type", "cmd", "msg", "value", "error", "timestamp", "id",
    "EN", "ack_rec", "batt", "bridge_volt", "poll", "awake", "sleep", "running", "status", "info",
    "set_state", "set_door", "set_fans", "set_time", 
    "mode", "cycle_manual", "measures_per_day", "device", "device_id",
    "temp", "hum", "ch4", "co", "nh3", "h2", "alc", "rssi", "node_id", "mics",
    "ack", "data", "manual", "auto", "measure", "stop", 
    "trigger_measure", "stop_measure", "open", "close", "on", "off",
    "MEASURE_STARTED", "MEASURE_STOPPED", "DOOR_OPENED", "DOOR_CLOSED",
    "FANS_ON", "FANS_OFF", "CONFIG_OK", "ERR_IN_AUTO", "BUSY", "JSON_ERR",
    "do_full_measure", "SYSTEM_READY", "time_req",      
    "dht", "sht", "soil_ss", "bh1750", "pzem", "ds18b20", "slave",
    "DeFire0", "DeFire1", "DeFire2", "DeFire3", "DeFire4",
    // --- BỔ SUNG ĐỂ KHỚP VỚI STATEMACHINE MỚI ---
    "door", "fan", "fans", "state", "WAKEUP_BY_GPIO", "UART_WAKEUP"
};

const int DICT_SIZE = sizeof(DICTIONARY) / sizeof(DICTIONARY[0]);

class PacketUtils {
public:
    static uint8_t getTokenId(const char* str) {
        if (!str) return 0xFF;
        for (uint8_t i = 0; i < DICT_SIZE; i++) if (strcmp(DICTIONARY[i], str) == 0) return i;
        return 0xFF;
    }

    static const char* getString(uint8_t id) {
        return (id < DICT_SIZE) ? DICTIONARY[id] : "unknown";
    }

    static int encodeJsonToBinary(const JsonDocument& doc, uint8_t* buffer, int maxLen) {
        int idx = 0;
        JsonObjectConst root = doc.as<JsonObjectConst>();
        for (JsonPairConst kv : root) {
            const char* keyStr = kv.key().c_str();
            // Bỏ qua ID thiết bị vì đã có trong Header LoRa
            if (strcmp(keyStr, "device_id") == 0 || strcmp(keyStr, "device") == 0) continue;
            
            uint8_t keyToken = getTokenId(keyStr);
            if (keyToken == 0xFF) continue; 
            if (idx + 1 >= maxLen) break;

            buffer[idx++] = DT_KEY_TOKEN; 
            buffer[idx++] = keyToken;

            JsonVariantConst val = kv.value();
            if (val.is<const char*>()) {
                const char* s = val.as<const char*>();
                uint8_t valToken = getTokenId(s);
                if (valToken != 0xFF) { 
                    if (idx + 2 > maxLen) break;
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
            } else if (val.is<float>() || val.is<int>()) { 
                 float f = val.as<float>();
                 if (f == (int32_t)f) {
                     int32_t i32 = (int32_t)f;
                     if (i32 >= -128 && i32 <= 127) { 
                         buffer[idx++] = DT_VAL_INT8; 
                         buffer[idx++] = (int8_t)i32; 
                     } else if (i32 >= -32768 && i32 <= 32767) { 
                         buffer[idx++] = DT_VAL_INT16; 
                         int16_t t = (int16_t)i32; 
                         memcpy(buffer+idx, &t, 2); idx+=2; 
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
        if (idx < maxLen) buffer[idx++] = DT_END;
        return idx;
    }

    static void decodeBinaryToJson(const uint8_t* buffer, int len, JsonDocument& doc) {
        int idx = 0; 
        const char* currentKey = nullptr;
        while (idx < len) {
            uint8_t type = buffer[idx++];
            if (type == DT_END) break;
            if (type == DT_KEY_TOKEN) {
                currentKey = getString(buffer[idx++]);
            } else if (currentKey) {
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
                        doc[currentKey] = v; // Giữ nguyên giá trị float
                        break; 
                    }
                    case DT_VAL_RAW_STR: { 
                        uint8_t l = buffer[idx++]; 
                        if(idx + l <= len){
                            char t[256];
                            memcpy(t, buffer + idx, l); 
                            t[l] = 0; 
                            doc[currentKey] = String(t); 
                            idx += l;
                        } 
                        break; 
                    }
                }
                currentKey = nullptr; 
            }
        }
    }
};