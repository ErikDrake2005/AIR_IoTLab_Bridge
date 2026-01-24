#pragma once
#include <Arduino.h>
#include "config.h"

class PowerManager {
public:
    void begin();
    void update();  // Gọi trong loop để kiểm tra pin định kỳ
    
    float getVoltage();        // Đọc ngay lập tức
    float getCachedVoltage();  // Trả về giá trị cached (nhanh hơn)
    
    bool isBatteryLow();       
    bool isBatteryOk();        
    bool isAdapterConnected();
    
    bool isLowPowerMode() const { return _lowPowerMode; }
    bool shouldEnterLowPower() const { return _shouldEnterLowPower; }
    void clearLowPowerFlag() { _shouldEnterLowPower = false; }
    
    void deepSleep(uint64_t seconds);   // Deep sleep (wake by timer)
    void lightSleep(uint64_t seconds);  // Light sleep (wake by timer)
    
private:
    float _cachedVoltage = 0.0;
    unsigned long _lastCheckMs = 0;
    bool _lowPowerMode = false;
    bool _shouldEnterLowPower = false;
};