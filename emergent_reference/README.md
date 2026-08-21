# Firmware & Google Apps Script — Relay Timer

Repo target: `Firmware-code-gs_relaytimer`

## Struktur
```
gas/Code.gs                        # Container-Bound Apps Script (Sheet-Driven)
firmware/relay_timer/relay_timer.ino  # ESP32 generic firmware (Captive Portal)
firmware/manifest.json             # ESP Web Tools flashing manifest
firmware/bin/                      # (tempatkan hasil compile: bootloader/partitions/app .bin)
```

## Google Apps Script (Zero-Touch)
1. Buat Google Sheet baru → Extensions → Apps Script → tempel `Code.gs`.
2. Reload sheet, jalankan menu **Relay Timer → Inisialisasi Master Template** (membuat tab `Config`, `State`, `Logs`).
3. Isi tab **Config** (AUTH_TOKEN, DEVICE_KEY, dll).
4. Deploy → **New deployment → Web app**: Execute as *Me*, Access *Anyone*.
5. Salin URL `/exec`.

Distribusi ke end-user via link **"Make a Copy"** sheet — tanpa mengedit kode.

### Kontrak Respon JSON
```json
{ "status":"SUCCESS|ERROR", "code":200, "data":{}, "message":"", "timestamp":"ISO" }
```
Actions: `PING`, `GET_STATUS`, `SET_RELAY`, `HEARTBEAT`, `POLL`.

## Firmware ESP32 (Flash-Once)
### Compile (Arduino IDE / arduino-cli)
- Board: ESP32 Dev Module
- Library: `ESPAsyncWebServer`, `AsyncTCP`, `ArduinoJson`
- Partition Scheme: Default (dengan LittleFS)

Export compiled binary → letakkan di `firmware/bin/`:
- `bootloader.bin` (offset 0x1000)
- `partitions.bin` (offset 0x8000)
- `relay_timer_v1.0.0.bin` (offset 0x10000)

### Perilaku
- Boot: cek tombol BOOT (GPIO0). Tahan **>5s** → paksa AP Mode. Tahan **10s** → Factory Reset.
- Config invalid / kosong → AP Mode (Captive Portal `RelayTimer-Setup-XXXX` @ `192.168.4.1`).
- Config valid → STA Mode: konek WiFi, sync NTP, heartbeat + poll relay.
- Gagal konek 30s → AP cadangan 10 menit (retry WiFi utama tiap 60s). **Config tidak pernah dihapus otomatis.**
- Task Watchdog Timer 15s aktif; setiap HTTP client di-`http.end()`.

### Flashing Browser
Host `manifest.json` + folder `bin/` di server PWA (`/firmware/`). Buka halaman `/install` PWA di Chrome/Edge dan klik **Flash Firmware**.
