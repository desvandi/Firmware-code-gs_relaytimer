/**
 * Timer Digital Relay v4.0 — Google Apps Script (AI Insights via Gemini)
 *
 * Round 10A fixes (audit round 10):
 *   R10A-1: HMAC metadata moved from HTTP headers → URL query parameters.
 *           GAS Web App event object does NOT expose HTTP request headers;
 *           previous http.addHeader() approach was silently broken.
 *           Auth metadata now sent as: ?deviceId=...&timestamp=...&nonce=...&signature=...
 *   R10A-2: Nonce check wrapped in LockService.getScriptLock() for atomicity.
 *           Previous check-then-claim had race condition (two concurrent
 *           requests could both pass the check).
 *
 * Privacy: ESP32 sends anonymous device ID (first 16 chars of SHA-256(MAC)).
 *   Gemini never sees the real MAC. The anonymous ID is used as a cache key
 *   and to look up the per-device HMAC secret from Script Properties.
 *
 * Deploy as a Google Apps Script Web App:
 * 1. Open https://script.google.com → New Project
 * 2. Paste this code
 * 3. Set GEMINI_API_KEY in Project Settings → Script Properties
 * 4. For each ESP32 device, add a Script Property:
 *    Key:   DEVICE_<anonymousId>_SECRET
 *    Value: <64 hex chars from Serial Monitor>
 *    (anonymousId = first 16 chars of SHA-256(MAC), printed on boot)
 * 5. Deploy → New Deployment → Type: Web App
 *    - Execute as: Me
 *    - Who has access: Anyone (anonymous — HMAC provides auth)
 * 6. Copy deployment URL → set as GAS_INSIGHTS_URL in firmware Config.h
 *    and NEXT_PUBLIC_GAS_INSIGHTS_URL in Vercel env vars.
 *
 * ESP32 sends:
 *   POST <GAS_URL>?deviceId=<16hex>&timestamp=<unixSec>&nonce=<16hex>&signature=<64hex>
 *   Body: { mac, status: {...}, logs: [...] }
 *   Canonical = timestamp + "\n" + nonce + "\n" + deviceId + "\n" + body
 *   signature = HMAC-SHA256(secret, canonical)
 */

// === CONFIG ===
const GEMINI_API_KEY = PropertiesService.getScriptProperties().getProperty('GEMINI_API_KEY');
const GEMINI_MODEL = 'gemini-1.5-flash';
const GEMINI_URL = `https://generativelanguage.googleapis.com/v1beta/models/${GEMINI_MODEL}:generateContent?key=${GEMINI_API_KEY}`;

// Cache config
const CACHE_KEY_PREFIX = 'insights_';
const DATA_KEY_PREFIX = 'data_';
const NONCE_KEY_PREFIX = 'nonce_';
const CACHE_TTL_MS = 60 * 60 * 1000;        // 1 hour (insights)
const DATA_TTL_MS = 6 * 60 * 60 * 1000;     // 6 hours (raw logs)
const NONCE_TTL_SEC = 600;                   // 10 min (nonce replay window)

// Validation limits (P1 #15)
const MAX_BODY_SIZE = 16384;                  // 16 KB
const MAX_LOGS = 100;
const MAX_LOG_MESSAGE_LEN = 500;
const MAX_CHANNEL_NAME_LEN = 32;
const MAX_CHANNELS = 12;

// HMAC config
const TIMESTAMP_TOLERANCE_SEC = 300;          // ±5 minutes

// PLN tariff (Indonesia) — used for cost estimation
const ELECTRICITY_RATE_RUPIAH_PER_KWH = 1467;

/**
 * Validate anonymous device ID: must be exactly 16 hex chars.
 */
function isValidDeviceId(id) {
  if (!id) return false;
  const cleaned = String(id).toUpperCase().replace(/[^A-F0-9]/g, '');
  return cleaned.length === 16;
}

function normalizeDeviceId(id) {
  return String(id || '').toUpperCase().replace(/[^A-F0-9]/g, '');
}

/**
 * GET endpoint — PWA fetches insights for a specific device.
 * PWA does NOT have the HMAC secret (only ESP32 does), so GET uses
 * a simpler auth model: the anonymous device ID itself is the "key".
 */
function doGet(e) {
  const mac = normalizeDeviceId(e.parameter.mac);
  if (!isValidDeviceId(mac)) {
    return jsonOut({ success: false, error: 'Invalid device ID (must be 16 hex chars)' });
  }

  const action = e.parameter.action || 'insights';

  if (action === 'insights') {
    return getInsights(mac);
  } else if (action === 'health') {
    return jsonOut({
      success: true,
      status: 'ok',
      geminiConfigured: !!GEMINI_API_KEY,
      geminiModel: GEMINI_MODEL,
      cacheTtlMs: CACHE_TTL_MS,
      serverTime: new Date().toISOString(),
    });
  }

  return jsonOut({ success: false, error: 'Unknown action: ' + action });
}

/**
 * POST endpoint — ESP32 pushes logs + status, triggers Gemini analysis.
 *
 * R10A-1 (audit round 10): Auth metadata sent as URL query parameters
 * (NOT HTTP headers — GAS Web App event object does not expose headers).
 *
 * URL: POST <GAS_URL>?deviceId=<16hex>&timestamp=<unixSec>&nonce=<16hex>&signature=<64hex>
 * Body: { mac, status: {...}, logs: [...] }
 *
 * Canonical request = timestamp + "\n" + nonce + "\n" + deviceId + "\n" + rawBody
 * signature = HMAC-SHA256(deviceSecret, canonical)
 *
 * R10A-2 (audit round 10): Nonce check + HMAC verify wrapped in
 * LockService.getScriptLock() to prevent race condition on concurrent requests.
 */
function doPost(e) {
  const lock = LockService.getScriptLock();
  try {
    // R10A-2: Acquire lock BEFORE reading nonce — prevents race condition
    // where two concurrent requests both see nonce as "not used".
    // 30s timeout is generous; ESP32 HTTP timeout is 8s.
    if (!lock.tryLock(30000)) {
      return jsonOut({ success: false, error: 'Server busy — could not acquire lock' });
    }

    // R10A-1: Read auth metadata from e.parameter (URL query params), NOT headers
    const deviceId = normalizeDeviceId(e.parameter.deviceId);
    const timestampStr = e.parameter.timestamp || '';
    const nonce = e.parameter.nonce || '';
    const signature = (e.parameter.signature || '').toUpperCase();

    if (!isValidDeviceId(deviceId)) {
      return jsonOut({ success: false, error: 'Missing or invalid deviceId parameter' });
    }
    if (!timestampStr || !nonce || !signature) {
      return jsonOut({ success: false, error: 'Missing timestamp/nonce/signature parameters' });
    }

    // P1 #15: Validate body size BEFORE parsing
    const rawBody = e.postData.contents;
    if (!rawBody || rawBody.length > MAX_BODY_SIZE) {
      return jsonOut({ success: false, error: 'Body too large (max ' + MAX_BODY_SIZE + ' bytes)' });
    }

    // Look up the device's shared secret from Script Properties
    const secretKey = 'DEVICE_' + deviceId + '_SECRET';
    const secretHex = PropertiesService.getScriptProperties().getProperty(secretKey);
    if (!secretHex || secretHex.length !== 64) {
      Logger.log('No secret configured for device: ' + deviceId);
      return jsonOut({ success: false, error: 'Device not registered (no HMAC secret found)' });
    }

    // Verify timestamp ±5 min
    const timestamp = parseInt(timestampStr, 10);
    if (isNaN(timestamp)) {
      return jsonOut({ success: false, error: 'Invalid timestamp parameter' });
    }
    const serverTime = Math.floor(Date.now() / 1000);
    if (Math.abs(serverTime - timestamp) > TIMESTAMP_TOLERANCE_SEC) {
      Logger.log('Timestamp out of range: client=' + timestamp + ' server=' + serverTime);
      return jsonOut({ success: false, error: 'Timestamp out of tolerance (±5 min)' });
    }

    // R10A-2: Nonce check is now atomic (inside lock)
    const nonceCacheKey = NONCE_KEY_PREFIX + deviceId + '_' + nonce;
    const existingNonce = CacheService.getScriptCache().get(nonceCacheKey);
    if (existingNonce) {
      Logger.log('Nonce replay detected: ' + nonce);
      return jsonOut({ success: false, error: 'Nonce already used (replay detected)' });
    }

    // Compute expected HMAC
    // Canonical = timestamp + "\n" + nonce + "\n" + deviceId + "\n" + rawBody
    const canonical = timestampStr + '\n' + nonce + '\n' + deviceId + '\n' + rawBody;
    const secretBytes = Utilities.computeHmacSha256Signature(
      canonical,
      Utilities.newBlob(hexToBytes_(secretHex)).getBytes()
    );
    const computedSigHex = bytesToHex_(secretBytes).toUpperCase();

    // Constant-time compare
    if (!constantTimeEquals_(signature, computedSigHex)) {
      Logger.log('HMAC mismatch: expected=' + computedSigHex + ' got=' + signature);
      return jsonOut({ success: false, error: 'Invalid signature' });
    }

    // Mark nonce as used (still inside lock — atomic)
    CacheService.getScriptCache().put(nonceCacheKey, '1', NONCE_TTL_SEC);

    // Release lock — rest of processing (JSON parse, Gemini call) doesn't need lock
    lock.releaseLock();

    // Parse body
    const body = JSON.parse(rawBody);
    const mac = normalizeDeviceId(body.mac);
    const logs = body.logs || [];
    const status = body.status || {};

    if (!isValidDeviceId(mac)) {
      return jsonOut({ success: false, error: 'Invalid device ID in body (must be 16 hex chars)' });
    }

    // Validate body schema
    if (mac !== deviceId) {
      return jsonOut({ success: false, error: 'Device ID mismatch (param vs body)' });
    }
    if (!Array.isArray(logs) || logs.length > MAX_LOGS) {
      return jsonOut({ success: false, error: 'Invalid logs (must be array, max ' + MAX_LOGS + ' entries)' });
    }
    if (typeof status !== 'object' || status === null) {
      return jsonOut({ success: false, error: 'Invalid status (must be object)' });
    }

    // Validate channel names + log messages (truncate if too long)
    if (status.channels) {
      if (!Array.isArray(status.channels) || status.channels.length > MAX_CHANNELS) {
        return jsonOut({ success: false, error: 'Invalid channels (max ' + MAX_CHANNELS + ')' });
      }
      status.channels = status.channels.map(ch => {
        if (ch.name && String(ch.name).length > MAX_CHANNEL_NAME_LEN) {
          ch.name = String(ch.name).substring(0, MAX_CHANNEL_NAME_LEN);
        }
        return ch;
      });
    }
    const validatedLogs = logs.map(l => {
      if (l.message && String(l.message).length > MAX_LOG_MESSAGE_LEN) {
        l.message = String(l.message).substring(0, MAX_LOG_MESSAGE_LEN);
      }
      return l;
    });

    // Store latest data in cache (for GET polling by PWA)
    CacheService.getScriptCache().put(
      DATA_KEY_PREFIX + mac,
      JSON.stringify({ logs: validatedLogs, status, ts: Date.now() }),
      Math.floor(DATA_TTL_MS / 1000)
    );

    // Generate insights
    const insights = generateInsights(mac, validatedLogs, status);
    return jsonOut({ success: true, insights: insights });
  } catch (err) {
    Logger.log('doPost error: ' + err);
    return jsonOut({ success: false, error: String(err) });
  } finally {
    // Ensure lock is released if still held (e.g., exception before explicit release)
    if (lock.hasLock()) {
      lock.releaseLock();
    }
  }
}

/**
 * Get cached insights or generate new ones
 */
function getInsights(mac) {
  const cacheKey = CACHE_KEY_PREFIX + mac;
  const cached = CacheService.getScriptCache().get(cacheKey);

  if (cached) {
    try {
      const parsed = JSON.parse(cached);
      if (Date.now() - parsed.generatedAt < CACHE_TTL_MS) {
        return jsonOut({ success: true, insights: parsed.insights, cached: true });
      }
    } catch (e) {
      Logger.log('Cache parse error: ' + e);
    }
  }

  // Get stored data
  const dataStr = CacheService.getScriptCache().get(DATA_KEY_PREFIX + mac);
  if (!dataStr) {
    return jsonOut({
      success: true,
      insights: getMockInsights(mac),
      mock: true,
      message: 'No logs received yet. ESP32 will POST logs every hour once configured.'
    });
  }

  let data;
  try {
    data = JSON.parse(dataStr);
  } catch (e) {
    return jsonOut({ success: false, error: 'Corrupted cached data' });
  }

  const insights = generateInsights(mac, data.logs, data.status);

  // Cache for 1 hour
  CacheService.getScriptCache().put(
    cacheKey,
    JSON.stringify({ insights, generatedAt: Date.now() }),
    Math.floor(CACHE_TTL_MS / 1000)
  );

  return jsonOut({ success: true, insights: insights, cached: false });
}

/**
 * Call Gemini API with prompt built from logs + status
 */
function generateInsights(mac, logs, status) {
  if (!GEMINI_API_KEY) {
    Logger.log('GEMINI_API_KEY not set — returning mock insights');
    return getMockInsights(mac);
  }

  const prompt = buildPrompt(mac, logs, status);

  try {
    const payload = {
      contents: [{ parts: [{ text: prompt }] }],
      generationConfig: {
        temperature: 0.7,
        maxOutputTokens: 2048,
      }
    };

    const options = {
      method: 'post',
      contentType: 'application/json',
      payload: JSON.stringify(payload),
      muteHttpExceptions: true,
    };

    const response = UrlFetchApp.fetch(GEMINI_URL, options);
    const result = JSON.parse(response.getContentText());

    if (result.candidates && result.candidates[0]) {
      const text = result.candidates[0].content.parts[0].text;
      return parseGeminiResponse(text, mac);
    } else {
      Logger.log('Gemini returned no candidates: ' + JSON.stringify(result).slice(0, 500));
      return getMockInsights(mac);
    }
  } catch (err) {
    Logger.log('Gemini API error: ' + err);
    return getMockInsights(mac);
  }
}

/**
 * Build analysis prompt for Gemini
 */
function buildPrompt(mac, logs, status) {
  const channels = status.channels || [];
  const channelSummary = channels.map(ch =>
    `CH${ch.id} "${ch.name}": ${ch.state ? 'ON' : 'OFF'} via ${ch.source}, mode=${ch.modeAuto ? 'auto' : 'manual'}, ` +
    `energy=${ch.energyWh || 0}Wh, wattage=${ch.wattage || 10}W`
  ).join('\n');

  // Include PZEM power meter data if available
  let pzemSummary = '';
  if (status.voltage !== undefined) {
    const energyTodayKwh = status.energyToday || 0;
    const costTodayRp = Math.round(energyTodayKwh * ELECTRICITY_RATE_RUPIAH_PER_KWH);

    pzemSummary = `
POWER METER (PZEM-004T v3.0):
  Voltage: ${status.voltage || 0}V
  Current: ${status.current || 0}A
  Active Power: ${status.power || 0}W
  Apparent Power: ${status.apparentPower || 0}VA
  Reactive Power: ${status.reactivePower || 0}VAR
  Energy Total: ${status.energy || 0}kWh
  Energy Today: ${energyTodayKwh}kWh (est. Rp ${costTodayRp.toLocaleString('id-ID')})
  Frequency: ${status.frequency || 50}Hz
  Power Factor: ${status.powerFactor || 0}
  Max Power Today: ${status.powerMax || 0}W
  Avg Power Today: ${status.powerAvg || 0}W`;
  }

  const logSummary = logs.slice(0, 50).map(l => {
    const ts = l.timestamp ? new Date(l.timestamp).toISOString().slice(0, 19) : 'unknown';
    return `${ts} [${l.type}] ${l.message}`;
  }).join('\n');

  return `You are an IoT home automation and energy advisor. Analyze this Timer Relay device data and provide actionable recommendations.

DEVICE: anonymous ID ${mac}, Firmware ${status.firmwareVersion || '4.0.0'}, Uptime ${status.uptimeSeconds || 0}s
${pzemSummary}

CHANNELS:
${channelSummary}

RECENT LOGS (last ${Math.min(logs.length, 50)}):
${logSummary}

Provide 3-5 insights as a JSON array. Each insight MUST have this structure:
{
  "category": "habit_analysis" | "energy_analysis" | "fault_detection" | "predictive_maintenance" | "pir_recommendation",
  "severity": "info" | "warning" | "critical",
  "title": "Short title (max 60 chars)",
  "body": "Detailed explanation (2-3 sentences). Reference specific data like voltage, current, power, energy, channel names.",
  "channelId": <number or null>,
  "action": { "label": "Button text", "type": "apply_suggestion" | "review" | "dismiss" }
}

Focus on:
1. Usage patterns (relays always on/off at same time, based on logs)
2. Energy waste (high consumption, long ON durations, compare PZEM readings with relay states)
3. Faults (relay stuck ON, PIR not triggering, voltage anomalies)
4. Maintenance (relay cycle count from logs, contact wear estimate)
5. PIR optimization (rarely triggered sensors, sensitivity)
6. Power quality (voltage stability 220V ±10%, power factor target ≥0.9, frequency 50Hz ±0.5)
7. Cost estimation (use Energy Today × Rp ${ELECTRICITY_RATE_RUPIAH_PER_KWH}/kWh)

Respond ONLY with the JSON array, no markdown fences or extra text.`;
}

/**
 * Parse Gemini response (handle both JSON and markdown-fenced)
 */
function parseGeminiResponse(text, mac) {
  let clean = text.trim();
  if (clean.startsWith('```')) {
    clean = clean.replace(/^```(?:json)?\n?/, '').replace(/\n?```$/, '');
  }

  try {
    const insights = JSON.parse(clean);
    if (Array.isArray(insights)) {
      return insights.map((ins, i) => ({
        id: `gemini-${mac}-${Date.now()}-${i}`,
        generatedAt: Date.now(),
        source: 'gemini',
        ...ins,
      }));
    }
  } catch (e) {
    Logger.log('Parse error: ' + e + ' | raw: ' + clean.slice(0, 200));
  }
  return getMockInsights(mac);
}

/**
 * Fallback mock insights (when no API key or Gemini fails)
 */
function getMockInsights(mac) {
  return [
    {
      id: `mock-${mac}-1`,
      category: 'habit_analysis',
      severity: 'info',
      title: 'Waiting for data',
      body: 'No logs have been received yet. Once the ESP32 starts POSTing hourly logs (requires GAS_INSIGHTS_URL set in firmware Config.h), Gemini will analyze device patterns and provide actionable insights here.',
      channelId: null,
      action: { label: 'Dismiss', type: 'dismiss' },
      generatedAt: Date.now(),
      source: 'mock',
    },
  ];
}

// === Crypto helpers ===

function hexToBytes_(hex) {
  const bytes = [];
  for (let i = 0; i < hex.length; i += 2) {
    bytes.push(parseInt(hex.substr(i, 2), 16));
  }
  return bytes;
}

function bytesToHex_(bytes) {
  return bytes.map(b => ('0' + b.toString(16)).slice(-2)).join('');
}

function constantTimeEquals_(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) {
    diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  }
  return diff === 0;
}

/**
 * Helper: return JSON response
 */
function jsonOut(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
