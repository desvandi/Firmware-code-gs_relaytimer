// =============================================================================
// Web/Handlers/RestJournalHelper.h — Journal lifecycle helper for REST handlers
// =============================================================================
// PHASE: P2-2 F-P0-2 C2 (auditor FINAL APPROVAL)
//
// AUDITOR GUARDRAILS (Phase B REV.3 §4.4 — non-negotiable):
//
//   Helper OWNS (journal lifecycle ONLY):
//     - requestId validation (charset, length)
//     - command hash generation (calls Utils::computeCommandHash — single source)
//     - duplicate lookup (checkDuplicateAndRespond)
//     - storeIntent (PENDING)
//     - markExecuting (EXECUTING)
//     - commitTransaction / commitTransactionFromPending (COMMITTED)
//     - clearEntryOnValidationFailure (pre-mutation cleanup)
//
//   Helper does NOT own (handler keeps):
//     - Domain-specific validation (channelId range, time format, password strength)
//     - Actual mutation calls (relayEngine.setManual, Storage::config.saveSchedule, etc.)
//     - Response body construction (handler builds its own ACK JSON)
//     - HTTP response sending (Web::http.send(...) — handler's responsibility)
//
//   Anti-pattern (FORBIDDEN per §4.6):
//     A `dispatchAndCommit()` helper that switches on command type and calls
//     relayEngine.setManual / Storage::config.saveSchedule / etc. directly.
//     This would make the helper a RestCommandDispatcher — re-creating the
//     dispatch problem we're trying to avoid.
//
// FLOW (per auditor Phase B REV.3 §4.5):
//   Handler entry
//     ↓
//   [handler] requireAuth, requireCsrf, requireBody, parse JSON
//     ↓
//   [handler] domain validation (channelId, time format, etc.)
//     ↓
//   [helper] validateRequestId(requestId)         ──── sends 400 if invalid
//     ↓
//   [helper] computeCommandHash(doc, type)         ──── returns hash string
//     ↓
//   [helper] checkDuplicateAndRespond(rid, hash)   ──── sends 200 (replay) or 409 if dup
//     ↓ (only if not duplicate)
//   [helper] storeIntentOrReject(rid, hash, ...)   ──── sends 503 if NVS fails
//     ↓
//   [helper] markExecutingOrAbort(rid)              ──── sends 503 + clearEntry if fails
//     ↓ (EXECUTING mode only)
//   [handler] ACTUAL MUTATION                       ──── domain-specific code
//     ↓                                                (relayEngine.setManual, etc.)
//   [handler] build ACK JSON                       ──── domain-specific shape
//     ↓
//   [helper] commit*OrFailure(rid, ackJson)        ──── sends 503 if commit fails
//     ↓
//   [handler] Web::http.send(200, ackJson)          ──── success response
//
// HARD INVARIANT (Phase B REV.3 §9.4):
//   Web::http.send(200, ...) implies journal state == COMMITTED
//   (handler MUST NOT send HTTP 200 unless commit helper returned true)
//
// CANONICAL HASH (auditor C2 reminder):
//   "Jangan mengubah canonical hash schema lagi. Utils::computeCommandHash()
//    sekarang sudah menjadi single source of truth."
//   This helper calls Utils::computeCommandHash() — does NOT redefine schema.
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_REST_JOURNAL_HELPER_H
#define TIMER12_WEB_HANDLERS_REST_JOURNAL_HELPER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"            // for Web::sendError / Web::sendSuccess
#include "TransactionJournal.h"  // for Services::journal + TransactionState
#include "CommandHash.h"       // for Utils::computeCommandHash (single source of truth)
#include "LogService.h"        // for Services::Log
#include "Config.h"            // for Core::LogType
#include "AuthManager.h"        // for Services::auth (used by Common.h)

namespace Web { namespace Rest {

// =============================================================================
// 1. requestId validation (charset + length)
//
// DELEGATED to Web::validateRequestId() in Common.h to avoid duplication.
// Both functions use identical rules (mirror MqttClient.cpp lines 876-898).
//
// This wrapper exists so REST handlers have a single namespace for journal
// lifecycle operations, but the actual validation logic lives in Common.h
// alongside requireAuth/requireCsrf/requireBody (the other request validators).
// =============================================================================
inline bool validateRequestId(const String& requestId) {
  return Web::validateRequestId(requestId);
}

// =============================================================================
// 2. command hash generation (calls shared Utils::computeCommandHash)
//
// Auditor C2 reminder: "Jangan mengubah canonical hash schema lagi.
// Utils::computeCommandHash() sekarang sudah menjadi single source of truth."
//
// This helper is a thin pass-through to the shared function. It exists so
// REST handlers have a single point of invocation — if future schema changes
// are needed, they happen in CommandHash.h, not here.
// =============================================================================
inline String computeCommandHash(const DynamicJsonDocument& doc) {
  return Utils::computeCommandHash(doc);
}

// =============================================================================
// 3. duplicate lookup + ACK replay
//
// Checks if requestId already exists in journal. If so:
//   - COMMITTED + same hash → replay original ACK (HTTP 200)
//   - COMMITTED_UNKNOWN + same hash → replay with disclaimer (HTTP 200)
//   - PENDING/EXECUTING + same hash → HTTP 409 "in progress"
//   - UNKNOWN + same hash → HTTP 409 "ambiguous"
//   - FAILED + same hash → clear and allow retry (fall through, return false)
//   - Any state + DIFFERENT hash → HTTP 409 "requestId reuse rejected"
//     (security: same requestId must always bind to same command)
//
// Returns true if requestId was already processed (caller should NOT re-execute).
// On true, this function has already sent the appropriate HTTP response.
//
// Mirrors MqttClient.cpp lines 911-1012 for cross-ingress contract symmetry.
// =============================================================================
inline bool checkDuplicateAndRespond(const String& requestId, const String& commandHash) {
  // isProcessed() returns true only for COMMITTED / COMMITTED_UNKNOWN.
  // For PENDING/EXECUTING/UNKNOWN/FAILED, we need explicit checks below.
  String existingHash = Services::journal.getCommandHash(requestId);
  if (existingHash.length() == 0) {
    // requestId not in journal — fresh command, fall through to execution
    return false;
  }

  // requestId exists in journal — verify hash matches
  if (existingHash != commandHash) {
    // SECURITY: requestId reuse with different command — reject
    Serial.printf("[REST] SECURITY: requestId reuse with different command! rid=%s\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::AuthFail,
      "SECURITY (REST): requestId reuse with different command: " + requestId, 0);
    Web::sendError(409, "requestId reuse with different command — rejected");
    return true;
  }

  // Hash matches — handle based on existing state
  Services::TransactionState state = Services::journal.getTransactionState(requestId);

  if (state == Services::TransactionState::COMMITTED) {
    // True duplicate — replay original ACK
    Serial.printf("[REST] Duplicate (COMMITTED): %s — replaying original ACK\n",
                  requestId.c_str());
    String originalAckJson = Services::journal.getAckJson(requestId);
    if (originalAckJson.length() > 0) {
      // Replay the exact ACK JSON from journal (preserves requestId + data shape)
      Web::sendSecurityHeaders();
      Web::http.send(200, "application/json; charset=utf-8", originalAckJson);
    } else {
      // ACK JSON missing from journal — send generic success
      Web::sendSuccess("Duplicate command (already executed)");
    }
    return true;
  }

  if (state == Services::TransactionState::COMMITTED_UNKNOWN) {
    // Reconciled at boot — replay with disclaimer
    Serial.printf("[REST] Duplicate (COMMITTED_UNKNOWN): %s — replaying with disclaimer\n",
                  requestId.c_str());
    String originalAckJson = Services::journal.getAckJson(requestId);
    if (originalAckJson.length() > 0) {
      Web::sendSecurityHeaders();
      Web::http.send(200, "application/json; charset=utf-8", originalAckJson);
    } else {
      Web::sendSuccess("Command may have executed (reconciled — GPIO matches desired)");
    }
    return true;
  }

  if (state == Services::TransactionState::FAILED) {
    // Proven not executed — clear and allow retry
    Serial.printf("[REST] Duplicate (FAILED): %s — proven not executed, allowing retry\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "FAILED transaction retried (proven not executed): " + requestId, 0);
    if (!Services::journal.clearEntry(requestId)) {
      // clearEntry failed — NVS write error
      Web::sendError(503, "Internal error: cannot clear FAILED transaction (NVS write failure) — please retry");
      return true;  // block retry — caller should not execute
    }
    // Fall through — caller may execute as fresh command
    return false;
  }

  if (state == Services::TransactionState::UNKNOWN) {
    // Cannot determine if executed — surface to PWA
    Serial.printf("[REST] Duplicate (UNKNOWN): %s — cannot determine if executed\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "UNKNOWN transaction retried (cannot determine if executed — NOT auto-retrying): " + requestId, 0);
    Web::sendError(409,
      "AMBIGUOUS: transaction state is UNKNOWN (cannot determine if previously executed). "
      "For idempotent commands (relay ON/OFF), retry is safe. For other commands, "
      "verify device state before retrying.");
    return true;
  }

  // PENDING or EXECUTING — command was in-flight during crash or concurrent request
  Serial.printf("[REST] Duplicate (%s): %s — in progress\n",
                state == Services::TransactionState::EXECUTING ? "EXECUTING" : "PENDING",
                requestId.c_str());
  Web::sendError(409, "requestId in progress (PENDING or EXECUTING) — retry later");
  return true;
}

// =============================================================================
// 4. storeIntent wrapper
//
// Creates PENDING journal entry. On success, returns true.
// On failure (NVS write error, journal full), sends HTTP 503 and returns false.
//
// Pre-condition: requestId has been validated, commandHash computed,
//                 checkDuplicateAndRespond returned false (not a duplicate).
// Post-condition: journal has PENDING entry for this requestId.
// =============================================================================
inline bool storeIntentOrReject(const String& requestId, const String& commandHash,
                                  uint8_t intentChannelId = 0,
                                  bool intentDesiredState = false,
                                  bool intentPreviousKnown = false) {
  if (!Services::journal.storeIntent(requestId, commandHash,
                                       intentChannelId, intentDesiredState,
                                       intentPreviousKnown)) {
    Serial.printf("[REST] FATAL: storeIntent failed for rid=%s — refusing to execute\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "storeIntent FAILED (REST) — command rejected: " + requestId, 0);
    Web::sendError(503,
      "DURABILITY_FAILURE: cannot store transaction intent — please retry");
    return false;
  }
  return true;
}

// =============================================================================
// 5. markExecuting wrapper (EXECUTING mode only)
//
// Transitions PENDING → EXECUTING. On success, returns true.
// On failure, calls clearEntry (restoring EMPTY) and sends HTTP 503.
//
// INVARIANT (Phase B REV.3 §7): clearEntry is allowed here because NO mutation
// has occurred yet (markExecuting failed before physical mutation began).
// This is rule #3 in the universal failure invariant matrix.
// =============================================================================
inline bool markExecutingOrAbort(const String& requestId) {
  if (!Services::journal.markExecuting(requestId)) {
    Serial.printf("[REST] markExecuting failed for rid=%s — aborting\n",
                  requestId.c_str());
    // No mutation has occurred — clearEntry is safe and correct
    if (!Services::journal.clearEntry(requestId)) {
      // clearEntry itself failed — log but don't change HTTP response
      Serial.println("[REST] WARNING: clearEntry failed after markExecuting failure");
    }
    Web::sendError(503, "Internal error: cannot mark transaction as executing");
    return false;
  }
  return true;
}

// =============================================================================
// 6a. commit (FROM_PENDING path — for atomic mutations like schedule, PIR, etc.)
//
// Transitions PENDING → COMMITTED + queues ACK.
// On success, returns true. On failure, sends HTTP 503 (DOES NOT clearEntry —
// INVARIANT B: mutation may have occurred, evidence must be preserved).
//
// Post-condition on success: journal state == COMMITTED, ACK queued.
// Caller may now send HTTP 200 with ackJson.
// =============================================================================
inline bool commitFromPendingOrFailure(const String& requestId, const String& ackJson) {
  if (!Services::journal.commitTransactionFromPending(requestId, ackJson)) {
    Serial.printf("[REST] commitTransactionFromPending FAILED for rid=%s — preserving evidence\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "Transaction commit FAILED (REST, FROM_PENDING) for " + requestId +
      " — preserving evidence, publishing DURABILITY_FAILURE", 0);
    // INVARIANT B: mutation may have occurred — DO NOT clearEntry
    Web::sendError(503,
      "DURABILITY_FAILURE: transaction could not be committed to NVS journal — please retry");
    return false;
  }
  return true;
}

// =============================================================================
// 6b. commit (EXECUTING path — for physical mutations like relay, OTA, etc.)
//
// Transitions EXECUTING → COMMITTED + queues ACK.
// On success, returns true. On failure, sends HTTP 503 (DOES NOT clearEntry —
// INVARIANT B: physical mutation has already occurred, evidence must be preserved).
//
// Post-condition on success: journal state == COMMITTED, ACK queued.
// Caller may now send HTTP 200 with ackJson.
// =============================================================================
inline bool commitExecutingOrFailure(const String& requestId, const String& ackJson) {
  if (!Services::journal.commitTransaction(requestId, ackJson)) {
    Serial.printf("[REST] commitTransaction FAILED for rid=%s — preserving evidence\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "Transaction commit FAILED (REST, EXECUTING) for " + requestId +
      " — preserving evidence, publishing DURABILITY_FAILURE", 0);
    // INVARIANT B: physical mutation has already occurred — DO NOT clearEntry
    Web::sendError(503,
      "DURABILITY_FAILURE: transaction could not be committed to NVS journal — please retry");
    return false;
  }
  return true;
}

// =============================================================================
// 7. clearEntry on validation failure (pre-mutation cleanup)
//
// Used when domain validation fails AFTER storeIntent was called but BEFORE
// any mutation has occurred. Clears the PENDING entry so the journal doesn't
// accumulate "ghost" entries for commands that never executed.
//
// INVARIANT (Phase B REV.3 §7 rule 4): clearEntry is allowed here because NO
// mutation has occurred — only PENDING state existed.
//
// This helper does NOT send HTTP response — caller sends the appropriate 4xx
// response for the validation failure. The helper just cleans up the journal.
// =============================================================================
inline bool clearEntryOnValidationFailure(const String& requestId) {
  if (!Services::journal.clearEntry(requestId)) {
    // clearEntry itself failed — log but don't change HTTP response
    Serial.printf("[REST] WARNING: clearEntry failed for rid=%s — slot may need manual cleanup\n",
                  requestId.c_str());
    return false;
  }
  return true;
}

}} // namespace Web::Rest

#endif // TIMER12_WEB_HANDLERS_REST_JOURNAL_HELPER_H
