// =============================================================================
// CommandHashBaseline.cpp — capture current _computeCommandHash output
// =============================================================================
// Run BEFORE extraction to capture hash vectors. These vectors are then used
// by CommandHashEquivalenceTest.cpp AFTER extraction to verify byte-equivalence.
//
// Approach: call production _handleCommand() with known JSON for each type.
// The journal stores the commandHash internally. Read it back via
// journal.getCommandHash(requestId). This avoids any source modification
// (the function is static in MqttClient.cpp so we can't call it directly).
//
// This is a throwaway file — its sole purpose is to capture baseline hashes.
// =============================================================================
#define private public
#include "MqttClient.h"
#undef private

#include "MqttClientDeps.h"
#include "TransactionJournal.h"
#include "JournalRecord.h"
#include <cstdio>

using namespace Services;
using Services::journal;

static void resetEnv() {
  Preferences::clearAllStorage();
  for (int i = 0; i < 12; i++) {
    Core::relayState[i] = false;
    Core::relaySource[i] = Core::RelaySource::Manual;
    Core::channels[i].schedCount = 0;
    strncpy(Core::channels[i].name, "", 1);
  }
  host_shim::g_espRestartCalled = false;
  journal.~TransactionJournal();
  new (&journal) TransactionJournal();
  journal.begin();
  journal.setBootPhase(Services::BootPhase::RUNNING);
}

int main() {
  printf("=== Command Hash Baseline Vectors ===\n");
  printf("(Captured via production _handleCommand -> journal.getCommandHash)\n");
  printf("(These are produced by the CURRENT static _computeCommandHash in MqttClient.cpp)\n\n");

  // For commands that don't reach markExecuting (would fail validation), the
  // hash IS computed and stored via storeIntent BEFORE validation runs.
  // So journal.getCommandHash returns the actual hash even for "invalid"
  // commands — as long as storeIntent was called.
  // For getStatus (read-only, CommitMode::NONE), NO storeIntent is called,
  // so we cannot capture its hash via this method. We use a different
  // approach for getStatus: send "reboot" via _handleOta which DOES call
  // storeIntent.

  MqttClient mqtt;

  // ---- relay on ch1 ----
  {
    resetEnv();
    const char* json = R"({"type":"relay","action":"on","requestId":"cap-relay-on","channelId":1})";
    mqtt._handleCommand(json);
    printf("relay_on_ch1: %s\n", journal.getCommandHash("cap-relay-on").c_str());
  }
  // ---- relay off ch12 ----
  {
    resetEnv();
    const char* json = R"({"type":"relay","action":"off","requestId":"cap-relay-off","channelId":12})";
    mqtt._handleCommand(json);
    printf("relay_off_ch12: %s\n", journal.getCommandHash("cap-relay-off").c_str());
  }
  // ---- relay set_mode ch3 manual true ----
  {
    resetEnv();
    const char* json = R"({"type":"relay","action":"set_mode","requestId":"cap-relay-sm","channelId":3,"mode":"manual","manualState":true})";
    mqtt._handleCommand(json);
    printf("relay_setmode_ch3_manual_true: %s\n", journal.getCommandHash("cap-relay-sm").c_str());
  }
  // ---- schedule upsert ch1 ----
  {
    resetEnv();
    const char* json = R"({"type":"schedule","action":"upsert","requestId":"cap-sched-1","channelId":1,"id":0,"onTime":"07:00","offTime":"18:00","dayMask":127,"enabled":true})";
    mqtt._handleCommand(json);
    printf("schedule_upsert_ch1: %s\n", journal.getCommandHash("cap-sched-1").c_str());
  }
  // ---- schedule upsert ch5 id2 disabled ----
  {
    resetEnv();
    const char* json = R"({"type":"schedule","action":"upsert","requestId":"cap-sched-2","channelId":5,"id":2,"onTime":"22:30","offTime":"06:15","dayMask":31,"enabled":false})";
    mqtt._handleCommand(json);
    printf("schedule_upsert_ch5_id2_disabled: %s\n", journal.getCommandHash("cap-sched-2").c_str());
  }
  // ---- pir config id1 enabled 120 ----
  {
    resetEnv();
    const char* json = R"({"type":"pir","action":"config","requestId":"cap-pir-1","id":1,"enabled":true,"holdTime":120})";
    mqtt._handleCommand(json);
    printf("pir_config_id1_enabled_120: %s\n", journal.getCommandHash("cap-pir-1").c_str());
  }
  // ---- pir config id4 disabled 300 ----
  {
    resetEnv();
    const char* json = R"({"type":"pir","action":"config","requestId":"cap-pir-2","id":4,"enabled":false,"holdTime":300})";
    mqtt._handleCommand(json);
    printf("pir_config_id4_disabled_300: %s\n", journal.getCommandHash("cap-pir-2").c_str());
  }
  // ---- channel rename ch1 Kitchen ----
  {
    resetEnv();
    const char* json = R"({"type":"channel","action":"rename","requestId":"cap-chan-1","channelId":1,"name":"Kitchen"})";
    mqtt._handleCommand(json);
    printf("channel_rename_ch1_Kitchen: %s\n", journal.getCommandHash("cap-chan-1").c_str());
  }
  // ---- time set ----
  {
    resetEnv();
    const char* json = R"({"type":"time","action":"set","requestId":"cap-time-1","datetime":"2024-01-15T10:30:00"})";
    mqtt._handleCommand(json);
    printf("time_set: %s\n", journal.getCommandHash("cap-time-1").c_str());
  }
  // ---- system reboot ----
  {
    resetEnv();
    const char* json = R"({"type":"system","action":"reboot","requestId":"cap-sys-reboot"})";
    mqtt._handleCommand(json);
    printf("system_reboot: %s\n", journal.getCommandHash("cap-sys-reboot").c_str());
  }
  // ---- system resetEnergyStats ----
  {
    resetEnv();
    const char* json = R"({"type":"system","action":"resetEnergyStats","requestId":"cap-sys-reset"})";
    mqtt._handleCommand(json);
    printf("system_resetEnergyStats: %s\n", journal.getCommandHash("cap-sys-reset").c_str());
  }
  // ---- config setDevice ----
  {
    resetEnv();
    const char* json = R"({"type":"config","action":"setDevice","requestId":"cap-conf-1","deviceName":"TestDevice","timezone":7})";
    mqtt._handleCommand(json);
    printf("config_setDevice: %s\n", journal.getCommandHash("cap-conf-1").c_str());
  }
  // ---- config setDevice minimal (deviceName="X", no timezone) ----
  {
    resetEnv();
    const char* json = R"({"type":"config","action":"setDevice","requestId":"cap-conf-2","deviceName":"X"})";
    mqtt._handleCommand(json);
    printf("config_setDevice_minimal: %s\n", journal.getCommandHash("cap-conf-2").c_str());
  }
  // ---- OTA update (via _handleOta — uses same _computeCommandHash) ----
  {
    resetEnv();
    // Set OTA_ED25519_PUBLIC_KEY_HEX to non-empty so we get past the Ed25519 check.
    // OTA_HTTPS_ROOT_CA stays empty so _downloadAndVerifyOta fails AFTER markExecuting.
    // The hash is computed BEFORE the validation chain, so we'll capture it.
    Core::OTA_ED25519_PUBLIC_KEY_HEX = "00";
    const char* json = R"({"action":"update","requestId":"cap-ota-1","url":"https://example.com/fw.bin","version":"4.1.0","size":100000,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","signature":"00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"})";
    mqtt._handleOta(json);
    printf("ota_update: %s\n", journal.getCommandHash("cap-ota-1").c_str());
    Core::OTA_ED25519_PUBLIC_KEY_HEX = "";
  }

  printf("\n=== End of baseline vectors ===\n");
  return 0;
}
