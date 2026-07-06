/**
 * @file rtos.h
 * @brief Platform-selected FreeRTOS access + small compatibility shims, so the
 *        radio layer's task/mutex/ISR code compiles on both targets.
 *
 * Both the ESP32 (arduino-esp32) and the nRF52 (Adafruit nRF52 core) run on
 * FreeRTOS, but they differ in two ways this header papers over:
 *   - Header paths: the ESP-IDF puts them under "freertos/…"; the nRF52 core
 *     exposes them at the top level.
 *   - The ESP32 is dual-core and offers xTaskCreatePinnedToCore(); the nRF52 is
 *     single-core and has only xTaskCreate(). We provide a pinned-create that
 *     ignores the core argument there, so callers stay identical.
 *   - IRAM_ATTR (place an ISR in RAM) is an ESP32 concept; it is a no-op on the
 *     nRF52, whose code already executes from RAM-cached flash without it.
 */
#ifndef LORA_SERIAL_PLATFORM_RTOS_H_
#define LORA_SERIAL_PLATFORM_RTOS_H_

#if defined(ARDUINO_ARCH_ESP32)

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
// IRAM_ATTR and xTaskCreatePinnedToCore come from the ESP-IDF as usual.

#else  // ---- nRF52 (Adafruit core) and other single-core FreeRTOS targets ----

#include <Arduino.h>    // the nRF52 core boots FreeRTOS; pulls the RTOS headers
#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#ifndef IRAM_ATTR
#define IRAM_ATTR   // ESP32-only placement attribute; nothing to do here
#endif

/**
 * @brief Single-core stand-in for the ESP32's core-pinned task create.
 *
 * Ignores the core-affinity argument (there is only one core) and forwards to
 * the standard xTaskCreate(), so radio-task setup is written once.
 *
 * @param[in]  fn      task entry function.
 * @param[in]  name    task name (for debugging).
 * @param[in]  stack   stack depth in words.
 * @param[in]  arg     opaque argument passed to fn.
 * @param[in]  prio    task priority.
 * @param[out] handle  receives the created task handle (may be null).
 * @param[in]  core    core affinity — ignored on a single-core MCU.
 * @return pdPASS on success, else an error from xTaskCreate().
 */
static inline BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t fn, const char* name, const uint32_t stack, void* const arg,
    UBaseType_t prio, TaskHandle_t* const handle, const BaseType_t core) {
  (void)core;
  return xTaskCreate(fn, name, stack, arg, prio, handle);
}

#endif  // ARDUINO_ARCH_ESP32

#endif  // LORA_SERIAL_PLATFORM_RTOS_H_
