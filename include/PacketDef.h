#pragma once
#include <Arduino.h>
#define ID_MAX_LEN 16 // quy định số ký tự tối đa cho Device ID
// --- HEADER CHUNG (20 bytes) ---
struct PacketHeader {
    char deviceId[ID_MAX_LEN]; 
    uint32_t counter;          
};

// --- CÁC LOẠI GÓI TIN (MsgType) ---
enum MsgType : uint8_t {
    TYPE_SENSOR_DATA = 0x01, // Node -> Gateway: Dữ liệu đo đạc
    TYPE_CMD_CTRL = 0x02, // Gateway -> Node: Lệnh điều khiển (Open, Fan, Mode...)
    TYPE_CMD_CONFIG = 0x03, // Gateway -> Node: Cài đặt (Time, Cycle)
    TYPE_RESPONSE = 0x04, // Node -> Gateway: Phản hồi (ACK hoặc ERROR)
    TYPE_TIME_REQ = 0x05  // Node -> Gateway: Xin cập nhật giờ hệ thống
};

// --- MÃ LỆNH CHI TIẾT ---
enum CmdCode : uint8_t {
    // 1. NHÓM ĐIỀU KHIỂN (Gateway gửi xuống)
    CMD_MODE_MANUAL = 10,
    CMD_MODE_AUTO = 11,
    CMD_TRIGGER_MEASURE = 12,
    CMD_STOP_MEASURE = 13,
    CMD_OPEN_DOOR = 20,
    CMD_CLOSE_DOOR = 21,
    CMD_FANS_ON = 22,
    CMD_FANS_OFF = 23,
    // 2. NHÓM CẤU HÌNH (Gateway gửi xuống)
    CMD_SYSTEM_TIME_OK = 30, // Phản hồi giờ cho Time Req
    CMD_SET_CYCLE = 31, // Cài đặt số lần đo/ngày
    CMD_SET_SCHEDULE = 32, // Cài đặt lịch đo cụ thể
    // 3. NHÓM ACK (Node phản hồi thành công)
    ACK_OK = 100, // Thành công chung chung
    ACK_OPEN_DOOR_OK = 101, 
    ACK_CLOSE_DOOR_OK = 102, 
    ACK_FANS_ON_OK = 103, 
    ACK_FANS_OFF_OK = 104, 
    ACK_MODE_MANUAL_OK = 105, 
    ACK_MODE_AUTO_OK = 106, 
    ACK_TRIGGER_OK = 107, 
    ACK_STOP_MEASURE_OK = 108, 
    ACK_SET_CYCLE_OK = 109, 
    ACK_SET_TIME_OK = 110,
    // 4. NHÓM LỖI (Node báo cáo lỗi)
    ERR_UNKNOWN = 200, // Lệnh không hiểu
    ERR_BUSY = 201, // Đang bận đo (busy_measuring)
    ERR_IN_AUTO = 202, // Không thể Manual khi đang Auto (cannot_manual_in_auto)
    ERR_EXECUTE_FAIL = 203  // Lỗi phần cứng (Sensor lỗi...)
};
// --- CẤU TRÚC GÓI TIN (STRUCT) ---
// 1. Dữ liệu Cảm biến (16 bytes payload)
struct __attribute__((packed)) SensorData {
    uint8_t msgType;
    int16_t temp;
    uint16_t hum;
    uint16_t ch4;
    uint16_t co;
    uint16_t alc;
    uint16_t nh3;
    uint16_t h2;
};
// 2. Lệnh Điều khiển đơn giản (2 bytes payload)
struct __attribute__((packed)) ControlCmd {
    uint8_t msgType;
    uint8_t cmdCode;
};

// 3. Lệnh Cấu hình & Thời gian (12 bytes payload)
struct __attribute__((packed)) ConfigCmd {
    uint8_t msgType; // TYPE_CMD_CONFIG
    uint8_t cmdCode; // CMD_SET_CYCLE, CMD_SYSTEM_TIME_OK...
    uint16_t value;  // Dùng cho Set Cycle
    // Dùng cho Time Sync hoặc Set Schedule (HH:MM:SS)
    uint16_t year;   
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};
// 4. Phản hồi ACK/ERROR (2 bytes payload)
struct __attribute__((packed)) ResponsePacket {
    uint8_t msgType; // TYPE_RESPONSE
    uint8_t ackCode; // ACK_... hoặc ERR_...
};
// 5. Yêu cầu đồng bộ giờ (1 byte payload)
struct __attribute__((packed)) SimplePacket {
    uint8_t msgType; // TYPE_TIME_REQ
    uint8_t dummy;   // Padding cho chẵn byte (không bắt buộc nhưng tốt cho AES)
};
struct __attribute__((packed)) TriggerCmd {
    uint8_t msgType;  // TYPE_CMD_CTRL (0x02)
    uint8_t cmdCode;  // CMD_TRIGGER_MEASURE (12)
    uint8_t cycle;    // Số chu kỳ đo (1 byte)
};