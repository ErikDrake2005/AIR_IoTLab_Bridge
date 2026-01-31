#pragma once
#include <Arduino.h>
#include "config.h"

class PowerManager {
public:
    void begin();
    void update();  // Gọi trong loop để kiểm tra pin định kỳ
    
    float getVoltage();        // Đọc điện áp thực (V)
    float getCachedVoltage();  // Trả về giá trị cached (nhanh hơn)
    int getBatteryPercent();   // Đọc % pin (0-100%)
    int getCachedPercent();    // Trả về % cached
    
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
    int _cachedPercent = 0;
    unsigned long _lastCheckMs = 0;
    bool _lowPowerMode = false;
    bool _shouldEnterLowPower = false;
    int _voltageToPercent(float voltage);  // Convert voltage to 0-100%
};