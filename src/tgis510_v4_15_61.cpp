// TGIS-510 -- Thermal Glue Inspection System
// Ref: TGIS-510_cpp_V4_15.61
//
// Home-lab / after-hours project. Separate from the 410 Rotaliner Tubing Seal
// Seam Monitor (factory floor, S7-300/ATmega2560) -- do not conflate.
//
// Industrial QC system detecting hot-melt glue application on tubes moving
// at high speed. Confirms glue presence, temperature, and quantity across
// both glue strips per tube pass, and pushes a stable QC-confirmation image
// to an operator HMI (Omron NS12).
//
// Division of responsibility:
//   - Keyence IV2-G30/G300CA owns hot-melt trace start/end pass/fail. ESP32
//     fires a trigger pulse; Keyence returns a result pulse straight to a
//     PLC input (wired directly, not through the ESP32). It does not
//     detect tube boundaries.
//   - MLX90640 owns strip presence, temperature and quantity (3-4 tube
//     sample window acceptable).
//   - Encoder (single-channel pulse train, no direction) gives real tube
//     position/length, used to project forward and fire triggers at
//     adjustable lead distances ahead of the MLX90640 and Keyence stations.
//   - Tube presence sensor gives the ground-truth leading/trailing edges
//     that projection is anchored to.

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MLX90640.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_NeoPixel.h>
#include <esp_task_wdt.h>
#include "driver/pcnt.h"

#ifndef FW_VERSION_STRING
#define FW_VERSION_STRING "V4.15.41"
#endif
#ifndef FW_FILE_STRING
#define FW_FILE_STRING "tgis510_v4_15_41.cpp"
#endif
static const char *FW_VERSION = FW_VERSION_STRING;
static const char *FW_FILE = FW_FILE_STRING;

// Watchdog: recovers from a hung control loop (e.g. a stalled NS12 link)
// instead of leaving fast-stop/interlock outputs stuck indefinitely.
static const uint32_t WATCHDOG_TIMEOUT_S = 3;

// Periodic Serial diagnostic report cadence.
static const uint32_t DIAGNOSTIC_INTERVAL_MS = 1000;

namespace Pins {
constexpr uint8_t KEYENCE_TRIGGER_PIN = 1;   // 24V/220ohm OUTPUT, Conn 9
constexpr uint8_t ESP_OPTO_3 = 2; // 24V/220ohm OUTPUT, Conn 8 -- TO_PLC_COMM bit 2
// GPIO3: S3 boot-strapping pin, avoid.
constexpr uint8_t ENCODER_PULSE_PIN = 4;     // 24V/10K-1.5K divider -> 3.2V INPUT, Conn 7
constexpr uint8_t PRESENCE_SENSOR_PIN = 5;   // 24V/10K-1.5K divider -> 3.2V INPUT, Conn 6
// Conn 5 was Keyence Result until V4.15.55; Keyence's result output now
// wires directly to a PLC input instead, freeing this pin for the ESP/PLC
// comms byte below.
constexpr uint8_t ESP_INPUT_3 = 6;   // 24V/10K-1.5K divider -> 3.2V INPUT, Conn 5 -- FROM_PLC_COMM bit 2
constexpr uint8_t I2C_SDA = 8;
constexpr uint8_t I2C_SCL = 9;
// GPIO10, GPIO11: field report marks these NC.

// Waveshare ESP32-S3-Zero onboard WS2812 RGB LED.
constexpr uint8_t RGB_LED = 21;

constexpr uint8_t NS12_TX = 43;
constexpr uint8_t NS12_RX = 44;
} // namespace Pins

// =====================================================================
// I2C bus (shared: MLX90640 + MCP23017)
// Confirmed working: 800kHz. 1MHz silently broke MCP23017 enumeration
// (safety-relevant -- MCP owns stop/interlock I/O) with no error other than
// "MCP initialized: NO" in diagnostics. Do NOT return to 1MHz without
// re-verifying MCP23017 survives it.
// =====================================================================
static const uint32_t I2C_CLOCK_HZ = 800000UL;

bool isI2CAddressPresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// =====================================================================
// Onboard RGB status LED.
// =====================================================================
Adafruit_NeoPixel statusLed(1, Pins::RGB_LED, NEO_RGB + NEO_KHZ800);

void setStatusLed(uint8_t red, uint8_t green, uint8_t blue) {
  statusLed.setPixelColor(0, statusLed.Color(red, green, blue));
  statusLed.show();
}

// =====================================================================
// Board / I2C bring-up diagnostics -- run once at startup.
// =====================================================================
void printBoardInformation() {
  Serial.println();
  Serial.println(F("CONTROLLER INFORMATION"));
  Serial.println(F("----------------------------------------------------"));
  Serial.printf("CPU frequency      : %u MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Flash size         : %.1f kB\n", ESP.getFlashChipSize() / 1024.0f);
  Serial.printf("Free heap          : %.1f kB\n", ESP.getFreeHeap() / 1024.0f);
  Serial.printf("PSRAM detected     : %s\n", psramFound() ? "YES" : "NO");
}

void runI2CScanner() {
  Serial.println();
  Serial.println(F("I2C SCANNER"));
  Serial.println(F("----------------------------------------------------"));
  uint8_t deviceCount = 0;

  for (uint8_t address = 1; address < 127; address++) {
    if (isI2CAddressPresent(address)) {
      Serial.printf("Device found       : 0x%02X\n", address);
      deviceCount++;
    }
  }

  if (deviceCount == 0) {
    Serial.println(F("No I2C devices found."));
  } else {
    Serial.printf("Total devices      : %u\n", deviceCount);
  }
}

// =====================================================================
// MLX90640
// 32Hz is the confirmed-stable *nominal* refresh ceiling at 800kHz, and
// MLX_FRAME_PERIOD_MS below (derived from MLX_MEASURED_FPS) only paces how
// often loop() asks the sensor for a frame -- it is a fixed assumption, not
// a live measurement. The actual windowed measurement lives at runtime in
// `measuredFramesPerSecond` (see updateFrameRate()) and is what diagnostics
// / HMI telemetry report. 64Hz fails with error -8 here -- an I2C
// bandwidth wall (64Hz needs ~196KB/s vs ~100KB/s usable at 800kHz), not a
// timing bug.
static const mlx90640_refreshrate_t MLX_REFRESH_RATE_NOMINAL = MLX90640_32_HZ;
static const float MLX_MEASURED_FPS = 32.0f;
static const uint32_t MLX_FRAME_PERIOD_MS = (uint32_t)(1000.0f / MLX_MEASURED_FPS); // ~31ms

Adafruit_MLX90640 mlx;
// Holds raw-ADC-delta values (see below) in the live acquisition path, NOT
// calibrated degrees C -- kept as "mlxFrame" and the same 32x24 float
// layout because CaptureController, downsampleMaxBlock(), and the rest of
// the analysis pipeline were already written generically against "a float
// per pixel compared to MIN/MAX constants" and don't care what physical
// unit that float represents.
float mlxFrame[32 * 24];

bool mlxDetected = false;
bool mlxInitialized = false;
bool lastFrameValid = false;
uint32_t successfulFrameCount = 0;
uint32_t failedFrameCount = 0;
uint8_t consecutiveFrameFailures = 0;
constexpr uint8_t FRAME_FAILURE_RECOVERY_COUNT = 5;

float minimumTemperatureC = NAN;
float maximumTemperatureC = NAN;
float averageTemperatureC = NAN;

float measuredFramesPerSecond = 0.0f;
uint32_t fpsWindowStartMs = 0;
uint32_t fpsWindowFrameCount = 0;

bool continuousMlxTestMode = false;
uint32_t lastContinuousMlxPrintMs = 0;
constexpr uint32_t CONTINUOUS_MLX_PRINT_INTERVAL_MS = 250;

//
// Subpage-to-pixel combination verified against the actual driver source
// (utility/MLX90640_API.cpp, MLX90640_CalculateTo()), not guessed:
//   row = pixelNumber / 32, col = pixelNumber % 32
//   chessPattern = (row % 2) ^ (col % 2)
// A pixel's valid data lives in whichever of the two raw reads has
// frameData[833] (the subpage index) equal to that pixel's chessPattern.
// getRawFrame() does not guarantee frameData0 is always subpage 0 -- it
// just returns "whichever subpage was next ready" twice -- so both reads
// are checked per pixel rather than assumed.
// =====================================================================
uint16_t rawPage0[834];
uint16_t rawPage1[834];

// Per-pixel idle baseline (raw ADC counts, signed), captured once via the
// 'B' serial command. Needed because each pixel has its own EEPROM offset/
// gain trim -- real fixed-pattern noise, not sensor noise -- so raw counts
// are only meaningful for threshold detection relative to a pixel's own
// idle value, not compared directly against a single global threshold.
float rawBaseline[32 * 24] = {0};
bool rawBaselineCaptured = false;
bool rawBaselineCaptureInProgress = false;
uint8_t rawBaselineFramesCollected = 0;
float rawBaselineAccumulator[32 * 24] = {0};
constexpr uint8_t RAW_BASELINE_FRAME_COUNT = 32;

// Combines one getRawFrame() result into a signed, per-pixel raw-ADC-count
// array (NOT baseline-subtracted -- see subtractBaseline() below). Returns
// false if getRawFrame() itself failed.
bool readMlxRawCombined(float *outPixels) {
  int status = mlx.getRawFrame(rawPage0, rawPage1);

  if (status != 0) return false;

  uint16_t subpageOf0 = rawPage0[833];
  uint16_t subpageOf1 = rawPage1[833];

  for (int pixelNumber = 0; pixelNumber < 32 * 24; pixelNumber++) {
    int row = pixelNumber / 32;
    int col = pixelNumber % 32;
    int chessPattern = (row % 2) ^ (col % 2);

    uint16_t raw;

    if (chessPattern == subpageOf0) {
      raw = rawPage0[pixelNumber];
    } else if (chessPattern == subpageOf1) {
      raw = rawPage1[pixelNumber];
    } else {
      // Neither read claims this pixel's subpage -- shouldn't happen if
      // frameData0/1 are genuinely the two different subpages, but don't
      // fabricate a value if it does.
      outPixels[pixelNumber] = NAN;
      continue;
    }

    // Raw ADC counts are signed via two's complement, same convention the
    // driver's own MLX90640_CalculateTo() uses on frameData[pixelNumber].
    int32_t signedRaw = (int32_t)raw;

    if (signedRaw > 32767) signedRaw -= 65536;
    outPixels[pixelNumber] = (float)signedRaw;
  }
  return true;
}

void subtractBaseline(const float *rawPixels, float *outDelta) {
  for (int i = 0; i < 32 * 24; i++) {
    outDelta[i] = rawPixels[i] - rawBaseline[i];
  }
}

void startRawBaselineCapture() {
  rawBaselineCaptureInProgress = true;
  rawBaselineFramesCollected = 0;

  for (int i = 0; i < 32 * 24; i++) rawBaselineAccumulator[i] = 0;
  Serial.printf("[MLX] Baseline capture starting -- averaging %u idle frames.\n",
                RAW_BASELINE_FRAME_COUNT);
}

// Call once per successful raw read while a baseline capture is running.
void serviceRawBaselineCapture(const float *rawPixels) {
  if (!rawBaselineCaptureInProgress) return;

  for (int i = 0; i < 32 * 24; i++) rawBaselineAccumulator[i] += rawPixels[i];
  rawBaselineFramesCollected++;

  if (rawBaselineFramesCollected >= RAW_BASELINE_FRAME_COUNT) {
    for (int i = 0; i < 32 * 24; i++) {
      rawBaseline[i] = rawBaselineAccumulator[i] / (float)RAW_BASELINE_FRAME_COUNT;
    }
    rawBaselineCaptureInProgress = false;
    rawBaselineCaptured = true;
    Serial.println(F("[MLX] Baseline capture complete."));
  }
}

// PLACEHOLDER, no physical grounding yet (see MATRIX_RAW_DELTA_MIN/MAX
// below for why): sanity bound wide enough to allow real signal up to
// several times CAPTURE_TRIGGER_RAW_DELTA with margin, while still
// rejecting a wildly out-of-range single-pixel glitch. Needs the same
// bench characterization as MATRIX_RAW_DELTA_MIN/MAX.
constexpr float MIN_PLAUSIBLE_RAW_DELTA = -5000.0f;
constexpr float MAX_PLAUSIBLE_RAW_DELTA = 5000.0f;
uint16_t lastFrameRejectedPixelCount = 0;

bool isPlausibleTemp(float t) {
  return isfinite(t) && t >= MIN_PLAUSIBLE_RAW_DELTA && t <= MAX_PLAUSIBLE_RAW_DELTA;
}

void calculateFrameStatistics() {
  float sumC = 0.0f;
  float minC = INFINITY;
  float maxC = -INFINITY;
  uint16_t validCount = 0;
  uint16_t rejectedCount = 0;

  for (size_t i = 0; i < 32 * 24; i++) {
    float t = mlxFrame[i];

    if (isfinite(t) && !isPlausibleTemp(t)) rejectedCount++;

    if (!isPlausibleTemp(t)) continue;

    if (t < minC) minC = t;

    if (t > maxC) maxC = t;
    sumC += t;
    validCount++;
  }

  lastFrameRejectedPixelCount = rejectedCount;

  if (validCount > 0) {
    minimumTemperatureC = minC;
    maximumTemperatureC = maxC;
    averageTemperatureC = sumC / (float)validCount;
  } else {
    minimumTemperatureC = NAN;
    maximumTemperatureC = NAN;
    averageTemperatureC = NAN;
  }
}

void updateFrameRate() {
  uint32_t now = millis();
  uint32_t elapsed = now - fpsWindowStartMs;

  if (elapsed >= 2000UL) {
    measuredFramesPerSecond = (float)fpsWindowFrameCount * 1000.0f / (float)elapsed;
    fpsWindowStartMs = now;
    fpsWindowFrameCount = 0;
  }
}

bool initializeMlx() {
  if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    return false;
  }
  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX_REFRESH_RATE_NOMINAL);
  return true;
}

void attemptCameraRecovery() {
  Serial.println();
  Serial.println(F("CAMERA RECOVERY"));
  Serial.println(F("----------------------------------------------------"));
  Serial.println(F("5 consecutive frame reads failed -- reinitializing I2C + MLX90640."));

  mlxInitialized = false;
  consecutiveFrameFailures = 0;

  Wire.end();
  delay(50);
  Wire.begin(Pins::I2C_SDA, Pins::I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  Wire.setTimeOut(1000);
  delay(50);

  mlxDetected = isI2CAddressPresent(MLX90640_I2CADDR_DEFAULT);

  if (mlxDetected) {
    mlxInitialized = initializeMlx();
  }

  if (mlxInitialized) {
    Serial.println(F("Camera recovery successful."));
    setStatusLed(0, 25, 0);
  } else {
    Serial.println(F("Camera recovery failed."));
    setStatusLed(30, 0, 0);
  }
}

static const float MATRIX_TEMP_MIN_C = 20.0f;
static const float MATRIX_TEMP_MAX_C = 180.0f;
static const float CAPTURE_TRIGGER_TEMP_C = 30.0f;

// PLACEHOLDER (Action Item 7): needs real 200 m/min validation. Tuning knob
// for "does one sampling window match one tube's FOV transit".
static const uint8_t CAPTURE_SAMPLE_COUNT = 4;

//
// These are placeholders in a more literal sense than MATRIX_TEMP_MIN_C/
// MAX_C ever were: a Celsius guess has real-world grounding (hot melt
// glue is known to run 150-200C); a raw ADC delta has none -- it could
// plausibly be tens or thousands depending on gain and resolution
// settings. These values are arbitrary compile-time stand-ins only. To
// get real numbers: capture a baseline with 'B' at room temp, then use
// 'X' against both an idle scene and a known-hot scene, read the
// reported raw-minus-baseline deltas, and use those.
static const float MATRIX_RAW_DELTA_MIN = 0.0f;      // PLACEHOLDER, no physical grounding
static const float MATRIX_RAW_DELTA_MAX = 150.0f;    // tuned against a real hand-heat bench test
static const float CAPTURE_TRIGGER_RAW_DELTA = 40.0f; // tuned against a real hand-heat bench test

// =====================================================================
// Timing constraint (critical, unresolved):
// At ~8 FPS (125ms/frame) and 32mm standoff, a tube's glue trace transits
// the MLX90640 FOV in ~6-27ms depending on lens variant -- 5-20x faster
// than one frame acquisition. Encoder synchronization is therefore
// non-optional. Still unconfirmed: lens variant mounted, tube length along
// travel axis, actual line speed vs the 200 m/min ceiling assumption.
// The capture/composite logic below is deliberately max-hold, not
// averaging, to cope with this: see CaptureController.
// =====================================================================

// =====================================================================
// Glue strip zones within the 32x24 analysis frame.
// PLACEHOLDER: exact column ranges for the two glue strips are not yet
// characterized against a real tube. Defaulting to left/right halves.
// =====================================================================
namespace StripZone {
constexpr uint8_t COLS = 32;
constexpr uint8_t ROWS = 24;
constexpr uint8_t STRIP1_COL_START = 0, STRIP1_COL_END = 15;  // PLACEHOLDER
constexpr uint8_t STRIP2_COL_START = 16, STRIP2_COL_END = 31; // PLACEHOLDER
} // namespace StripZone

// =====================================================================
// MCP23017 -- machine I/O (stop/interlock), shares the I2C bus.
// Address 0x20 (A0/A1/A2 grounded).
// MCP is polled only during Standby/TubeGap, ~20ms cadence -- an I2C
// transaction every loop() iteration would cost InspectingTube latency it
// can't afford.
// =====================================================================
Adafruit_MCP23X17 mcp;
static const uint8_t MCP_I2C_ADDR = 0x20;
static const uint32_t MCP_POLL_INTERVAL_MS = 20;

namespace McpPin {
// Internal status LED (enclosure-mounted) -- a single RGB LED, one color
// channel lit at a time (not 3 simultaneous channels mixing to a blended
// color).
constexpr uint8_t ILED_R = 8;  // GPB0, OUTPUT
constexpr uint8_t ILED_G = 9;  // GPB1, OUTPUT
constexpr uint8_t ILED_B = 10; // GPB2, OUTPUT
constexpr uint8_t ELED_Y = 11; // GPB3, OUTPUT (3.3V/470ohm) -- external yellow LED
// External status LED (operator-visible) -- single RGB LED (same one-
// channel-at-a-time convention as internal) PLUS a separate discrete
// yellow LED, so 4 selectable colors total, not 3.
constexpr uint8_t ELED_R = 12; // GPB4, OUTPUT
constexpr uint8_t ELED_G = 13; // GPB5, OUTPUT
constexpr uint8_t ELED_B = 14; // GPB6, OUTPUT
constexpr uint8_t INPUT_2 = 0; // GPA0, INPUT, Conn 1 -- FROM_PLC_COMM bit 1
constexpr uint8_t OPTO_2 = 1;  // GPA1, OUTPUT (24V/220ohm), Conn 3 -- TO_PLC_COMM bit 1
constexpr uint8_t INPUT_1 = 2; // GPA2, INPUT, Conn 2 -- FROM_PLC_COMM bit 0
constexpr uint8_t OPTO_1 = 3;  // GPA3, OUTPUT (24V/220ohm), Conn 4 -- TO_PLC_COMM bit 0
} // namespace McpPin

bool mcpOk = false;

constexpr uint8_t kMcpOutputPins[9] = {McpPin::ILED_R, McpPin::ILED_G, McpPin::ILED_B,
                                        McpPin::ELED_R, McpPin::ELED_G, McpPin::ELED_B,
                                        McpPin::ELED_Y, McpPin::OPTO_1, McpPin::OPTO_2};
bool mcpOutputState[9] = {false, false, false, false, false, false, false, false, false};

void toggleMcpOutput(uint8_t idx) {
  if (!mcpOk || idx >= 9) return;
  mcpOutputState[idx] = !mcpOutputState[idx];
  mcp.digitalWrite(kMcpOutputPins[idx], mcpOutputState[idx]);
}

// =====================================================================
// Encoder -- ZATOR LMZ02, single-channel pulse train, no direction.
// Uses the ESP32 PCNT peripheral. 16-bit HW counter is drained into a
// 64-bit running total every service() call, well inside its wrap period
// at any plausible pulse rate for this line.
// =====================================================================
class EncoderTracker {
public:
  void begin(uint8_t pulseGpio) {
    pinMode(pulseGpio, INPUT_PULLDOWN);
    pcnt_config_t cfg = {};
    cfg.pulse_gpio_num = pulseGpio;
    cfg.ctrl_gpio_num = PCNT_PIN_NOT_USED;
    cfg.channel = PCNT_CHANNEL_0;
    cfg.unit = PCNT_UNIT_0;
    cfg.pos_mode = PCNT_COUNT_INC;
    cfg.neg_mode = PCNT_COUNT_DIS;
    cfg.lctrl_mode = PCNT_MODE_KEEP;
    cfg.hctrl_mode = PCNT_MODE_KEEP;
    cfg.counter_h_lim = 30000;
    cfg.counter_l_lim = 0;
    pcnt_unit_config(&cfg);
    pcnt_set_filter_value(PCNT_UNIT_0, 100); // ns glitch filter
    pcnt_filter_enable(PCNT_UNIT_0);
    pcnt_counter_pause(PCNT_UNIT_0);
    pcnt_counter_clear(PCNT_UNIT_0);
    pcnt_counter_resume(PCNT_UNIT_0);
    lastHwCount = 0;
    totalCounts = 0;
  }

  void service() {
    int16_t hw = 0;
    pcnt_get_counter_value(PCNT_UNIT_0, &hw);
    int32_t delta = (int32_t)hw - (int32_t)lastHwCount;

    if (delta < 0) {
      delta += 30000; // wrapped past counter_h_lim
    }
    totalCounts += delta;
    lastHwCount = hw;
  }

  int64_t total() const { return totalCounts; }

private:
  int16_t lastHwCount = 0;
  int64_t totalCounts = 0;
};

EncoderTracker encoder;

// PLACEHOLDER (Action Item 3): blocked on confirming ZATOR LMZ02 encoder
// PPR against real hardware.
static float ENCODER_COUNTS_PER_MM = 1.0f;

// PLACEHOLDER (Action Item 3): distance from the presence sensor to the
// MLX90640 station, used for forward projection.
static float PRESENCE_TO_MLX_DISTANCE_MM = 100.0f;

//
// Both values are meant to be read from the HMI via RM (operator types
// them into numeric input objects on the touchscreen) -- see
// serviceHmiInputPolling() below. RM currently gets zero response from
// this PT (see the NS12 field-report comment on why), so these start at
// PLACEHOLDER fallback values and stay there, with
// hotMeltPositionsFromHmi staying false, until that's resolved.
static float hotMeltStartPositionMm = 150.0f; // PLACEHOLDER fallback
static float hotMeltEndPositionMm = 10.0f;    // PLACEHOLDER fallback
bool hotMeltPositionsFromHmi = false;

// PLACEHOLDER: HMI numeric-input word scale unconfirmed against the real
// CX-Designer project -- assumed here as plain integer mm (one $W count =
// 1mm). If those input objects actually use a decimal-shifted scale (e.g.
// x10 for 0.1mm resolution, matching the x10 convention already used
// elsewhere in this file's telemetry), change this to match.
constexpr float HMI_POSITION_MM_PER_COUNT = 1.0f;

// Tube length, learned from the previous tube's presence-sensor
// leading/trailing edge encoder distance (see handlePresenceEdge()).
// Needed to project where the CURRENT tube's trailing edge will be, since
// End Position above is referenced from that edge, not a fixed distance
// from the presence sensor.
float learnedTubeLengthMm = 0.0f;
bool tubeLengthLearned = false;
int64_t tubeEndEncoderCount = 0;

// =====================================================================
// Tube presence sensor -- ground-truth leading/trailing edge anchor.
// =====================================================================
volatile bool presenceEdgePending = false;
volatile bool presenceState = false;
void IRAM_ATTR presenceIsr() {
  presenceState = digitalRead(Pins::PRESENCE_SENSOR_PIN) == HIGH;
  presenceEdgePending = true;
}

// =====================================================================
// Keyence IV2-G30 trigger.
// KEYENCE_TRIGGER_PIN drives IN1 in external trigger mode. Min ON 100us,
// min OFF 1.2ms. A blocking digitalWrite HIGH->LOW sequence risks a pulse
// too narrow for reliable detection (needs scope verification) -- this is
// therefore a non-blocking pending-low state serviced every loop, not a
// delay()-based pulse.
// =====================================================================
class KeyenceTrigger {
public:
  void begin(uint8_t pin) {
    gpio = pin;
    pinMode(gpio, OUTPUT);
    digitalWrite(gpio, LOW);
  }

  void fire() {
    digitalWrite(gpio, HIGH);
    pulseStartUs = micros();
    pending = true;
  }

  // Comfortably wider than the 100us Keyence minimum and the MCP23017
  // ISR/polling response time. The Keyence-side "Strobe Output One-Shot ON
  // Time" should also be set wider than this on the sensor itself.
  static const uint32_t PULSE_WIDTH_US = 500;

  void service() {
    if (pending && (uint32_t)(micros() - pulseStartUs) >= PULSE_WIDTH_US) {
      digitalWrite(gpio, LOW);
      pending = false;
    }
  }

private:
  uint8_t gpio = 0;
  uint32_t pulseStartUs = 0;
  bool pending = false;
};

KeyenceTrigger keyenceTrigger;

// =====================================================================
// NS12 HMI / Memory Link protocol
// Ref: Omron NS-Series Host Connection Manual (Cat. No. V085-E1-07), S3
// "Connection via Memory Link". Confirmed applicable to this NS12-TS00B-V2
// unit's Memory Link mode. Commands: WM (write $W), RM (read $W).
//
// Frame layout (all ASCII). *S='0' selects SUM (checksum) OFF + SET-write /
// variable-length read -- no checksum byte is appended, which is only
// valid because *S explicitly says SUM is off:
//   Write : ESC 'W' 'M' '0' AAAA(4-hex addr) LL(2-dec count)
//           D,D,...(comma-separated hex, zero-suppressed) CR
//   Read  : ESC 'R' 'M' '0' AAAA(4-hex addr) LL(2-dec count) CR
//   Read response: ESC 'R' 'M' [maybe '0' echoed] AAAA LL D,D,... CR
//
// The exact RM response framing (whether *S is echoed back, shifting the
// address field by one byte) has not been independently pinned down either
// -- parseRmResponse() below tries both candidate offsets and locks onto
// whichever one validates against the address/count actually requested.

#define NS12_DEBUG_RAW_RX 0

namespace NS12 {
constexpr uint8_t ESC = 0x1B;
constexpr long BAUD = 38400; // requires matching CX-Designer Comm Settings on the panel side
constexpr uint32_t RM_READ_TIMEOUT_MS = 250;

// Word Lamp matrix -- default/trusted mode, 16x8 grid at $W700-$W827,
// column-major, stride 8: address(col,row) = 700 + col*8 + row.
constexpr uint16_t MATRIX_BASE_ADDR = 700;
constexpr uint8_t MATRIX_COLS_DEFAULT = 16;
constexpr uint8_t MATRIX_ROWS_DEFAULT = 8;

// 16x8 is the default, trusted mode -- a full 32x24 burst starves the PT
// of time to service RM reads. Column-paced 32x24 is opt-in/experimental,
// self-monitored: auto-reverts to 16x8 if RM success rate collapses (see
// checkDisplayAutoFallback()).
constexpr bool ENABLE_EXPERIMENTAL_32x24 = false;
constexpr uint8_t MATRIX_COLS_EXPERIMENTAL = 32;
constexpr uint8_t MATRIX_ROWS_EXPERIMENTAL = 24;

// Word Lamp palette: 10 entries, index 0-9. Index 0 renders as blank/off on
// this PT -- clamp the coldest output to index 1, never 0, so a write
// always shows something.
constexpr uint8_t PALETTE_MIN_INDEX = 1;
constexpr uint8_t PALETTE_MAX_INDEX = 9;

// Display refresh decoupled from live camera streaming, velocity-adaptive
// per-tube throttle (floor only -- see requestDisplayPush()).
constexpr uint32_t TARGET_DISPLAY_REFRESH_MS = 2000;

constexpr uint16_t TELEMETRY_BASE_ADDR = 100;
constexpr uint16_t TELEMETRY_WORD_COUNT = 9;
constexpr uint32_t TELEMETRY_WRITE_INTERVAL_MS = 250;

// Low-rate read used only to keep the auto-fallback's RM success-rate
// stats alive (see checkDisplayAutoFallback()) -- without some RM traffic
// those stats never move and the fallback can never trigger. Mirrors the
// bench-tested file's $W10 "operator test input" convention; repoint if
// the CX-Designer project already uses $W10 for something else.
#define NS12_ENABLE_RM_POLLING 1

constexpr uint16_t HOTMELT_START_POSITION_ADDR = 11;
constexpr uint16_t HOTMELT_END_POSITION_ADDR = 12;
constexpr uint32_t RM_POLL_INTERVAL_MS = 1000; // operator input changes rarely -- no need to poll fast

constexpr uint16_t CURRENT_SCREEN_ADDR = 50;

// Hard protocol ceiling, not a tuning knob: the WM/RM wire format's LL
// field is exactly 2 decimal digits, so no single WM command can
// legitimately carry more than 99 words -- sending more doesn't fail
// loudly, it silently desyncs the frame. Also sizes the sendWM() stack
// buffer; the largest real chunk today is 24 words (experimental 32x24
// columns), so 99 leaves comfortable margin.
constexpr uint16_t MAX_WM_WORDS = 99;

// Same LL-field ceiling applied to WB (bit write). Only ever used for 5
// lamp bits at once (BUTTON_COUNT) in this file, so this is a generous
// margin, not a tight fit.
constexpr uint16_t MAX_WB_BITS = 99;

// Spacing between successive column writes in the experimental 32x24 mode.
//
// Derived, not guessed: one column WM frame is ESC+'W'+'M'+'0' (4) +
// 4-hex address (4) + 2-decimal count (2) + comma-separated hex data
// (rows values, each 1 digit since palette indices are 0-9, plus
// rows-1 commas) + CR (1). At MATRIX_ROWS_EXPERIMENTAL=24 that's 58 bytes;
// at 8N1 (10 bits/byte) and BAUD=9600 that takes ~60.4ms to physically
// drain off the wire. A fixed interval shorter than that would issue a new
// column write before the previous one finished transmitting -- the same
// "PT starved mid-write" failure mode column-pacing exists to avoid, just
// recurring at smaller scale. Computed with a 50% margin so it stays
// correct if BAUD or MATRIX_ROWS_EXPERIMENTAL ever change.
constexpr uint16_t COLUMN_DATA_CHARS =
    (uint16_t)MATRIX_ROWS_EXPERIMENTAL + ((uint16_t)MATRIX_ROWS_EXPERIMENTAL - 1);
constexpr uint16_t COLUMN_FRAME_BYTES =
    4 /*ESC W M '0'*/ + 4 /*hex addr*/ + 2 /*dec count*/ + COLUMN_DATA_CHARS + 1 /*CR*/;
constexpr uint32_t COLUMN_FRAME_TX_TIME_US =
    (uint32_t)COLUMN_FRAME_BYTES * 10UL * 1000000UL / (uint32_t)BAUD;
constexpr uint32_t COLUMN_WRITE_INTERVAL_MS =
    (COLUMN_FRAME_TX_TIME_US * 3UL / 2UL) / 1000UL + 1UL; // +50% margin, ceil to ms

//
// COULD NOT VERIFY against the official Host Connection Manual (Cat. No.
// V085-E1-07) -- WebFetch to every candidate manual/documentation host
// was blocked by this sandbox's network egress policy. This was reasoned
// from the available evidence, not confirmed against primary
// documentation, at the time it was written.
//
// RM-only, deliberately NOT applied to WM (see sendWM): WM writes already
// get transport-level acks with plain (unoffset) addresses -- there is no
// evidence WM needs this offset, and applying an unverified offset to the
// one path that already "works" would risk breaking known-good telemetry/
// matrix traffic just to test a read-side hypothesis. If this offset turns
// out to be real and WM also needs it, that's a separate, deliberate change
// once RM confirms the theory -- not bundled in here.
constexpr uint16_t RM_WORD_ADDRESS_OFFSET = 16384;

// Unused by requestRB()/sendWB() -- bit addresses ($B) take the plain
// address, unlike words ($W) above. Left declared since other comments
// in this file still refer to it by name.
constexpr uint16_t RB_BIT_ADDRESS_OFFSET = 16384;

// HMI push-button inputs, confirmed from the real CX-Designer Symbol Table
// (project 510_HotMel_20260902_1, I/O Comments "SETUP Button" / "ALARM LOG
// Button" / "TREND FULL Button" / "TEST Button" / "DIAG Button").
constexpr uint16_t BUTTON_SETUP_ADDR = 30;
constexpr uint16_t BUTTON_ALARM_LOG_ADDR = 31;
constexpr uint16_t BUTTON_TREND_FULL_ADDR = 32;
constexpr uint16_t BUTTON_TEST_ADDR = 33;
constexpr uint16_t BUTTON_DIAG_ADDR = 34;
constexpr uint8_t BUTTON_COUNT = 5;

constexpr uint16_t LAMP_SETUP_ADDR = 40;
constexpr uint16_t LAMP_ALARM_LOG_ADDR = 41;
constexpr uint16_t LAMP_TREND_FULL_ADDR = 42;
constexpr uint16_t LAMP_TEST_ADDR = 43;
constexpr uint16_t LAMP_DIAG_ADDR = 44;

constexpr uint32_t BUTTON_POLL_INTERVAL_MS = 2000;

constexpr uint16_t DIAG_BUTTON_BASE_ADDR = 50;
constexpr uint8_t DIAG_BUTTON_COUNT = 28;
constexpr uint32_t DIAG_BUTTON_POLL_INTERVAL_MS = 2000; // safety-net only, same reasoning as BUTTON_POLL_INTERVAL_MS
} // namespace NS12

class NS12Manager {
public:
  void begin() {
    Serial2.setTxBufferSize(1024);
    Serial2.begin(NS12::BAUD, SERIAL_8N1, Pins::NS12_RX, Pins::NS12_TX);
    lastTelemetryMs = millis();
  }

  // WM: write `count` words starting at `startAddr`. count > 99 is a
  // protocol violation (the wire LL field is exactly 2 decimal digits),
  // not a soft limit -- clamped defensively and counted as a failure so a
  // regression is visible in diagnostics instead of silently desyncing
  // the frame.
  void sendWM(uint16_t startAddr, const uint16_t *data, uint16_t count) {
    if (count > NS12::MAX_WM_WORDS) {
      count = NS12::MAX_WM_WORDS;
      wmOversizedCount++;
    }
    char frame[4 + 4 + 2 + NS12::MAX_WM_WORDS * 5 + 1];
    size_t n = 0;
    frame[n++] = (char)NS12::ESC;
    frame[n++] = 'W';
    frame[n++] = 'M';
    frame[n++] = '0';
    // WM intentionally uses the plain, unoffset address -- see the
    // RM_WORD_ADDRESS_OFFSET comment in the NS12 namespace for why this
    // stays decoupled from the RM-side offset test.
    n += writeHex4(&frame[n], startAddr);
    n += writeDecimal2(&frame[n], (uint8_t)count);

    for (uint16_t i = 0; i < count; i++) {
      if (i > 0) frame[n++] = ',';
      n += writeHexCompact(&frame[n], data[i]);
    }
    frame[n++] = '\r';

#if NS12_DEBUG_RAW_RX
    Serial.print(F("[NS12] TX WM: "));

    for (size_t i = 0; i < n; i++) printRawByte((uint8_t)frame[i]);
    Serial.println();
#endif

    wmAttempts++;
    size_t sent = Serial2.write(reinterpret_cast<uint8_t *>(frame), n);

    if (sent != n) {
      // Partial write -- retry once immediately, picking up right after the
      // bytes that did make it into the driver's TX buffer. Still
      // non-blocking: Serial2.write() only falls short when that buffer is
      // full, and a second call either drains what's left or doesn't.
      sent += Serial2.write(reinterpret_cast<uint8_t *>(frame + sent), n - sent);
    }

    if (sent != n) {
      // Frame never fully went out even after the retry -- the PT's copy of
      // this address range is now stale/blank and will stay that way until
      // the next push. Log addr/count so a recurring blank column on the
      // display can be matched to a specific dropped write instead of just
      // the aggregate wmFailures count.
      wmFailures++;
      Serial.printf("[NS12] WM write failed: addr=0x%04X count=%u (%u/%u bytes sent)\n",
                    startAddr, (unsigned)count, (unsigned)sent, (unsigned)n);
    }
    // Blocking flush() intentionally not used for WM -- a stalled TX flush
    // here would block the whole control loop during InspectingTube.
    markTxBusy(n);
  }

  // RM: request `count` words (max 32) starting at `startAddr`. Sends the
  // request only and returns immediately -- non-blocking, unlike the prior
  // synthesis's spin-wait version, which could stall the whole loop for up
  // to RM_READ_TIMEOUT_MS at a time this file now also drives hard-real-
  // time Keyence pulse timing and encoder tracking. Call service() every
  // loop() iteration to drive the response state machine.
  bool requestRM(uint16_t startAddr, uint8_t count) {
    if (readPending || count == 0 || count > 32) return false;
    // See markTxBusy()'s comment: defer starting a read until any recent
    // WM/WB send is estimated to have actually finished draining off the
    // wire, not just been handed to Serial2.write().
    if ((int32_t)(millis() - txBusyUntilMs) < 0) return false;

    // See NS12::RM_WORD_ADDRESS_OFFSET -- currently 16384 under field test.
    // wireAddr (not the caller's plain startAddr) is what's actually sent
    // AND what the response is validated against below, since the PT would
    // echo back whatever address it actually processed.
    uint16_t wireAddr = (uint16_t)(startAddr + NS12::RM_WORD_ADDRESS_OFFSET);

    char frame[16];
    size_t n = 0;
    frame[n++] = (char)NS12::ESC;
    frame[n++] = 'R';
    frame[n++] = 'M';
    frame[n++] = '0';
    n += writeHex4(&frame[n], wireAddr);
    n += writeDecimal2(&frame[n], count);
    frame[n++] = '\r';

#if NS12_DEBUG_RAW_RX
    Serial.print(F("[NS12] TX RM: "));

    for (size_t i = 0; i < n; i++) printRawByte((uint8_t)frame[i]);
    Serial.println();
#endif

    rmAttempts++;
    purgeRxBeforeRequest();
    size_t sent = Serial2.write(reinterpret_cast<uint8_t *>(frame), n);

    if (sent != n) {
      // Request itself never fully went out -- don't burn the full
      // RM_READ_TIMEOUT_MS waiting on a reply to a frame the PT never saw.
      rmWriteFailures++;
      return false;
    }
    Serial2.flush(); // request frame is <=11 bytes -- flush cost here is negligible

    readPending = true;
    pendingCmdType = 'M';
    readSentMs = millis();
    readLineUsed = 0;
    expectedAddr = wireAddr; // matched against the response's own address field, which is the wire address
    expectedCount = count;
    return true;
  }

  // RB: request `count` bits (max 32) starting at `startAddr`, for the
  // HMI push-buttons ($B30-$B34). Same non-blocking shape as requestRM()
  // -- shares the single read-pending state machine (only one request,
  // word or bit, can ever be in flight), routed by pendingCmdType so the
  // response validates against 'B' instead of 'M' and completion counts
  // into the separate rb* counters.
  bool requestRB(uint16_t startAddr, uint8_t count) {
    if (readPending || count == 0 || count > 32) return false;

    if ((int32_t)(millis() - txBusyUntilMs) < 0) return false; // see markTxBusy()

    uint16_t wireAddr = startAddr;

    char frame[16];
    size_t n = 0;
    frame[n++] = (char)NS12::ESC;
    frame[n++] = 'R';
    frame[n++] = 'B';
    frame[n++] = '0';
    n += writeHex4(&frame[n], wireAddr);
    n += writeDecimal2(&frame[n], count);
    frame[n++] = '\r';

#if NS12_DEBUG_RAW_RX
    Serial.print(F("[NS12] TX RB: "));

    for (size_t i = 0; i < n; i++) printRawByte((uint8_t)frame[i]);
    Serial.println();
#endif

    rbAttempts++;
    purgeRxBeforeRequest();
    size_t sent = Serial2.write(reinterpret_cast<uint8_t *>(frame), n);

    if (sent != n) {
      rbWriteFailures++;
      return false;
    }
    Serial2.flush();

    readPending = true;
    pendingCmdType = 'B';
    readSentMs = millis();
    readLineUsed = 0;
    expectedAddr = wireAddr;
    expectedCount = count;
    return true;
  }

  // WB: write `count` bits starting at `startAddr`. Fire-and-forget, same
  // shape as sendWM() -- no response expected.
  void sendWB(uint16_t startAddr, const bool *bits, uint8_t count) {
    if (count > NS12::MAX_WB_BITS) {
      count = NS12::MAX_WB_BITS;
      wbOversizedCount++;
    }
    static const char kHexDigits[] = "0123456789ABCDEF";
    uint8_t hexDigitCount = (uint8_t)((count + 3) / 4);
    char frame[4 + 4 + 2 + (NS12::MAX_WB_BITS + 3) / 4 + 1];
    size_t n = 0;
    frame[n++] = (char)NS12::ESC;
    frame[n++] = 'W';
    frame[n++] = 'B';
    frame[n++] = '0';
    n += writeHex4(&frame[n], startAddr);
    n += writeDecimal2(&frame[n], count);

    for (uint8_t d = 0; d < hexDigitCount; d++) {
      uint8_t nibble = 0;

      for (uint8_t k = 0; k < 4; k++) {
        uint8_t bitIndex = (uint8_t)(d * 4 + k);

        if (bitIndex < count && bits[bitIndex]) {
          nibble = (uint8_t)(nibble | (1 << (3 - k)));
        }
      }
      frame[n++] = kHexDigits[nibble];
    }
    frame[n++] = '\r';

#if NS12_DEBUG_RAW_RX
    Serial.print(F("[NS12-WB-TX] addr=0x"));
    Serial.print(startAddr, HEX);
    Serial.print(F(" count="));
    Serial.print(count);
    Serial.print(F(" raw ("));
    Serial.print(n);
    Serial.print(F(" bytes): "));

    for (size_t i = 0; i < n; i++) printRawByteAlways((uint8_t)frame[i]);
    Serial.println();
#endif

    wbAttempts++;
    size_t sent = Serial2.write(reinterpret_cast<uint8_t *>(frame), n);

    if (sent != n) {
      wbFailures++;
    }
    // No blocking flush -- same rationale as sendWM(): never stall the loop.
    markTxBusy(n);
  }

  // Must be called every loop() iteration. Drives the periodic telemetry
  // push and the non-blocking read state machine. Never blocks. RM
  // requests themselves are now driven externally by
  // serviceHmiInputPolling() (see below), not from inside here -- this
  // just needs to keep pumping pollIncoming() regardless of who called
  // requestRM().
  void service() {
    uint32_t now = millis();

    // Gated on !readPending: RM_READ_TIMEOUT_MS and
    // TELEMETRY_WRITE_INTERVAL_MS are both 250-300ms (and the HMI input
    // poll adds a third, slower request source), so a telemetry WM write
    // could otherwise fire in the middle of an in-flight RM read on this
    // shared half-visible UART. pollIncoming() only checks that a byte
    // stream starts at an ESC, not where it actually came from -- see the
    // field-report comment on the NS12 namespace for why that matters
    // (every captured "RM response" so far had a WM-shaped header, before
    // this gating existed). This delays telemetry by at most one
    // RM_READ_TIMEOUT_MS window, not lost. Harmless no-op with RM polling
    // disabled (readPending then never becomes true).
    if (!readPending && now - lastTelemetryMs >= NS12::TELEMETRY_WRITE_INTERVAL_MS) {
      lastTelemetryMs = now;
      sendWM(NS12::TELEMETRY_BASE_ADDR, telemetry, NS12::TELEMETRY_WORD_COUNT);
    }

    pollIncoming(now);
  }

  // Pop semantics: returns true (once) for the most recently completed
  // successful RM read, then clears until the next one lands. Written by
  // parseReadResponse() on success; consumed by serviceHmiInputPolling().
  // Gated on lastReadKind == Word so this never accidentally consumes a
  // completed RB (bit) read meant for consumeReadBit() instead -- both
  // share the same single-slot "last completed read" state since only one
  // request (word or bit) is ever in flight at a time. addrOut is
  // translated back to the caller's plain $Wn label (WIRE address minus
  // NS12::RM_WORD_ADDRESS_OFFSET) -- the offset, if any, stays entirely
  // internal to this class; external code never has to think about it, in
  // either direction.
  bool consumeReadWord(uint16_t &addrOut, uint16_t &valueOut) {
    if (!lastReadValid || lastReadKind != ReadKind::Word) return false;
    addrOut = (uint16_t)(lastReadAddrValue - NS12::RM_WORD_ADDRESS_OFFSET);
    valueOut = lastReadWordValue;
    lastReadValid = false;
    return true;
  }

  bool consumeReadBit(uint16_t &addrOut, bool &valueOut, uint16_t &rawValueOut) {
    if (!lastReadValid || lastReadKind != ReadKind::Bit) return false;
    addrOut = lastReadAddrValue;
    rawValueOut = lastReadWordValue;
    valueOut = (lastReadWordValue & 0x80) != 0;
    lastReadValid = false;
    return true;
  }

  void setTelemetry(uint16_t heartbeat, uint16_t fpsX10, uint16_t minX10, uint16_t maxX10,
                     uint16_t avgX10, uint16_t goodFrames, uint16_t badFrames,
                     uint16_t stateValue, uint16_t statusWord) {
    telemetry[0] = heartbeat;
    telemetry[1] = fpsX10;
    telemetry[2] = minX10;
    telemetry[3] = maxX10;
    telemetry[4] = avgX10;
    telemetry[5] = goodFrames;
    telemetry[6] = badFrames;
    telemetry[7] = stateValue;
    telemetry[8] = statusWord;
  }

  uint32_t rmAttemptCount() const { return rmAttempts; }
  uint32_t rmSuccessCount() const { return rmSuccesses; }
  uint32_t rmWriteFailureCount() const { return rmWriteFailures; }
  uint32_t rmTimeoutCount() const { return rmTimeouts; }
  uint32_t rmParseErrorCount() const { return rmParseErrors; }
  uint32_t wmAttemptCount() const { return wmAttempts; }
  uint32_t wmFailureCount() const { return wmFailures; }
  uint32_t wmOversizedCountValue() const { return wmOversizedCount; }
  void resetRmStats() { rmAttempts = 0; rmSuccesses = 0; }

  uint32_t rbAttemptCount() const { return rbAttempts; }
  uint32_t rbSuccessCount() const { return rbSuccesses; }
  uint32_t rbWriteFailureCount() const { return rbWriteFailures; }
  uint32_t rbTimeoutCount() const { return rbTimeouts; }
  uint32_t rbParseErrorCount() const { return rbParseErrors; }
  uint32_t wbAttemptCount() const { return wbAttempts; }
  uint32_t wbFailureCount() const { return wbFailures; }

  // Exposed so the matrix-push free functions (serviceDisplayThrottle(),
  // serviceMatrixPacing()) can defer a WM write the same way service()
  // defers telemetry -- see the field-report comment on the NS12
  // namespace for why a WM write during a pending RM read is suspect.
  bool isReadPending() const { return readPending; }

  bool consumeNotifyBit(uint16_t &addrOut, bool &valueOut) {
    if (!notifyValid) return false;
    addrOut = lastNotifyAddr;
    valueOut = lastNotifyValue;
    notifyValid = false;
    return true;
  }
  uint32_t sbNotifyCount() const { return notifySbCount; }
  uint32_t sbNotifyRejectedCount() const { return notifySbRejectedCount; }

private:
  uint16_t telemetry[NS12::TELEMETRY_WORD_COUNT] = {};
  uint32_t lastTelemetryMs = 0;

  bool readPending = false;
  char pendingCmdType = 'M'; // 'M' (RM/word) or 'B' (RB/bit) -- which request is in flight
  uint32_t readSentMs = 0;
  uint16_t expectedAddr = 0;
  uint8_t expectedCount = 0;
  char readLineBuffer[40] = {};
  size_t readLineUsed = 0;

  uint16_t lastNotifyAddr = 0;
  bool lastNotifyValue = false;
  bool notifyValid = false;
  uint32_t notifySbCount = 0;
  uint32_t notifySbRejectedCount = 0;

  // Set by parseReadResponse() on a successful parse, popped by
  // consumeReadWord() or consumeReadBit() depending on lastReadKind -- see
  // those methods' comments.
  enum class ReadKind : uint8_t { Word, Bit };
  bool lastReadValid = false;
  ReadKind lastReadKind = ReadKind::Word;
  uint16_t lastReadAddrValue = 0;
  uint16_t lastReadWordValue = 0;

  uint32_t rmAttempts = 0;
  uint32_t rmSuccesses = 0;
  uint32_t rmWriteFailures = 0;
  uint32_t rmTimeouts = 0;
  uint32_t rmParseErrors = 0;
  uint32_t wmAttempts = 0;
  uint32_t wmFailures = 0;
  uint32_t wmOversizedCount = 0;

  uint32_t rbAttempts = 0;
  uint32_t rbSuccesses = 0;
  uint32_t rbWriteFailures = 0;
  uint32_t rbTimeouts = 0;
  uint32_t rbParseErrors = 0;
  uint32_t wbAttempts = 0;
  uint32_t wbFailures = 0;
  uint32_t wbOversizedCount = 0;

  uint32_t txBusyUntilMs = 0;

  void purgeRxBeforeRequest() {
    pollIncoming(millis());
    readLineUsed = 0;
  }

  // Called from sendWM()/sendWB() right after Serial2.write(). Those
  // writes are deliberately non-blocking (no flush()), so their bytes can
  // still be physically draining off the wire when a read request starts
  // a few loop() iterations later -- and this link echoes/leaks its own
  // transmitted bytes back onto RX. Accumulates rather than overwrites --
  // if a previous send's estimated drain time hasn't passed yet, a new
  // send's transmission time queues up behind it, approximating how the
  // actual UART TX FIFO drains sends in order.
  // Non-blocking by construction: this only changes when requestRM()/
  // requestRB() are willing to start, never stalls loop() itself, so it
  // cannot introduce the Keyence 100us-pulse jitter a real flush() would.
  void markTxBusy(size_t frameBytes) {
    uint32_t txMs = (uint32_t)((frameBytes * 10UL * 1000UL) / (uint32_t)NS12::BAUD) + 5UL;
    uint32_t now = millis();
    uint32_t baseline = ((int32_t)(txBusyUntilMs - now) > 0) ? txBusyUntilMs : now;
    txBusyUntilMs = baseline + txMs;
  }

  static void printRawByte(uint8_t value) {
#if NS12_DEBUG_RAW_RX
    if (value == NS12::ESC) { Serial.print(F("[ESC]")); return; }

    if (value == '\r') { Serial.print(F("[CR]")); return; }

    if (value >= 0x20 && value < 0x7F) { Serial.print((char)value); return; }
    Serial.print('[');

    if (value < 0x10) Serial.print('0');
    Serial.print(value, HEX);
    Serial.print(']');
#else
    (void)value;
#endif
  }

  // Unconditional (not gated by NS12_DEBUG_RAW_RX) -- a parse failure is
  // exactly the case that needs visibility by default. Non-static so it
  // can check pendingCmdType to name the right command letter below.
  void dumpRejectedLine(const char *line, size_t len) {
    Serial.print(F("[NS12] R"));
    Serial.print(pendingCmdType);
    Serial.print(F(" parse failed, raw response ("));
    Serial.print(len);
    Serial.print(F(" bytes): "));

    for (size_t i = 0; i < len; i++) {
      printRawByteAlways((uint8_t)line[i]);
    }
    Serial.println();
    // Flag explicitly rather than making the reader notice: a genuine reply
    // starts 'R' followed by the command letter we requested ('M' or 'B').
    // If it starts 'W' instead, this isn't a PT response at all -- it's
    // shaped like one of OUR OWN WM/WB writes, most likely a loopback/echo
    // or a write that fired while this read was still pending. See the
    // field-report comment on the NS12 namespace.
    if (len >= 3 && line[1] == 'W' && (line[2] == 'M' || line[2] == 'B')) {
      Serial.print(F("[NS12]   ^ starts 'W',"));
      Serial.print(line[2]);
      Serial.print(F(" -- looks like our own W"));
      Serial.print(line[2]);
      Serial.println(F(" traffic, not a genuine reply. Check for TX/RX "
                        "loopback or PT echo."));
    }
  }

  static void printRawByteAlways(uint8_t value) {
    if (value == NS12::ESC) { Serial.print(F("[ESC]")); return; }

    if (value >= 0x20 && value < 0x7F) { Serial.print((char)value); return; }
    Serial.print('[');

    if (value < 0x10) Serial.print('0');
    Serial.print(value, HEX);
    Serial.print(']');
  }

  void handleCompleteLine(const char *line, size_t len) {
    if (len >= 3 && (uint8_t)line[0] == NS12::ESC && line[1] == 'S' && line[2] == 'B') {
      if (parseNotifySB(line, len)) {
        notifySbCount++;
      } else {
        notifySbRejectedCount++;
      }
      return;
    }

    if (!readPending) return;
    bool isBit = (pendingCmdType == 'B');

    if (parseReadResponse(line, len)) {
      if (isBit) rbSuccesses++; else rmSuccesses++;
    } else {
      if (isBit) rbParseErrors++; else rmParseErrors++;
      dumpRejectedLine(line, len);
    }
    readPending = false;
  }

  void pollIncoming(uint32_t now) {
    while (Serial2.available() > 0 && readLineUsed < sizeof(readLineBuffer) - 1) {
      char ch = (char)Serial2.read();

      // A genuine frame always starts with ESC. Anything arriving before
      // that first ESC is stray (e.g. overlap with a WM write) and is
      // discarded rather than corrupting the line.
      if (readLineUsed == 0 && (uint8_t)ch != NS12::ESC) {
        continue;
      }

      if (ch == '\r') {
        readLineBuffer[readLineUsed] = '\0';
        handleCompleteLine(readLineBuffer, readLineUsed);
        readLineUsed = 0;
        continue;
      }

      readLineBuffer[readLineUsed++] = ch;
    }

    if (!readPending) return;

    if (readLineUsed >= sizeof(readLineBuffer) - 1) {
      if (pendingCmdType == 'B') rbParseErrors++; else rmParseErrors++;
      readPending = false;
      readLineUsed = 0;
      return;
    }

    if (now - readSentMs > NS12::RM_READ_TIMEOUT_MS) {
      if (pendingCmdType == 'B') rbTimeouts++; else rmTimeouts++;
      readPending = false;
    }
  }

  // SB: PT memory ($B) change notice -- unsolicited, sent by the PT on its
  // own whenever a $B bit at or above the panel's configured "Notice
  // start $B" changes. Manual format: ESC 'S' 'B' *A(4-hex addr)
  // *B(2-hex, "01" fixed -- always exactly one bit) *D(1-hex: 0=OFF/
  // 1=ON) SUM(2-hex), no CR counted here (stripped by the caller) -- 12
  // bytes total, always. Same SUM convention as parseReadResponse():
  // lower byte of the sum of every byte from ESC through *D. Address is
  // the plain PT memory address, same as RB/WB -- bits never take the
  // $W-style offset.
  bool parseNotifySB(const char *response, size_t len) {
    if (len != 12 || (uint8_t)response[0] != NS12::ESC || response[1] != 'S' ||
        response[2] != 'B') {
      return false;
    }

    if (response[7] != '0' || response[8] != '1') return false; // *B always "01"
    char d = response[9];

    if (d != '0' && d != '1') return false;

    char sumText[3] = {response[10], response[11], '\0'};
    uint8_t receivedSum = (uint8_t)strtoul(sumText, nullptr, 16);
    uint8_t computedSum = 0;

    for (size_t i = 0; i < 10; i++) computedSum += (uint8_t)response[i];

    if (computedSum != receivedSum) return false;

    char addrText[5] = {response[3], response[4], response[5], response[6], '\0'};
    lastNotifyAddr = (uint16_t)strtoul(addrText, nullptr, 16);
    lastNotifyValue = (d == '1');
    notifyValid = true;
    return true;
  }

  // Response framing: ESC 'R' 'M'/'B' [maybe '0' echoed] AAAA(4-hex)
  // LL(2-dec) D,D,... SUM(2-hex) CR -- shared by both RM (word) and RB (bit)
  // reads, distinguished by pendingCmdType (set in requestRM()/requestRB()).
  // The '0' echo has not been independently confirmed on this PT, so both
  // candidate offsets are tried; whichever validates wins.
  bool parseReadResponse(const char *response, size_t len) {
    if (len < 9 || (uint8_t)response[0] != NS12::ESC || response[1] != 'R' ||
        response[2] != pendingCmdType) {
      return false;
    }

    static const uint8_t candidateOffsets[] = {3, 4};

    for (uint8_t offset : candidateOffsets) {
      size_t headerLen = (size_t)offset + 6;

      if (len < headerLen) continue;

      char addrText[5] = {response[offset], response[offset + 1], response[offset + 2],
                          response[offset + 3], '\0'};
      char countText[3] = {response[offset + 4], response[offset + 5], '\0'};
      uint16_t addr = (uint16_t)strtoul(addrText, nullptr, 16);
      uint8_t count = (uint8_t)strtoul(countText, nullptr, 10);

      if (addr != expectedAddr || count != expectedCount) continue;

      size_t totalTailLen = len - headerLen;

      if (totalTailLen < 3 || totalTailLen - 2 >= 8) continue; // >=1 data char + 2 sum chars
      size_t dataLen = totalTailLen - 2;
      char dataText[8];
      memcpy(dataText, &response[headerLen], dataLen);
      dataText[dataLen] = '\0';
      char *comma = strchr(dataText, ',');

      if (comma) *comma = '\0';

      char sumText[3] = {response[headerLen + dataLen], response[headerLen + dataLen + 1], '\0'};
      uint8_t receivedSum = (uint8_t)strtoul(sumText, nullptr, 16);
      uint8_t computedSum = 0;

      for (size_t i = 0; i < headerLen + dataLen; i++) computedSum += (uint8_t)response[i];

      if (computedSum != receivedSum) continue;

      char *endPtr = nullptr;
      unsigned long parsedValue = strtoul(dataText, &endPtr, 16);

      if (endPtr == dataText) continue;

      lastReadAddrValue = addr;
      lastReadWordValue = (uint16_t)parsedValue;
      lastReadKind = (pendingCmdType == 'B') ? ReadKind::Bit : ReadKind::Word;
      lastReadValid = true;

#if NS12_DEBUG_RAW_RX
      Serial.print(F("[NS12-FRAME] R"));
      Serial.print(pendingCmdType);
      Serial.print(F(" offset="));
      Serial.print(offset);
      Serial.print(F(" addrText=\""));
      Serial.print(addrText);
      Serial.print(F("\" countText=\""));
      Serial.print(countText);
      Serial.print(F("\" dataText=\""));
      Serial.print(dataText);
      Serial.print(F("\" sum=0x"));
      Serial.print(receivedSum, HEX);
      Serial.print(F(" (ok) raw ("));
      Serial.print(len);
      Serial.print(F(" bytes): "));

      for (size_t i = 0; i < len; i++) printRawByteAlways((uint8_t)response[i]);
      Serial.println();
#endif
      return true;
    }
    return false;
  }

  static size_t writeHex4(char *dst, uint16_t v) {
    char tmp[5];
    snprintf(tmp, sizeof(tmp), "%04X", v);
    memcpy(dst, tmp, 4);
    return 4;
  }
  static size_t writeDecimal2(char *dst, uint8_t v) {
    char tmp[3];
    snprintf(tmp, sizeof(tmp), "%02u", v);
    memcpy(dst, tmp, 2);
    return 2;
  }
  // Zero-suppressed hex (e.g. 0 -> "0", 10 -> "A") -- matches the comma-
  // separated, variable-width data encoding confirmed on the bench.
  static size_t writeHexCompact(char *dst, uint16_t v) {
    char tmp[5];
    int len = snprintf(tmp, sizeof(tmp), "%X", v);
    memcpy(dst, tmp, (size_t)len);
    return (size_t)len;
  }
};

NS12Manager ns12;

// =====================================================================
// Word Lamp raw-delta-to-palette mapping and matrix downsample. Despite
// the name (kept to minimize churn at call sites), this maps a raw-ADC-
// delta value (see MATRIX_RAW_DELTA_MIN/MAX above), not degrees C.
// =====================================================================
uint8_t tempToPaletteIndex(float rawDelta) {
  float t = (rawDelta - MATRIX_RAW_DELTA_MIN) / (MATRIX_RAW_DELTA_MAX - MATRIX_RAW_DELTA_MIN);

  if (t < 0) t = 0;

  if (t > 1) t = 1;
  uint8_t idx = NS12::PALETTE_MIN_INDEX +
                (uint8_t)(t * (NS12::PALETTE_MAX_INDEX - NS12::PALETTE_MIN_INDEX));

  if (idx < NS12::PALETTE_MIN_INDEX) idx = NS12::PALETTE_MIN_INDEX;

  if (idx > NS12::PALETTE_MAX_INDEX) idx = NS12::PALETTE_MAX_INDEX;
  return idx;
}

// Downsamples the 32x24 analysis frame into a displayCols x displayRows
// grid using max-per-block (matches the max-hold philosophy: don't average
// away a hot pixel for HMI visibility).
void downsampleMaxBlock(const float *src, uint8_t displayCols, uint8_t displayRows,
                         float *dst) {
  uint8_t blockW = StripZone::COLS / displayCols;
  uint8_t blockH = StripZone::ROWS / displayRows;

  for (uint8_t dc = 0; dc < displayCols; dc++) {
    for (uint8_t dr = 0; dr < displayRows; dr++) {
      float m = -1000.0f;

      for (uint8_t x = 0; x < blockW; x++) {
        for (uint8_t y = 0; y < blockH; y++) {
          uint8_t sc = dc * blockW + x;
          uint8_t sr = dr * blockH + y;
          float v = src[sr * StripZone::COLS + sc];
          // Skip implausible pixels so one glitching pixel can't paint a
          // false hot spot on the HMI (see isPlausibleTemp()).
          if (isPlausibleTemp(v) && v > m) m = v;
        }
      }
      dst[dc * displayRows + dr] = m;
    }
  }
}

// Display refresh decoupled from live camera streaming, velocity-adaptive
// per-tube throttle. TARGET_DISPLAY_REFRESH_MS (2000ms) is a floor, not a
// fixed cadence: pushes never happen closer together than this (protects
// the PT the same way the column-pacing above does), but when tubes are
// passing slower than that the display still updates once per tube rather
// than sitting idle for the rest of the 2000ms window.
static const uint32_t MIN_DISPLAY_REFRESH_MS = NS12::TARGET_DISPLAY_REFRESH_MS;
uint32_t lastDisplayPushMs = 0;
uint32_t lastTubeLatchMs = 0;
uint32_t currentDisplayRefreshMs = NS12::TARGET_DISPLAY_REFRESH_MS;
bool displayPushQueued = false;
float pendingDisplayFrame[StripZone::COLS * StripZone::ROWS];

void requestDisplayPush(const float *frame) {
  uint32_t now = millis();
  uint32_t interTubeGapMs = now - lastTubeLatchMs;
  lastTubeLatchMs = now;
  currentDisplayRefreshMs = interTubeGapMs > MIN_DISPLAY_REFRESH_MS ? interTubeGapMs
                                                                     : MIN_DISPLAY_REFRESH_MS;
  memcpy(pendingDisplayFrame, frame, sizeof(pendingDisplayFrame));
  displayPushQueued = true;
}

// State for the experimental 32x24 column-paced push: one column (of `rows`
// words) is sent per COLUMN_WRITE_INTERVAL_MS tick from serviceMatrixPacing(),
// instead of one 768-word burst -- the actual mitigation for the PT being
// starved of time to service RM reads by one oversized write.
struct PendingMatrixWrite {
  bool active = false;
  uint8_t cols = 0, rows = 0;
  uint16_t words[NS12::MATRIX_COLS_EXPERIMENTAL * NS12::MATRIX_ROWS_EXPERIMENTAL];
  uint16_t bandAddr = 0;
  uint16_t band01[2] = {0, 0};
  uint8_t nextCol = 0;
  uint32_t lastWriteMs = 0;
};
PendingMatrixWrite pendingMatrix;

// Troubleshooting aid: increments once per fully-completed matrix push
// (every column plus the band-max write) and gets pushed to the word right
// after the band-max pair (bandAddr+2 -- 830 at default 16x8, since bandAddr
// itself is 828 there; computed off bandAddr rather than hardcoded so it
// stays clear of the matrix's own pixel range in experimental 32x24 mode
// too). Watch this on the panel: if it keeps incrementing but a column
// stays blank, the ESP genuinely finished sending and the fault is on the
// PT side, not a dropped WM write.
uint16_t matrixPushCounter = 0;

// Runtime-effective mode: starts at the compile-time default but can be
// latched false by checkDisplayAutoFallback() below if the experimental
// 32x24 mode is starving RM reads.
bool experimental32x24Effective = NS12::ENABLE_EXPERIMENTAL_32x24;

void pushWordLampMatrix(const float *compositeFrame) {
  uint8_t cols = experimental32x24Effective ? NS12::MATRIX_COLS_EXPERIMENTAL
                                             : NS12::MATRIX_COLS_DEFAULT;
  uint8_t rows = experimental32x24Effective ? NS12::MATRIX_ROWS_EXPERIMENTAL
                                             : NS12::MATRIX_ROWS_DEFAULT;

  static float displayBuf[32 * 24];

  if (cols == StripZone::COLS && rows == StripZone::ROWS) {
    memcpy(displayBuf, compositeFrame, sizeof(float) * cols * rows);
  } else {
    downsampleMaxBlock(compositeFrame, cols, rows, displayBuf);
  }

  static uint16_t words[32 * 24];
  // Two halves (rows 0..rows/2-1, rows/2..rows-1), matching the documented
  // $W828 (rows 1-4) / $W829 (rows 5-8) band-maximum layout at 16x8.
  float bandMax[2] = {-1000, -1000};

  for (uint8_t c = 0; c < cols; c++) {
    for (uint8_t r = 0; r < rows; r++) {
      float v = displayBuf[c * rows + r];
      words[c * rows + r] = tempToPaletteIndex(v);
      uint8_t half = (r < rows / 2) ? 0 : 1;

      if (v > bandMax[half]) bandMax[half] = v;
    }
  }
  uint16_t band01[2] = {(uint16_t)(bandMax[0] * 10), (uint16_t)(bandMax[1] * 10)};

  // Always column-paced, including the default 16x8 mode -- never a single
  // cols*rows-word burst. The wire protocol's LL field is exactly 2 decimal
  // digits, so no single WM command can legitimately carry more than 99
  // words; splitting into column writes (rows=8 or 24 words each, both
  // well under 99) keeps every write inside that limit. Lands the band-max
  // words at $828/$829 (MATRIX_BASE_ADDR + cols*rows = 700+128), and the
  // matrixPushCounter troubleshooting word right after at $830 -- see its
  // declaration for what it's for.
  memcpy(pendingMatrix.words, words, sizeof(uint16_t) * cols * rows);
  pendingMatrix.cols = cols;
  pendingMatrix.rows = rows;
  pendingMatrix.bandAddr = NS12::MATRIX_BASE_ADDR + cols * rows;
  pendingMatrix.band01[0] = band01[0];
  pendingMatrix.band01[1] = band01[1];
  pendingMatrix.nextCol = 0;
  pendingMatrix.lastWriteMs = 0; // fire the first column on the next service() tick
  pendingMatrix.active = true;
}

void pushTestPattern() {
  static float testPattern[StripZone::COLS * StripZone::ROWS];

  for (size_t i = 0; i < StripZone::COLS * StripZone::ROWS; i++) {
    testPattern[i] = MATRIX_RAW_DELTA_MIN + (MATRIX_RAW_DELTA_MAX - MATRIX_RAW_DELTA_MIN) *
                                                 ((float)i / (StripZone::COLS * StripZone::ROWS));
  }
  pushWordLampMatrix(testPattern);
  Serial.println(F("[DIAG] Test pattern pushed. Send 'R' to clear it."));
}

// Called every loop() iteration; flushes a queued composite once the
// current velocity-adaptive throttle interval (requestDisplayPush) has
// elapsed.
void serviceDisplayThrottle() {
  if (!displayPushQueued) return;

  if (ns12.isReadPending()) return; // defer -- see field-report comment on NS12 namespace
  uint32_t now = millis();

  if (now - lastDisplayPushMs < currentDisplayRefreshMs) return;
  lastDisplayPushMs = now;
  displayPushQueued = false;
  pushWordLampMatrix(pendingDisplayFrame);
}

// Called every loop() iteration; no-op unless a paced matrix push is
// active. Handles both the default 16x8 mode and experimental 32x24 --
// pendingMatrix.cols/rows are set per-push in pushWordLampMatrix(), never
// hardcoded here.
void serviceMatrixPacing() {
  if (!pendingMatrix.active) return;

  if (ns12.isReadPending()) return; // defer -- see field-report comment on NS12 namespace
  uint32_t now = millis();

  if (now - pendingMatrix.lastWriteMs < NS12::COLUMN_WRITE_INTERVAL_MS) return;
  pendingMatrix.lastWriteMs = now;

  uint8_t c = pendingMatrix.nextCol;
  uint16_t addr = NS12::MATRIX_BASE_ADDR + (uint16_t)c * pendingMatrix.rows;
  ns12.sendWM(addr, &pendingMatrix.words[(size_t)c * pendingMatrix.rows], pendingMatrix.rows);
  pendingMatrix.nextCol++;

  if (pendingMatrix.nextCol >= pendingMatrix.cols) {
    ns12.sendWM(pendingMatrix.bandAddr, pendingMatrix.band01, 2);
    matrixPushCounter++;
    ns12.sendWM((uint16_t)(pendingMatrix.bandAddr + 2), &matrixPushCounter, 1);
    pendingMatrix.active = false;
  }
}

// Self-monitor for the experimental 32x24 column-paced mode: if RM read
// success rate collapses under real traffic, fall back to the trusted
// 16x8 mode rather than keep pushing into a PT that can't service reads.
void checkDisplayAutoFallback() {
  if (!experimental32x24Effective) return;

  if (ns12.rmAttemptCount() >= 50) {
    float successRate = (float)ns12.rmSuccessCount() / (float)ns12.rmAttemptCount();

    if (successRate < 0.5f) {
      experimental32x24Effective = false;
      pendingMatrix.active = false; // abandon any in-flight paced push
      Serial.println(F("[NS12] WARNING: RM success rate collapsed under 32x24 "
                        "traffic, falling back to 16x8 display mode."));
    }
    ns12.resetRmStats();
  }
}

// =====================================================================
// Capture / QC composite -- max-hold per tube pass.
// ARMED watches the frame's raw-delta max against CAPTURE_TRIGGER_RAW_DELTA ->
// SAMPLING accumulates CAPTURE_SAMPLE_COUNT frames -> LATCHED freezes the
// HMI image until rearmed.
//
// Aggregation is deliberately max-hold, not averaging -- the tube moves
// under a fixed FOV, so different frames see different physical sections;
// averaging would dilute/hide a glue trace that exited frame mid-window.
// Max-hold composites the hottest value seen at each cell across the
// whole transit.
// =====================================================================
enum class CaptureState { ARMED, SAMPLING, LATCHED };

struct GlueStripResult {
  bool present = false;
  float maxTempC = 0;
  uint16_t hotPixelCount = 0;
};

class CaptureController {
public:
  void rearm() {
    state = CaptureState::ARMED;
    sampleCount = 0;
  }

  void onNewFrame(const float *frame) {
    switch (state) {
    case CaptureState::ARMED: {
      float maxT = frameMax(frame);

      if (maxT >= CAPTURE_TRIGGER_RAW_DELTA) {
        // Seed with -INFINITY for implausible pixels rather than copying
        // them verbatim -- otherwise a single glitching pixel elsewhere in
        // the trigger frame (not even the one that crossed the threshold)
        // would ride along into the QC composite and strip evaluation.
        for (size_t i = 0; i < StripZone::COLS * StripZone::ROWS; i++) {
          compositeFrame[i] = isPlausibleTemp(frame[i]) ? frame[i] : -INFINITY;
        }
        sampleCount = 1;
        state = CaptureState::SAMPLING;
      }
      break;
    }
    case CaptureState::SAMPLING: {
      for (size_t i = 0; i < StripZone::COLS * StripZone::ROWS; i++) {
        if (isPlausibleTemp(frame[i]) && frame[i] > compositeFrame[i]) {
          compositeFrame[i] = frame[i];
        }
      }
      sampleCount++;

      if (sampleCount >= CAPTURE_SAMPLE_COUNT) {
        evaluateStrips();
        state = CaptureState::LATCHED;
        requestDisplayPush(compositeFrame);
        Serial.printf("[QC] strip1 present=%d maxT=%.1fC hotPx=%u | "
                      "strip2 present=%d maxT=%.1fC hotPx=%u\n",
                      strip1Result.present, strip1Result.maxTempC, strip1Result.hotPixelCount,
                      strip2Result.present, strip2Result.maxTempC, strip2Result.hotPixelCount);
      }
      break;
    }
    case CaptureState::LATCHED:
      // Frozen until rearm().
      break;
    }
  }

  CaptureState currentState() const { return state; }
  const float *latchedFrame() const { return compositeFrame; }
  const GlueStripResult &strip1() const { return strip1Result; }
  const GlueStripResult &strip2() const { return strip2Result; }

private:
  CaptureState state = CaptureState::ARMED;
  uint8_t sampleCount = 0;
  float compositeFrame[StripZone::COLS * StripZone::ROWS] = {0};
  GlueStripResult strip1Result, strip2Result;

  // -INFINITY if no pixel in the frame is plausible -- always < any real
  // CAPTURE_TRIGGER_RAW_DELTA, so a fully-glitched frame simply never
  // triggers rather than triggering on frame[0] regardless of its
  // validity (the previous version didn't check frame[0] at all).
  static float frameMax(const float *frame) {
    float m = -INFINITY;

    for (size_t i = 0; i < StripZone::COLS * StripZone::ROWS; i++) {
      if (isPlausibleTemp(frame[i]) && frame[i] > m) m = frame[i];
    }
    return m;
  }

  void evaluateStrips() {
    strip1Result = evalStrip(StripZone::STRIP1_COL_START, StripZone::STRIP1_COL_END);
    strip2Result = evalStrip(StripZone::STRIP2_COL_START, StripZone::STRIP2_COL_END);
  }

  GlueStripResult evalStrip(uint8_t colStart, uint8_t colEnd) {
    GlueStripResult r;

    for (uint8_t c = colStart; c <= colEnd; c++) {
      for (uint8_t row = 0; row < StripZone::ROWS; row++) {
        float v = compositeFrame[row * StripZone::COLS + c];

        if (!isPlausibleTemp(v)) continue; // unfilled cell (-INFINITY seed) or stray glitch

        if (v > r.maxTempC) r.maxTempC = v;

        if (v >= CAPTURE_TRIGGER_RAW_DELTA) r.hotPixelCount++;
      }
    }
    r.present = r.hotPixelCount > 0;
    return r;
  }
};

CaptureController capture;

// =====================================================================
// System state machine
// Startup -> Standby -> WaitingForTube -> InspectingTube / TubeGap ->
// FaultStop
// =====================================================================
enum class SystemState { Startup, Standby, WaitingForTube, InspectingTube, TubeGap, FaultStop };
SystemState state = SystemState::Startup;
SystemState lastLoggedState = SystemState::Startup;

// Per-tube one-shot trigger flags, reset on each new leading edge.
bool tubeMlxArmed = false;
bool tubeKeyenceStartFired = false;
bool tubeKeyenceEndFired = false;
int64_t tubeStartEncoderCount = 0;
uint32_t lastMcpPollMs = 0;
uint32_t lastMlxFrameMs = 0;
uint32_t lastDiagnosticMs = 0;
uint32_t heartbeatCounter = 0;

const char *stateName(SystemState s) {
  switch (s) {
  case SystemState::Startup: return "Startup";
  case SystemState::Standby: return "Standby";
  case SystemState::WaitingForTube: return "WaitingForTube";
  case SystemState::InspectingTube: return "InspectingTube";
  case SystemState::TubeGap: return "TubeGap";
  case SystemState::FaultStop: return "FaultStop";
  }
  return "?";
}

void enterFaultStop() {
  state = SystemState::FaultStop;
}

// =====================================================================
// Position projection: fires the MLX capture arm and the two Keyence
// checks (Start/End of glue bead) at the configured lead distances ahead
// of the tube's leading edge, using the encoder as the distance reference
// and the presence sensor as the anchor.
// =====================================================================
void serviceTubePositionTracking() {
  if (state != SystemState::InspectingTube) return;

  float travelledMm = (float)(encoder.total() - tubeStartEncoderCount) / ENCODER_COUNTS_PER_MM;

  if (!tubeMlxArmed && travelledMm >= PRESENCE_TO_MLX_DISTANCE_MM) {
    capture.rearm();
    tubeMlxArmed = true;
  }

  // Start-of-glue check: referenced from the LEADING edge, known
  // immediately -- fires from the very first tube.
  if (!tubeKeyenceStartFired && travelledMm >= hotMeltStartPositionMm) {
    keyenceTrigger.fire();
    tubeKeyenceStartFired = true;
  }

  // End-of-glue check: referenced from the TRAILING edge, which isn't
  // knowable until a full tube has passed the presence sensor and its
  // length has been measured (tubeLengthLearned, set in
  // handlePresenceEdge() below). Stays un-fired for the entire first
  // tube; from the second tube on, uses the previous tube's measured
  // length to compute where this tube's trailing edge will be.
  if (tubeLengthLearned && !tubeKeyenceEndFired) {
    float endTriggerMm = learnedTubeLengthMm - hotMeltEndPositionMm;

    if (travelledMm >= endTriggerMm) {
      // Both checks share one KeyenceTrigger with a single pending-pulse
      // slot -- fine as long as Start/End are far enough apart in travel
      // distance to not overlap the 500us pulse, true for any plausible
      // tube length and line speed.
      keyenceTrigger.fire();
      tubeKeyenceEndFired = true;
    }
  }
}

void handlePresenceEdge() {
  bool rising = presenceState;
  presenceEdgePending = false;

  if (rising) {
    // Leading edge -- new tube entering the zone.
    if (state == SystemState::WaitingForTube || state == SystemState::TubeGap) {
      state = SystemState::InspectingTube;
      tubeStartEncoderCount = encoder.total();
      tubeMlxArmed = false;
      tubeKeyenceStartFired = false;
      tubeKeyenceEndFired = false;
    }
  } else {
    // Trailing edge -- tube has cleared the zone. Measure this tube's
    // length now (leading-to-trailing encoder distance) for the NEXT
    // tube's End-position projection -- see hotMeltEndPositionMm comment.
    if (state == SystemState::InspectingTube) {
      tubeEndEncoderCount = encoder.total();
      learnedTubeLengthMm =
          (float)(tubeEndEncoderCount - tubeStartEncoderCount) / ENCODER_COUNTS_PER_MM;
      bool hadNoEndCheck = !tubeKeyenceEndFired;
      tubeLengthLearned = true;

      if (hadNoEndCheck) {
        Serial.printf("[QC] Tube cleared without an End-position check -- measured length "
                      "%.1fmm now available for the next tube.\n",
                      learnedTubeLengthMm);
      }
      state = SystemState::TubeGap;
    }
  }
}

uint16_t currentScreenNumber = 0;
bool currentScreenNumberValid = false;

// =====================================================================
// HMI input polling -- rotates a low-rate RM read between the two
// operator-entered position words (HOTMELT_START_POSITION_ADDR,
// HOTMELT_END_POSITION_ADDR) and the current-screen-number word
// (CURRENT_SCREEN_ADDR), and routes any successful result into
// hotMeltStartPositionMm/hotMeltEndPositionMm/currentScreenNumber.
// Entirely inert whenever NS12_ENABLE_RM_POLLING is 0: no request ever
// goes out, so the fallback PLACEHOLDER values above stay in effect and
// hotMeltPositionsFromHmi stays false. Safe to call unconditionally from
// loop() either way.
// =====================================================================
#if NS12_ENABLE_RM_POLLING
uint32_t lastHmiPollMs = 0;
uint8_t nextHmiPollIndex = 0;

int8_t wordVerifyIndex = -1;
#endif

#if NS12_ENABLE_RM_POLLING
constexpr uint16_t kButtonAddrs[NS12::BUTTON_COUNT] = {
    NS12::BUTTON_SETUP_ADDR, NS12::BUTTON_ALARM_LOG_ADDR, NS12::BUTTON_TREND_FULL_ADDR,
    NS12::BUTTON_TEST_ADDR, NS12::BUTTON_DIAG_ADDR};
const char *const kButtonNames[NS12::BUTTON_COUNT] = {"SETUP", "ALARM LOG", "TREND FULL", "TEST",
                                                       "DIAG"};
bool buttonState[NS12::BUTTON_COUNT] = {};
uint32_t lastButtonPollMs = 0;
uint8_t nextButtonPollIndex = 0;

constexpr char kDiagCommandChars[NS12::DIAG_BUTTON_COUNT] = {
    'S', 'W', 'I', 'G', 'F', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'A', 'K', 'P', 'M', 'C', 'B', 'X', 'R', 'D', 'H', 'V', 'J', 'N', 'L'};
const char *const kDiagButtonNames[NS12::DIAG_BUTTON_COUNT] = {
    "STANDBY",      "WAIT_TUBE",       "INSPECTING",  "TUBE_GAP",
    "FAULT_STOP",   "IO1",             "IO2",         "IO3",
    "IO4",          "IO5",             "IO6",         "IO7",
    "IO8",          "IO9",             "OPTO3",       "KEYENCE_TRIG",
    "PLC_STATUS",   "TEST_PATTERN",    "CAPTURE_REARM", "BASELINE_CAPTURE",
    "FRAME_DUMP",   "REARM_BLANK",     "DIAGNOSTICS", "BURST_PROBE",
    "WORD_VERIFY",  "WB_RB_SELFTEST",  "NO_OFFSET_TEST", "MLX_LIVE_TOGGLE"};
bool diagButtonState[NS12::DIAG_BUTTON_COUNT] = {};
uint32_t lastDiagButtonPollMs = 0;
uint8_t nextDiagButtonPollIndex = 0;
uint32_t diagButtonPressCount = 0;

void setButtonStatusLed(uint8_t buttonIndex, bool on) {
  if (!mcpOk || buttonIndex >= NS12::BUTTON_COUNT) return;
  mcpOutputState[buttonIndex] = on;
  mcp.digitalWrite(kMcpOutputPins[buttonIndex], on);
}

// Flips a button's status LED (current on/off state already tracked in
// mcpOutputState[], reused rather than adding a second state array) --
// called once per momentary press, not per raw bit level. Keyed only off
// NS12::BUTTON_COUNT/kButtonAddrs/kMcpOutputPins, so any button added to
// those arrays later gets latching LED behavior with no extra code here.
void toggleButtonStatusLed(uint8_t buttonIndex) {
  if (!mcpOk || buttonIndex >= NS12::BUTTON_COUNT) return;
  setButtonStatusLed(buttonIndex, !mcpOutputState[buttonIndex]);
}

uint32_t burstProbeUntilMs = 0;
constexpr uint32_t BURST_PROBE_DURATION_MS = 8000;

uint16_t noOffsetTestAddr = 0;
bool noOffsetTestPending = false;
uint32_t buttonPressCount[NS12::BUTTON_COUNT] = {};
#endif

//
//   ESP32 -> PLC: TO_PLC_COMM, a 3-bit byte. Bits 0-1 (McpPin::OPTO_1/
//   OPTO_2) carry PLC_STATUS, one of 4 mutually-exclusive states (STOP=0/
//   ALARM=1/WARNING=2/READY=3). Bit 2/MSB (Pins::ESP_OPTO_3) is currently
//   always 0 since PlcStatus has no state above READY=3 -- reserved
//   headroom, not yet assigned. Bit-to-pin assignment and the state codes
//   are both this file's choice, not yet confirmed against the S7 program
//   -- verify before relying on it, and update setStatus()/PlcStatus
//   together if either needs to change.
//
//   PLC -> ESP32: FROM_PLC_COMM, a 3-bit byte. Bit 0 (McpPin::INPUT_1) =
//   ACKNOWLEDGE, bit 1 (McpPin::INPUT_2) = MACHINE_RUNNING -- independent
//   flags, true/false in any combination, no encoding. Which physical
//   input is which is this file's placeholder pairing -- confirm against
//   the S7 program. Bit 2 (Pins::ESP_INPUT_3) is read and tracked
//   (plcControlBit2 below) but nothing acts on its value yet -- it used
//   to carry Keyence Result, which now wires directly to a PLC input
//   instead of through the ESP32.
//   Bits 0-1 are read only during Standby/TubeGap, same ~20ms-cadence
//   restriction as the rest of this file's MCP polling (an I2C
//   transaction every loop() iteration would cost InspectingTube latency
//   it can't afford) -- flag to the user if the S7 program needs to see
//   these change faster than that, e.g. mid-tube-pass. Bit 2 is a plain
//   ESP32 GPIO with no such I2C cost, but is read on the same cadence for
//   consistency; move it out of servicePlcControl() if it ever needs to
//   react faster than Standby/TubeGap allows.
// =====================================================================
namespace PlcComms {
enum class PlcStatus : uint8_t { STOP = 0, ALARM = 1, WARNING = 2, READY = 3 };

void setStatus(PlcStatus s) {
  uint8_t code = static_cast<uint8_t>(s);
  digitalWrite(Pins::ESP_OPTO_3, (code >> 2) & 1);

  if (!mcpOk) return;
  mcp.digitalWrite(McpPin::OPTO_1, (code >> 0) & 1);
  mcp.digitalWrite(McpPin::OPTO_2, (code >> 1) & 1);
}
} // namespace PlcComms

bool plcAcknowledge = false;
bool plcMachineRunning = false;
bool plcControlBit2 = false; // FROM_PLC_COMM bit 2 -- tracked/logged only, no behavior wired to it yet
PlcComms::PlcStatus plcLastCommandedStatus = PlcComms::PlcStatus::STOP;

void serviceHmiInputPolling() {
#if NS12_ENABLE_RM_POLLING
  uint32_t now = millis();

  if (wordVerifyIndex >= 0) {
    if (!ns12.isReadPending()) {
      ns12.requestRM((uint16_t)(30 + wordVerifyIndex), 1);
    }
  } else if (!ns12.isReadPending() && now - lastHmiPollMs >= NS12::RM_POLL_INTERVAL_MS) {
    lastHmiPollMs = now;
    static const uint16_t kHmiPollAddrs[3] = {
        NS12::HOTMELT_START_POSITION_ADDR, NS12::HOTMELT_END_POSITION_ADDR,
        NS12::CURRENT_SCREEN_ADDR};
    ns12.requestRM(kHmiPollAddrs[nextHmiPollIndex], 1);
    nextHmiPollIndex = (nextHmiPollIndex + 1) % 3;
  }

  uint16_t addr, value;

  if (ns12.consumeReadWord(addr, value)) {
    if (wordVerifyIndex >= 0 && addr == (uint16_t)(30 + wordVerifyIndex)) {
      static const char *const kVerifyNames[5] = {"SETUP", "ALARM LOG", "TREND FULL", "TEST",
                                                    "DIAG"};
      Serial.printf("[WORD-VERIFY] RM $W%u (%s) = 0x%04X\n", (unsigned)(30 + wordVerifyIndex),
                    kVerifyNames[wordVerifyIndex], value);
      wordVerifyIndex++;

      if (wordVerifyIndex >= 5) {
        wordVerifyIndex = -1;
        Serial.println(F("[WORD-VERIFY] Done -- compare these 5 values against the "
                          "[HMI-RAW] lines for the same buttons."));
      }
    } else if (addr == NS12::HOTMELT_START_POSITION_ADDR) {
      hotMeltStartPositionMm = (float)value * HMI_POSITION_MM_PER_COUNT;
      hotMeltPositionsFromHmi = true;
    } else if (addr == NS12::HOTMELT_END_POSITION_ADDR) {
      hotMeltEndPositionMm = (float)value * HMI_POSITION_MM_PER_COUNT;
      hotMeltPositionsFromHmi = true;
    } else if (addr == NS12::CURRENT_SCREEN_ADDR) {
      currentScreenNumber = value;
      currentScreenNumberValid = true;
    }
  }
#endif
}

// =====================================================================
// Telemetry helpers -- feed the NS12 $W100-$W108 block every loop (the
// manager only actually transmits it every NS12::TELEMETRY_WRITE_INTERVAL_MS).
// =====================================================================
uint16_t toUnsignedX10(float value) {
  if (!isfinite(value) || value <= 0.0f) return 0;
  if (value >= 6553.5f) return 65535;
  return (uint16_t)(value * 10.0f + 0.5f);
}

uint16_t buildStatusWord() {
  uint16_t status = 0;

  if (mlxDetected) status |= (1u << 0);

  if (mlxInitialized) status |= (1u << 1);

  if (lastFrameValid) status |= (1u << 2);

  if (mcpOk) status |= (1u << 3);

  if (state == SystemState::FaultStop) status |= (1u << 15);
  return status;
}

// =====================================================================
// Periodic Serial diagnostic report.
// =====================================================================
void printDiagnostics() {
  Serial.println();
  Serial.println(F("---- DIAGNOSTICS ----"));
  Serial.printf("State              : %s\n", stateName(state));

  if (!mcpOk) Serial.println(F("MCP initialized    : NO"));

  if (!mlxDetected) Serial.println(F("Camera detected    : NO"));

  if (!mlxInitialized) Serial.println(F("Camera initialized : NO"));

  if (!lastFrameValid) Serial.println(F("Last frame         : FAILED"));
  Serial.printf("Measured FPS       : %.2f\n", measuredFramesPerSecond);

  if (rawBaselineCaptureInProgress) {
    Serial.printf("Raw baseline       : capturing now... (%u/%u frames, previous baseline %s)\n",
                  rawBaselineFramesCollected, RAW_BASELINE_FRAME_COUNT,
                  rawBaselineCaptured ? "still in use until this completes" : "none yet -- stats frozen");
  } else if (!rawBaselineCaptured) {
    Serial.println(F("Raw baseline       : NOT CAPTURED (send 'B')"));
  }
  Serial.printf("Min/Max/Avg raw delta : %.0f / %.0f / %.0f\n",
                minimumTemperatureC, maximumTemperatureC, averageTemperatureC);

  if (lastFrameRejectedPixelCount != 0) {
    Serial.printf("Implausible pixels : %u (outside %.0f..%.0f raw delta, rejected)\n",
                  lastFrameRejectedPixelCount, MIN_PLAUSIBLE_RAW_DELTA, MAX_PLAUSIBLE_RAW_DELTA);
  }
  Serial.printf("Good/Failed frames : %lu / %lu\n",
                (unsigned long)successfulFrameCount, (unsigned long)failedFrameCount);
  Serial.printf("Encoder count (raw/mm) : %lld / %.1f\n", (long long)encoder.total(),
                (float)encoder.total() / ENCODER_COUNTS_PER_MM);

  if (capture.currentState() != CaptureState::ARMED) {
    const char *captureStateStr = (capture.currentState() == CaptureState::SAMPLING)
                                       ? "SAMPLING"
                                       : "LATCHED";
    Serial.printf("Capture state      : %s\n", captureStateStr);
  }
  Serial.printf("Strip1 present/maxT/hotPx : %d / %.1f / %u\n",
                capture.strip1().present, capture.strip1().maxTempC,
                capture.strip1().hotPixelCount);
  Serial.printf("Strip2 present/maxT/hotPx : %d / %.1f / %u\n",
                capture.strip2().present, capture.strip2().maxTempC,
                capture.strip2().hotPixelCount);

  Serial.printf("NS12 WM attempts/failures/oversized : %lu / %lu / %lu\n",
                (unsigned long)ns12.wmAttemptCount(), (unsigned long)ns12.wmFailureCount(),
                (unsigned long)ns12.wmOversizedCountValue());
#if NS12_ENABLE_RM_POLLING
  Serial.printf("NS12 RM attempts/success/writeFail/timeout/parseErr : %lu / %lu / %lu / %lu / %lu\n",
                (unsigned long)ns12.rmAttemptCount(), (unsigned long)ns12.rmSuccessCount(),
                (unsigned long)ns12.rmWriteFailureCount(), (unsigned long)ns12.rmTimeoutCount(),
                (unsigned long)ns12.rmParseErrorCount());
  Serial.printf("NS12 RB attempts/success/writeFail/timeout/parseErr : %lu / %lu / %lu / %lu / %lu\n",
                (unsigned long)ns12.rbAttemptCount(), (unsigned long)ns12.rbSuccessCount(),
                (unsigned long)ns12.rbWriteFailureCount(), (unsigned long)ns12.rbTimeoutCount(),
                (unsigned long)ns12.rbParseErrorCount());
  Serial.printf("NS12 WB attempts/failures : %lu / %lu\n", (unsigned long)ns12.wbAttemptCount(),
                (unsigned long)ns12.wbFailureCount());
  Serial.printf("NS12 SB notify count/rejected : %lu / %lu\n",
                (unsigned long)ns12.sbNotifyCount(), (unsigned long)ns12.sbNotifyRejectedCount());
#else
  Serial.println(F("NS12 RM polling    : disabled (PT doesn't respond -- see NS12 namespace comment)"));
#endif
  Serial.printf("PLC_STATUS (commanded) : %u (0=STOP/1=ALARM/2=WARNING/3=READY)\n",
                (unsigned)plcLastCommandedStatus);
  Serial.printf("PLC_CONTROL ACKNOWLEDGE/MACHINE_RUNNING/bit2 : %d / %d / %d\n", plcAcknowledge,
                plcMachineRunning, plcControlBit2);
  Serial.printf("HotMelt Start/End position (mm) : %.1f / %.1f (%s)\n",
                hotMeltStartPositionMm, hotMeltEndPositionMm,
                hotMeltPositionsFromHmi ? "from HMI" : "PLACEHOLDER fallback, not from HMI yet");

  if (currentScreenNumberValid) {
    Serial.printf("Current HMI screen : %u\n", (unsigned)currentScreenNumber);
  } else {
    Serial.println(F("Current HMI screen : not yet read ($W50)"));
  }

  if (tubeLengthLearned) {
    Serial.printf("Tube length          : %.1f mm (from previous tube)\n", learnedTubeLengthMm);
  } else {
    Serial.println(F("Tube length          : not yet learned (no tube has cleared the sensor yet)"));
  }
  Serial.printf("Free heap          : %.1f kB\n", ESP.getFreeHeap() / 1024.0f);
}

#if NS12_ENABLE_RM_POLLING

void handleHmiButtonPress(uint8_t index) {
  buttonPressCount[index]++;
  Serial.printf("[HMI] %s button pressed.\n", kButtonNames[index]);
  switch (index) {
  case 3: // TEST -- same one-shot pattern as the 'M' serial command
    pushTestPattern();
    break;
  case 4: // DIAG -- same immediate report as the 'D' serial command
    printDiagnostics();
    break;
  // SETUP / ALARM LOG / TREND FULL: no subsystem exists yet for these --
  // no setup-parameter screen, no alarm log, no trend recording. Real
  // behavior needs a spec -- what SETUP should configure, where the alarm
  // log lives, what TREND FULL should show. Each has a dedicated status
  // LED instead (set in serviceHmiButtonPolling() -- see
  // setButtonStatusLed()'s comment) as its physical confirmation.
  default:
    break;
  }
}

// buttonState[i] tracks the raw momentary "button is pressed" bit purely
// for rising-edge detection -- the LED itself is latched by
// toggleButtonStatusLed(), not mirrored from this bit, since a momentary
// press would otherwise light the LED only while the screen is held down.
void applyButtonBitUpdate(uint8_t i, bool pressed) {
  bool wasPressed = buttonState[i];
  buttonState[i] = pressed;

  if (pressed && !wasPressed) {
    toggleButtonStatusLed(i);
    Serial.printf("[SWITCH] %s -> %s\n", kButtonNames[i], mcpOutputState[i] ? "ON" : "OFF");

    for (uint8_t j = 0; j < NS12::BUTTON_COUNT; j++) {
      if (j == i || !mcpOutputState[j]) continue;
      bool offBit = false;
      ns12.sendWB(kButtonAddrs[j], &offBit, 1);
      setButtonStatusLed(j, false);
      Serial.printf("[SWITCH] %s -> OFF (reset by %s)\n", kButtonNames[j], kButtonNames[i]);
    }
    handleHmiButtonPress(i);
  }
}

bool addrToDiagIndex(uint16_t addr, uint8_t &indexOut) {
  if (addr < NS12::DIAG_BUTTON_BASE_ADDR) return false;
  uint16_t idx = addr - NS12::DIAG_BUTTON_BASE_ADDR;

  if (idx >= NS12::DIAG_BUTTON_COUNT) return false;
  indexOut = (uint8_t)idx;
  return true;
}

// Forward declaration: handleSerialCommand() is defined further down this
// file (it also handles literal keystrokes from the USB serial monitor),
// but applyDiagButtonUpdate() -- called from serviceHmiButtonPolling(),
// which comes first -- needs to call it now that HMI buttons can fire the
// exact same commands. Reusing it directly means every one of the 28
// diagnostic actions has exactly one implementation, not two copies to
// keep in sync.
void handleSerialCommand(char c);

void applyDiagButtonUpdate(uint8_t i, bool pressed) {
  bool wasPressed = diagButtonState[i];
  diagButtonState[i] = pressed;

  if (pressed && !wasPressed) {
    diagButtonPressCount++;
    Serial.printf("[HMI-DIAG] %s ($B%u) -> '%c'\n", kDiagButtonNames[i],
                  (unsigned)(NS12::DIAG_BUTTON_BASE_ADDR + i), kDiagCommandChars[i]);
    handleSerialCommand(kDiagCommandChars[i]);
  }
}
#endif

void serviceHmiButtonPolling() {
#if NS12_ENABLE_RM_POLLING
  uint32_t now = millis();

  uint16_t notifyAddr;
  bool notifyPressed;

  if (ns12.consumeNotifyBit(notifyAddr, notifyPressed)) {
    bool matched = false;

    for (uint8_t i = 0; i < NS12::BUTTON_COUNT; i++) {
      if (kButtonAddrs[i] != notifyAddr) continue;
      applyButtonBitUpdate(i, notifyPressed);
      matched = true;
      break;
    }

    if (!matched) {
      uint8_t diagIndex;

      if (addrToDiagIndex(notifyAddr, diagIndex)) {
        applyDiagButtonUpdate(diagIndex, notifyPressed);
      }
    }
  }

  if (noOffsetTestPending) {
    if (!ns12.isReadPending() && ns12.requestRB(noOffsetTestAddr, 1)) {
      noOffsetTestPending = false;
    }
  } else {
    uint32_t pollInterval = (now < burstProbeUntilMs) ? 0 : NS12::BUTTON_POLL_INTERVAL_MS;

    if (!ns12.isReadPending() && now - lastButtonPollMs >= pollInterval) {
      lastButtonPollMs = now;
      ns12.requestRB(kButtonAddrs[nextButtonPollIndex], 1);
      nextButtonPollIndex = (nextButtonPollIndex + 1) % NS12::BUTTON_COUNT;
    }
  }

  if (!noOffsetTestPending && !ns12.isReadPending() &&
      now - lastDiagButtonPollMs >= NS12::DIAG_BUTTON_POLL_INTERVAL_MS) {
    lastDiagButtonPollMs = now;
    ns12.requestRB(NS12::DIAG_BUTTON_BASE_ADDR + nextDiagButtonPollIndex, 1);
    nextDiagButtonPollIndex = (nextDiagButtonPollIndex + 1) % NS12::DIAG_BUTTON_COUNT;
  }

  uint16_t addr;
  bool pressed;
  uint16_t rawValue;

  if (ns12.consumeReadBit(addr, pressed, rawValue)) {
    bool matched = false;

    for (uint8_t i = 0; i < NS12::BUTTON_COUNT; i++) {
      if (kButtonAddrs[i] != addr) continue;

#if NS12_DEBUG_RAW_RX
      Serial.printf("[HMI-RAW] %-11s $B%-3u raw=0x%02X bit7=%u (bit0=%u)\n", kButtonNames[i],
                    kButtonAddrs[i], rawValue, (unsigned)((rawValue & 0x80) != 0),
                    (unsigned)(rawValue & 1));
#endif
      applyButtonBitUpdate(i, pressed);
      matched = true;
      break;
    }

    if (!matched) {
      uint8_t diagIndex;

      if (addrToDiagIndex(addr, diagIndex)) {
        applyDiagButtonUpdate(diagIndex, pressed);
      }
    }
  }
#endif
}

// =====================================================================
// PlcComms input side -- reads the S7-315-2's 2 control flags
// (ACKNOWLEDGE/MACHINE_RUNNING) off the MCP23017. Folded into the same
// Standby/TubeGap-only, ~20ms-cadence MCP poll window loop() already uses
// (see that block's own comment) rather than a separate timer, since an
// extra independent I2C poll interval would just double the I2C traffic
// for no benefit.
// =====================================================================
// Debounced against relay chatter/line noise and the PLC's own outputs not
// switching atomically: a bit is only accepted once it reads the same on
// two consecutive ~20ms polls, so a one-poll glitch never reaches
// plcAcknowledge/plcMachineRunning/plcControlBit2 or their log lines.
void servicePlcControl() {
  if (!mcpOk || !(state == SystemState::Standby || state == SystemState::TubeGap)) return;
  static bool ackPrevRaw = false, runningPrevRaw = false, bit2PrevRaw = false;
  bool ack = (mcp.digitalRead(McpPin::INPUT_1) == LOW); // INPUT_PULLUP: idle HIGH
  bool running = (mcp.digitalRead(McpPin::INPUT_2) == LOW);
  bool bit2 = (digitalRead(Pins::ESP_INPUT_3) == HIGH); // FROM_PLC_COMM bit 2, no action wired yet

  if (ack == ackPrevRaw && ack != plcAcknowledge) {
    plcAcknowledge = ack;
    Serial.printf("[PLC] ACKNOWLEDGE -> %s\n", ack ? "ACTIVE" : "idle");
  }
  ackPrevRaw = ack;

  if (running == runningPrevRaw && running != plcMachineRunning) {
    plcMachineRunning = running;
    Serial.printf("[PLC] MACHINE_RUNNING -> %s\n", running ? "ACTIVE" : "idle");
  }
  runningPrevRaw = running;

  if (bit2 == bit2PrevRaw && bit2 != plcControlBit2) {
    plcControlBit2 = bit2;
    Serial.printf("[PLC] FROM_PLC_COMM bit 2 -> %s\n", bit2 ? "ACTIVE" : "idle");
  }
  bit2PrevRaw = bit2;
}

//
// Separately, the HMI's own SETUP/ALARM LOG/TREND FULL/TEST/DIAG push-
// buttons ($B30-$B34) are polled over NS12 RB (see serviceHmiButtonPolling()
// below) and dispatch through handleHmiButtonPress() -- TEST and DIAG there
// call the same pushTestPattern()/printDiagnostics() as 'M' and 'D' here.
// PLC_CONTROL/PLC_STATUS (servicePlcControl(), above) are unrelated to any
// of this -- the S7-315-2 PLC link, not the HMI.
// =====================================================================
// Boot-time visual/functional check of every MCP-driven status LED, one
// at a time -- confirms all 7 are wired and addressed correctly without
// waiting for a real button press or fault condition.
void scanMcpStatusLeds() {
  if (!mcpOk) return;
  static const uint8_t ledPins[] = {McpPin::ILED_R, McpPin::ILED_G, McpPin::ILED_B,
                                     McpPin::ELED_R, McpPin::ELED_G, McpPin::ELED_B,
                                     McpPin::ELED_Y};
  static const char *const ledNames[] = {"ILED_R", "ILED_G", "ILED_B",
                                          "ELED_R", "ELED_G", "ELED_B", "ELED_Y"};

  for (size_t i = 0; i < sizeof(ledPins) / sizeof(ledPins[0]); ++i) {
    Serial.printf("[SETUP] %s -> ON\n", ledNames[i]);
    mcp.digitalWrite(ledPins[i], HIGH);
    delay(250);
    mcp.digitalWrite(ledPins[i], LOW);
    delay(150);
  }
  Serial.println(F("[SETUP] MCP LED sweep complete."));
}

void handleSerialCommand(char c) {
  switch (c) {
  case 'S': state = SystemState::Standby; break;
  case 'W': state = SystemState::WaitingForTube; break;
  case 'I': state = SystemState::InspectingTube; break;
  case 'G': state = SystemState::TubeGap; break;
  case 'F': enterFaultStop(); break;
  case '1': case '2': case '3': case '4': case '5':
  case '6': case '7': case '8': case '9': {
    uint8_t idx = c - '1';

    if (mcpOk) {
      toggleMcpOutput(idx);
      Serial.printf("[IO-TEST] MCP output #%c (pin %u) -> %s\n", c, kMcpOutputPins[idx],
                    mcpOutputState[idx] ? "HIGH" : "LOW");
    }
    break;
  }
  case 'A': {
    bool newState = !digitalRead(Pins::ESP_OPTO_3);
    digitalWrite(Pins::ESP_OPTO_3, newState);
    Serial.printf("[IO-TEST] ESP_OPTO_3 (GPIO%u) -> %s\n", Pins::ESP_OPTO_3,
                  newState ? "HIGH" : "LOW");
    break;
  }
  case 'K':
    keyenceTrigger.fire();
    Serial.println(F("[IO-TEST] Keyence trigger pulse fired (GPIO1)."));
    break;
  case 'P': {
    static const char *kStatusNames[4] = {"STOP", "ALARM", "WARNING", "READY"};
    uint8_t nextCode = (static_cast<uint8_t>(plcLastCommandedStatus) + 1) % 4;
    plcLastCommandedStatus = static_cast<PlcComms::PlcStatus>(nextCode);
    PlcComms::setStatus(plcLastCommandedStatus);
    Serial.printf("[PLC] PLC_STATUS -> %u (%s)\n", nextCode, kStatusNames[nextCode]);
    break;
  }
  case 'M':
    pushTestPattern();
    break;
  case 'C':
    capture.rearm();
    Serial.println(F("[DIAG] Capture forced/rearmed."));
    break;
  case 'B':

    if (!rawBaselineCaptureInProgress) {
      startRawBaselineCapture();
    } else {
      Serial.println(F("[DIAG] Baseline capture already in progress."));
    }
    break;
  case 'X': {
    Serial.println(F("[DIAG] Frame dump: raw-delta (live pipeline) vs calibrated C (one-off"));
    Serial.println(F("       mlx.getFrame() snapshot, for correlating real thresholds):"));

    if (!rawBaselineCaptured) {
      Serial.println(F("[DIAG] WARNING: no baseline captured yet ('B') -- raw-delta values"));
      Serial.println(F("       below are meaningless (baseline defaults to 0)."));
    }
    static float calibratedSnapshot[StripZone::COLS * StripZone::ROWS];
    bool calibratedOk = mlxInitialized && (mlx.getFrame(calibratedSnapshot) == 0);

    for (size_t i = 0; i < StripZone::COLS * StripZone::ROWS; i++) {
      Serial.print(mlxFrame[i], 0);
      Serial.print('/');

      if (calibratedOk) {
        Serial.print(calibratedSnapshot[i], 1);
      } else {
        Serial.print(F("?"));
      }
      Serial.print(i % StripZone::COLS == StripZone::COLS - 1 ? '\n' : ' ');
    }
    break;
  }
  case 'R': {
    capture.rearm();
    float blankFrame[StripZone::COLS * StripZone::ROWS];

    for (size_t i = 0; i < StripZone::COLS * StripZone::ROWS; i++) {
      blankFrame[i] = MATRIX_RAW_DELTA_MIN;
    }
    pushWordLampMatrix(blankFrame);
    Serial.println(F("[DIAG] Rearmed, display cleared."));
    break;
  }
  case 'D':
    printDiagnostics();
    break;
  case 'L':
    continuousMlxTestMode = !continuousMlxTestMode;
    Serial.printf("[MLX-LIVE] continuous reading %s%s\n",
                  continuousMlxTestMode ? "ON" : "OFF",
                  (continuousMlxTestMode && !rawBaselineCaptured)
                      ? " (WARNING: no baseline yet -- send 'B' first)"
                      : "");
    break;
  case 'H':
    burstProbeUntilMs = millis() + BURST_PROBE_DURATION_MS;
    Serial.printf("[HMI-PROBE] Burst mode ON for %lu ms -- hold any HMI button NOW, watch for "
                  "[NS12-FRAME]/[HMI-RAW] lines on its address.\n",
                  (unsigned long)BURST_PROBE_DURATION_MS);
    break;
#if NS12_ENABLE_RM_POLLING
  case 'V':
    wordVerifyIndex = 0;
    Serial.println(F("[WORD-VERIFY] Reading $W30..$W34 via RM (confirmed-correct word path) -- "
                      "compare against the RB [HMI-RAW] values for $B30..$B34."));
    break;
#endif
  case 'J': {
    bool oneBit = true;
    ns12.sendWB(NS12::BUTTON_SETUP_ADDR, &oneBit, 1);
    burstProbeUntilMs = millis() + 4000;
    Serial.println(F("[WB-RB-TEST] Wrote 1 to $B30 (SETUP) ourselves, no touchscreen involved. "
                      "Watching for bit7=1 on the next [HMI-RAW] SETUP line..."));
    break;
  }
#if NS12_ENABLE_RM_POLLING
  case 'N': {
    bool oneBit = true;
    ns12.sendWB(NS12::BUTTON_SETUP_ADDR, &oneBit, 1);
    noOffsetTestAddr = NS12::BUTTON_SETUP_ADDR;
    noOffsetTestPending = true;
    Serial.println(F("[WB-RB-SANITY] Wrote 1 to $B30, reading it back via RB -- watch the next "
                      "[NS12-FRAME] line for dataText=\"80\"."));
    break;
  }
#endif
  default:
    break;
  }
}

// =====================================================================
// setup() / loop()
// =====================================================================
void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitStart = millis();

  while (!Serial && (millis() - serialWaitStart < 3000UL)) {
    delay(10);
  }

  statusLed.begin();
  statusLed.clear();
  statusLed.show();
  setStatusLed(0, 0, 20); // dim blue during startup

  Serial.printf("TGIS-510 %s (%s) booting...\n", FW_VERSION, FW_FILE);

  Wire.begin(Pins::I2C_SDA, Pins::I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  Wire.setTimeOut(1000);

  printBoardInformation();
  runI2CScanner();

  mcpOk = mcp.begin_I2C(MCP_I2C_ADDR, &Wire);
  Serial.printf("MCP initialized: %s\n", mcpOk ? "YES" : "NO");

  if (mcpOk) {
    for (uint8_t p : {McpPin::ILED_R, McpPin::ILED_G, McpPin::ILED_B, McpPin::ELED_R,
                       McpPin::ELED_G, McpPin::ELED_B, McpPin::ELED_Y, McpPin::OPTO_1,
                       McpPin::OPTO_2}) {
      mcp.pinMode(p, OUTPUT);
      mcp.digitalWrite(p, LOW);
    }

    for (uint8_t p : {McpPin::INPUT_1, McpPin::INPUT_2}) {
      mcp.pinMode(p, INPUT_PULLUP);
    }
    scanMcpStatusLeds();
  }

  mlxDetected = isI2CAddressPresent(MLX90640_I2CADDR_DEFAULT);
  bool mlxOk = mlxDetected && initializeMlx();
  mlxInitialized = mlxOk;
  Serial.printf("MLX90640 initialized: %s\n", mlxOk ? "YES" : "NO");
  setStatusLed(mlxOk ? 0 : 30, mlxOk ? 25 : 0, 0);

  ns12.begin();

  // PULLDOWN: gives a defined idle LOW consistent with handlePresenceEdge()'s
  // rising-edge = "tube entering" assumption. This pin carries a real 24V
  // signal through a 10K/1.5K divider -- see ENCODER_PULSE_PIN's begin()
  // comment for why that makes the internal pull practically a no-op.
  pinMode(Pins::PRESENCE_SENSOR_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(Pins::PRESENCE_SENSOR_PIN), presenceIsr, CHANGE);

  keyenceTrigger.begin(Pins::KEYENCE_TRIGGER_PIN);
  encoder.begin(Pins::ENCODER_PULSE_PIN);

  pinMode(Pins::ESP_OPTO_3, OUTPUT);
  digitalWrite(Pins::ESP_OPTO_3, LOW);

  pinMode(Pins::ESP_INPUT_3, INPUT_PULLDOWN); // FROM_PLC_COMM bit 2, polled in servicePlcControl()

  fpsWindowStartMs = millis();
  lastDiagnosticMs = millis();

  if (mcpOk && mlxOk) {
    state = SystemState::Standby;
  } else {
    enterFaultStop();
  }

  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true); // true = panic/reset on timeout
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();

  encoder.service();
  keyenceTrigger.service();

  if (presenceEdgePending) {
    handlePresenceEdge();
  }


  serviceTubePositionTracking();
  serviceHmiInputPolling();
  serviceHmiButtonPolling();

  if ((state == SystemState::Standby || state == SystemState::TubeGap) && mcpOk) {
    uint32_t now = millis();

    if (now - lastMcpPollMs >= MCP_POLL_INTERVAL_MS) {
      lastMcpPollMs = now;
      servicePlcControl();

      if (state == SystemState::TubeGap) {
        state = SystemState::WaitingForTube;
        capture.rearm();
      }
    }
  }

  // PLC_STATUS output: STOP whenever FaultStop, READY otherwise. ALARM and
  // WARNING have no trigger condition defined yet -- this project has no
  // existing concept of a "warning, but not a fault" state to map onto
  // them; use 'P' to drive them manually for bench/PLC-program testing
  // until a real condition is decided.
  {
    PlcComms::PlcStatus wantStatus =
        (state == SystemState::FaultStop) ? PlcComms::PlcStatus::STOP : PlcComms::PlcStatus::READY;

    if (wantStatus != plcLastCommandedStatus) {
      plcLastCommandedStatus = wantStatus;
      PlcComms::setStatus(wantStatus);
    }
  }

  if (mlxInitialized) {
    uint32_t nowMs = millis();

    if (nowMs - lastMlxFrameMs >= MLX_FRAME_PERIOD_MS) {
      lastMlxFrameMs = nowMs;
      static float rawPixelsNow[32 * 24];
      bool rawOk = readMlxRawCombined(rawPixelsNow);
      lastFrameValid = rawOk;

      if (rawOk) {
        successfulFrameCount++;
        fpsWindowFrameCount++;
        consecutiveFrameFailures = 0;
        updateFrameRate();
        setStatusLed(0, 18, 0); // brief green heartbeat

        if (rawBaselineCaptureInProgress) {
          serviceRawBaselineCapture(rawPixelsNow);
        } else if (rawBaselineCaptured) {
          subtractBaseline(rawPixelsNow, mlxFrame);
          calculateFrameStatistics();
          capture.onNewFrame(mlxFrame);

          if (continuousMlxTestMode &&
              nowMs - lastContinuousMlxPrintMs >= CONTINUOUS_MLX_PRINT_INTERVAL_MS) {
            lastContinuousMlxPrintMs = nowMs;
            Serial.printf("[MLX-LIVE] min/max/avg raw delta=%.0f/%.0f/%.0f rejected=%u\n",
                          minimumTemperatureC, maximumTemperatureC, averageTemperatureC,
                          lastFrameRejectedPixelCount);
          }
        }
        // else: no baseline yet and none in progress -- raw reads succeed
        // (fps/heartbeat/recovery logic all still work) but nothing feeds
        // the QC/HMI pipeline until 'B' is run once. See the 'B'/'X'
        // serial commands.
      } else {
        failedFrameCount++;
        consecutiveFrameFailures++;
        setStatusLed(25, 8, 0);

        if (consecutiveFrameFailures >= FRAME_FAILURE_RECOVERY_COUNT) {
          attemptCameraRecovery();
        }
      }
    }
  } else {
    // Retry camera detection every second without locking the CPU.
    static uint32_t lastRetryMs = 0;

    if (millis() - lastRetryMs >= 1000UL) {
      lastRetryMs = millis();
      mlxDetected = isI2CAddressPresent(MLX90640_I2CADDR_DEFAULT);

      if (mlxDetected) {
        mlxInitialized = initializeMlx();

        if (mlxInitialized) {
          consecutiveFrameFailures = 0;
          fpsWindowStartMs = millis();
          fpsWindowFrameCount = 0;
          setStatusLed(0, 25, 0);
          Serial.println(F("MLX90640 recovered and initialized."));
        }
      }
    }
  }

  ns12.setTelemetry((uint16_t)(++heartbeatCounter),
                     toUnsignedX10(measuredFramesPerSecond),
                     toUnsignedX10(minimumTemperatureC),
                     toUnsignedX10(maximumTemperatureC),
                     toUnsignedX10(averageTemperatureC),
                     (uint16_t)successfulFrameCount,
                     (uint16_t)failedFrameCount,
                     (uint16_t)state,
                     buildStatusWord());
  ns12.service();

  serviceDisplayThrottle();
  serviceMatrixPacing();
  checkDisplayAutoFallback();

  if (Serial.available()) {
    handleSerialCommand((char)Serial.read());
  }

  if (state != lastLoggedState) {
    Serial.printf("[STATE] %s -> %s\n", stateName(lastLoggedState), stateName(state));
    lastLoggedState = state;
  }

  if (millis() - lastDiagnosticMs >= DIAGNOSTIC_INTERVAL_MS) {
    lastDiagnosticMs = millis();
    printDiagnostics();
  }
}
