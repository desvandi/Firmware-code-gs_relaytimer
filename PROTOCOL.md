# Protocol Specification — Timer Digital Relay v4.2

> Implements the protocol requirements from the Industrial-Grade
> Implementation Directive §24-29, §35-36, §67.

---

## 1. Transport Layers

| Layer | Transport | Auth | Used for |
|---|---|---|---|
| L1 REST | HTTP (LAN) or HTTPS (Cloudflare Tunnel) | JWT + CSRF | PWA → ESP32 (LAN mode) |
| L2 MQTT | TCP 1883 (dev) or TLS 8883/8884 (prod) | Per-device username/password + ACL | PWA → ESP32 (remote, CGNAT-friendly) |
| L3 GAS | HTTPS POST to script.google.com | HMAC-SHA256 + nonce + timestamp | ESP32 → Gemini AI insights |

---

## 2. REST API (brief §32, §67)

### 2.1 Envelope

All REST responses follow:

```json
{
  "success": true,
  "message": "",
  "data": { ... }
}
```

or:

```json
{
  "success": false,
  "message": "ERR_AUTH_001 — Invalid credentials",
  "data": null
}
```

### 2.2 Endpoints

| Method | Path | Auth | Purpose |
|---|---|---|---|
| POST | /api/login | None (skipCsrf) | Login → JWT + CSRF token |
| POST | /api/logout | JWT | Logout (invalidate refresh token) |
| GET | /api/session | JWT | Check session validity |
| POST | /api/refresh | Refresh cookie | Rotate access+refresh tokens |
| GET | /api/status | JWT | Full SystemStatus telemetry (brief §31) |
| GET | /api/version | JWT | Firmware version + OTA info |
| GET | /api/health | JWT | Hardware diagnostics |
| POST | /api/relay | JWT + CSRF | Relay ON/OFF/set_mode (with requestId) |
| POST | /api/channel | JWT + CSRF | Rename channel (with requestId) |
| POST | /api/schedule | JWT + CSRF | Upsert schedule (with requestId) |
| DELETE | /api/schedule?id=N | JWT + CSRF | Delete schedule (with requestId) |
| POST | /api/pir | JWT + CSRF | Configure PIR (with requestId) |
| POST | /api/pir/test | JWT + CSRF | Test-trigger PIR |
| POST | /api/time | JWT + CSRF | Set RTC time |
| GET | /api/log?type=X&channelId=Y&limit=N | JWT | Get activity logs |
| GET | /api/audit_log | JWT | Get audit log |
| GET | /api/config | JWT | Get full config |
| POST | /api/config | JWT + CSRF | Set config |
| POST | /api/config/device | JWT + CSRF | Set device name + timezone |
| POST | /api/config/password | JWT + CSRF | Change password |
| GET | /api/config/export | JWT | Export config JSON |
| POST | /api/config/import | JWT + CSRF | Import config JSON (validated) |
| POST | /api/reboot | JWT + CSRF | Reboot ESP32 |
| POST | /api/ota | JWT + CSRF | Upload OTA binary (multipart) |
| POST | /api/ota/check | JWT + CSRF | Check for update available |
| POST | /api/factory_reset/prepare | JWT + CSRF | Generate reset token (60-s TTL) |
| POST | /api/factory_reset/confirm | JWT + CSRF | Execute factory reset (token required) |

### 2.3 Transaction Headers

All state-changing REST requests (POST/PUT/DELETE) must include:

```
Authorization: Bearer <JWT>
X-CSRF-Token: <csrf-token-from-cookie>
Content-Type: application/json
```

Body includes `requestId` (UUID v4) for transaction dedup:

```json
{
  "channelId": 1,
  "action": "on",
  "mode": "manual",
  "manualState": true,
  "requestId": "550e8400-e29b-41d4-a716-446655440000"
}
```

---

## 3. MQTT Topics (brief §34-36)

### 3.1 Topic Structure

```
timer12/<mac>/status    — ESP32 publishes SystemStatus JSON (every 5s)
timer12/<mac>/command   — PWA publishes command JSON, ESP32 executes
timer12/<mac>/log       — ESP32 publishes activity log entries (real-time)
timer12/<mac>/online    — ESP32 publishes "1" on connect, "0" on disconnect (LWT)
timer12/<mac>/ota       — PWA publishes OTA update commands (signed)
timer12/<mac>/ack       — ESP32 publishes ACK for each command
```

`<mac>` is the device's MAC address (lowercase, no colons). Broker ACL
restricts each device to its own topic subtree.

### 3.2 Command Message Format

```json
{
  "type": "relay" | "schedule" | "channel" | "pir" | "system" | "ota",
  "action": "on" | "off" | "set_mode" | "upsert" | "delete" | "rename" | ...,
  "requestId": "uuid-v4",
  "payload": { ... }
}
```

### 3.3 ACK Message Format

```json
{
  "requestId": "uuid-v4",
  "success": true,
  "message": "",
  "data": { ... },
  "timestamp": 1697000000
}
```

ACKs are stored in the NVS-backed TransactionJournal so a duplicate
command (same requestId) replays the original ACK byte-for-byte without
re-executing the mutation (brief §26).

### 3.4 QoS Strategy (brief §36)

| Topic | QoS | Rationale |
|---|---|---|
| `timer12/<mac>/command` | 1 | At-least-once delivery — duplicate detection via requestId |
| `timer12/<mac>/ack` | 1 | At-least-once — duplicate ACK is safe |
| `timer12/<mac>/ota` | 1 | At-least-once — signed + idempotent |
| `timer12/<mac>/status` | 0 | At-most-once — telemetry is time-series, loss tolerable |
| `timer12/<mac>/log` | 0 | At-most-once — log entries are timestamped for ordering |
| `timer12/<mac>/online` | 1 + retain | LWT must be reliable — otherwise stale online status |

---

## 4. Transaction Engine States (brief §24)

Every command transitions through:

```
RECEIVED → VALIDATED → PREPARED → COMMITTED → EXECUTED → ACK_PENDING → ACK_DELIVERED
                       (failure) ↘ REJECTED  (timeout) ↘ ABORTED
```

- **RECEIVED**: command arrived via REST or MQTT
- **VALIDATED**: JSON schema, field ranges, deviceId match
- **PREPARED**: canonical hash computed, TransactionJournal consulted
- **COMMITTED**: TransactionJournal entry written (durable)
- **EXECUTED**: physical relay state applied
- **ACK_PENDING**: ACK JSON constructed, MQTT publish attempted
- **ACK_DELIVERED**: MQTT QoS 1 PUBACK received, or retry queued

### 4.1 Crash Window (brief §25)

If power loss occurs between COMMITTED and EXECUTED:
- On reboot, TransactionJournal is replayed
- For each pending transaction, determine if mutation was actually applied
  by comparing current physical state with the journal's `desiredState`
- Idempotent relay commands are safe to replay
- For non-idempotent operations (e.g.OTA), require explicit operator confirmation

---

## 5. Monotonic Telemetry Sequence (brief §22)

Every `/api/status` response and MQTT status publication includes:

```json
{
  "telemetrySequence": 1234
}
```

`telemetrySequence` is a 32-bit unsigned integer that increments on every
`publishStatus()` call. PWA/GAS can detect:

- **Packet loss**: if PWA receives sequence N then sequence N+2 (gap of 1)
- **Reordering**: if PWA receives sequence N+1 before sequence N
- **Device reboot**: if sequence drops back to a lower value (counter resets on boot)

---

## 6. Schema Validation (brief §73, §88)

Firmware rejects malformed input at multiple layers:

| Layer | Validation |
|---|---|
| JSON syntax | ArduinoJson parse returns false on malformed JSON |
| Body size | `requireBody(MAX_BODY_SIZE)` rejects > 16 KB |
| Field types | Each handler validates `doc["field"].is<X>()` before reading |
| Range checks | `channelId` 1..12, `pirId` 1..4, `dayMask` 0..127, etc. |
| String length | Channel name ≤ 20 chars, schedule time "HH:MM" format |
| Numeric range | `onTime`/`offTime` parsed as minutes 0..1439 |
| Unknown fields | Currently tolerated (forward compat). Future v4.3 may reject. |
