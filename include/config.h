#pragma once
// --- ID ---
#define DEVICE_ID "AIR_VL_01" // Để Bridge nhận diện Key ID
// --- UART ---
#define UART_BAUD 921600
#define UART_RX_PIN 16
#define UART_TX_PIN 17
// --- LoRa ---
#define LORA_FREQ 433E6
#define LORA_CS_PIN 5
#define LORA_RST_PIN  4
#define LORA_DIO0_PIN 2
#define LORA_SCK_PIN  18
#define LORA_MISO_PIN 19
#define LORA_MOSI_PIN 23
#define LORA_TX_POWER   20
#define LORA_SF         8      
#define LORA_BW         125E3
#define LORA_CR         5
#define LORA_SYNC_WORD  0xF3 
// --- SYSTEM ---
#define QUEUE_SIZE      20