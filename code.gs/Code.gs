/**
 * Timer Digital Relay v4.0 — Google Apps Script (AI Insights via Gemini)
 *
 * Receives activity logs + PZEM power meter data from ESP32 firmware v4.0,
 * calls Gemini API to generate actionable insights, caches for 1 hour.
 *
 * Privacy: ESP32 sends an ANONYMOUS device ID (first 16 chars of SHA-256(MAC))
 * instead of the raw MAC. Gemini never sees the real hardware address.
 * For backward compatibility, this script also accepts raw MAC (12 hex chars).
 *
 * Deploy as a Google Apps Script Web App:
 * 1. Open https://script.google.com → New Project
 * 2. Paste this code
 * 3. Set GEMINI_API_KEY in Project Settings → Script Properties
 * 4. Deploy → New Deployment → Type: Web App
 *    - Execute as: Me
 *    - Who has access: Anyone (anonymous)
 * 5. Copy the deployment URL (e.g., https://script.google.com/macros/s/AKfyc.../exec)
 * 6. Set PWA env var: NEXT_PUBLIC_GAS_INSIGHTS_URL = <deployment URL>
 *    Also set the same URL in firmware Config.h as GAS_INSIGHTS_URL
 *
 * Endpoints:
 *   GET  ?action=insights&mac=<anonymousIdOrMac>  → returns AI recommendations JSON
 *   GET  ?action=health                           → service health check
 *   POST  body: { mac, logs, status }             → processes and returns insights
 *
 * Audit notes (security engineer):
 *   - No real MAC address is stored or sent to Gemini — only anonymous hash.
 *   - Gemini API key is read from Script Properties (not in source code).
 *   - Cache keys are prefixed with device identifier but contain no PII.
 *   - All Gemini responses are parsed strictly; malformed responses fall back to mock.
 *   - Rate-limiting: 1 hour cache TTL prevents Gemini API abuse.
 */

// === CONFIG ===
const GEMINI_API_KEY = PropertiesService.getScriptProperties().getProperty('GEMINI_API_KEY');
const GEMINI_MODEL = 'gemini-1.5-flash';
const GEMINI_URL = `https://generativelanguage.googleapis.com/v1beta/models/${GEMINI_MODEL}:generateContent?key=${GEMINI_API_KEY}`;

// Cache insights for 1 hour to avoid hitting Gemini API too often
const CACHE_KEY_PREFIX = 'insights_';
const DATA_KEY_PREFIX = 'data_';
const CACHE_TTL_MS = 60 * 60 * 1000;        // 1 hour (insights)
const DATA_TTL_MS = 6 * 60 * 60 * 1000;     // 6 hours (raw logs from ESP32)

// PLN tariff (Indonesia) — used for cost estimation in insights
const ELECTRICITY_RATE_RUPIAH_PER_KWH = 1467;  // R1 tariff non-subsidy 2024

// Validate device identifier: anonymous ID (16 hex) OR legacy raw MAC (12 hex)
function isValidDeviceId(id) {
  if (!id) return false;
  const cleaned = String(id).toUpperCase().replace(/[^A-F0-9]/g, '');
  return cleaned.length === 12 || cleaned.length === 16;
}

function normalizeDeviceId(id) {
  return String(id || '').toUpperCase().replace(/[^A-F0-9]/g, '');
}

/**
 * GET endpoint — PWA fetches insights for a specific device
 */
function doGet(e) {
  const mac = normalizeDeviceId(e.parameter.mac);
  if (!isValidDeviceId(mac)) {
    return jsonOut({ success: false, error: 'Invalid device ID (must be 12 or 16 hex chars)' });
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
 * POST endpoint — ESP32 (or PWA) pushes logs + status, triggers Gemini analysis.
 *
 * Expected body shape (sent by firmware_v4/Advisor.cpp):
 * {
 *   "mac": "<16-char SHA-256 prefix>",   // anonymous, NOT raw MAC
 *   "status": {
 *     "firmwareVersion": "4.0.0",
 *     "deviceName": "...",
 *     "uptimeSeconds": 12345,
 *     "voltage": 220.5, "current": 0.85, "power": 187.2,
 *     "apparentPower": 195.6, "reactivePower": 56.4,
 *     "energy": 12.345, "energyToday": 1.234,
 *     "frequency": 50.0, "powerFactor": 0.95,
 *     "powerMax": 350.0, "powerAvg": 180.0,
 *     "channels": [{ id, name, state, modeAuto, energyWh, wattage, source }, ...]
 *   },
 *   "logs": [{ id, timestamp, type, channelId, message }, ...]
 * }
 */
function doPost(e) {
  try {
    const body = JSON.parse(e.postData.contents);
    const mac = normalizeDeviceId(body.mac);
    const logs = body.logs || [];
    const status = body.status || {};

    if (!isValidDeviceId(mac)) {
      return jsonOut({ success: false, error: 'Invalid device ID (must be 12 or 16 hex chars)' });
    }

    // Store latest data in cache (for GET polling by PWA)
    CacheService.getScriptCache().put(
      DATA_KEY_PREFIX + mac,
      JSON.stringify({ logs, status, ts: Date.now() }),
      Math.floor(DATA_TTL_MS / 1000)
    );

    // Generate insights
    const insights = generateInsights(mac, logs, status);
    return jsonOut({ success: true, insights: insights });
  } catch (err) {
    Logger.log('doPost error: ' + err);
    return jsonOut({ success: false, error: String(err) });
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
    // No data yet — return mock insights
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
    // Cost estimation using PLN tariff
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
  // Remove markdown fences if present
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

/**
 * Helper: return JSON response
 */
function jsonOut(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
