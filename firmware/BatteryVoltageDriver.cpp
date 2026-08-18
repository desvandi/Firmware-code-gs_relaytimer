// =============================================================================
// BatteryVoltageDriver.cpp — pack voltage source abstraction
// =============================================================================
// Source = ADS1115_AIN3_BPLUS (default): pulls the calibrated B+ reading from
//   Ads1115Driver adsCell2 (channel 3). The ADS1115 already applies the
//   divider ratio + gain/offset calibration in its own driver, so we just
//   sample it.
//
// Source = ESP32_ADC1 (optional): reads the configured ADC1 pin, applies
//   divider + gain/offset calibration. Uses analogReadMilliVolts() (esp32-arduino
//   core helper) when available; falls back to raw analogRead.
//
// Output is EMA-smoothed (brief §45). Raw samples may be used by ResistanceEstimator
// (which has its own buffer) so over-filtering of the smoothed output is OK.
// =============================================================================
#include "BatteryVoltageDriver.h"
#include "Ads1115Driver.h"
#include "Config.h"
#include "BatteryConfig.h"
#include <cmath>
#include <algorithm>

namespace Drivers {

BatteryVoltageDriver packVoltage;

const char* BatteryVoltageDriver::getSourceStr() const {
  switch (Battery::PACK_VOLTAGE_SOURCE) {
    case Battery::PackVoltageSource::ADS1115_AIN3_BPLUS:
      return "ads1115_bplus";
    case Battery::PackVoltageSource::ESP32_ADC1:
      return (_status == PackVoltageStatus::NotConfigured) ? "esp32_adc1_unconfigured"
                                                            : "esp32_adc1";
  }
  return "unavailable";
}

bool BatteryVoltageDriver::begin() {
  _available = false;
  _filteredV = NAN;
  _status = PackVoltageStatus::NotConfigured;
  _emaV = 0.0f;
  _emaInit = false;

  if (Battery::PACK_VOLTAGE_SOURCE == Battery::PackVoltageSource::ADS1115_AIN3_BPLUS) {
    // Availability depends on ADS1115 #2 (adsCell2) being initialized.
    // We don't block here — begin() returns true even if ADS isn't ready yet;
    // tick() will mark unavailable if ADS reading is invalid.
    _available = true;
    _status = PackVoltageStatus::Ok;
    Serial.println("[PACKV] source = ADS1115_AIN3_BPLUS (default — see BatteryConfig.h deviation note)");
    return true;
  }
  if (Battery::PACK_VOLTAGE_SOURCE == Battery::PackVoltageSource::ESP32_ADC1) {
    if (Battery::PACK_ADC_PIN == 255) {
      // Brief §52: STOP + REPORT rather than silently using an unsafe pin.
      Serial.println("[PACKV] FATAL: PACK_VOLTAGE_SOURCE=ESP32_ADC1 but PACK_ADC_PIN=255 (unassigned).");
      Serial.println("[PACKV]        Owner must assign a free ADC1 GPIO in BatteryConfig.h (after");
      Serial.println("[PACKV]        auditing PIR/relay/I2C conflicts) or revert to ADS1115_AIN3_BPLUS.");
      _status = PackVoltageStatus::NotConfigured;
      return false;
    }
    // Configure ADC pin (ADC1 only — Wi-Fi reliability preserved per brief §11)
    // Note: analogRead on ADC1 is safe with Wi-Fi; ADC2 is forbidden.
    _available = true;
    _status = PackVoltageStatus::Ok;
    Serial.printf("[PACKV] source = ESP32_ADC1, pin=GPIO%d, divider=×%.2f\n",
                  Battery::PACK_ADC_PIN, (double)Battery::PACK_ADC_DIVIDER_RATIO);
    return true;
  }
  return false;
}

uint16_t BatteryVoltageDriver::_oversampleAdc(uint8_t pin, uint16_t n) const {
  uint32_t sum = 0;
  for (uint16_t i = 0; i < n; i++) sum += analogRead(pin);
  return (uint16_t)(sum / n);
}

bool BatteryVoltageDriver::_sampleEsp32Adc() {
  if (Battery::PACK_ADC_PIN == 255) {
    _status = PackVoltageStatus::NotConfigured;
    return false;
  }
  // Use ESP32 analogReadMilliVolts() — handles per-chip calibration.
  uint32_t mv = analogReadMilliVolts(Battery::PACK_ADC_PIN);
  _lastRawAdc = (uint16_t)analogRead(Battery::PACK_ADC_PIN);  // also keep raw count
  if (mv == 0 || mv > 3300) {
    _status = PackVoltageStatus::AdcInvalid;
    return false;
  }
  float vAdc = mv / 1000.0f;
  float vPack = vAdc * Battery::PACK_ADC_DIVIDER_RATIO;
  // Apply two-point calibration
  vPack = vPack * Battery::PACK_ADC_GAIN + Battery::PACK_ADC_OFFSET;
  if (vPack < 0.0f || vPack > 60.0f) {  // sanity bound
    _status = PackVoltageStatus::OutOfRange;
    return false;
  }
  if (!_emaInit) {
    _emaV = vPack;
    _emaInit = true;
  } else {
    _emaV = _emaV * (1.0f - Battery::ADC_SMOOTH_ALPHA)
            + vPack * Battery::ADC_SMOOTH_ALPHA;
  }
  _filteredV = _emaV;
  _status = PackVoltageStatus::Ok;
  return true;
}

float BatteryVoltageDriver::_readAds1115Bplus() const {
  // ADS1115 #2 AIN3 = B+ (brief §16). Channel index 3 on adsCell2.
  AdsStatus s = adsCell2.getChannelStatus(3);
  if (s != AdsStatus::Ok) return NAN;
  return adsCell2.getChannelV(3);
}

void BatteryVoltageDriver::tick() {
  if (!_available) return;
  unsigned long now = millis();
  if (now - _lastSampleMs < Battery::PACK_ADC_INTERVAL_MS) return;
  _lastSampleMs = now;

  if (Battery::PACK_VOLTAGE_SOURCE == Battery::PackVoltageSource::ADS1115_AIN3_BPLUS) {
    float v = _readAds1115Bplus();
    if (std::isnan(v) || v < 0.0f || v > 60.0f) {
      _status = PackVoltageStatus::AdcInvalid;
      _filteredV = NAN;
      return;
    }
    if (!_emaInit) {
      _emaV = v;
      _emaInit = true;
    } else {
      _emaV = _emaV * (1.0f - Battery::ADC_SMOOTH_ALPHA)
              + v * Battery::ADC_SMOOTH_ALPHA;
    }
    _filteredV = _emaV;
    _status = PackVoltageStatus::Ok;
    return;
  }
  if (Battery::PACK_VOLTAGE_SOURCE == Battery::PackVoltageSource::ESP32_ADC1) {
    _sampleEsp32Adc();
    return;
  }
  _status = PackVoltageStatus::NotConfigured;
}

} // namespace Drivers
