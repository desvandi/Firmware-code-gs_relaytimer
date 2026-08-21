/**
 * ============================================================================
 *  RELAY TIMER — Google Apps Script (Container-Bound, Sheet-Driven, Stateless)
 *  Repo: Firmware-code-gs_relaytimer
 *  Standar: Zero-Touch Deployment. Distribusi via "Make a Copy" Google Sheet.
 *  User TIDAK perlu membuka IDE Apps Script atau mengedit kode apapun.
 * ============================================================================
 *
 *  Cara pakai (end-user):
 *   1. Buka link "Make a Copy" Master Template Google Sheet.
 *   2. Edit tab "Config" (AUTH_TOKEN, DEVICE_KEY, dll).
 *   3. Deploy > New deployment > Web app > Execute as "Me", Access "Anyone".
 *   4. Salin URL /exec ke PWA & Captive Portal ESP32.
 *
 *  Semua variabel dibaca dinamis dari tab "Config" + CacheService.
 *  DILARANG hardcode Spreadsheet ID -> selalu getActiveSpreadsheet().
 * ============================================================================
 */

var CONFIG_SHEET = "Config";
var LOG_SHEET = "Logs";
var STATE_SHEET = "State";
var CACHE_TTL = 21600; // 6 jam

/* -------------------------------------------------------------------------- */
/*  DYNAMIC CONFIG READER + CACHING                                           */
/* -------------------------------------------------------------------------- */

function getConfig(key) {
  var cache = CacheService.getScriptCache();
  var cachedValue = cache.get("CFG_" + key);
  if (cachedValue !== null) {
    return cachedValue;
  }
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = ss.getSheetByName(CONFIG_SHEET);
  if (!sheet) throw new Error("Tab 'Config' tidak ditemukan di spreadsheet.");
  var data = sheet.getDataRange().getValues();
  for (var i = 1; i < data.length; i++) {
    if (data[i][0] === key) {
      var val = data[i][1] === null ? "" : data[i][1].toString();
      cache.put("CFG_" + key, val, CACHE_TTL);
      return val;
    }
  }
  throw new Error("Configuration Key Not Found: " + key);
}

function getConfigSafe(key, fallback) {
  try {
    return getConfig(key);
  } catch (e) {
    return fallback;
  }
}

/* -------------------------------------------------------------------------- */
/*  STANDARD JSON RESPONSE CONTRACT                                           */
/* -------------------------------------------------------------------------- */

function jsonOutput(status, code, data, message) {
  var payload = {
    status: status,
    code: code,
    data: data || {},
    message: message || "",
    timestamp: new Date().toISOString(),
  };
  return ContentService
    .createTextOutput(JSON.stringify(payload))
    .setMimeType(ContentService.MimeType.JSON);
}

function success(data, message) { return jsonOutput("SUCCESS", 200, data, message || "OK"); }
function fail(code, message)    { return jsonOutput("ERROR", code, {}, message); }

/* -------------------------------------------------------------------------- */
/*  AUTH LAYER (validated by AUTH_TOKEN, access = Anyone)                     */
/* -------------------------------------------------------------------------- */

function authorize(token) {
  var expected = getConfig("AUTH_TOKEN");
  if (!token || token.length < 16) return false;
  return token === expected;
}

/* -------------------------------------------------------------------------- */
/*  REQUEST PARSING                                                           */
/* -------------------------------------------------------------------------- */

function parseBody(e) {
  try {
    if (e && e.postData && e.postData.contents) {
      return JSON.parse(e.postData.contents);
    }
  } catch (err) { /* fallthrough */ }
  return (e && e.parameter) ? e.parameter : {};
}

/* -------------------------------------------------------------------------- */
/*  ENTRY POINTS                                                              */
/* -------------------------------------------------------------------------- */

function doGet(e) {
  return routeRequest(parseBody(e));
}

function doPost(e) {
  return routeRequest(parseBody(e));
}

function routeRequest(req) {
  try {
    var action = (req.action || "").toUpperCase();

    // PING tidak butuh device, hanya token.
    if (action === "PING") {
      if (!authorize(req.token)) return fail(401, "Token Otorisasi Salah.");
      return success({ message: "PONG", device: getConfigSafe("DEVICE_KEY", "") }, "PONG");
    }

    if (!authorize(req.token)) {
      return fail(401, "URL Apps Script valid namun Token Otorisasi salah.");
    }

    switch (action) {
      case "GET_STATUS":  return handleGetStatus(req);
      case "SET_RELAY":   return handleSetRelay(req);
      case "HEARTBEAT":   return handleHeartbeat(req);   // dipanggil ESP32
      case "POLL":        return handlePoll(req);        // ESP32 ambil perintah
      default:            return fail(400, "Action tidak dikenali: " + action);
    }
  } catch (err) {
    return fail(500, "Internal error: " + err.message);
  }
}

/* -------------------------------------------------------------------------- */
/*  STATE SHEET (perintah relay & status terakhir)                           */
/* -------------------------------------------------------------------------- */

function getStateSheet() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = ss.getSheetByName(STATE_SHEET);
  if (!sheet) {
    sheet = ss.insertSheet(STATE_SHEET);
    sheet.appendRow(["relay_index", "gpio", "state", "timer_sec", "updated_at"]);
    var map = getConfigSafe("RELAY_GPIO_MAP", "26,27,14,12").split(",");
    for (var i = 0; i < map.length; i++) {
      sheet.appendRow([i, map[i].trim(), "OFF", 0, new Date().toISOString()]);
    }
  }
  return sheet;
}

function handleGetStatus(req) {
  var sheet = getStateSheet();
  var data = sheet.getDataRange().getValues();
  var relays = [];
  for (var i = 1; i < data.length; i++) {
    relays.push({
      relay: data[i][0],
      gpio: data[i][1],
      state: data[i][2],
      timer_sec: data[i][3],
      remaining: computeRemaining(data[i][2], data[i][3], data[i][4]),
      updated_at: data[i][4],
    });
  }
  return success({
    device: getConfig("DEVICE_KEY"),
    relays: relays,
    safety_cutoff_sec: parseInt(getConfigSafe("DEFAULT_RELAY_TIMEOUT_SEC", "3600"), 10),
  }, "Status perangkat terkini.");
}

function computeRemaining(state, timerSec, updatedAt) {
  if (state !== "ON" || !timerSec) return 0;
  var elapsed = (Date.now() - new Date(updatedAt).getTime()) / 1000;
  var rem = Math.max(0, Math.floor(timerSec - elapsed));
  return rem;
}

function handleSetRelay(req) {
  var idx = parseInt(req.relay, 10);
  var state = (req.state || "OFF").toUpperCase() === "ON" ? "ON" : "OFF";
  var timer = parseInt(req.timer_sec || 0, 10);

  var safety = parseInt(getConfigSafe("DEFAULT_RELAY_TIMEOUT_SEC", "3600"), 10);
  if (state === "ON" && (timer <= 0 || timer > safety)) {
    timer = timer <= 0 ? 0 : safety; // hormati safety cutoff
  }

  var sheet = getStateSheet();
  var data = sheet.getDataRange().getValues();
  for (var i = 1; i < data.length; i++) {
    if (parseInt(data[i][0], 10) === idx) {
      sheet.getRange(i + 1, 3).setValue(state);
      sheet.getRange(i + 1, 4).setValue(timer);
      sheet.getRange(i + 1, 5).setValue(new Date().toISOString());
      break;
    }
  }

  writeLog("SET_RELAY", "Relay " + idx + " -> " + state + " (timer " + timer + "s)");
  notifyTelegram("Relay " + idx + " di-" + (state === "ON" ? "NYALAKAN" : "MATIKAN"));
  return success({ relay: idx, state: state, timer_sec: timer }, "Perintah relay tersimpan.");
}

/* ESP32 mengirim heartbeat berkala (free heap, uptime). */
function handleHeartbeat(req) {
  writeLog("HEARTBEAT", "heap=" + (req.free_heap || "?") + " uptime=" + (req.uptime || "?"));
  return success({ ack: true, server_time: new Date().toISOString() }, "Heartbeat diterima.");
}

/* ESP32 polling: ambil state relay untuk dieksekusi. */
function handlePoll(req) {
  return handleGetStatus(req);
}

/* -------------------------------------------------------------------------- */
/*  LOGGING + ROTATION                                                        */
/* -------------------------------------------------------------------------- */

function getLogSheet() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = ss.getSheetByName(LOG_SHEET);
  if (!sheet) {
    sheet = ss.insertSheet(LOG_SHEET);
    sheet.appendRow(["timestamp", "type", "message"]);
  }
  return sheet;
}

function writeLog(type, message) {
  var sheet = getLogSheet();
  sheet.appendRow([new Date().toISOString(), type, message]);
  rotateLogs(sheet);
}

function rotateLogs(sheet) {
  var maxRows = parseInt(getConfigSafe("LOG_MAX_ROWS", "2000"), 10);
  var total = sheet.getLastRow() - 1; // minus header
  if (total > maxRows) {
    var excess = total - maxRows;
    sheet.deleteRows(2, excess); // hapus baris tertua (tepat di bawah header)
  }
}

/* -------------------------------------------------------------------------- */
/*  TELEGRAM NOTIFICATION (OPTIONAL)                                          */
/* -------------------------------------------------------------------------- */

function notifyTelegram(text) {
  var botToken = getConfigSafe("TELEGRAM_BOT_TOKEN", "");
  var chatId = getConfigSafe("TELEGRAM_CHAT_ID", "");
  if (!botToken || !chatId) return; // opsional
  try {
    UrlFetchApp.fetch("https://api.telegram.org/bot" + botToken + "/sendMessage", {
      method: "post",
      contentType: "application/json",
      payload: JSON.stringify({ chat_id: chatId, text: "[RelayTimer] " + text }),
      muteHttpExceptions: true,
    });
  } catch (e) { /* jangan gagalkan request utama */ }
}

/* -------------------------------------------------------------------------- */
/*  SETUP HELPER (jalankan sekali via menu untuk membuat tab Config)         */
/* -------------------------------------------------------------------------- */

function onOpen() {
  SpreadsheetApp.getUi()
    .createMenu("Relay Timer")
    .addItem("Inisialisasi Master Template", "initMasterTemplate")
    .addItem("Bersihkan Cache Config", "clearConfigCache")
    .addToUi();
}

function initMasterTemplate() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = ss.getSheetByName(CONFIG_SHEET) || ss.insertSheet(CONFIG_SHEET);
  sheet.clear();
  var rows = [
    ["KEY", "VALUE", "SIFAT", "KETERANGAN"],
    ["AUTH_TOKEN", "usr_sec_99a8b7c6d5e4f3a21", "MANDATORY", "Token keamanan autentikasi PWA & ESP32"],
    ["DEVICE_KEY", "RELAY_CTRL_01", "MANDATORY", "Identifier unik perangkat ESP32"],
    ["TELEGRAM_BOT_TOKEN", "", "OPTIONAL", "Token Bot Telegram"],
    ["TELEGRAM_CHAT_ID", "", "OPTIONAL", "ID Chat/Group Telegram"],
    ["DEFAULT_RELAY_TIMEOUT_SEC", "3600", "MANDATORY", "Auto turn-off safety timeout (detik)"],
    ["LOG_MAX_ROWS", "2000", "SYSTEM", "Batas maksimum baris rotasi log"],
    ["RELAY_GPIO_MAP", "26,27,14,12", "SYSTEM", "Pemetaan pin GPIO relay"],
  ];
  sheet.getRange(1, 1, rows.length, 4).setValues(rows);
  sheet.setFrozenRows(1);
  getStateSheet();
  getLogSheet();
  clearConfigCache();
  SpreadsheetApp.getUi().alert("Master Template siap. Silakan deploy sebagai Web App (Anyone).");
}

function clearConfigCache() {
  var cache = CacheService.getScriptCache();
  var keys = ["AUTH_TOKEN", "DEVICE_KEY", "TELEGRAM_BOT_TOKEN", "TELEGRAM_CHAT_ID",
              "DEFAULT_RELAY_TIMEOUT_SEC", "LOG_MAX_ROWS", "RELAY_GPIO_MAP"];
  for (var i = 0; i < keys.length; i++) cache.remove("CFG_" + keys[i]);
}
