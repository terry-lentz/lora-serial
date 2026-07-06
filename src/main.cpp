/**
 * @file main.cpp
 * @brief Composition root for the LoRa serial-transport firmware
 *        (XIAO ESP32S3 + Wio-SX1262).
 *
 * A transparent, reliable, encrypted serial link: bytes in the USB port of one
 * board come out the other's, byte-exact (Selective-Repeat ARQ + Ascon AEAD).
 * Built as the single `node_raw` env, flashed identically to both boards.
 *
 * Both boards run the SAME firmware; the half-duplex role is auto-elected at
 * boot from each chip's factory MAC (lower MAC initiates, takes address 1), so
 * the two ends are interchangeable with no per-board configuration. See the
 * MAC_ROLE discovery code in fw_device.cpp. (Build -DMAC_ROLE=0 for the legacy
 * scheme where the role came from a build-flag address.)
 *
 * Layout (header = interface, .cpp = implementation):
 *   fw_config.h        - build config, constants, types, shared extern globals
 *   fw_radio.{h,cpp}   - radio layer (class Radio: LED, ISR, modes, Tx/Rx)
 *   fw_host.{h,cpp}    - USB<->link glue (rings, PSRAM ingest, AT mode,
 *                        pairing, settings)
 *   fw_diag.{h,cpp}    - crash/health diagnostics + software watchdog
 *   fw_session.{h,cpp} - per-session key handshake (forward secrecy)
 *   fw_device.{h,cpp}  - device orchestration (class Device: setup/loop, the
 *                        turn engine, role discovery, recovery, ADR)
 *   lib/linklayer/     - the portable, natively-unit-tested data-link layer
 *
 * This file owns the definitions of the shared globals plus setup()/loop(),
 * which just forward to the static-singleton g_device (see fw_device.h).
 */
#include <RadioLib.h>

#include "fw_config.h"
#include "fw_device.h"   // g_device.Setup() / g_device.Loop()

// ---- Definitions of the globals declared extern in fw_config.h ----
ModemSettings        cfg;                     ///< live runtime modem settings
Preferences          prefs;                   ///< NVS "loramodem" handle
link_layer::LinkLayer<4096>  g_link;          ///< windowed-ARQ link engine
link_layer::ModeSwitch       g_modesw;        ///< PHY mode-switch state
link_layer::AdrController     g_adr;          ///< 'auto' mode decision engine
// Wio-SX1262 board-to-board control pins (SX1262 <-> XIAO ESP32S3).
static const int     kPinNss  = 41;   // SPI chip-select (NSS)
static const int     kPinDio1 = 39;   // DIO1 interrupt (RX/TX done)
static const int     kPinRst  = 42;   // NRST hardware reset
static const int     kPinBusy = 40;   // BUSY line
/// The RadioLib Module (SPI + control pins). A NAMED global, not new'd: there
/// is one radio for the whole run, so it lives statically with no heap (rule
/// 5). fw_radio.cpp reaches it (extern) to bound the SPI BUSY-line timeout.
Module               radio_module(kPinNss, kPinDio1, kPinRst, kPinBusy);
/// the SX1262 driver, bound to radio_module above.
SX1262               radio(&radio_module);

void (*g_rx_idle_hook)() = nullptr;           ///< radio-wait host-I/O idle hook

uint8_t  g_link_key[16];                       ///< active AEAD key (live swap)
uint8_t  g_static_key[16];                     ///< long-term/static AEAD key

// --------------------------------------------------------------------------
// Arduino entry points. All the orchestration — radio bring-up, role
// discovery/pairing, the half-duplex turn engine, link recovery, and ADR —
// lives in the static-singleton Device (fw_device.{h,cpp}); these forward to
// it so the entry points stay trivial.
// --------------------------------------------------------------------------
// The diagnostic env:usbprobe build supplies its own setup()/loop() (see
// src/usb_probe.cpp) and skips all of this; guard so there is one definition.
#ifndef USB_PHONE_PROBE
/**
 * @brief Arduino boot entry point; forwards to Device::Setup().
 */
void setup() { g_device.Setup(); }

/**
 * @brief Arduino main-loop entry point; forwards to Device::Loop().
 */
void loop()  { g_device.Loop();  }
#endif  // USB_PHONE_PROBE
