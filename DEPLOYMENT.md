# Deployment Guide — Timer Digital Relay v4.3.8

> Implements deployment requirements from the Industrial-Grade Implementation
> Directive §50, §80-84, §103.

---

## 1. Architecture Decision Tree

```
Do you need remote internet access?
├── NO (LAN only)
│   └── Use REST API + Cloudflare Tunnel for PWA → ESP32 LAN access
│       (No MQTT broker needed; PWA runs on Vercel)
└── YES (CGNAT-friendly, behind MiFi/router)
    └── Deploy self-hosted Mosquitto MQTT broker with TLS + ACL
        (PWA → MQTT broker → ESP32 via MQTT)

Do you need AI insights?
├── NO
│   └── Skip GAS deployment. PWA shows mock insights.
└── YES
    └── Deploy Google Apps Script Web App + configure Gemini API key
```

---

## 2. Production Deployment Checklist

### 2.1 Firmware Build

- [ ] Generate Ed25519 signing keypair: `python3 scripts/sign_firmware.py --gen-keys`
  - **NEVER commit the private key** — store on the signing machine only
- [ ] Paste the **public key** (64 hex chars) into `Config.h::OTA_ED25519_PUBLIC_KEY_HEX`
- [ ] Paste the **DigiCert/GlobalSign root CA** PEM into `Config.h::OTA_HTTPS_ROOT_CA`
  (for GitHub Releases HTTPS validation)
- [ ] Set `OTA_ALLOWED_HOSTS` to `github.com,raw.githubusercontent.com` (or your CDN)
- [ ] Configure `MQTT_BROKER_HOST` to your self-hosted broker domain
- [ ] Set `MQTT_BROKER_PORT` to `8883` (TLS)
- [ ] Set `MQTT_BROKER_USERNAME` / `MQTT_BROKER_PASSWORD` (per-device credential)
- [ ] Paste Let's Encrypt root CA PEM into `MQTT_ROOT_CA`
- [ ] Set `ALLOWED_CORS_ORIGINS` to your PWA's Vercel URL (NOT `*`)
- [ ] Set `GAS_INSIGHTS_URL` to your deployed GAS Web App URL (or leave empty to disable AI)
- [ ] Build: `pio run -e production` (uses `-DPRODUCTION_BUILD` flag — fail-closed)
- [ ] Sign the binary: `python3 scripts/sign_firmware.py firmware.bin 4.3.8`
- [ ] Upload signed binary + `.sha256` + `.sig` + `.ota.json` to GitHub Releases

### 2.2 MQTT Broker (Mosquitto)

- [ ] Provision VPS (Hetzner/DigitalOcean, ~Rp 75rb/bln)
- [ ] Install Mosquitto 2.x
- [ ] Configure `/etc/mosquitto/mosquitto.conf`:
  ```
  listener 8883
  allow_anonymous false
  password_file /etc/mosquitto/passwd
  acl_file /etc/mosquitto/acl
  certfile /etc/letsencrypt/live/broker.example.com/cert.pem
  keyfile /etc/letsencrypt/live/broker.example.com/privkey.pem
  cafile /etc/letsencrypt/live/broker.example.com/chain.pem
  ```
- [ ] Create per-device credentials: `mosquitto_passwd -b /etc/mosquitto/passwd timer12-A1A2B3 <password>`
- [ ] Configure ACL (`/etc/mosquitto/acl`):
  ```
  user timer12-A1A2B3
  topic write timer12/A1A2B3/status
  topic write timer12/A1A2B3/log
  topic write timer12/A1A2B3/online
  topic write timer12/A1A2B3/ack
  topic read timer12/A1A2B3/command
  topic read timer12/A1A2B3/ota
  ```
- [ ] Restart Mosquitto: `systemctl restart mosquitto`
- [ ] Verify TLS: `mosquitto_sub -h broker.example.com -p 8883 --cafile isrgrootx1.pem -u timer12-A1A2B3 -P <password> -t timer12/A1A2B3/status`

### 2.3 Google Apps Script

- [ ] Open https://script.google.com → New Project
- [ ] Paste the contents of `code.gs/Code.gs`
- [ ] Set Script Properties:
  - `GEMINI_API_KEY` = `<your Gemini API key>`
  - `DEVICE_<anonymousId>_SECRET` = `<64-hex from ESP32 Serial Monitor at first boot>`
  - (anonymousId = first 16 chars of SHA-256(MAC), printed on boot)
- [ ] Deploy → New Deployment → Type: Web App
  - Execute as: Me
  - Who has access: Anyone (anonymous — HMAC provides auth)
- [ ] Copy deployment URL → set as `GAS_INSIGHTS_URL` in `Config.h` and `NEXT_PUBLIC_GAS_INSIGHTS_URL` in Vercel

### 2.4 PWA (Vercel)

- [ ] Fork `desvandi/Remote-Relay` to your GitHub account
- [ ] Import to Vercel: https://vercel.com/new
- [ ] Configure Environment Variables:
  - `NEXT_PUBLIC_API_BASE_URL` = `https://your-tunnel.example.com` (Cloudflare Tunnel URL to ESP32 LAN)
  - `NEXT_PUBLIC_MQTT_BROKER_URL` = `wss://broker.example.com:8884/mqtt` (WebSocket TLS)
  - `NEXT_PUBLIC_MQTT_USERNAME` = `pwa-frontend` (NOT a device credential — separate broker account)
  - `NEXT_PUBLIC_MQTT_PASSWORD` = `<password>`
  - `NEXT_PUBLIC_GAS_INSIGHTS_URL` = `<GAS Web App URL>`
  - `JWT_SECRET` = `<32-byte random string>` (only for mock/demo mode — NEVER in production)
  - `MOCK_USER` / `MOCK_PASSWORD` = `<credentials>` (only for staging/demo, NEVER prod)
- [ ] Deploy
- [ ] Verify PWA loads at `https://your-app.vercel.app`
- [ ] Verify PWA installs (Add to Home Screen on Android/iOS)

### 2.5 ESP32 Provisioning

1. Flash firmware binary via USB: `pio run -e production -t upload`
   (Use production env to get `-DPRODUCTION_BUILD` flag)
2. On first boot, ESP32 enters WiFi Config Portal mode (AP `Timer12-Setup`)
3. Connect to AP, browse to `192.168.4.1`, enter WiFi SSID + password
4. ESP32 reboots → joins WiFi → prints to Serial:
   - MAC address
   - Anonymous device ID (first 16 chars of SHA-256(MAC))
   - JWT secret (64 hex chars)
   - MQTT topic password (8 chars)
   - GAS HMAC secret (64 hex chars)
   - Device PIN (6 digits)
5. Copy all secrets to safe storage (password manager)
6. Configure GAS Script Property `DEVICE_<anonymousId>_SECRET` = GAS HMAC secret
7. Configure MQTT broker account for this device (per-device username/password)
8. Verify PWA can connect (use device PIN to pair)

---

## 3. Reproducible Build (brief §84)

| Component | Version |
|---|---|
| PlatformIO Core | 6.x (pinned in `platformio.ini`) |
| ESP32 Arduino Core | 3.3.7 (pinned via `espressif32@^6.5.0`) |
| Compiler | GCC 11.x (bundled with ESP-IDF) |
| Node.js | 20.x LTS (for PWA) |
| Package Manager | Bun 1.1.x |
| Next.js | 16.1.x (pinned in package.json) |
| React | 19.0.x (pinned in package.json) |
| TypeScript | 5.x (pinned in package.json) |

Firmware production binary is reproducible IF:
- Same PlatformIO version
- Same ESP32 core version
- Same source tree (same commit SHA)
- Same `Config.h` settings (incl. `__DATE__`/`__TIME__` may differ — but OTA signing is over SHA-256 hash, so signature is deterministic for a given binary)

---

## 4. CI/CD (brief §82)

Recommended CI pipeline (GitHub Actions):

```yaml
# .github/workflows/firmware.yml
name: Firmware CI
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with: { python-version: '3.11' }
      - run: pip install platformio
      - run: pio run -e development      # syntax + compile check
      - run: pio run -e production        # production build (fail-closed)
      - name: Secret scan
        run: |
          pip install trufflehog
          trufflehog git file://. --only-verified
```

```yaml
# .github/workflows/pwa.yml
name: PWA CI
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: oven-sh/setup-bun@v2
      - run: bun install
      - run: bun run lint
      - run: bunx tsc --noEmit
      - run: bun run build
      - name: Secret scan
        run: npx trufflehog git file://. --only-verified
```

CI MUST fail on:
- Compile error
- Lint error
- TypeScript error
- Build failure
- Secret detection (PEM, private key, API key, MQTT password, JWT secret, HMAC secret) — brief §83
