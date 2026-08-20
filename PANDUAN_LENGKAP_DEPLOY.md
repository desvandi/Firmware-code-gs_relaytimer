# PANDUAN LENGKAP DEPLOY — Timer Digital Relay v4.3

> **Biaya total: Rp 0/bulan** — HiveMQ Cloud Free + Vercel Free + Google Apps Script Free + GitHub Free
>
> **Semua fitur aktif:** relay control, scheduler, PIR, PZEM, battery monitoring (8S LiFePO4), OTA, AI insights

---

## DAFTAR ISI

1. [Persiapan](#1-persiapan)
2. [Setup HiveMQ Cloud Free](#2-setup-hivemq-cloud-free)
3. [Install PlatformIO + Download Firmware](#3-install-platformio--download-firmware)
4. [Konfigurasi ESP32 (Config.h)](#4-konfigurasi-esp32-configh)
5. [Build & Flash Firmware](#5-build--flash-firmware)
6. [Deploy PWA ke Vercel](#6-deploy-pwa-ke-vercel)
7. [Konfigurasi PWA Environment Variables](#7-konfigurasi-pwa-environment-variables)
8. [Setup Google Apps Script (AI Insights)](#8-setup-google-apps-script-ai-insights)
9. [Generate OTA Ed25519 Keys](#9-generate-ota-ed25519-keys)
10. [First Boot ESP32 + Provisioning](#10-first-boot-esp32--provisioning)
11. [Verifikasi Sistem](#11-verifikasi-sistem)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. PERSIAPAN

### Yang dibutuhkan:

| Item | Wajib? | Keterangan |
|---|---|---|
| ESP32-WROOM-32 | ✅ | Board development (38-pin) |
| 12-channel relay module (active-LOW) | ✅ | 5V relay dengan optocoupler |
| DS3231 RTC module | ✅ | + battery CR2035 |
| PZEM-004T v3.0 | ✅ | AC power meter |
| 4× PIR sensor HC-SR501 | ✅ | Motion sensor |
| INA219 ×2 | Untuk battery monitoring | External shunt 0.75mΩ |
| ADS1115 ×2 | Untuk battery monitoring | 4-channel 16-bit ADC |
| SHT31 | Untuk ambient monitoring | Temperature + humidity |
| 8S LiFePO4 battery pack | Untuk battery monitoring | Nominal 24V |
| USB cable (micro-USB) | ✅ | Untuk flash + monitor |
| WiFi network | ✅ | 2.4GHz (ESP32 tidak support 5GHz) |
| Akun GitHub | ✅ | Untuk download code |
| Akun Vercel (gratis) | ✅ | Hosting PWA |
| Akun HiveMQ Cloud (gratis) | ✅ | MQTT broker |
| Akun Google | ✅ | GAS + Gemini API |
| Komputer/laptop | ✅ | Mac, Windows, atau Linux |

### Software yang diinstall:

```bash
# 1. Python 3 (sudah ada di Mac, install dari python.org untuk Windows)
python3 --version  # harus >= 3.8

# 2. PlatformIO
pip install platformio
pio --version  # verifikasi

# 3. Git (sudah ada di Mac, install dari git-scm.com untuk Windows)
git --version

# 4. Node.js + Bun (untuk PWA)
# Install Node.js dari nodejs.org (LTS version)
# Install Bun:
curl -fsSL https://bun.sh/install | bash

# 5. Vercel CLI (opsional, bisa juga via dashboard)
npm install -g vercel
```

---

## 2. SETUP HIVEMQ CLOUD FREE

### 2.1 Daftar Akun

1. Buka https://www.hivemq.com/cloud/
2. Klik **"Sign Up for Free"**
3. Isi email + password (tidak butuh credit card)
4. Verifikasi email

### 2.2 Buat Cluster

1. Login ke dashboard HiveMQ Cloud
2. Klik **"Create Cluster"**
3. Pilih:
   - Cloud Provider: **AWS** (atau GCP, pilih yang terdekat dengan lokasi Anda)
   - Region: **eu-central-1** (Frankfurt) atau **us-east-1** (Virginia)
   - Plan: **Free** (100 connections, 10MB/bulan)
4. Beri nama cluster: `timer12`
5. Klik **"Create"**
6. Tunggu 1-5 menit sampai status = **Running** ✅

### 2.3 Catat Broker URL

Di dashboard cluster, pada bagian **"Connection Information"**, catat:

```
Broker ID:        8ded4ffaf23949459f4727ba1b83df52
MQTT URL (TLS):   8ded4ffaf23949459f4727ba1b83df52.s1.eu.hivemq.cloud:8883
WebSocket (WSS):  8ded4ffaf23949459f4727ba1b83df52.s1.eu.hivemq.cloud:8884/mqtt
```

**Yang Anda butuhkan:**

| Field | Nilai | Dipakai di |
|---|---|---|
| Host | `8ded4ffaf23949459f4727ba1b83df52.s1.eu.hivemq.cloud` | ESP32 Config.h |
| Port TLS | `8883` | ESP32 Config.h |
| Port WSS | `8884` | PWA Vercel |
| WSS URL | `wss://8ded4ffaf23949459f4727ba1b83df52.s1.eu.hivemq.cloud:8884/mqtt` | PWA Vercel |

### 2.4 Buat MQTT Credentials

Di dashboard HiveMQ Cloud:

1. Masuk ke cluster `timer12`
2. Tab **"Access Management"** → **"Credentials"**
3. Klik **"Create Credentials"**

**Buat 2 user:**

**User 1 — ESP32:**
```
Username:   timer12-esp32
Password:   12345678Qwertyuiop
Permission: Publish and Subscribe
```

**User 2 — PWA:**
```
Username:   pwa-frontend
Password:   12345678Qwertyuiop
Permission: Publish and Subscribe
```

> **Catatan keamanan:** Untuk production sebenarnya, gunakan password yang berbeda untuk ESP32 dan PWA. Dan buat credential per-device (timer12-esp32-A1B2C3, timer12-esp32-D4E5F6, dst).

### 2.5 Dapatkan Root CA Certificate

HiveMQ Cloud menggunakan Let's Encrypt untuk TLS. Root CA = **ISRG Root X1**.

Salin certificate berikut (ini adalah ISRG Root X1 standar):

```
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qy
HB5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+U
CB5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
```

Simpan ini — akan dipakai di ESP32 Config.h.

---

## 3. INSTALL PLATFORMIO + DOWNLOAD FIRMWARE

### 3.1 Install PlatformIO

```bash
# Mac/Linux:
pip3 install platformio

# Windows (PowerShell):
pip install platformio

# Verifikasi:
pio --version
# Output: PlatformIO Core, version 6.x.x
```

### 3.2 Download Firmware

```bash
# Clone repository
git clone https://github.com/desvandi/Firmware-code-gs_relaytimer.git
cd Firmware-code-gs_relaytimer/firmware
```

### 3.3 Verifikasi Struktur File

```bash
ls
# Output harus mencakup:
# Config.h          platformio.ini     firmware_v4.ino
# RelayDriver.cpp   MqttClient.cpp     StatusHandlers.h
# BatteryConfig.h   Ina219Driver.cpp  Ads1115Driver.cpp
# Sht31Driver.cpp  BatteryMonitor.cpp partitions_ota_1mb5.csv
# ... dan file lainnya
```

---

## 4. KONFIGURASI ESP32 (Config.h)

Buka file `Config.h` di text editor (VS Code, nano, atau editor lainnya):

```bash
nano Config.h
```

### 4.1 Cari dan ubah baris-barbar berikut:

**MQTT Broker (sekitar baris 130-140):**
```cpp
// Ganti dari:
constexpr const char* MQTT_BROKER_HOST = "broker.hivemq.com";
constexpr uint16_t MQTT_BROKER_PORT = 1883;
constexpr const char* MQTT_BROKER_USERNAME = "";
constexpr const char* MQTT_BROKER_PASSWORD = "";

// Menjadi:
constexpr const char* MQTT_BROKER_HOST = "8ded4ffaf23949459f4727ba1b83df52.s1.eu.hivemq.cloud";
constexpr uint16_t MQTT_BROKER_PORT = 8883;  // TLS
constexpr const char* MQTT_BROKER_USERNAME = "timer12-esp32";
constexpr const char* MQTT_BROKER_PASSWORD = "12345678Qwertyuiop";
```

**TLS Root CA (sekitar baris 145):**
```cpp
// Ganti dari:
constexpr const char* MQTT_ROOT_CA = "";

// Menjadi (copy SEMUA baris dari Root CA di Langkah 2.5):
constexpr const char* MQTT_ROOT_CA =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
  "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJl\n"
  "c2VhcmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwWTATBgcqhkjO\n"
  "PQIBBggqhkjOPQMBBwNCAAS2zPnzgK5KKh+r/0hbFjFl2RovUCnIYcGc9Lm\n"
  "... (paste SEMUA baris dari certificate ISRG Root X1) ...\n"
  "-----END CERTIFICATE-----\n";
```

> **Penting:** Setiap baris certificate harus di-wrap dalam tanda kutip `"..."` dan diakhiri dengan `\n`. Tanpa `\n`, certificate tidak valid.

**CORS Origin (sekitar baris 155):**
```cpp
// Ganti dari:
constexpr const char* ALLOWED_CORS_ORIGINS = "*";

// Menjadi URL PWA Anda di Vercel (setelah deploy di Langkah 6):
constexpr const char* ALLOWED_CORS_ORIGINS = "https://remote-relay.vercel.app";
```

> Jika URL Vercel Anda berbeda (contoh: `https://timer-relay.vercel.app`), sesuaikan.

**GAS URL (sekitar baris 165):**
```cpp
// Biarkan kosong dulu, isi setelah setup GAS di Langkah 8:
constexpr const char* GAS_INSIGHTS_URL = "";
```

### 4.2 Verifikasi platformio.ini

Pastikan file `platformio.ini` sudah berisi:

```ini
board_build.partitions = partitions_ota_1mb5.csv
build_flags =
  ${common.build_flags}
  -DDEVELOPMENT_BUILD
  -DBATTERY_MONITORING_ENABLED
```

> `-DBATTERY_MONITORING_ENABLED` wajib ada — tanpa ini, battery monitoring tidak compile dan ~200KB flash tidak terpakai.

### 4.3 Simpan Config.h

```bash
# Simpan (jika pakai nano: Ctrl+O, Enter, Ctrl+X)
```

---

## 5. BUILD & FLASH FIRMWARE

### 5.1 Hubungkan ESP32 ke Komputer

- Colokkan USB cable ke ESP32
- Pastikan kabel mendukung data (bukan charge-only)
- Catat port serial:
  - **Mac:** `/dev/cu.SLAB_USBtoUART` atau `/dev/cu.usbserial-XXXX`
  - **Windows:** `COM3` (atau COM4, COM5 — cek di Device Manager)
  - **Linux:** `/dev/ttyUSB0`

### 5.2 Build Firmware

```bash
cd Firmware-code-gs_relaytimer/firmware

# Build untuk development (HiveMQ Cloud Free):
pio run -e development
```

Output yang benar:
```
Processing development (platform: espressif32@^6.5.0; board: esp32dev)
RAM:   22.5% (used 73780 bytes from 327680 bytes)
Flash: ~75%  (used ~1350000 bytes from 1572864 bytes)
========================= [SUCCESS] ==========================
```

Jika **FAILED**: lihat [Troubleshooting](#12-troubleshooting).

### 5.3 Flash ke ESP32

```bash
pio run -e development -t upload
```

Tunggu sampai muncul:
```
Writing at 0x00010000 — (5%) ...
Writing at 0x00058000 — (25%) ...
Writing at 0x000c0000 — (50%) ...
...
Hash of data verified.
Hard resetting via RTS pin...
========================= [SUCCESS] ==========================
```

### 5.4 Monitor Serial (First Boot)

```bash
pio device monitor -e development
```

Output yang akan muncul:

```
========================================
Timer 12 Relay v4.3.8
Build: Aug 20 2026 10:00:00
========================================
[WiFi] Starting (STA primary, AP fallback)
[WiFi] Connected! IP: 192.168.1.50
[MQTT] Using TLS (port 8883)
[MQTT] topics: status=timer12/AABBCCDDEEFF/status
[MQTT] connected!

Anonymous Device ID: AABBCCDDEEFF1234
JWT Secret: <64 hex chars>
MQTT Topic Password: <8 chars>
GAS HMAC Secret: <64 hex chars>
Device PIN: <6 digits>

Boot complete. Ready.
```

**CATAT SEMUA SECRET INI** — simpan di password manager atau catatan aman. Secret ini hanya muncul sekali.

| Secret | Dipakai untuk | Simpan di mana |
|---|---|---|
| Anonymous Device ID | GAS Script Property | Catatan aman |
| JWT Secret | REST mode auth (opsional) | Catatan aman |
| MQTT Topic Password | Tidak dipakai (HiveMQ Cloud pakai username/password) | Catatan aman |
| GAS HMAC Secret | GAS Script Property `DEVICE_<id>_SECRET` | Catatan aman |
| Device PIN | PWA pairing (future) | Catatan aman |

Tekan `Ctrl+C` untuk keluar dari monitor.

---

## 6. DEPLOY PWA KE VERCEL

### 6.1 Download PWA Code

```bash
# Di terminal baru
git clone https://github.com/desvandi/Remote-Relay.git
cd Remote-Relay
```

### 6.2 Install Dependencies

```bash
bun install
```

### 6.3 Deploy ke Vercel

**Opsi A: Via Vercel CLI:**

```bash
vercel
# Pilih:
#   Set up and deploy? → Y
#   Which scope? → pilih akun Anda
#   Project name? → remote-relay (atau nama lain)
#   Framework preset? → Next.js
#   Root directory? → ./
#   Override settings? → N
```

Setelah deploy, catat URL: `https://remote-relay.vercel.app` (atau URL yang diberikan).

**Opsi B: Via Vercel Dashboard:**

1. Buka https://vercel.com → New Project
2. Import repository `Remote-Relay` dari GitHub
3. Framework Preset: **Next.js**
4. Klik **Deploy**
5. Tunggu sampai status = **Ready**
6. Catat URL produksi

### 6.4 Update CORS di ESP32 (jika URL berbeda)

Jika URL Vercel Anda bukan `https://remote-relay.vercel.app`, update `Config.h`:

```cpp
constexpr const char* ALLOWED_CORS_ORIGINS = "https://url-anda.vercel.app";
```

Rebuild + reflash:
```bash
cd Firmware-code-gs_relaytimer/firmware
pio run -e development -t upload
```

---

## 7. KONFIGURASI PWA ENVIRONMENT VARIABLES

### 7.1 Buka Vercel Dashboard

1. https://vercel.com → pilih project **Remote-Relay**
2. Settings → Environment Variables
3. Tambahkan setiap variable berikut:

### 7.2 Variable yang perlu di-set:

| Variable | Value | Environment |
|---|---|---|
| `NEXT_PUBLIC_API_BASE_URL` | *(kosong)* | Production + Preview |
| `NEXT_PUBLIC_MQTT_BROKER_URL` | `wss://8ded4ffaf23949459f4727ba1b83df52.s1.eu.hivemq.cloud:8884/mqtt` | Production + Preview |
| `NEXT_PUBLIC_MQTT_USERNAME` | `pwa-frontend` | Production + Preview |
| `NEXT_PUBLIC_MQTT_PASSWORD` | `12345678Qwertyuiop` | Production + Preview |
| `NEXT_PUBLIC_GAS_INSIGHTS_URL` | *(kosong dulu — isi setelah Langkah 8)* | Production |
| `DEMO_MODE` | *(kosong)* | All |
| `NEXT_PUBLIC_DEMO_MODE` | *(kosong)* | All |
| `JWT_SECRET` | *(kosong — tidak dipakai di MQTT mode)* | All |

### 7.3 Redeploy

Setelah set semua variables:
1. Vercel Dashboard → Deployments → pilih yang terbaru
2. Klik **"..." → Redeploy**
3. Tunggu sampai status = **Ready**

### 7.4 Verifikasi PWA

Buka URL Vercel di browser:
1. Login page muncul
2. Klik login → dashboard muncul
3. Cek: status ESP32 muncul (relay OFF, uptime, WiFi RSSI)
4. Jika status tidak muncul: lihat [Troubleshooting](#12-troubleshooting)

---

## 8. SETUP GOOGLE APPS SCRIPT (AI INSIGHTS)

### 8.1 Buat Project GAS

1. Buka https://script.google.com → **New Project**
2. Beri nama: `Timer12 AI Insights`
3. Hapus kode default, paste **SEMUA** isi file `code.gs/Code.gs` dari firmware repo

### 8.2 Set Script Properties

Di GAS editor:
1. Project Settings (gear icon di kiri)
2. Scroll ke **"Script Properties"**
3. Tambahkan properties:

| Property name | Value |
|---|---|
| `GEMINI_API_KEY` | Dari https://aistudio.google.com (klik "Get API Key") |
| `DEVICE_AABBCCDDEEFF1234_SECRET` | 64 hex chars dari Serial Monitor ESP32 (Langkah 5.4) |

> Ganti `AABBCCDDEEFF1234` dengan Anonymous Device ID yang muncul di Serial Monitor ESP32 Anda.

### 8.3 Deploy GAS Web App

1. Klik **Deploy → New Deployment**
2. Type: **Web app**
3. Execute as: **Me**
4. Who has access: **Anyone** (anonymous — HMAC provides auth)
5. Klik **Deploy**
6. Authorize permissions jika diminta
7. Copy URL deployment: `https://script.google.com/macros/s/AKfyc.../exec`

### 8.4 Update Config.h + Vercel dengan GAS URL

**ESP32 Config.h:**
```cpp
constexpr const char* GAS_INSIGHTS_URL = "https://script.google.com/macros/s/AKfyc.../exec";
```

Rebuild + reflash:
```bash
cd Firmware-code-gs_relaytimer/firmware
pio run -e development -t upload
```

**Vercel Environment Variables:**
```
NEXT_PUBLIC_GAS_INSIGHTS_URL=https://script.google.com/macros/s/AKfyc.../exec
```

Redeploy PWA.

---

## 9. GENERATE OTA ED25519 KEYS (opsional — untuk OTA updates)

> Skip bagian ini jika belum butuh OTA. Bisa setup nanti.

### 9.1 Generate Key Pair

```bash
cd Firmware-code-gs_relaytimer/scripts

# Generate keypair (sekali saja):
python3 sign_firmware.py --gen-keys

# Output:
# Private key: ota_private_key.pem  → SIMPAN DI AMAN, jangan commit ke git!
# Public key:  ota_public_key.pem
# Public key hex: a1b2c3d4...  (64 hex chars)
```

### 9.2 Update Config.h

```cpp
constexpr const char* OTA_ED25519_PUBLIC_KEY_HEX = "a1b2c3d4...";  // 64 hex chars dari output
```

### 9.3 OTA HTTPS Root CA

```cpp
// DigiCert Global Root CA (untuk GitHub Releases HTTPS)
constexpr const char* OTA_HTTPS_ROOT_CA =
  "-----BEGIN CERTIFICATE-----\n"
  "...\n"
  "-----END CERTIFICATE-----\n";
```

Download dari: https://dl.digicert.com/DigiCertGlobalRootCA.crt

### 9.4 Sign Firmware Binary

```bash
# Build dulu:
cd ../firmware
pio run -e development

# Sign binary:
cd ../scripts
python3 sign_firmware.py ../firmware/.pio/build/development/firmware.bin 4.3.8

# Output:
# firmware.bin.sha256
# firmware.bin.sig
# firmware.bin.ota.json
```

Upload `firmware.bin` + `.sha256` + `.sig` + `.ota.json` ke GitHub Releases.

---

## 10. FIRST BOOT ESP32 + PROVISIONING

### 10.1 First Boot Sequence

Setelah flash (Langkah 5.3), ESP32 akan:

1. Boot → print banner
2. Coba connect WiFi (jika sudah dikonfigurasi)
3. Jika WiFi belum dikonfigurasi → buka AP `Timer12-Setup`:
   - Hubungkan HP/komputer ke WiFi `Timer12-Setup`
   - Browse ke `http://192.168.4.1`
   - Masukkan WiFi SSID + password
   - ESP32 reboot → join WiFi

4. Setelah WiFi connected → MQTT connect ke HiveMQ Cloud
5. Generate secrets (JWT, MQTT password, GAS secret, PIN)
6. Print semua secret ke Serial Monitor
7. Mark boot healthy → siap pakai

### 10.2 Verifikasi ESP32 → PWA Connection

1. Buka PWA di browser: `https://remote-relay.vercel.app`
2. Login
3. Dashboard harus menampilkan:
   - ESP32 status: **ONLINE** ✅
   - Relay channels (12 channels)
   - PZEM power meter (jika sensor terhubung)
   - Battery monitoring (jika sensor terhubung)
   - Uptime, WiFi RSSI, free heap
4. Toggle salah satu relay → ESP32 ACK → status berubah real-time

---

## 11. VERIFIKASI SISTEM

### 11.1 Test Relay Control

1. Di PWA dashboard, klik relay card CH1
2. Klik "ON" → relay harus klik (fisik) + status berubah jadi ON
3. Klik "OFF" → relay mati + status OFF

### 11.2 Test Scheduler

1. Buka tab Scheduler di PWA
2. Tambah schedule: CH1, ON 18:00, OFF 06:00, setiap hari
3. Saat jam 18:00 → relay otomatis ON
4. Saat jam 06:00 → relay otomatis OFF

### 11.3 Test PIR (jika terhubung)

1. Buka tab PIR di PWA
2. Enable PIR 1
3. Wave tangan di depan PIR sensor → relay ON
4. Tunggu hold time (default 120s) → relay OFF

### 11.4 Test Battery Monitoring (jika sensor terhubung)

1. Dashboard harus menampilkan section "DC Energy & Battery Monitoring"
2. Pack Voltage (~26V untuk 8S LiFePO4)
3. 8 cell voltages (~3.3V per cell)
4. Battery current (signed — negative saat charging)
5. Power flow diagram (MPPT → Battery → Inverter)
6. Ambient temperature + humidity (SHT31)

### 11.5 Test AI Insights (jika GAS terkonfigurasi)

1. Tunggu ~1 jam (ESP32 POST ke GAS tiap 1 jam)
2. Buka tab AI Insights di PWA
3. Card insights dari Gemini AI muncul

---

## 12. TROUBLESHOOTING

### ESP32 tidak connect WiFi

```bash
# Monitor serial:
pio device monitor -e development

# Jika muncul: "WiFi: Config Portal"
# → Hubungkan ke AP "Timer12-Setup" → 192.168.4.1 → masukkan WiFi credentials
```

### ESP32 tidak connect MQTT

```bash
# Cek di monitor:
# [MQTT] connect failed, state=-2  → TLS connection failed
#   → Cek Root CA (harus copy paste tepat dengan \n)
#   → Cek broker host (harus persis dari HiveMQ dashboard)
#   → Cek port (8883 untuk TLS)

# [MQTT] connect failed, state=5  → Connection refused (not authorized)
#   → Cek username/password (harus persis dari HiveMQ credentials)

# [MQTT] connect failed, state=-1  → Connection timeout
#   → Cek WiFi connected
#   → Cek DNS resolve (broker host bisa di-resolve)
```

### PWA tidak menerima data dari ESP32

1. Cek ESP32 MQTT connected (via Serial Monitor)
2. Cek Vercel env vars (NEXT_PUBLIC_MQTT_BROKER_URL dll)
3. Cek browser console (F12) — error MQTT connection?
4. Cek HiveMQ Cloud dashboard — connection count?

### Firmware build FAILED

```bash
# Error: "Build profile not selected"
# → Pastikan platformio.ini ada di folder firmware/
# → Pastikan pakai: pio run -e development

# Error: "Sketch too big"
# → Pastikan partitions_ota_1mb5.csv ada di folder firmware/
# → Pastikan platformio.ini: board_build.partitions = partitions_ota_1mb5.csv

# Error: Library not found
# → Run: pio pkg install
```

### Port tidak terdeteksi (Mac)

```bash
# Install driver CP210x:
# Download dari https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
# Install → restart → cek:
ls /dev/cu.*
```

### Port tidak terdeteksi (Windows)

```bash
# Device Manager → Ports (COM & LPT)
# Cari "Silicon Labs CP210x" atau "USB Serial"
# Catat COM port number
# Upload: pio run -e development -t upload --upload-port COM3
```

---

## RINGKASAN KONFIGURASI AKHIR

### ESP32 Config.h:

```cpp
// MQTT Broker
constexpr const char* MQTT_BROKER_HOST = "8ded4ffaf23949459f4727ba1b83df52.s1.eu.hivemq.cloud";
constexpr uint16_t MQTT_BROKER_PORT = 8883;
constexpr const char* MQTT_BROKER_USERNAME = "timer12-esp32";
constexpr const char* MQTT_BROKER_PASSWORD = "12345678Qwertyuiop";

// TLS Root CA (ISRG Root X1 — Let's Encrypt)
constexpr const char* MQTT_ROOT_CA =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
  "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJl\n"
  "c2VhcmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwWTATBgcqhkjO\n"
  "PQIBBggqhkjOPQMBBwNCAAS2zPnzgK5KKh+r/0hbFjFl2RovUCnIYcGc9Lm\n"
  "... (copy SEMUA baris dari Root CA) ...\n"
  "-----END CERTIFICATE-----\n";

// CORS (URL PWA Anda di Vercel)
constexpr const char* ALLOWED_CORS_ORIGINS = "https://remote-relay.vercel.app";

// GAS (setelah deploy di Langkah 8)
constexpr const char* GAS_INSIGHTS_URL = "https://script.google.com/macros/s/AKfyc.../exec";
```

### PWA Vercel Environment Variables:

```env
NEXT_PUBLIC_API_BASE_URL=
NEXT_PUBLIC_MQTT_BROKER_URL=wss://8ded4ffaf23949459f4727ba1b83df52.s1.eu.hivemq.cloud:8884/mqtt
NEXT_PUBLIC_MQTT_USERNAME=pwa-frontend
NEXT_PUBLIC_MQTT_PASSWORD=12345678Qwertyuiop
NEXT_PUBLIC_GAS_INSIGHTS_URL=https://script.google.com/macros/s/AKfyc.../exec
DEMO_MODE=
NEXT_PUBLIC_DEMO_MODE=
JWT_SECRET=
```

### GAS Script Properties:

```
GEMINI_API_KEY = <API key dari https://aistudio.google.com>
DEVICE_AABBCCDDEEFF1234_SECRET = <64 hex chars dari Serial Monitor ESP32>
```

### Biaya:

| Komponen | Platform | Biaya/bulan |
|---|---|---|
| MQTT Broker | HiveMQ Cloud Free | Rp 0 |
| PWA Hosting | Vercel Free | Rp 0 |
| TLS Certificate | Let's Encrypt (via HiveMQ) | Rp 0 |
| AI Insights | Google Apps Script + Gemini | Rp 0 |
| Code + OTA | GitHub | Rp 0 |
| **Total** | | **Rp 0** |

### Limit HiveMQ Cloud Free:

- 100 koneksi simultan
- 10MB data/bulan
- Cukup untuk 1-5 ESP32 dengan status publish tiap 5 detik
