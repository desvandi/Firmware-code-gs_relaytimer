// =============================================================================
// BatteryConfig.h — Centralized configuration for DC energy & battery monitoring
// Timer Digital Relay v4.1 — Advanced DC Energy & Battery Monitoring
// -----------------------------------------------------------------------------
// All battery/DC calibration constants live here. Do NOT scatter calibration
// numbers through source files. (Engineering brief §8, §49.)
//
// Sign conventions (brief §5, §6, §21):
//   Ibattery > 0  → battery DISCHARGING  (Battery → Inverter/system)
//   Ibattery < 0  → battery CHARGING     (MPPT → Battery)
//   Iinverter > 0 → inverter consuming DC
//   Imppt = Iinverter - Ibattery         (derived — brief §6)
//
// Power sign follows current sign:
//   Pbattery  = Vbattery × Ibattery     (>0 discharge, <0 charge)
//   Pinverter = Vbattery × Iinverter    (≥0)
//   Pmppt     = Vbattery × Imppt        (≥0 when MPPT sourcing)
// =============================================================================
#pragma once
#ifndef TIMER12_BATTERY_CONFIG_H
#define TIMER12_BATTERY_CONFIG_H

#include <cstdint>

// v4.3.11: Preprocessor-level enable/disable for battery monitoring.
// When BATTERY_MONITORING_ENABLED is NOT defined, all battery .cpp files
// compile to nothing (saves ~200KB flash). Relay/scheduler/PIR/PZEM/
// MQTT/REST/OTA remain fully operational.
// To ENABLE: add -DBATTERY_MONITORING_ENABLED to build_flags (PlatformIO)
//   or uncomment the #define below (Arduino IDE).
// To DISABLE: leave commented (saves flash for relay-only installations).
// #define BATTERY_MONITORING_ENABLED

#ifdef BATTERY_MONITORING_ENABLED
  #define BATTERY_ENABLED 1
#else
  #define BATTERY_ENABLED 0
#endif

namespace Battery {

// ---------- FIRMWARE FEATURE FLAG ----------
// Master switch for the entire DC energy / battery monitoring subsystem.
// When false (BATTERY_MONITORING_ENABLED not defined), all battery drivers
// return unavailable and the SystemStatus payload omits the battery/powerFlow/
// environment blocks. Relay/scheduler/PIR/PZEM/MQTT/REST/OTA remain fully
// operational. (Brief §46, §47.)
constexpr bool ENABLED = (BATTERY_ENABLED == 1);

// ---------- BATTERY CHEMISTRY / TOPOLOGY ----------
// 8S LiFePO4, nominal 24 V. (Brief §3.)
constexpr uint8_t  NUM_CELLS               = 8;
constexpr float    CELL_NOMINAL_V          = 3.30f;   // LiFePO4 resting nominal
constexpr float    CELL_MIN_SAFE_V         = 2.50f;   // hard floor (BMS-protected)
constexpr float    CELL_MAX_SAFE_V         = 3.65f;   // 100% SoC target per cell
constexpr float    PACK_NOMINAL_V          = CELL_NOMINAL_V * NUM_CELLS;     // 26.4
constexpr float    PACK_MAX_CHARGE_V       = CELL_MAX_SAFE_V * NUM_CELLS;    // 29.2
constexpr float    PACK_MIN_DISCHARGE_V    = CELL_MIN_SAFE_V * NUM_CELLS;    // 20.0

// ---------- CELL VALIDATION THRESHOLDS (Brief §17, §18) ----------
// A calculated cell voltage is valid only if 0 < v < 4.0 V. These are soft
// thresholds for diagnostics; reject impossible values outright.
constexpr float    CELL_VALID_MIN_V        = 0.10f;   // <0.10 → invalid (sensor/I2C fault)
constexpr float    CELL_VALID_MAX_V        = 4.00f;   // >4.00 → invalid (single LiFePO4 cell cannot exceed)
constexpr float    CELL_UNDERVOLTAGE_WARN  = 2.80f;   // WARNING: cell < 2.80 V
constexpr float    CELL_OVERVOLTAGE_WARN   = 3.55f;   // WARNING: cell > 3.55 V
constexpr float    CELL_IMBALANCE_WARN     = 0.080f;  // cellDelta > 80 mV → WARNING
constexpr float    CELL_IMBALANCE_FAULT    = 0.200f;  // cellDelta > 200 mV → FAULT
// Cumulative node ordering tolerance: C[n] must be >= C[n-1] - tolerance.
// Negative deltas are normal because of ADC noise; we only flag gross errors.
constexpr float    NODE_ORDER_TOLERANCE_V  = 0.150f;  // 150 mV tolerance for C[n]<C[n-1]

// ---------- SHUNTS (Brief §8) ----------
// Both INA219 modules use external shunts. The onboard R100 is removed.
constexpr float    BATTERY_SHUNT_OHMS     = 0.00075f;   // 0.75 mΩ (75 mV @ 100 A)
constexpr float    INVERTER_SHUNT_OHMS    = 0.00075f;
constexpr float    SHUNT_BUR_V            = 0.075f;     // 75 mV full-scale
constexpr float    SHUNT_MAX_CURRENT_A    = 100.0f;     // 100 A

// Polarity correction (Brief §7): apply in software so that positive current
// matches the semantic contract (Battery→Inverter discharge for INA219 #1).
// Set to -1.0 if the physical shunt orientation is reversed.
constexpr float    BATTERY_CURRENT_SIGN   = -1.0f;
constexpr float    INVERTER_CURRENT_SIGN  = +1.0f;

// ---------- I2C ADDRESSES (Brief §15) ----------
// Verify no collision with DS3231 (0x68) on the shared bus.
constexpr uint8_t  INA219_BATTERY_ADDR    = 0x40;
constexpr uint8_t  INA219_INVERTER_ADDR   = 0x41;
constexpr uint8_t  ADS1115_CELL1_ADDR     = 0x48;   // ADS1115 #1: AIN0..AIN3 = C1..C4
constexpr uint8_t  ADS1115_CELL2_ADDR     = 0x49;   // ADS1115 #2: AIN0..AIN3 = C5,C6,C7,B+
constexpr uint8_t  SHT31_ADDR             = 0x44;

// ---------- ADS1115 CHANNEL MAPPING (Brief §16) ----------
// Explicit enum-style constants — no magic numbers.
namespace AdsChannel {
  constexpr uint8_t AIN0 = 0;
  constexpr uint8_t AIN1 = 1;
  constexpr uint8_t AIN2 = 2;
  constexpr uint8_t AIN3 = 3;
}
// ADS1115 #1 (0x48): AIN0=C1, AIN1=C2, AIN2=C3, AIN3=C4
// ADS1115 #2 (0x49): AIN0=C5, AIN1=C6, AIN2=C7, AIN3=B+
// (Brief §16: explicit channel mapping for cumulative cell-node measurement.)

// ---------- ADS1115 DIVIDER & CALIBRATION (Brief §13, §14) ----------
// Each cell-node input (C1..B+) passes through a resistor divider so that the
// max input (29.2 V at B+) maps to ≤ ADS1115 FSR with PGA gain 2/3 (±6.144 V).
// Suggested: R_TOP = 100 kΩ, R_BOTTOM = 10 kΩ → ratio 1/11.
//   At 29.2 V → 2.6545 V (within ±6.144 V FSR, plenty of headroom).
constexpr float    ADS_DIVIDER_RATIO      = 11.0f;    // Vpack / Vadc
constexpr float    ADS_PGA_GAIN           = 6.144f;   // ±6.144 V full scale (PGA gain 2/3)
// Calibration constants (two-point gain/offset). Replace with measured values
// during commissioning. (Brief §12: do NOT invent calibration constants.)
//   vCalibrated = vRaw * adsGain + adsOffset
constexpr float    ADS1_GAIN              = 1.0f;
constexpr float    ADS1_OFFSET            = 0.0f;
constexpr float    ADS2_GAIN              = 1.0f;
constexpr float    ADS2_OFFSET            = 0.0f;

// ---------- PACK VOLTAGE SOURCE (Brief §10, §51, §52 — deviation) ----------
// ESP32 ADC1 is fully occupied by I2C (32/33) + PIR (34/35/36/39). GPIO37/38
// are NOT broken out on standard WROOM-32 modules. The brief §10 prefers a
// dedicated ESP32 ADC pin but §51/§52 forbid moving PIR pins without owner
// approval. Therefore the default authoritative pack voltage source is the
// calibrated B+ reading from ADS1115 #2 AIN3 (already allocated per §16).
//
// Owners who redesign the PIR pinout can switch to ESP32_ADC1 by setting
// PACK_VOLTAGE_SOURCE = PACK_VOLTAGE_SOURCE_ESP32_ADC1 and assigning a valid
// ADC1 GPIO below.
enum class PackVoltageSource : uint8_t {
  ADS1115_AIN3_BPLUS = 0,   // default — uses ADS1115 #2's existing AIN3 = B+ node
  ESP32_ADC1         = 1,  // optional — requires owner to assign free ADC1 pin
};
constexpr PackVoltageSource PACK_VOLTAGE_SOURCE = PackVoltageSource::ADS1115_AIN3_BPLUS;

// ESP32 ADC1 settings — only used when PACK_VOLTAGE_SOURCE == ESP32_ADC1.
// Default 255 (invalid) → driver returns UNAVAILABLE. Owner must assign.
constexpr uint8_t   PACK_ADC_PIN            = 255;   // 255 = unassigned
constexpr float      PACK_ADC_DIVIDER_RATIO = 11.0f; // 150k/15k → /11
// Two-point calibration: Vpack = Vadc * gain + offset
constexpr float      PACK_ADC_GAIN          = 1.0f;
constexpr float      PACK_ADC_OFFSET        = 0.0f;
constexpr uint16_t   PACK_ADC_SAMPLES       = 8;       // oversample ×8 → ~13-bit
constexpr uint16_t   PACK_ADC_INTERVAL_MS   = 200;     // 5 Hz raw sample rate

// ---------- ESP32 ADC DIVIDER (Brief §10, §11) ----------
// Suggested hardware: R_TOP=150kΩ, R_BOTTOM=15kΩ, +1k series + 100nF to GND.
// At 29.2 V → Vadc ≈ 2.65 V (under 3.3 V domain with margin).

// ---------- SAMPLING RATES (Brief §44 — non-blocking) ----------
constexpr uint16_t   INA219_INTERVAL_MS     = 200;     // ~5 Hz per branch
constexpr uint16_t   ADS1115_INTERVAL_MS    = 100;     // ~10 Hz round-robin (8 ch)
constexpr uint16_t   SHT31_INTERVAL_MS      = 1000;    // 1 Hz
constexpr uint16_t   ENERGY_TICK_MS          = 1000;    // 1 Hz energy integration

// ---------- FILTERING (Brief §45) ----------
constexpr uint8_t    ADC_SMOOTH_SAMPLES      = 4;      // exponential smoothing window
constexpr float      ADC_SMOOTH_ALPHA        = 0.25f;  // EMA α=0.25 (slow smoothing)
constexpr float      CURRENT_SMOOTH_ALPHA    = 0.20f;  // EMA α=0.20 for currents

// ---------- ENERGY INTEGRATION (Brief §23, §24) ----------
// RAM accumulators only. Do NOT write to NVS on every sample. Persist at most
// every ENERGY_PERSIST_INTERVAL_MS (default 5 min) and only if dirty.
constexpr uint32_t   ENERGY_PERSIST_INTERVAL_MS = 300000; // 5 min
constexpr float      ENERGY_SPIKE_REJECT_W    = 5000.0f;  // reject >5 kW instantaneous
constexpr float      CURRENT_SPIKE_REJECT_A   = 120.0f;  // reject >120 A (sensor glitch)

// ---------- SOC (Brief §24) ----------
// SOC is a coulomb-counting estimate, NOT laboratory-grade. The capacity MUST
// be configured; if 0, SOC is exposed as unavailable (null), never fabricated.
// Default 0 = "owner must set BATTERY_CAPACITY_AH before SOC is computed."
constexpr float    BATTERY_CAPACITY_AH     = 0.0f;     // 0 = UNAVAILABLE
constexpr float    SOC_FULL_VOLTAGE         = PACK_MAX_CHARGE_V;   // sync @ 29.2 V
constexpr float    SOC_EMPTY_VOLTAGE       = PACK_MIN_DISCHARGE_V;// sync @ 20.0 V
constexpr float    SOC_SYNC_HYSTERESIS_V    = 0.20f;   // 200 mV hysteresis

// ---------- POWER-FLOW CONSISTENCY (Brief §22) ----------
// Expected: Pmppt + PbatteryDischarge ≈ Pinverter (within tolerance).
// Use debounced detection — don't alarm on noisy single samples.
constexpr float    POWER_FLOW_TOLERANCE_W  = 50.0f;    // ±50 W absolute tolerance
constexpr float    POWER_FLOW_TOLERANCE_PCT = 0.10f;   // +10% relative tolerance
constexpr uint16_t POWER_FLOW_PERSIST_MS    = 5000;    // 5 s sustained before flag

// ---------- RESISTANCE ESTIMATION (Brief §25, §26, §29) ----------
// Passive ΔV/ΔI estimation. Only compute when |ΔI| ≥ MIN_DELTA_I_A.
constexpr float    RESISTANCE_MIN_DELTA_I_A    = 1.0f;   // 1 A step minimum
constexpr float    RESISTANCE_MIN_DELTA_V_V     = 0.005f; // 5 mV minimum
constexpr uint16_t RESISTANCE_SAMPLE_WINDOW_MS  = 2000;   // 2 s window
constexpr uint16_t RESISTANCE_SETTLE_MS         = 800;   // 800 ms settling after step
constexpr float    RESISTANCE_MAX_VALID_OHMS     = 0.500f; // reject >500 mΩ (artifact)

// ---------- TEST LOAD (Brief §27) ----------
// Optional diagnostic test architecture. Disabled by default — owner must
// explicitly configure a safe resistive test load + relay channel.
constexpr bool     TEST_LOAD_ENABLED         = false;
constexpr float    TEST_LOAD_RESISTANCE_OHMS = 0.0f;   // 0 = not configured
constexpr float    TEST_LOAD_MAX_CURRENT_A   = 0.0f;
constexpr uint8_t  TEST_LOAD_RELAY_CHANNEL   = 0;     // 0 = not mapped (1..12 valid)
constexpr uint16_t TEST_LOAD_SETTLE_MS       = 1500;   // 1.5 s settling

// ---------- NVS KEYS (Brief §48 — wear-conscious persistence) ----------
// Only energy counters + SOC state are persisted (not raw samples).
constexpr const char* NVS_NAMESPACE_BATTERY       = "bat";
constexpr const char* NVS_KEY_CHARGED_WH          = "chg_wh";
constexpr const char* NVS_KEY_DISCHARGED_WH       = "dch_wh";
constexpr const char* NVS_KEY_CHARGED_AH          = "chg_ah";
constexpr const char* NVS_KEY_DISCHARGED_AH       = "dch_ah";
constexpr const char* NVS_KEY_PV_WH               = "pv_wh";
constexpr const char* NVS_KEY_INV_DC_WH           = "inv_wh";
constexpr const char* NVS_KEY_SOC                 = "soc_pct";

} // namespace Battery

#endif // TIMER12_BATTERY_CONFIG_H
