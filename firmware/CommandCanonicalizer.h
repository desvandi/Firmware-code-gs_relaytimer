// =============================================================================
// Services/CommandCanonicalizer.h — PD-001 Canonical Command & Transaction Model
// =============================================================================
// PHASE 6 — PD-001: Canonical Command & Transaction Identity Model.
//
// This module is the SINGLE shared canonical command representation used by
// BOTH REST and MQTT ingress paths. It owns three responsibilities:
//
//   1. transactionId (requestId) validation
//   2. Canonical command serialization + SHA-256 commandHash
//   3. Duplicate / Conflict decision against TransactionJournal
//
// BOUNDARY (per PD-001 directive):
//   - This module does NOT persist anything. Persistence is owned by
//     TransactionJournal (PD-002 scope).
//   - This module does NOT execute mutations. Mutation is owned by
//     RelayEngine / Scheduler / etc (PD-004 scope).
//   - This module does NOT enforce expiry / retry policy. That is PD-003.
//   - This module ONLY provides:
//       (a) deterministic identity validation,
//       (b) deterministic canonical serialization,
//       (c) deterministic hash,
//       (d) deterministic duplicate/conflict decision.
//
// CANONICAL COMMAND MODEL (per Final Canonical Production Contract):
//
//   version         : uint8_t   (currently 1)
//   transactionId   : string    (a.k.a. requestId — semantic alias, NOT a
//                                 second identity; one transaction = one id)
//   type            : string    (lowercase, e.g. "relay", "schedule", "pir",
//                                 "channel", "time", "system", "config", "ota")
//   action          : string    (lowercase, e.g. "on", "off", "set_mode",
//                                 "upsert", "delete", "rename", ...)
//   payload         : object    (per-type canonical field set, deterministic
//                                 field order, explicit type representation)
//   issuedAt        : string?   (envelope, NOT part of hash)
//   expiresAt       : string?   (envelope, NOT part of hash)
//
// COMMAND HASH CONTRACT:
//
//   commandHash = SHA-256( version
//                         + canonical(type)        // lowercase
//                         + canonical(action)      // lowercase
//                         + canonical(payload) )   // deterministic
//
//   EXCLUDED from hash input (envelope-only):
//     - transactionId / requestId
//     - issuedAt / expiresAt
//     - MQTT topic / HTTP headers / HTTP method / ACK metadata
//
// INVARIANTS:
//   - Same logical operation (version + type + action + payload) → SAME hash,
//     regardless of transport, field order, whitespace, or requestId.
//   - Different transactionId, same logical operation → SAME hash.
//   - Same transactionId, different logical operation → DIFFERENT hash
//     (triggers CONFLICT decision, not duplicate).
//
// REGISTRY (per directive §15):
//   The canonical schema per (type, action) is hardcoded in this module.
//   Adding a new command type requires extending the registry below. Unknown
//   types or actions are REJECTED (never silently ignored).
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_COMMAND_CANONICALIZER_H
#define TIMER12_SERVICES_COMMAND_CANONICALIZER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Crypto.h"

namespace Services {

// Protocol version of the canonical command envelope.
// Bumped only on breaking change to the canonical schema itself.
// Per directive §13: unsupported version → REJECT (no silent downgrade).
static const uint8_t CANONICAL_COMMAND_VERSION = 1;

// Maximum lengths for transactionId validation (per directive §5).
static const size_t MAX_TRANSACTION_ID_LEN = 64;
static const size_t MIN_TRANSACTION_ID_LEN = 1;

// Outcome of a duplicate / conflict lookup against the journal.
enum class TransactionDecision : uint8_t {
  NEW       = 0,  // transactionId not found in journal — proceed with execution
  DUPLICATE = 1,  // transactionId found, commandHash MATCHES — replay original ACK
  CONFLICT  = 2,  // transactionId found, commandHash DIFFERS — REJECT, no mutation
};

// Result of canonicalizing + hashing an incoming command.
struct CanonicalResult {
  bool ok;                       // false if validation failed
  String transactionId;          // validated transactionId (empty if missing)
  String commandHash;            // SHA-256 hex of canonical serialization (empty if !ok)
  String canonicalString;        // the pre-hash canonical string (for debugging/logging)
  String errorMessage;           // human-readable error (empty if ok)
};

// Result of a transaction decision lookup.
struct DecisionResult {
  TransactionDecision decision;
  String previousHash;           // hash stored in journal (empty if NEW)
  String previousAckJson;        // ACK JSON stored in journal (empty if NEW)
};

class CommandCanonicalizer {
public:
  // -----------------------------------------------------------------------
  // validateTransactionId
  //
  // Per directive §5:
  //   - non-empty
  //   - bounded length (1..64)
  //   - deterministic string
  //   - no control characters
  //   - no whitespace
  //   - allowed charset: [a-zA-Z0-9-_]  (UUID v4 / URL-safe base64url subset)
  //
  // Returns true if valid. On failure, errorMessageOut is populated.
  // -----------------------------------------------------------------------
  static bool validateTransactionId(const String& tid,
                                    String& errorMessageOut);

  // -----------------------------------------------------------------------
  // validateProtocolVersion
  //
  // Per directive §13: unsupported version → REJECT (no silent downgrade).
  // Currently only version 1 is supported.
  // -----------------------------------------------------------------------
  static bool validateProtocolVersion(int version,
                                      String& errorMessageOut);

  // -----------------------------------------------------------------------
  // canonicalizeAndHash
  //
  // Builds the canonical command representation from a parsed JSON document
  // and computes its SHA-256 hash.
  //
  // The input doc is expected to contain at minimum:
  //   - "type"    : string (lowercase or uppercase — will be lowercased)
  //   - "action"  : string (lowercase or uppercase — will be lowercased)
  //   - "requestId" : string (validated as transactionId)
  //   - per-type payload fields
  //
  // The optional "version" field is validated if present (defaults to 1).
  // The optional "issuedAt" / "expiresAt" fields are accepted as envelope
  // metadata but EXCLUDED from the hash (per directive §12).
  //
  // Returns CanonicalResult with ok=false if validation fails.
  // -----------------------------------------------------------------------
  static CanonicalResult canonicalizeAndHash(const JsonDocument& doc);

  // -----------------------------------------------------------------------
  // decideTransaction
  //
  // Looks up the transactionId in the journal and returns one of:
  //   NEW       — not found, proceed with execution
  //   DUPLICATE — found with same hash, replay original ACK
  //   CONFLICT  — found with different hash, REJECT (no mutation)
  //
  // Per directive §16: existing transaction records MUST NOT be overwritten
  // by a new command. Conflict → REJECT only.
  // -----------------------------------------------------------------------
  static DecisionResult decideTransaction(const String& transactionId,
                                          const String& commandHash);

  // -----------------------------------------------------------------------
  // isKnownCommandType
  //
  // Per directive §14/§15: returns true if (type, action) is a registered
  // command in the canonical registry.
  // -----------------------------------------------------------------------
  static bool isKnownCommandType(const String& type, const String& action);

  // -----------------------------------------------------------------------
  // isFieldAllowed
  //
  // Per directive §10 (Optional Field Policy) + §15 (Command Registry):
  // returns true if `fieldName` is a recognized field for the given type.
  // Used by ingress layers to reject unknown fields BEFORE hashing.
  //
  // Note: "type", "action", "requestId", "version", "issuedAt", "expiresAt"
  // are envelope fields and always allowed.
  // -----------------------------------------------------------------------
  static bool isFieldAllowed(const String& type, const String& fieldName);

private:
  // -----------------------------------------------------------------------
  // buildCanonicalString
  //
  // Constructs the deterministic canonical serialization:
  //   "v{version}|{type}|{action}|field1=val1|field2=val2|..."
  //
  // Field order is FIXED per type (per directive §8/§9). Values are
  // rendered with explicit type representation:
  //   - bool   → "true" / "false"
  //   - int    → decimal string (no leading zeros, no sign for non-negative)
  //   - string → verbatim (NO lowercasing of payload strings per directive §8)
  //
  // Returns empty string on schema violation.
  // -----------------------------------------------------------------------
  static String buildCanonicalString(const JsonDocument& doc,
                                     const String& type,
                                     const String& action,
                                     String& errorMessageOut);
};

} // namespace Services

#endif // TIMER12_SERVICES_COMMAND_CANONICALIZER_H
