// =============================================================================
// Services/JournalRecord.h — Transaction Journal v4 Record (Rev26 normative)
// =============================================================================
// Phase 1 Implementation per Rev26 APPROVED design.
//
// Record Layout (Rev14 §4, Rev26 confirmed):
//
//   Offset  Field              Size  Description
//   ------  ----------------   ----  ------------------------------------------
//   0       magic              2     0x54, 0x4A ("TJ")
//   2       schemaVersion      1     4
//   3       generation         4     uint32 LE
//   7       recordCRC          4     CRC-32/ISO-HDLC over bytes 0..2 + bytes 11..end
//   --- CANONICAL PAYLOAD (byte 11 onward) ---
//   11      recordState        1
//   12      requestIdLen       1     0..64
//   13..   requestId          var
//   ..      commandHashLen     1     0..64
//   ..      commandHash        var
//   ..      channelId          1     0=N/A, 1..NUM_CHANNELS
//   ..      desiredState       1     0=OFF, 1=ON, 0xFF=N/A
//   ..      previousKnownState 1     0=OFF, 1=ON
//   ..      attempt            1
//   ..      timestamp          4     uint32 LE
//   ..      ackLen             2     uint16 LE, 0..1024
//   ..      ackJson            var
//   ..      (padding to BLOB_SIZE, zeros, NO semantic meaning)
//
// CRC (Rev14 §4, Rev26 confirmed):
//   Algorithm: CRC-32/ISO-HDLC
//   API: ~esp_crc32_le(0xFFFFFFFF, data, len) & 0xFFFFFFFF
//   Test vector: "123456789" → 0xCBF43926
//   CRC INPUT: bytes[0..6] (header) ++ bytes[11..actualPayloadEnd] (canonical payload)
//   CRC does NOT cover: bytes[7..10] (CRC field), padding bytes.
//
// Canonical Equivalence (Rev14 §4):
//   canonicalEqual(A, B) ≡
//       A.schemaVersion == B.schemaVersion
//       AND A.canonicalLength == B.canonicalLength
//       AND memcmp(A.canonicalBytes, B.canonicalBytes, A.canonicalLength) == 0
//
// Generation Ordering (Rev14 §4):
//   Classifier (exact sequence):
//     if genA == genB → GEN_EQUAL
//     else if distAB == 1 → GEN_NEWER_B
//     else if distBA == 1 → GEN_NEWER_A
//     else if distAB == 0x80000000 → GEN_AMBIGUOUS
//     else → GEN_INVALID
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_JOURNAL_RECORD_H
#define TIMER12_SERVICES_JOURNAL_RECORD_H

#include <Arduino.h>
#include <cstdint>
#include <cstring>

namespace Services {

// --- Constants (from Rev14 §4, Rev26 confirmed) ---

static const uint8_t  JOURNAL_SCHEMA_VERSION = 4;
static const uint16_t BLOB_MAGIC1 = 0x54;  // 'T'
static const uint16_t BLOB_MAGIC2 = 0x4A;  // 'J'
static const uint8_t  BLOB_HEADER_SIZE = 11;  // magic(2) + ver(1) + gen(4) + CRC(4)
static const uint16_t BLOB_SIZE = 1200;
static const uint8_t  MAX_REQUEST_ID_LEN = 64;
static const uint8_t  MAX_COMMAND_HASH_LEN = 64;
static const uint16_t MAX_ACK_JSON_LEN = 1024;

// --- Record States (Rev14 §4) ---

enum class RecordState : uint8_t {
    EMPTY             = 0,
    PENDING           = 1,
    EXECUTING         = 2,
    COMMITTED         = 3,
    COMMITTED_UNKNOWN = 4,
    UNKNOWN           = 5,
    FAILED            = 6,
    EXECUTION_FAILED_OUTPUT_MISMATCH = 7,
    // CORRUPTED is a derived state, NOT stored in record (Rev14 §4, Rev9 §2)
};

// --- Generation Relationship (Rev14 §4) ---

enum class GenRelation {
    GEN_EQUAL,
    GEN_NEWER_A,   // A is 1 newer than B
    GEN_NEWER_B,   // B is 1 newer than A
    GEN_AMBIGUOUS,
    GEN_INVALID,
};

// --- Parse Result ---

enum class ParseResult {
    PARSE_VALID,
    PARSE_INVALID,
};

// --- JournalRecord struct (in-RAM representation) ---

struct JournalRecord {
    // Header fields
    uint8_t  schemaVersion;
    uint32_t generation;
    uint32_t crc;

    // Canonical payload fields
    RecordState recordState;
    String requestId;
    String commandHash;
    uint8_t channelId;       // 0=N/A, 1..NUM_CHANNELS
    uint8_t desiredState;    // 0=OFF, 1=ON, 0xFF=N/A
    uint8_t previousKnownState; // 0=OFF, 1=ON
    uint8_t attempt;
    uint32_t timestamp;
    String ackJson;

    // Derived (from safe parse)
    uint16_t canonicalLength;  // bytes from recordState (byte 11) to end of ackJson

    JournalRecord()
        : schemaVersion(JOURNAL_SCHEMA_VERSION)
        , generation(0)
        , crc(0)
        , recordState(RecordState::EMPTY)
        , channelId(0)
        , desiredState(0xFF)
        , previousKnownState(0)
        , attempt(0)
        , timestamp(0)
        , canonicalLength(0)
    {}
};

// =============================================================================
// Serialization / Deserialization (Rev14 §4: Safe Record Parsing)
// =============================================================================

// Serialize record to blob (BLOB_SIZE bytes).
//
// RETURNS:
//   actualPayloadEnd (offset after ackJson, before padding) on success.
//   0 on FAILURE — caller MUST NOT write the blob to NVS.
//
// FAILURE CONDITIONS (P1-1 closure — STRICT, never silently truncate):
//   1. blobSize < BLOB_SIZE
//   2. rec.requestId.length()   > MAX_REQUEST_ID_LEN     (64)
//   3. rec.commandHash.length() > MAX_COMMAND_HASH_LEN   (64)
//   4. rec.ackJson.length()     > MAX_ACK_JSON_LEN       (1024)
//
// On failure, the blob is left unmodified and 0 is returned.
//
// The minimum successful actualPayloadEnd is BLOB_HEADER_SIZE + 11 = 22
// (recordState + reqIdLen + hashLen + channelId + desiredState + prev +
//  attempt + timestamp(4) + ackLen(2)), so 0 unambiguously signals failure.
//
// Padding is zeroed on success (convention, not semantic per Rev14).
uint16_t serializeRecord(const JournalRecord& rec, uint8_t* blob, uint16_t blobSize);

// Deserialize blob to record.
// Every variable-length field is bounds-checked before advancing cursor.
// canonicalLength is derived ONLY from a successfully validated parse.
// If any bounds check fails → PARSE_INVALID.
ParseResult deserializeRecord(const uint8_t* blob, uint16_t blobLen, JournalRecord& outRec);

// =============================================================================
// CRC (Rev14 §4: CRC-32/ISO-HDLC)
// =============================================================================

// Compute CRC over header[0..6] ++ canonicalPayload[11..actualPayloadEnd].
// Uses: ~esp_crc32_le(0xFFFFFFFF, ...) & 0xFFFFFFFF
// CRC does NOT cover: bytes[7..10] (CRC field), padding bytes.
uint32_t computeRecordCRC(const uint8_t* blob, uint16_t actualPayloadEnd);

// Verify CRC: recompute and compare with stored CRC at bytes[7..10].
bool verifyRecordCRC(const uint8_t* blob, uint16_t actualPayloadEnd);

// CRC test vector: "123456789" → 0xCBF43926
// Phase 1 gate: MUST pass before proceeding to Phase 2.
bool verifyCRCGate();

// =============================================================================
// Canonical Equivalence (Rev14 §4)
// =============================================================================

// canonicalEqual(A, B) ≡
//     A.schemaVersion == B.schemaVersion
//     AND A.canonicalLength == B.canonicalLength
//     AND memcmp(A.canonicalBytes, B.canonicalBytes, A.canonicalLength) == 0
//
// canonicalBytes = bytes starting at recordState (byte 11)
// canonicalLength = actual payload length (from safe parse, excluding padding)
// schemaVersion is checked separately before payload comparison
bool canonicalEqual(const JournalRecord& A, const JournalRecord& B);

// Byte-level canonicalEqual on raw blobs (for dual-copy comparison).
// Compares bytes[11..canonicalLength+11] between two blobs.
// Requires canonicalLength to be derived from safe parse.
bool canonicalEqualBlobs(const uint8_t* blobA, const uint8_t* blobB,
                         uint16_t canonicalLenA, uint16_t canonicalLenB,
                         uint8_t schemaA, uint8_t schemaB);

// =============================================================================
// Generation Ordering (Rev14 §4: directional, wrap-safe)
// =============================================================================

// Classify generation relationship (exact sequence per Rev14 §4).
// distAB = (uint32_t)(genB - genA)
// distBA = (uint32_t)(genA - genB)
//
// if genA == genB → GEN_EQUAL
// else if distAB == 1 → GEN_NEWER_B
// else if distBA == 1 → GEN_NEWER_A
// else if distAB == 0x80000000 → GEN_AMBIGUOUS
// else → GEN_INVALID
GenRelation classifyGeneration(uint32_t genA, uint32_t genB);

// Forward distance: how many increments from a to reach b.
// forwardDistance(a, b) = (uint32_t)(b - a)
inline uint32_t forwardDistance(uint32_t a, uint32_t b) {
    return (uint32_t)(b - a);
}

} // namespace Services

#endif // TIMER12_SERVICES_JOURNAL_RECORD_H
