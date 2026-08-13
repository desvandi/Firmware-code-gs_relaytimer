// =============================================================================
// Drivers/PirDriver.cpp
// =============================================================================
#include "PirDriver.h"
#include "Globals.h"
#include "LogService.h"

namespace Drivers {

PirDriver pir;

void PirDriver::begin() {
  for (uint8_t i = 0; i < Core::NUM_PIR; i++) {
    pinMode(Core::PIR_PINS[i], INPUT);
    Core::pirState[i] = Core::PirState{};
    Core::pirState[i].lastSampleTime = 0;
    Core::pirState[i].sampleIdx = 0;
  }
  Core::pirStartupTime = millis();
}

bool PirDriver::readDebounced(uint8_t idx) {
  if (idx >= Core::NUM_PIR) return false;
  unsigned long now = millis();
  if (now - Core::pirState[idx].lastSampleTime >= Core::PIR_DEBOUNCE_INTERVAL_MS) {
    Core::pirState[idx].sampleHistory[Core::pirState[idx].sampleIdx] =
      (digitalRead(Core::PIR_PINS[idx]) == HIGH) ? 1 : 0;
    Core::pirState[idx].sampleIdx =
      (Core::pirState[idx].sampleIdx + 1) % Core::PIR_DEBOUNCE_SAMPLES;
    Core::pirState[idx].lastSampleTime = now;
  }
  uint8_t highCount = 0;
  for (uint8_t i = 0; i < Core::PIR_DEBOUNCE_SAMPLES; i++) {
    if (Core::pirState[idx].sampleHistory[i]) highCount++;
  }
  return highCount >= Core::PIR_DEBOUNCE_THRESHOLD;
}

void PirDriver::checkStuck(uint8_t idx) {
  if (idx >= Core::NUM_PIR) return;
  bool motion = Core::pirState[idx].motionNow;
  if (motion) {
    if (Core::pirState[idx].highSince == 0) {
      Core::pirState[idx].highSince = millis();
    } else if (!Core::pirState[idx].stuckAlerted &&
               millis() - Core::pirState[idx].highSince > Core::PIR_STUCK_TIMEOUT_MS) {
      Core::pirState[idx].stuckAlerted = true;
      Core::pirState[idx].stuckCooldownUntil = millis() + Core::PIR_STUCK_COOLDOWN_MS;
      char msg[80];
      snprintf(msg, sizeof(msg), "PIR %d STUCK >30min - force OFF 5min", idx + 1);
      Services::Log.append(Core::LogType::Error, msg, Core::PIR_CHANNEL_OFFSET + idx + 1);
    }
  } else {
    Core::pirState[idx].highSince = 0;
    if (Core::pirState[idx].stuckAlerted &&
        millis() > Core::pirState[idx].stuckCooldownUntil) {
      Core::pirState[idx].stuckAlerted = false;
      char msg[80];
      snprintf(msg, sizeof(msg), "PIR %d recovered from stuck state", idx + 1);
      Services::Log.append(Core::LogType::Error, msg, Core::PIR_CHANNEL_OFFSET + idx + 1);
    }
  }
}

void PirDriver::tick() {
  for (uint8_t i = 0; i < Core::NUM_PIR; i++) {
    if (millis() - Core::pirStartupTime < Core::PIR_WARMUP_MS) {
      Core::pirState[i].motionNow = false;
      for (uint8_t j = 0; j < Core::PIR_DEBOUNCE_SAMPLES; j++) {
        Core::pirState[i].sampleHistory[j] = 0;
      }
      continue;
    }
    bool prev = Core::pirState[i].motionNow;
    Core::pirState[i].motionNow = readDebounced(i);

    // Edge-detect: log on rising edge
    if (Core::pirState[i].motionNow && !prev) {
      Core::pirState[i].lastMotion = millis();
      Core::pirState[i].everTriggered = true;
      Core::pirState[i].triggerCountToday++;
      char msg[64];
      snprintf(msg, sizeof(msg), "PIR %d motion detected (CH%d)",
               i + 1, Core::PIR_CHANNEL_OFFSET + i + 1);
      Services::Log.append(Core::LogType::PirTrigger, msg,
                            Core::PIR_CHANNEL_OFFSET + i + 1);
    }
    checkStuck(i);
  }
}

bool PirDriver::isMotion(uint8_t idx) const {
  if (idx >= Core::NUM_PIR) return false;
  return Core::pirState[idx].motionNow;
}

bool PirDriver::isStuck(uint8_t idx) const {
  if (idx >= Core::NUM_PIR) return false;
  return Core::pirState[idx].stuckAlerted;
}

void PirDriver::testTrigger(uint8_t idx) {
  if (idx >= Core::NUM_PIR) return;
  if (millis() < Core::pirStartupTime + Core::PIR_WARMUP_MS) return;
  Core::pirState[idx].motionNow = true;
  Core::pirState[idx].lastMotion = millis();
  Core::pirState[idx].everTriggered = true;
  Core::pirState[idx].triggerCountToday++;
  char msg[64];
  snprintf(msg, sizeof(msg), "PIR %d manual test trigger", idx + 1);
  Services::Log.append(Core::LogType::PirTrigger, msg,
                        Core::PIR_CHANNEL_OFFSET + idx + 1);
}

void PirDriver::resetAll() {
  for (uint8_t i = 0; i < Core::NUM_PIR; i++) {
    Core::pirState[i] = Core::PirState{};
    Core::pirState[i].lastSampleTime = 0;
    Core::pirState[i].sampleIdx = 0;
  }
  Core::pirStartupTime = millis();
}

void PirDriver::resetDailyCounters() {
  for (uint8_t i = 0; i < Core::NUM_PIR; i++) {
    Core::pirState[i].triggerCountToday = 0;
  }
  Core::metrics.errorsToday = 0;
}

} // namespace Drivers
