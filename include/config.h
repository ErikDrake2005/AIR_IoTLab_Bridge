#pragma once
#include "KeyConfig.h" 

// --- UART & GPIO ---
#define UART_BAUD       921600
#define UART_RX_PIN     16 
#define UART_TX_PIN     17 
#define PIN_NODE_STATUS 25 
#define PIN_NODE_TRIGGER 26
#define PIN_BAT_ADC     34   

// --- POWER ---
#define VOLT_LOW_LIMIT  3.6  
#define VOLT_RECOVERY   3.9  
#define ADAPTER_VOLT    0.5  
#define BAT_DIVIDER     2.0  

// --- LORA ---
#define LORA_FREQ       433E6
#define LORA_CS_PIN     5
#define LORA_RST_PIN    4
#define LORA_DIO0_PIN   2
#define LORA_SCK_PIN    18
#define LORA_MISO_PIN   19
#define LORA_MOSI_PIN   23
#define LORA_TX_POWER   20
#define LORA_SF         8      
#define LORA_BW         125E3
#define LORA_CR         5
#define LORA_SYNC_WORD  0xF3 

// --- TIMING (ĐÃ TỐI ƯU CHO GATEWAY MỚI) ---
#define POLL_INTERVAL_SEC  10     
// Tăng lên 500ms để chắc chắn bắt được gói tin từ Gateway (Gateway delay ~100ms)
#define POLL_TIMEOUT_MS    500   
// Chờ xác nhận ngủ lâu hơn chút để an toàn
#define GW_CONFIRM_MS      3000  
#define DEEP_SLEEP_BAT_SEC 3600