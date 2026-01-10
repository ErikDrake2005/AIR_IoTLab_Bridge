#pragma once
#include <Arduino.h>
#include "config.h"

class PowerManager {
public:
    void begin();
    float getVoltage(); 
    bool isBatteryLow();       
    bool isBatteryOk();        
    bool isAdapterConnected(); 
    void sleepForSeconds(uint64_t seconds);
};