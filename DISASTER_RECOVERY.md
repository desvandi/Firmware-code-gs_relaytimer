# Disaster Recovery — Timer Digital Relay v4.3.8

> Implements disaster recovery requirements from the Industrial-Grade
> Implementation Directive §68.

---

## 1. Scenario: ESP32 Hardware Failure

**Trigger**: Device stops responding (no MQTT publications, no REST API, no ping).

### Recovery steps

1. **Identify failure mode**
   - Check PWA: device shows OFFLINE for >5 min → confirmed hardware failure
   - Check broker logs: device's LWT message "0" published
   - Power cycle device — if recovers → brownout/watchdog issue (transient)
   - If still unresponsive → hardware replacement needed

2. **Order replacement ESP32-WROOM-32 module**
   - Same hardware revision (WROOM-32 38-pin) to preserve GPIO contract
   - Pre-flash production firmware via USB before deployment

3. **Provision new device** (per DEPLOYMENT.md §2.5)
   - Flash production firmware binary (with PRODUCTION_BUILD flag)
   - First boot → WiFi Config Portal → enter SSID/password
   - Record new secrets from Serial Monitor:
     - MAC address (will differ from old device — expected)
     - Anonymous device ID (different from old → must update MQTT broker ACL + GAS Script Properties)
     - JWT secret (per-device)
     - MQTT topic password (per-device)
     - GAS HMAC secret (per-device)
     - Device PIN (per-device)

4. **Update MQTT broker**
   - Remove old device's ACL entry
   - Add new device's username/password + ACL entry

5. **Update GAS Script Properties**
   - Remove `DEVICE_<old-anonymousId>_SECRET`
   - Add `DEVICE_<new-anonymousId>_SECRET` = new GAS HMAC secret

6. **Restore configuration**
   - Export config from PWA (old device's last known config)
   - On new device: PWA → Settings → Import Config (paste JSON)
   - Verify schedule + channel names + PIR config restored

7. **Verify device**
   - PWA shows ONLINE within 5s of boot
   - /api/status returns valid telemetry
   - Test one relay ON/OFF → physical relay clicks + ACK received
   - Check telemetry sequence increments (proves MQTT publishing)

8. **Decommission old device**
   - In PWA: Settings → Decommission Device (future feature)
   - All per-device credentials revoked at broker + GAS

### Expected downtime
- With hot-spare ESP32 + pre-flashed firmware: **5–15 minutes**
- Without spare (ordering + shipping): **3–10 days**

---

## 2. Scenario: MQTT Broker Failure

**Trigger**: PWA can't reach device, but device is online (LAN REST still works).

### Recovery steps

1. **Confirm broker down**
   - SSH to broker VPS: `systemctl status mosquitto`
   - Check DNS resolution: `dig broker.example.com`
   - Check TLS cert validity: `openssl s_client -connect broker.example.com:8883`

2. **Fallback to REST-only mode**
   - PWA auto-detects MQTT failure → switches to REST polling (3s interval)
   - REST works via Cloudflare Tunnel URL → no remote access impact
   - Some latency: ~3s status refresh vs ~0s for MQTT

3. **Restore broker**
   - If process crashed: `systemctl restart mosquitto`
   - If VPS down: reboot VPS, Mosquitto starts automatically
   - If TLS cert expired: renew via Let's Encrypt `certbot renew`

4. **Verify**
   - ESP32 reconnects within 30s (exponential backoff: 5s, 10s, 20s, 40s)
   - PWA shows MQTT mode active again
   - telemetrySequence increments

### Expected downtime
- Transient broker crash: **<1 minute** (with REST fallback, no operator impact)
- VPS reboot: **2–5 minutes**
- TLS cert renewal: **5–30 minutes**

---

## 3. Scenario: GAS Web App Failure

**Trigger**: PWA shows mock insights instead of real ones; ESP32 logs "GAS unreachable".

### Recovery steps

1. **Confirm GAS down**
   - Browse to GAS URL → check if returns `{"success":true,"status":"ok"}` for `?action=health`
   - Check Google Apps Script quotas: https://script.google.com → Executions
   - Check Gemini API key validity: https://aistudio.google.com

2. **Restore GAS**
   - If quota exhausted (Gemini free tier): wait 24h or upgrade to paid Gemini
   - If Script Properties lost: re-configure `GEMINI_API_KEY` and all `DEVICE_<id>_SECRET`
   - If deployment broken: redeploy → update `GAS_INSIGHTS_URL` in `Config.h` + Vercel env

3. **Verify**
   - Trigger manual insight fetch via PWA → real insights returned within 60s

### Expected downtime
- Quota exhaustion: **up to 24h** (Gemini free tier resets daily)
- Reconfiguration: **15–30 minutes**

---

## 4. Scenario: OTA Failure / Firmware Boot Loop

**Trigger**: Device offline after OTA, PWA can't reach it.

### Recovery steps

1. **Confirm boot loop**
   - If MQTT offline + no REST response + LED behavior indicates boot loop:
     most likely new firmware crashes on boot
   - The OTA boot-health check (R10B-6) should auto-rollback after 3 failed boots

2. **Wait for auto-rollback**
   - 3 failed boots → firmware rolls back to previous partition
   - Device comes online within 60s
   - Activity log shows: `OTA → rollback`

3. **Manual recovery if rollback fails**
   - Physical access required
   - Connect USB to ESP32
   - Flash known-good firmware: `pio run -e production -t upload`
   - ESP32 boots → OTA partition marked invalid → rollback logic cleared

4. **Investigate root cause**
   - Get crash dump from Serial Monitor before rollback
   - Use `monitor_filters = esp32_exception_decoder` (in platformio.ini)
     to decode the stack trace
   - Common causes:
     - Heap exhaustion (new feature allocates too much)
     - Stack overflow (deep recursion or large locals)
     - Null pointer deref (sensor driver change)
     - Watchdog timeout (long blocking call)

### Expected downtime
- Auto-rollback works: **<2 minutes** (3 boots × ~20s)
- Manual USB recovery: **15–30 minutes** (requires physical access)

---

## 5. Scenario: Configuration Corruption

**Trigger**: ESP32 boots but config seems wrong (channel names reset, schedules missing).

### Recovery steps

1. **Identify corruption**
   - /api/status shows default channel names ("Lampu Depan" etc.)
   - /api/config shows default schedule (1 entry)
   - Activity log: "Config CRC mismatch — using backup"

2. **Restore from backup**
   - ConfigStore uses A/B with CRC + backup file:
     - `/config.json` (primary)
     - `/config.bak` (backup)
     - `/config.tmp` (atomic write)
   - If primary CRC fails → load from .bak
   - If .bak also fails → load defaults (last resort)

3. **Restore from PWA export**
   - PWA → Settings → Export Config (if previously exported)
   - On new device: Settings → Import Config

4. **Investigate root cause**
   - NVS wear (after 100k+ writes — typical 10-year lifespan)
   - Power loss during config save → corrupted primary, .bak should be intact

### Expected downtime
- Auto-restore from backup: **<1 second** (transparent to operator)
- Manual restore from PWA export: **5–10 minutes**

---

## 6. Scenario: WiFi Credential Lost

**Trigger**: Device boots into WiFi Config Portal mode unexpectedly.

### Recovery steps

1. **Confirm** — Device boots to AP `Timer12-Setup` (SSID visible to phones)
2. **Re-enter credentials** — Connect to AP, browse to `192.168.4.1`, enter SSID/password
3. **Reboot** — Device joins WiFi in STA mode

This scenario triggers when:
- NVS namespace `timer12` was erased (factory reset, or NVS corruption)
- WiFi credentials were never set (first boot)

### Expected downtime
- **5 minutes** (operator re-enters credentials)

---

## 7. Scenario: RTC Battery Dead

**Trigger**: Device reboots and prints "RTC lost power - time invalid" + `RTC_INVALID` alarm.

### Recovery steps

1. **Confirm** — /api/health shows `rtcStatus: INVALID`
2. **Sync RTC** — PWA → Settings → Set RTC Time → Sync Now (uses PWA browser time)
3. **Verify** — `rtcStatus: VALID`, scheduler resumes

If RTC battery is permanently dead (DS3231 CR2032 depleted):
- Each reboot requires manual RTC sync (until battery replaced)
- Replace DS3231 module (CR2032 battery is soldered on most modules)

### Expected downtime
- Manual sync: **<1 minute** per reboot
- Hardware replacement: **15 minutes** (desolder DS3231, solder new)

---

## 8. Preventative Maintenance

| Frequency | Task | Reason |
|---|---|---|
| Monthly | Check PWA dashboard for any active alarms | Catch degraded sensors early |
| Monthly | Verify backup of `config.json` export | Disaster recovery preparedness |
| Quarterly | Inspect ESP32 + relay board visually (dust, corrosion, loose wires) | Environmental factors |
| Quarterly | Test OTA update flow with a signed test binary | Verify signing keys still valid |
| Annually | Replace DS3231 backup battery (CR2032) | Prevent RTC invalid on reboot |
| Annually | Renew Let's Encrypt cert on MQTT broker (auto-renew usually works, manual verify) | TLS connectivity |
| Annually | Audit MQTT broker ACL list (remove decommissioned devices) | Security hygiene |
| Annually | Rotate MQTT broker passwords (per-device) | Security hygiene |
| Bi-annually | Audit ESP32 flash for NVS wear indicators | Brief §29 — wear leveling |

---

## 9. Recovery Time Objectives (RTO)

| Scenario | RTO (with spare parts) | RTO (without spares) |
|---|---|---|
| Hardware failure (ESP32) | 15 min | 3–10 days |
| MQTT broker failure | 1 min (REST fallback) | 5 min |
| GAS failure | 0 (degraded mode) | 30 min |
| OTA boot loop | 2 min (auto-rollback) | 30 min (USB recovery) |
| Config corruption | 0 (auto-restore) | 10 min (manual) |
| WiFi credential lost | 5 min | 5 min |
| RTC battery dead | 1 min (manual sync) | 15 min (replace module) |
