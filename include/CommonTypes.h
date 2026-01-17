#pragma once
#include <Arduino.h>

// --- CÁC ĐỊNH NGHĨA CONSTANT ---
#define MAX_JSON_SIZE 1024 

// --- ENUM: CÁC CHẾ ĐỘ & TRẠNG THÁI ---
enum SystemMode {
    MODE_MANUAL = 0,
    MODE_AUTO = 1,
    MODE_TIMESTAMP = 2,
    MODE_SLEEP = 3
};

enum CycleState {
    STATE_IDLE,         
    STATE_PREPARE,      
    STATE_WAIT_1,       
    STATE_WAIT_2,       
    STATE_WAIT_3,       
    STATE_MANUAL_WAIT,  
    STATE_FINISH        
};

// --- STRUCT: GÓI DỮ LIỆU ---

// 1. Trạng thái máy
struct MachineStatus {
    SystemMode mode;           
    bool isMeasuring;
    bool isDoorOpen;
    bool isFanOn;
    int saved_manual_cycle;    
    int saved_daily_measures;  

    MachineStatus() {
        mode = MODE_MANUAL;
        isMeasuring = false;
        isDoorOpen = true;
        isFanOn = false;
        saved_manual_cycle = 1;
        saved_daily_measures = 4;
    }
};

// 2. Dữ liệu lệnh từ Server
struct CommandData {
    bool isValid;           
    String targetNID;       
    bool enable;            
    
    SystemMode setMode;     
    unsigned long timestamp;

    int manualInterval;     
    int autoMeasureCount;   
    String autoStartTime;   
    
    bool hasActions;        
    String chamberStatus;   
    String doorStatus;      
    String fanStatus;       

    CommandData() {
        isValid = false;
        targetNID = "";
        enable = false;
        setMode = MODE_MANUAL;
        timestamp = 0;
        manualInterval = 0;
        autoMeasureCount = 0;
        autoStartTime = "";
        hasActions = false;
        chamberStatus = "";
        doorStatus = "";
        fanStatus = "";
    }
};