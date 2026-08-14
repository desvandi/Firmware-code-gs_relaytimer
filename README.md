# Timer Digital Relay v4.0 — Firmware + Google Apps Script

> ESP32-WROOM-32 firmware (12-channel relay + 4 PIR + DS3231 RTC + PZEM-004T v3.0 power meter) and the Google Apps Script that bridges ESP32 logs to Gemini AI for energy insights.

[![Firmware Version](https://img.shields.io/badge/firmware-v4.0.0-blue)](#)
[![ESP32 Core](https://img.shields.io/badge/ESP32%20core-3.3.7-green)](#)
[![License](https://img.shields.io/badge/license-proprietary-lightgrey)](#)

This repo holds the **device-side code** for the Timer Digital Relay v4.0 system. The companion PWA dashboard lives in a separate repo: **[desvandi/Remote-Relay](https://github.com/desvandi/Remote-Relay)**.

---

## Repository Layout

```
Firmware-code-gs_relaytimer/
├── firmware/                       ← ESP32 Arduino sketch (53 files, flat layout)
│   ├── firmware_v4.ino             ← main entry (setup + loop)
│   ├── platformio.ini              ← PlatformIO config (optional — Arduino IDE also works)
│   ├── Config.h                    ← all compile-time constants (pins, broker, GAS URL, etc.)
│   ├── Types.h, Globals.h, Common.h
│   ├── RelayDriver.{cpp,h}         ← 12-channel active-LOW relay (boot glitch fix)
│   ├── PirDriver.{cpp,h}           ← 4× HC-SR501 (3-sample debounce, stuck detect)
│   ├── RtcDriver.{cpp,h}           ← DS3231 over I2C (400 kHz Fast Mode)
│   ├── PzemDriver.{cpp,h}          ← PZEM-004T v3.0 self-contained Modbus-RTU
│   ├── WifiManager.{cpp,h}         ← AP+STA, password derived from MAC
│   ├── MqttClient.{cpp,h}          ← MQTT remote control (TLS, ACK, dedup, LWT)
│   ├── HttpServer.{cpp,h}          ← REST API (22 routes) + CORS
│   ├── AuthManager.{cpp,h}         ← JWT (HS256), CSRF, rate limiter, factory-reset tokens
│   ├── OtaManager.{cpp,h}          ← Update library + GitHub Release check (stub)
│   ├── Scheduler.{cpp,h}           ← Schedule evaluation (overnight + dayMask)
│   ├── RelayEngine.{cpp,h}         ← Priority: Manual > PIR > Schedule > Off
│   ├── LogService.{cpp,h}          ← Activity log (JSON-lines) + audit log (plain text)
│   ├── ConfigStore.{cpp,h}         ← User config + Schedule + Device config (CRC + backup)
│   ├── FileSystem.{cpp,h}          ← LittleFS wrapper (mount, atomic write)
│   ├── Crypto.{cpp,h}              ← SHA-256, PBKDF2, HMAC-SHA256, JWT, base64url
│   ├── Crc.{cpp,h}                 ← CRC-32 (zlib polynomial)
│   ├── Json.{cpp,h}                ← ArduinoJson helpers, parseMinutes, password strength
│   ├── Advisor.{cpp,h}             ← GAS integration (hourly POST logs → Gemini insights)
│   └── *Handlers.h                 ← 22 REST route handlers (one header per resource)
│
├── code.gs/
│   └── Code.gs                     ← Google Apps Script (ESP32 → Gemini AI insights)
│
└── README.md                       ← this file
```

The firmware uses a **flat layout** (all `.cpp`/`.h` files at the root of `firmware/`) so it works with both the Arduino IDE (which auto-discovers `.ino` + same-folder sources) and PlatformIO. There is **no nested `src/` directory** because Arduino IDE does not scan subfolders by default — keeping everything flat avoids missing-include errors.

---

## Architecture Overview

```
                          ┌───────────────────────────┐
                          │  ESP32-WROOM-32 (this repo)│
                          │                           │
   PIR 1-4 ─── GPIO 34-39 │  RelayEngine             │ ── GPIO 13,14,16-23,25-27 ──→ Relay 1-12
   DS3231 ──── I2C (32,33) │  Scheduler               │
   PZEM-004T ─ UART2 (4,5) │  AuthManager (JWT/CSRF)  │
                          │  PzemDriver (Modbus-RTU)  │
                          │  Advisor (GAS pipeline)   │
                          └─────────┬────────┬────────┘
                                    │        │
                          REST (80) │        │ MQTT (1883/8883)
                                    │        │
                       ┌────────────┘        └─────────────┐
                       │                                    │
              ┌────────▼─────────┐               ┌──────────▼──────────┐
              │ Cloudflare Tunnel │               │  MQTT Broker        │
              │ (optional, LAN)   │               │  (HiveMQ public or  │
              └────────┬─────────┘               │   self-hosted w/TLS)│
                       │                          └──────────┬──────────┘
              ┌────────▼─────────┐                            │
              │  PWA on Vercel   │◄───────────────────────────┘
              │  (Remote-Relay)  │       WSS (8884) for PWA
              └──────────────────┘
                       ▲
                       │ HTTPS (hourly POST logs)
              ┌────────┴─────────┐
              │ Google Apps Script│
              │   (Code.gs)       │
              └────────┬─────────┘
                       │ HTTPS
              ┌────────▼─────────┐
              │  Gemini API       │
              │  (AI insights)    │
              └──────────────────┘
```

**Key design principle**: the ESP32 is the **single source of truth**. It keeps working even if internet, Cloudflare, Vercel, Google, or the MQTT broker are all down. The PWA is just a UI; all scheduling, PIR logic, RTC time, and relay control live in firmware and run locally.

---

## Firmware — Build & Flash

### Prerequisites

- **Arduino IDE 2.x** (recommended for end users) or **PlatformIO Core** (`pip install platformio`)
- **ESP32 Arduino Core v3.3.7+** by Espressif (Boards Manager → "esp32 by Espressif Systems")
- **Libraries** (Library Manager):
  - `RTClib` by Adafruit (DS3231)
  - `ArduinoJson` by Benoit Blanchon (v6.19+)
  - `PubSubClient` by Nick O'Leary (MQTT)
- USB driver for your board's USB-UART bridge:
  - CP2102 (most ESP32 dev boards) → Silicon Labs CP210x driver
  - CH340 (cheaper clones) → WCH CH340 driver
- USB cable (data-capable, not charge-only)

### Build with Arduino IDE

1. Clone this repo: `git clone https://github.com/desvandi/Firmware-code-gs_relaytimer.git`
2. Open `firmware/firmware_v4.ino` in Arduino IDE
3. Select board: **ESP32 Dev Module** (or match your specific board)
4. Set board options:
   - Upload Speed: `921600`
   - Flash Size: `4MB (32Mb)`
   - Partition Scheme: `Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)`
   - PSRAM: `Disabled` (most WROOM-32 boards have no PSRAM)
5. Click **Upload** (→ arrow)
6. Open Serial Monitor at **115200 baud** to view boot output

### Build with PlatformIO (alternative)

```bash
cd firmware
pio run                 # build only
pio run -t upload       # build + flash
pio device monitor      # serial monitor (115200 baud)
```

### First Boot — Configuration

On first boot, the firmware starts in **AP mode**:
- SSID: `Timer12-XXXXXX` (last 6 of MAC)
- Password: printed to Serial (also derived from MAC)
- Open `http://192.168.4.1` to configure:
  - WiFi SSID + password (for STA mode, internet access)
  - MQTT broker host + port + credentials (optional, for remote mode)
  - GAS URL (optional, for AI insights)

After configuration, the ESP32 reboots into **STA mode** (your home WiFi). If WiFi fails, it falls back to AP mode automatically.

### Default Credentials

The firmware generates a random admin password derived from the ESP32's MAC address on first boot:

```
T<last-4-hex-of-MAC><mid-4-hex><low-4-hex>
```

The password is printed to **Serial** during boot — copy and store it securely. The default username is `admin`. Change via `POST /api/config/password` (requires current password).

### MQTT Password

For MQTT mode, a separate password is generated (8 alphanumeric chars, uppercase). Also printed to Serial during boot:

```
MAC: A4CF12345678
MQTT Password: K7M3P9XQ
```

The PWA login uses these two values. The MQTT password is embedded in the topic path (`timer12/<mac>/<mqttPass>/command`) so anyone subscribing/publishing must know it.

---

## Hardware Pinout

| Component | GPIO | Notes                              |
|-----------|------|------------------------------------|
| Relay 1   | 13   | Active-LOW module                  |
| Relay 2   | 14   |                                    |
| Relay 3   | 16   |                                    |
| Relay 4   | 17   |                                    |
| Relay 5   | 18   |                                    |
| Relay 6   | 19   |                                    |
| Relay 7   | 21   |                                    |
| Relay 8   | 22   |                                    |
| Relay 9   | 23   |                                    |
| Relay 10  | 25   |                                    |
| Relay 11  | 26   |                                    |
| Relay 12  | 27   |                                    |
| PIR 1     | 34   | Input-only, no pull                |
| PIR 2     | 35   | Input-only, no pull                |
| PIR 3     | 36   | Input-only (SENSOR_VP)             |
| PIR 4     | 39   | Input-only (SENSOR_VN)             |
| I2C SDA   | 32   | DS3231                             |
| I2C SCL   | 33   | DS3231                             |
| PZEM RX   | 4    | ESP32 GPIO4 → PZEM TX              |
| PZEM TX   | 5    | ESP32 GPIO5 → PZEM RX              |

⚠️ **220V AC SAFETY**: Relays control mains voltage. Only wire when power is OFF at the breaker. Use adequate wire gauge (≥1.5mm² for 10A loads). Enclose in an IP-rated box. Add a fuse per channel. If unsure, consult a licensed electrician.

---

## Firmware — Key Subsystems

### 1. MQTT Remote Mode (`MqttClient.cpp`)

- **Outbound connection** to broker (works behind CGNAT/MiFi — no port forwarding needed)
- **Topic format**: `timer12/<mac>/<subtopic>` (R10C-3: password removed from topic)
- **Authentication**: via broker credentials (username/password in MQTT CONNECT)
- **Authorization**: via broker ACL (per-device topic restrictions)
- **TLS support**: if broker port is 8883/8884, uses `WiFiClientSecure` with `setCACert(MQTT_ROOT_CA)`. **Fail-closed**: if `MQTT_ROOT_CA` is empty in production mode, firmware refuses to connect (no `setInsecure()` fallback).
- **Production guard** (R10A-5): port 8883/8884 requires ALL of: `MQTT_BROKER_USERNAME`, `MQTT_BROKER_PASSWORD`, `MQTT_ROOT_CA`. Missing any → hard fail.
- **ACK transaction layer** (R10E-1: atomic publish+store):
  - Every command includes `requestId` (UUID)
  - Validation order (R10E-2): parse → validate type → validate fields (whitelist) → compute hash → dedup check → execute → ACK
  - Dedup buffer: 64 entries + 15min TTL (R10E-3)
  - On duplicate: replay ORIGINAL ACK JSON (not reconstructed — R10D-2/R10E-1)
  - requestId reuse with different command → rejected (R10A-3)
  - Unknown fields → rejected (R10D-3)
  - SET_STATE only (no TOGGLE, for idempotency)
- **LWT** (Last Will & Testament): publishes `0` to `online` topic on disconnect → PWA shows offline status
- **Status publishing**: every 5s, publishes full SystemStatus JSON to `status` topic (retained=false, QoS 1)

### 2. PZEM-004T v3.0 Power Meter (`PzemDriver.cpp`)

Self-contained Modbus-RTU implementation (no external library):

- **Reads every 1s** via UART2 (GPIO4/5, 9600 baud)
- **7 raw parameters**: voltage, current, active power, energy, frequency, power factor, alarm status
- **3 derived parameters**:
  - Apparent Power (VA) = V × A
  - Reactive Power (VAR) = √(VA² − W²)
  - Daily Energy (kWh) — accumulated since midnight, auto-reset on day change
- **Daily stats**: max power, average power, energy today
- **5 alarm thresholds** (configurable in `Config.h`):
  - Undervoltage (< 200V), Overvoltage (> 240V)
  - Overcurrent (> 16A), Overpower (> 3500W)
  - Low power factor (< 0.85)
- **Cooldown**: 60s between same-alarm repeats (prevents log spam)
- **CRC-16 (Modbus)** validation on every response — corrupt frames are dropped

### 3. Scheduler (`Scheduler.cpp`)

- **Per-channel schedules** (max 4 per channel × 12 channels = 48 max)
- **dayMask**: bitmask of 7 days (bit 0 = Sunday, bit 6 = Saturday)
- **Overnight handling**: if `onTime > offTime` (e.g., 18:00 → 06:00), schedule wraps midnight
- **1-minute resolution**: schedules checked every loop iteration against current RTC time
- **Conflict resolution**: if multiple schedules for same channel overlap, latest-wins (manual review recommended)

### 4. Relay Engine Priority (`RelayEngine.cpp`)

```
For each channel i (1..12):
  1. If channels[i].modeAuto == false:
       → Manual mode: relay = channels[i].manualState (source: 'manual' or 'off')
  2. Else if i has PIR (9..12) AND channels[i].pirEnabled:
       → If PIR stuck (HIGH > 30 min): ignore PIR, fall through to schedule
       → Else if PIR motion OR within hold-time window: relay = ON (source: 'pir')
       → Else if schedule active: relay = ON (source: 'schedule')
       → Else: OFF
  3. Else (no PIR or PIR disabled):
       → If schedule active: relay = ON (source: 'schedule')
       → Else: OFF
```

- PIR can only force ON, never force OFF
- PIR cannot override Manual mode
- Stuck PIR (HIGH > 30 min) is force-disabled for 5 min cooldown, then re-tested
- Source tracking (`Manual` / `Pir` / `Schedule` / `Off`) is exposed in `/api/status` for PWA UI

### 5. Auth & Security (`AuthManager.cpp`)

- **JWT (HS256)**: 1-hour TTL, signed with device-generated secret (stored in NVS)
- **CSRF**: separate `timer12_csrf` cookie (readable by JS, httpOnly=false), echoed in `X-CSRF-Token` header for mutations
- **Cookie flags**: `Secure` (in production), `SameSite=Strict`, `Path=/`
- **Rate limiter** (per IP): 5 fails → 60s block, 10 fails → 5 min block
- **Factory reset**: 2-step (prepare → confirm), one-time token (60s TTL)
- **Password storage**: PBKDF2-HMAC-SHA256 (10000 iterations, 16-byte salt)

### 6. GAS AI Pipeline (`Advisor.cpp`)

Every 1 hour (configurable via `GAS_POST_INTERVAL_MS` in `Config.h`):

1. Reads last 50 entries from `/activity.log` (LittleFS)
2. Collects PZEM data (voltage, current, power, energy, frequency, PF, etc.)
3. Computes **anonymous device ID** = `SHA-256(MAC).substring(0, 16)` — Gemini never sees the real MAC
4. Builds JSON payload (~4-12 KB) and POSTs to GAS URL
5. **Watchdog-safe**: HTTP timeout 8s (watchdog is 10s), `esp_task_wdt_reset()` called before, during (every 50 lines of file read), and after HTTP

---

## Google Apps Script — Deployment

`code.gs/Code.gs` is a single-file Google Apps Script that receives ESP32 logs and calls Gemini.

### Setup

1. Open **[script.google.com](https://script.google.com)** → New Project
2. Delete the default `Code.gs` content, paste the contents of `code.gs/Code.gs` from this repo
3. Set the Gemini API key:
   - Project Settings → Script Properties → Add property
   - Name: `GEMINI_API_KEY`
   - Value: your Gemini API key (get one free at **[aistudio.google.com](https://aistudio.google.com/app/apikey)**)
4. Deploy → New Deployment:
   - Type: **Web App**
   - Execute as: **Me**
   - Who has access: **Anyone** (anonymous — required because ESP32 has no Google auth)
5. Copy the deployment URL (e.g., `https://script.google.com/macros/s/AKfyc.../exec`)

### Wire the URL to ESP32 + PWA

- **ESP32 firmware** (`Config.h`):
  ```cpp
  #define GAS_INSIGHTS_URL "https://script.google.com/macros/s/AKfyc.../exec"
  ```
- **PWA** (Vercel env var):
  ```
  NEXT_PUBLIC_GAS_INSIGHTS_URL=https://script.google.com/macros/s/AKfyc.../exec
  ```

### Endpoints

| Method | URL                                       | Purpose                                      |
|--------|-------------------------------------------|----------------------------------------------|
| GET    | `?action=insights&mac=<anonymousId>`      | Fetch cached AI insights (1h TTL)            |
| GET    | `?action=health`                          | Service health + Gemini config check         |
| POST   | (root)                                    | ESP32 pushes logs → triggers Gemini analysis |

### Privacy & Safety

- ESP32 sends only the **first 16 chars of SHA-256(MAC)** — never the raw MAC. Gemini cannot reverse this to identify the device hardware address.
- Logs contain channel names, relay states, and event messages — review these before deploying if you have privacy-sensitive channel names (e.g., "Bedroom Lights").
- Gemini API key is stored in **Script Properties** (not in source code) — safe to commit this repo publicly.
- 1-hour cache TTL prevents Gemini API abuse (max 24 Gemini calls per device per day).
- All Gemini responses are strictly JSON-validated; malformed responses fall back to mock insights (no crash).

---

## Firmware v4.0 API Contract

All responses follow: `{ "success": bool, "message": string, "data": T }`.

| Method | Endpoint                              | Purpose                                  |
|--------|---------------------------------------|------------------------------------------|
| POST   | `/api/login`                          | JWT + CSRF token + cookies               |
| POST   | `/api/logout`                         | Clear session cookies                    |
| GET    | `/api/session`                        | Check current session                    |
| GET    | `/api/status`                         | Full SystemStatus (12 channels + PIRs)   |
| GET    | `/api/version`                        | FirmwareInfo + OTA status                |
| GET    | `/api/health`                         | Hardware diagnostics                     |
| POST   | `/api/relay`                          | SET_STATE on/off / set_mode (no TOGGLE)  |
| POST   | `/api/schedule`                       | Upsert schedule (per-channel, max 4)     |
| DELETE | `/api/schedule?id=N`                  | Delete schedule                          |
| POST   | `/api/pir`                            | Update PIR config (enabled / holdTime)   |
| POST   | `/api/pir/test`                       | Manual test trigger                      |
| POST   | `/api/time`                           | Set RTC time                             |
| GET    | `/api/log?type=&channelId=&limit=`    | Filterable activity log (JSON)           |
| GET    | `/api/audit_log`                      | Plain-text audit log                     |
| GET    | `/api/config`                         | User + device info                       |
| POST   | `/api/config`                         | Update username / password (legacy)      |
| POST   | `/api/config/device`                  | Update device name / timezone            |
| POST   | `/api/config/password`                | Change password (verify current)         |
| GET    | `/api/config/export`                  | Full backup JSON                         |
| POST   | `/api/config/import`                  | Restore from backup JSON                 |
| POST   | `/api/reboot`                         | Reboot ESP32                             |
| POST   | `/api/ota`                            | Upload firmware binary                   |
| POST   | `/api/ota/check`                      | Check GitHub Release for newer firmware  |
| POST   | `/api/factory_reset/prepare`          | Generate one-time reset token (60s)      |
| POST   | `/api/factory_reset/confirm`          | Execute factory reset                    |

---

## Security Audit Notes

This firmware has been through 8 rounds of security audit by an external engineer. Key hardening already applied:

### Already Fixed

- ✅ **MQTT transaction ACK layer** (PWA side): UUID requestId, 5s timeout, deep schema validation, settle-once connect pattern, subscribe-before-resolve
- ✅ **Firmware requestId dedup**: ring buffer of 16, re-ACKs duplicates (does NOT re-execute) — safe for at-least-once MQTT delivery
- ✅ **Idempotent relay control**: `SET_STATE ON/OFF` only (no `TOGGLE`) — retried commands converge to same state
- ✅ **JWT secret never leaks**: stored in NVS, not in any API response or log
- ✅ **CSRF double-submit cookie**: sameSite=Strict, constant-time compare
- ✅ **Factory reset 2-step**: prepare → confirm with one-time token (60s TTL)
- ✅ **Watchdog-safe GAS POST**: HTTP timeout 8s, `esp_task_wdt_reset()` during file reads and HTTP
- ✅ **Anonymous device ID for Gemini**: SHA-256(MAC) prefix, never raw MAC
- ✅ **PZEM alarm cooldown**: 60s between same-alarm repeats (prevents log spam)
- ✅ **CRC validation on PZEM Modbus**: corrupt frames dropped silently
- ✅ **Boot glitch fix**: relay pins pulled HIGH during boot (active-LOW relays stay OFF until explicitly driven)

### Known Limitations (updated Round 10E)

- ⚠️ **MQTT broker**: HiveMQ public broker (port 1883, anonymous) is the DEFAULT for development only. For production 220V relay control, deploy self-hosted Mosquitto with TLS (port 8883) + ACL + per-device credentials. Set `MQTT_BROKER_HOST`, `MQTT_BROKER_PORT=8883`, `MQTT_BROKER_USERNAME`, `MQTT_BROKER_PASSWORD`, `MQTT_ROOT_CA` in `Config.h`. Firmware will hard-fail if any is missing in production mode (R10A-5).
- ⚠️ **Ed25519 build verification**: PSA Crypto API identifiers are correct (R10D-1), but actual build + known-answer test on Arduino IDE 2.3.8 + ESP32 core 3.3.7 has NOT been verified by the developer. If `PSA_ECC_FAMILY_TWISTED_EDWARDS` is not defined, enable via menuconfig → mbedTLS → Elliptic Curve DH/DSA.
- ⚠️ **CORS**: `ALLOWED_CORS_ORIGINS = "*"` is the DEFAULT (development). For production, set to your PWA's Vercel URL in `Config.h`.
- ⚠️ **OTA signing**: `OTA_ED25519_PUBLIC_KEY_HEX` and `OTA_HTTPS_ROOT_CA` are empty by default. OTA will hard-fail until these are configured. Generate keypair with `scripts/sign_firmware.py --gen-keys`.
- ⚠️ **MQTT topic password**: REMOVED from topic in R10C-3. Auth is now via broker credentials. The `mqttPass` value from Serial Monitor is kept for backward compat but NOT used in topic path.

See `firmware/CONTRACT_VERIFICATION.md` (not included in this repo — ask the project owner) for the full audit trail.

---

## Companion PWA Dashboard

The web UI lives in a separate repo: **[desvandi/Remote-Relay](https://github.com/desvandi/Remote-Relay)**

- Next.js 16 PWA with App Router, TypeScript 5, Tailwind 4, shadcn/ui
- Deployed on Vercel (free tier)
- Two login modes:
  - **LAN/REST mode**: calls ESP32 REST API via Cloudflare Tunnel (fast, same-WiFi)
  - **MQTT remote mode**: connects directly to MQTT broker over WSS (works from anywhere, no port forwarding)

---

## License

Proprietary — built per the Timer Digital Relay v4.0 Engineering Brief. Contact the repo owner for licensing questions.
