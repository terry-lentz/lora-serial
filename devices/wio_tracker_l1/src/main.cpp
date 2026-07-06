// ===========================================================================
// Wio Tracker L1 Pro (nRF52840) — receive-and-display node (WORK IN PROGRESS).
//
// Brings up the SX1262 and RECEIVES the bench beacon (beacon_xiao/) on our PHY,
// then shows each packet + RSSI/SNR on the OLED and USB serial. This is the
// first real data over the link onto the display — the core of the display-node
// idea. Next step: swap the raw-PHY receive for the portable link protocol in
// ../../lib/linklayer so it interoperates with a real node_raw peer.
//
// PHY must match the beacon EXACTLY (921.0 MHz, SF7/BW250/CR4:5, sync 0x12,
// preamble 8, CRC on, TCXO 1.8V). 921.0 sits 2 MHz clear of the user's 923 pair.
//
// Radio wiring comes from the vendored Seeed variant: NSS=D4, DIO1=D1, RST=D2,
// BUSY=D3, SPI SCK=8/MISO=9/MOSI=10, TCXO on SX1262 DIO3 @1.8V. The RF path
// uses the module's DIO2 T/R switch (setDio2AsRfSwitch) plus the board's RX
// enable line (SX126X_RXEN=D5), held high for this receive-only node.
//
// Display: 128x64 SH1106 @ I2C 0x3D (SH110X driver). Brightness = SH1106
// contrast, cycled by the User button (D13) — now with darker MED/LOW steps.
// Heartbeat on LED_GREEN (LED_BLUE shares the buzzer pin, so we avoid it).
// ===========================================================================
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Arduino.h>
#include <RadioLib.h>
#include <Wire.h>

// ---- OLED (128x64 SH1106 @ 0x3C/0x3D) -------------------------------------
static const uint8_t kOledW     = 128;
static const uint8_t kOledH     = 64;
static const uint8_t kOledAddrA = 0x3C;
static const uint8_t kOledAddrB = 0x3D;
static const int8_t  kOledRst   = -1;
static Adafruit_SH1106G g_oled(kOledW, kOledH, &Wire, kOledRst);
static uint8_t g_oled_addr = 0;   // address that ACKed (0 = none)

// ---- Brightness (SH1106 contrast; MED/LOW darker than stage 1a) -----------
/** @brief A named brightness step and its SH1106 contrast value. */
struct BrightLevel {
  const char* name;      ///< label shown on the OLED / serial.
  uint8_t     contrast;  ///< SH1106 contrast (0x81 register) value.
};
static const BrightLevel kBright[] = {
    {"FULL", 0xFF}, {"MED", 0x30}, {"LOW", 0x02}};
static const uint8_t kNumBright = sizeof(kBright) / sizeof(kBright[0]);
static uint8_t g_bright = 1;   // boot at MED (slightly dimmer)

// ---- User button (active-low, left of the trackball) ----------------------
static const int      kPinBtn      = CANCEL_BUTTON_PIN;  // D13 / P0.08
static const uint32_t kBtnDebounce = 30;                 // ms

// ---- SX1262 radio (pins from the vendored variant) ------------------------
static Module g_mod(SX126X_CS, SX126X_DIO1, SX126X_RESET, SX126X_BUSY);
static SX1262 g_radio(&g_mod);

// PHY — byte-identical to spike/xiao_beacon.
static const float    kFreqMhz  = 921.0f;
static const float    kBwKhz    = 250.0f;
static const uint8_t  kSf       = 7;
static const uint8_t  kCr       = 5;
static const uint8_t  kSyncWord = 0x12;
static const uint16_t kPreamble = 8;
static const float    kTcxoV    = 1.8f;
static const int8_t   kRxDbm    = 10;   // TX power (unused in RX, begin() needs it)

// Set from the DIO1 ISR when a packet lands; drained in loop().
static volatile bool g_rx_flag = false;

// ---- Receive state shown on the display -----------------------------------
static uint32_t g_pkts    = 0;      // packets received this boot
static float    g_rssi    = 0;      // last RSSI (dBm)
static float    g_snr     = 0;      // last SNR (dB)
static char     g_last[24] = "(waiting...)";  // last payload text

static const uint32_t kTickMs = 1000;   // display/serial tick cadence

/** @brief DIO1 ISR: flag that a packet has arrived (drained in loop()). */
static void OnRxIsr() { g_rx_flag = true; }

/**
 * @brief Probe an I2C address for an ACK (device present).
 *
 * @param[in] addr  7-bit I2C address to poll.
 * @return true if a device acknowledged at that address.
 */
static bool I2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

/** @brief Apply the current brightness level to the panel and log it. */
static void ApplyBrightness() {
  if (!g_oled_addr) return;
  g_oled.setContrast(kBright[g_bright].contrast);
  Serial.print(F("[L1 1b] brightness -> "));
  Serial.println(kBright[g_bright].name);
}

/**
 * @brief Debounced poll of the User button; advances brightness on each press.
 */
static void PollButton() {
  static bool     stable   = HIGH;
  static bool     last_raw = HIGH;
  static uint32_t changed  = 0;
  bool raw = digitalRead(kPinBtn);
  if (raw != last_raw) {
    last_raw = raw;
    changed = millis();
  }
  if (millis() - changed > kBtnDebounce && raw != stable) {
    stable = raw;
    if (stable == LOW) {
      g_bright = (g_bright + 1) % kNumBright;
      ApplyBrightness();
    }
  }
}

/** @brief Redraw the OLED: banner, RX stats, last payload, brightness. */
static void DrawScreen() {
  if (!g_oled_addr) return;
  char line[32];
  g_oled.clearDisplay();
  g_oled.drawRect(0, 0, kOledW, kOledH, SH110X_WHITE);
  g_oled.setCursor(6, 4);
  g_oled.print(F("L1 RX 921.0"));
  g_oled.setCursor(6, 18);
  snprintf(line, sizeof line, "pkts:%lu rssi:%d", (unsigned long)g_pkts,
           (int)g_rssi);
  g_oled.print(line);
  g_oled.setCursor(6, 30);
  snprintf(line, sizeof line, "snr:%d.%d dB", (int)g_snr,
           (int)(g_snr < 0 ? -g_snr * 10 : g_snr * 10) % 10);
  g_oled.print(line);
  g_oled.setCursor(6, 42);
  g_oled.print(g_last);
  g_oled.setCursor(6, 54);
  g_oled.print(F("bright: "));
  g_oled.print(kBright[g_bright].name);
  g_oled.display();
}

void setup() {
  pinMode(LED_GREEN, OUTPUT);
  pinMode(kPinBtn, INPUT_PULLUP);

  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 1500;) {
  }
  Serial.println(F("\n[L1 1b] boot: OLED + SX1262 receive"));

  // --- OLED ---
  Wire.begin();
  uint8_t addr = I2cPresent(kOledAddrA)   ? kOledAddrA
                 : I2cPresent(kOledAddrB) ? kOledAddrB
                                          : 0;
  if (addr && g_oled.begin(addr, true)) {
    g_oled_addr = addr;
    g_oled.setTextSize(1);
    g_oled.setTextColor(SH110X_WHITE);
    ApplyBrightness();
  }
  Serial.print(F("[L1 1b] SH1106: "));
  Serial.println(g_oled_addr ? F("OK") : F("NOT FOUND"));

  // --- Radio ---
  // The board's RX-enable line: held high for this receive-only node so the RX
  // path/LNA is always armed; the module's DIO2 handles the T/R switch itself.
  pinMode(SX126X_RXEN, OUTPUT);
  digitalWrite(SX126X_RXEN, HIGH);

  int st = g_radio.begin(kFreqMhz, kBwKhz, kSf, kCr, kSyncWord, kRxDbm,
                         kPreamble, kTcxoV, false);
  Serial.print(F("[L1 1b] SX1262 begin: "));
  Serial.println(st);
  if (st == RADIOLIB_ERR_NONE) {
    g_radio.setDio2AsRfSwitch(true);
    g_radio.setCRC(true);
    g_radio.setPacketReceivedAction(OnRxIsr);
    int rs = g_radio.startReceive();
    Serial.print(F("[L1 1b] startReceive: "));
    Serial.println(rs);
  } else {
    snprintf(g_last, sizeof g_last, "radio err %d", st);
  }
  DrawScreen();
}

void loop() {
  PollButton();

  // Drain a received packet (flagged by the DIO1 ISR).
  if (g_rx_flag) {
    g_rx_flag = false;
    uint8_t buf[64];
    int len = g_radio.getPacketLength();
    int st = g_radio.readData(buf, sizeof buf - 1);
    if (st == RADIOLIB_ERR_NONE) {
      if (len > (int)sizeof buf - 1) len = sizeof buf - 1;
      buf[len > 0 ? len : 0] = 0;
      g_rssi = g_radio.getRSSI();
      g_snr  = g_radio.getSNR();
      g_pkts++;
      snprintf(g_last, sizeof g_last, "%s", (char*)buf);
      Serial.print(F("[L1 1b] RX #"));
      Serial.print(g_pkts);
      Serial.print(F(" rssi="));
      Serial.print(g_rssi);
      Serial.print(F(" snr="));
      Serial.print(g_snr);
      Serial.print(F(" \""));
      Serial.print((char*)buf);
      Serial.println(F("\""));
      DrawScreen();
    }
    g_radio.startReceive();   // re-arm
  }

  // 1 Hz heartbeat + periodic redraw (keeps uptime/RSSI fresh even when idle).
  static uint32_t tick = 0;
  static uint32_t last = 0;
  if (millis() - last < kTickMs) return;
  last = millis();
  digitalWrite(LED_GREEN, tick & 1);
  DrawScreen();
  tick++;
}
