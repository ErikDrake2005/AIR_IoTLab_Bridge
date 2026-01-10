#include "PowerManager.h"

void PowerManager::begin() { pinMode(PIN_BAT_ADC, INPUT); }

float PowerManager::getVoltage() {
    float total = 0;
    for(int i=0; i<10; i++) { total += analogRead(PIN_BAT_ADC); delay(1); }
    return (total / 10.0 / 4095.0) * 3.3 * BAT_DIVIDER;
}

bool PowerManager::isAdapterConnected() { return getVoltage() < ADAPTER_VOLT; }

bool PowerManager::isBatteryLow() {
    if (isAdapterConnected()) return false; 
    return getVoltage() < VOLT_LOW_LIMIT;
}

bool PowerManager::isBatteryOk() {
    if (isAdapterConnected()) return true;
    return getVoltage() >= VOLT_RECOVERY;
}

void PowerManager::sleepForSeconds(uint64_t seconds) {
    Serial.printf("[PWR] Sleep %llu s\n", seconds); Serial.flush();
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_deep_sleep_start();
}