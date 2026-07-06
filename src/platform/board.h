/**
 * @file board.h
 * @brief Per-board hardware wiring — the SX1262 control pins, the activity LED,
 *        and the RF-switch arrangement that differ between the supported
 *        boards. Selected at compile time so the shared radio glue names pins
 *        symbolically instead of hard-coding one board's numbers.
 *
 *  - XIAO ESP32S3 + Wio-SX1262: fixed GPIO numbers; the SX1262's own DIO2
 *    drives the antenna switch (no external enable line).
 *  - Seeed Wio Tracker L1 (nRF52840): pins come from the board variant's
 *    SX126X_* defines, and the board has an external RX-enable line (RXEN) in
 *    addition to the DIO2 switch.
 */
#ifndef LORA_SERIAL_PLATFORM_BOARD_H_
#define LORA_SERIAL_PLATFORM_BOARD_H_

#include <Arduino.h>   // active board variant: SX126X_*/LED_* defines (nRF52)

#if defined(ARDUINO_ARCH_ESP32)

// ---- XIAO ESP32S3 + Wio-SX1262 (board-to-board) --------------------------
#define BOARD_LORA_NSS   41   ///< SX1262 SPI chip-select (NSS)
#define BOARD_LORA_DIO1  39   ///< SX1262 DIO1 (RX/TX-done IRQ)
#define BOARD_LORA_RST   42   ///< SX1262 NRST hardware reset
#define BOARD_LORA_BUSY  40   ///< SX1262 BUSY line
#define BOARD_LED_PIN    48   ///< activity LED (green D1, via 330R)
// RF switch: driven by the SX1262's own DIO2; no external enable pin.

#else

// ---- Seeed Wio Tracker L1 (nRF52840) — pins from the board variant -------
#define BOARD_LORA_NSS   SX126X_CS      ///< SX1262 chip-select
#define BOARD_LORA_DIO1  SX126X_DIO1    ///< SX1262 DIO1 IRQ
#define BOARD_LORA_RST   SX126X_RESET   ///< SX1262 NRST
#define BOARD_LORA_BUSY  SX126X_BUSY    ///< SX1262 BUSY
#define BOARD_LED_PIN    LED_GREEN      ///< PIN_LED1 (LED_BLUE shares the buzzer)
#define BOARD_LORA_RXEN  SX126X_RXEN    ///< external RX-enable (with DIO2 switch)

#endif

#endif  // LORA_SERIAL_PLATFORM_BOARD_H_
