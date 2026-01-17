#pragma once
#include <Arduino.h>

// ========== ĐỊNH DANH ==========
#define BRIDGE_DEVICE_ID "NODE_01" 
#define MY_AES_KEY       "IoTLab@Bridge01" 

// ========== UART KẾT NỐI NODE ==========
#define UART_BAUD       115200 
#define UART_RX_PIN     16
#define UART_TX_PIN     17

// ========== HANDSHAKE VỚI NODE ==========
#define PIN_NODE_TRIGGER 26  
#define PIN_NODE_STATUS  25  

// ========== LORA CONFIG ==========
#define LORA_FREQ       433E6
#define LORA_TX_POWER   20
#define LORA_SF         8      
#define LORA_BW         125E3  
#define LORA_CR         5      
#define LORA_SYNC_WORD  0xF3   

// PINOUT
#define LORA_CS_PIN     5   
#define LORA_RST_PIN    4
#define LORA_DIO0_PIN   2   
#define LORA_SCK_PIN    18
#define LORA_MISO_PIN   19
#define LORA_MOSI_PIN   23

// ========== POWER MANAGEMENT (THÊM PHẦN NÀY ĐỂ SỬA LỖI) ==========
#define PIN_BAT_ADC      35   // Chân đo pin (Thường là 34 hoặc 35 trên ESP32)
#define BAT_DIVIDER      2.0  // Tỉ lệ phân áp (R1+R2)/R2. Thường là 2.0 (100k/100k)
#define ADAPTER_VOLT     4.0  // Nếu > 4.0V coi như đang cắm nguồn ngoài
#define VOLT_LOW_LIMIT   3.3  // Ngưỡng báo pin yếu
#define VOLT_RECOVERY    3.6  // Ngưỡng phục hồi pin