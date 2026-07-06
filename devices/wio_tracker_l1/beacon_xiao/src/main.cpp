// ===========================================================================
// Bench beacon — XIAO ESP32S3 + Wio-SX1262 raw-LoRa transmitter.
//
// A test companion for the Wio Tracker L1 receive-and-display node (../src):
// it transmits one short, labeled LoRa packet per second on OUR PHY so the L1
// has a signal to lock onto, WITHOUT the full ARQ/encryption stack — just a raw
// RadioLib transmit, so the L1 side proves radio interop in isolation.
//
// The PHY params below MUST match the L1 receiver exactly AND avoid the user's
// existing 923 MHz pair — hence 921.0 MHz.
//
// This TEMPORARILY replaces node_raw on one XIAO; restore it afterward with
//   make flash PORT=/dev/ttyACMx
// ===========================================================================
#include <Arduino.h>
#include <RadioLib.h>

// ---- Wio-SX1262 <-> XIAO ESP32S3 control pins (mirrors src/main.cpp) -------
static const int kPinNss  = 41;   // SPI chip-select (NSS)
static const int kPinDio1 = 39;   // DIO1 (TX-done / RX-done)
static const int kPinRst  = 42;   // NRST hardware reset
static const int kPinBusy = 40;   // BUSY line

// ---- PHY: the repo's "medium" preset, retuned off the user's 923 MHz pair ---
// Must be byte-identical to the L1 receiver or the two won't hear each other.
static const float    kFreqMhz  = 921.0f;  // 2 MHz below the user's 923 pair
static const float    kBwKhz    = 250.0f;  // "medium" bandwidth
static const uint8_t  kSf       = 7;       // spreading factor
static const uint8_t  kCr       = 5;       // coding rate 4/5
static const uint8_t  kSyncWord = 0x12;    // private link (not LoRaWAN 0x34)
static const uint16_t kPreamble = 8;       // symbols
static const float    kTcxoV    = 1.8f;    // Wio-SX1262 TCXO on SX1262 DIO3
static const int8_t   kTxDbm    = 10;      // modest power for a bench test

static const uint32_t kBeaconMs = 1000;    // one packet per second

static Module radio_module(kPinNss, kPinDio1, kPinRst, kPinBusy);
static SX1262 radio(&radio_module);

// Halt with a readable reason if radio bring-up fails (nothing else to do).
static void die(const char* what, int state) {
  Serial.print(F("[beacon] FATAL "));
  Serial.print(what);
  Serial.print(F(" state="));
  Serial.println(state);
  for (;;) {
    delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 1500;) {
  }
  Serial.println(F("\n[beacon] XIAO Wio-SX1262 raw-LoRa TX"));

  int st = radio.begin(kFreqMhz, kBwKhz, kSf, kCr, kSyncWord, kTxDbm,
                       kPreamble, kTcxoV, false);
  if (st != RADIOLIB_ERR_NONE) die("begin", st);
  // The Wio-SX1262's RF switch is driven by the SX1262's own DIO2.
  radio.setDio2AsRfSwitch(true);
  radio.setCRC(true);   // matches the L1 receiver's expectation

  Serial.print(F("[beacon] up @ "));
  Serial.print(kFreqMhz);
  Serial.println(F(" MHz SF7 BW250 CR4/5 sync0x12"));
}

void loop() {
  static uint32_t seq  = 0;
  static uint32_t last = 0;
  if (millis() - last < kBeaconMs) return;
  last = millis();

  char pkt[32];
  int n = snprintf(pkt, sizeof pkt, "L1-SPIKE %lu", (unsigned long)seq);
  int st = radio.transmit((uint8_t*)pkt, n);
  Serial.print(F("[beacon] tx seq="));
  Serial.print(seq);
  Serial.print(F(" -> "));
  Serial.println(st == RADIOLIB_ERR_NONE ? F("OK") : F("ERR"));
  seq++;
}
