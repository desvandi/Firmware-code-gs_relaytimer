// =============================================================================
// Services/JournalRecord.cpp — Transaction Journal v4 Record (Rev26 normative)
// =============================================================================
// Phase 1 Implementation per Rev26 APPROVED design.
//
// This file implements:
//   - serializeRecord() / deserializeRecord() (safe parse, bounds-checked)
//   - computeRecordCRC() / verifyRecordCRC() (CRC-32/ISO-HDLC via esp_crc32_le)
//   - verifyCRCGate() (test vector "123456789" → 0xCBF43926)
//   - canonicalEqual() / canonicalEqualBlobs() (byte-level, schema+length+memcmp)
//   - classifyGeneration() (directional, wrap-safe serial arithmetic)
//
// All implementations follow Rev14 §4 (consolidated by Rev26) byte-for-byte.
// =============================================================================
#include "JournalRecord.h"
#include <esp_crc.h>  // ESP-IDF esp_crc32_le()

namespace Services {

// ============================================================================
// Serialize: JournalRecord → blob[BLOB_SIZE]
// Returns: actualPayloadEnd (offset after ackJson, before padding) on success.
//          0 on FAILURE (over-limit input or insufficient blob size).
//
// P1-1 CLOSURE (Auditor Rev26 Phase-1 review):
//   This serializer is STRICT. It NEVER silently truncates canonical fields.
//   If any variable-length field exceeds its declared MAX, the call returns 0
//   and the blob is NOT modified. Callers MUST treat 0 as failure and MUST
//   NOT write the resulting blob to NVS.
//
//   Why strict: Rev14 §4 declares MAX_REQUEST_ID_LEN / MAX_COMMAND_HASH_LEN /
//   MAX_ACK_JSON_LEN as part of the canonical format. A parser rejects over-
//   limit values as PARSE_INVALID, so a serializer that silently truncates
//   would produce a record whose canonical bytes differ from the caller's
//   intent — breaking the round-trip identity property and causing silent
//   command-identity drift in the journal.
//
//   Sentinel choice: 0 is safe as failure because the minimum successful
//   actualPayloadEnd is BLOB_HEADER_SIZE + 1 (recordState) + 1 (requestIdLen)
//   + 1 (commandHashLen) + 1 (channelId) + 1 (desiredState) + 1 (previousKnownState)
//   + 1 (attempt) + 4 (timestamp) + 2 (ackLen) = 22. Therefore 0 unambiguously
//   signals failure and can never collide with a valid offset.
// ============================================================================
uint16_t serializeRecord(const JournalRecord& rec, uint8_t* blob, uint16_t blobSize) {
    // --- Pre-flight: REJECT over-limit input. Never truncate. (P1-1) ---
    if (rec.requestId.length() > MAX_REQUEST_ID_LEN) {
        return 0;
    }
    if (rec.commandHash.length() > MAX_COMMAND_HASH_LEN) {
        return 0;
    }
    if (rec.ackJson.length() > MAX_ACK_JSON_LEN) {
        return 0;
    }

    if (blobSize < BLOB_SIZE) return 0;

    memset(blob, 0, blobSize);  // zero-fill (padding convention)

    // --- Header (bytes 0..6) ---
    blob[0] = BLOB_MAGIC1;           // 'T'
    blob[1] = BLOB_MAGIC2;           // 'J'
    blob[2] = rec.schemaVersion;      // 4

    // generation (bytes 3..6, uint32 LE)
    blob[3] = rec.generation & 0xFF;
    blob[4] = (rec.generation >> 8) & 0xFF;
    blob[5] = (rec.generation >> 16) & 0xFF;
    blob[6] = (rec.generation >> 24) & 0xFF;

    // CRC field (bytes 7..10) — filled AFTER payload is written
    // Leave as 0 for now

    // --- Canonical Payload (byte 11 onward) ---
    uint16_t offset = BLOB_HEADER_SIZE;  // 11

    // recordState (1 byte)
    blob[offset++] = (uint8_t)rec.recordState;

    // requestIdLen + requestId
    // (length already validated ≤ MAX_REQUEST_ID_LEN above — no truncation here)
    uint8_t reqIdLen = (uint8_t)rec.requestId.length();
    blob[offset++] = reqIdLen;
    if (reqIdLen > 0) {
        memcpy(&blob[offset], rec.requestId.c_str(), reqIdLen);
        offset += reqIdLen;
    }

    // commandHashLen + commandHash
    // (length already validated ≤ MAX_COMMAND_HASH_LEN above — no truncation here)
    uint8_t hashLen = (uint8_t)rec.commandHash.length();
    blob[offset++] = hashLen;
    if (hashLen > 0) {
        memcpy(&blob[offset], rec.commandHash.c_str(), hashLen);
        offset += hashLen;
    }

    // channelId (1 byte)
    blob[offset++] = rec.channelId;

    // desiredState (1 byte)
    blob[offset++] = rec.desiredState;

    // previousKnownState (1 byte)
    blob[offset++] = rec.previousKnownState;

    // attempt (1 byte)
    blob[offset++] = rec.attempt;

    // timestamp (4 bytes, uint32 LE)
    blob[offset++] = rec.timestamp & 0xFF;
    blob[offset++] = (rec.timestamp >> 8) & 0xFF;
    blob[offset++] = (rec.timestamp >> 16) & 0xFF;
    blob[offset++] = (rec.timestamp >> 24) & 0xFF;

    // ackLen (2 bytes, uint16 LE)
    // (length already validated ≤ MAX_ACK_JSON_LEN above — no truncation here)
    uint16_t ackLen = (uint16_t)rec.ackJson.length();
    blob[offset++] = ackLen & 0xFF;
    blob[offset++] = (ackLen >> 8) & 0xFF;

    // ackJson
    if (ackLen > 0) {
        memcpy(&blob[offset], rec.ackJson.c_str(), ackLen);
        offset += ackLen;
    }

    uint16_t actualPayloadEnd = offset;

    // --- Compute CRC ---
    // CRC INPUT: bytes[0..6] (header) ++ bytes[11..actualPayloadEnd] (canonical payload)
    // CRC does NOT cover: bytes[7..10] (CRC field), padding bytes.
    uint32_t crc = computeRecordCRC(blob, actualPayloadEnd);

    // Store CRC at bytes[7..10] (uint32 LE)
    blob[7]  = crc & 0xFF;
    blob[8]  = (crc >> 8) & 0xFF;
    blob[9]  = (crc >> 16) & 0xFF;
    blob[10] = (crc >> 24) & 0xFF;

    return actualPayloadEnd;
}

// ============================================================================
// Deserialize: blob → JournalRecord (safe parse, bounds-checked)
// Rev14 §4: Every variable-length field is bounds-checked before advancing cursor.
// ============================================================================
ParseResult deserializeRecord(const uint8_t* blob, uint16_t blobLen, JournalRecord& outRec) {
    if (blobLen < BLOB_HEADER_SIZE) {
        return ParseResult::PARSE_INVALID;
    }

    // --- Verify magic ---
    if (blob[0] != BLOB_MAGIC1 || blob[1] != BLOB_MAGIC2) {
        return ParseResult::PARSE_INVALID;
    }

    // --- schemaVersion ---
    outRec.schemaVersion = blob[2];
    if (outRec.schemaVersion != JOURNAL_SCHEMA_VERSION) {
        return ParseResult::PARSE_INVALID;
    }

    // --- generation (bytes 3..6, uint32 LE) ---
    outRec.generation = blob[3] | ((uint32_t)blob[4] << 8) |
                        ((uint32_t)blob[5] << 16) | ((uint32_t)blob[6] << 24);

    // --- CRC (bytes 7..10, uint32 LE) ---
    outRec.crc = blob[7] | ((uint32_t)blob[8] << 8) |
                 ((uint32_t)blob[9] << 16) | ((uint32_t)blob[10] << 24);

    // --- Canonical Payload (byte 11 onward) ---
    uint16_t offset = BLOB_HEADER_SIZE;  // 11

    // Step 1: recordState (1 byte)
    if (offset >= blobLen) return ParseResult::PARSE_INVALID;
    outRec.recordState = (RecordState)blob[offset++];
    // Validate recordState is in valid range
    if ((uint8_t)outRec.recordState > (uint8_t)RecordState::EXECUTION_FAILED_OUTPUT_MISMATCH) {
        return ParseResult::PARSE_INVALID;
    }

    // Step 2: requestIdLen (1 byte)
    if (offset >= blobLen) return ParseResult::PARSE_INVALID;
    uint8_t reqIdLen = blob[offset++];
    if (reqIdLen > MAX_REQUEST_ID_LEN) return ParseResult::PARSE_INVALID;

    // Step 3: requestId
    if (offset + reqIdLen > blobLen) return ParseResult::PARSE_INVALID;
    if (reqIdLen > 0) {
        outRec.requestId = String((const char*)(&blob[offset]), reqIdLen);
    } else {
        outRec.requestId = "";
    }
    offset += reqIdLen;

    // Step 4: commandHashLen (1 byte)
    if (offset >= blobLen) return ParseResult::PARSE_INVALID;
    uint8_t hashLen = blob[offset++];
    if (hashLen > MAX_COMMAND_HASH_LEN) return ParseResult::PARSE_INVALID;

    // Step 5: commandHash
    if (offset + hashLen > blobLen) return ParseResult::PARSE_INVALID;
    if (hashLen > 0) {
        outRec.commandHash = String((const char*)(&blob[offset]), hashLen);
    } else {
        outRec.commandHash = "";
    }
    offset += hashLen;

    // Step 6: channelId (1 byte)
    if (offset >= blobLen) return ParseResult::PARSE_INVALID;
    outRec.channelId = blob[offset++];

    // Step 7: desiredState (1 byte)
    if (offset >= blobLen) return ParseResult::PARSE_INVALID;
    outRec.desiredState = blob[offset++];

    // Step 8: previousKnownState (1 byte)
    if (offset >= blobLen) return ParseResult::PARSE_INVALID;
    outRec.previousKnownState = blob[offset++];

    // Step 9: attempt (1 byte)
    if (offset >= blobLen) return ParseResult::PARSE_INVALID;
    outRec.attempt = blob[offset++];

    // Step 10: timestamp (4 bytes, uint32 LE)
    if (offset + 4 > blobLen) return ParseResult::PARSE_INVALID;
    outRec.timestamp = blob[offset] | ((uint32_t)blob[offset+1] << 8) |
                       ((uint32_t)blob[offset+2] << 16) | ((uint32_t)blob[offset+3] << 24);
    offset += 4;

    // Step 11: ackLen (2 bytes, uint16 LE)
    if (offset + 2 > blobLen) return ParseResult::PARSE_INVALID;
    uint16_t ackLen = blob[offset] | ((uint16_t)blob[offset+1] << 8);
    offset += 2;
    if (ackLen > MAX_ACK_JSON_LEN) return ParseResult::PARSE_INVALID;

    // Step 12: ackJson
    if (offset + ackLen > blobLen) return ParseResult::PARSE_INVALID;
    if (ackLen > 0) {
        outRec.ackJson = String((const char*)(&blob[offset]), ackLen);
    } else {
        outRec.ackJson = "";
    }
    offset += ackLen;

    // Step 13: canonicalLength = bytes from recordState (byte 11) to here
    outRec.canonicalLength = offset - BLOB_HEADER_SIZE;

    // Step 14: padding — NOT validated (Rev14 padding semantics: no semantic meaning)

    return ParseResult::PARSE_VALID;
}

// ============================================================================
// CRC: CRC-32/ISO-HDLC via esp_crc32_le
// Rev14 §4: ~esp_crc32_le(0xFFFFFFFF, data, len) & 0xFFFFFFFF
// CRC INPUT: bytes[0..6] (header) ++ bytes[11..actualPayloadEnd] (canonical payload)
// ============================================================================
uint32_t computeRecordCRC(const uint8_t* blob, uint16_t actualPayloadEnd) {
    // CRC over header[0..6] (7 bytes)
    uint32_t state = esp_crc32_le(0xFFFFFFFF, blob, 7);

    // Continue CRC over canonical payload[11..actualPayloadEnd]
    if (actualPayloadEnd > BLOB_HEADER_SIZE) {
        uint16_t payloadLen = actualPayloadEnd - BLOB_HEADER_SIZE;
        state = esp_crc32_le(state, blob + BLOB_HEADER_SIZE, payloadLen);
    }

    // Final complement
    return ~state & 0xFFFFFFFF;
}

// ============================================================================
// Verify CRC: recompute and compare with stored CRC at bytes[7..10]
// ============================================================================
bool verifyRecordCRC(const uint8_t* blob, uint16_t actualPayloadEnd) {
    if (actualPayloadEnd < BLOB_HEADER_SIZE) return false;

    uint32_t computed = computeRecordCRC(blob, actualPayloadEnd);

    // Stored CRC at bytes[7..10] (uint32 LE)
    uint32_t stored = blob[7] | ((uint32_t)blob[8] << 8) |
                      ((uint32_t)blob[9] << 16) | ((uint32_t)blob[10] << 24);

    return computed == stored;
}

// ============================================================================
// CRC Gate: test vector "123456789" → 0xCBF43926
// Phase 1 gate: MUST pass before proceeding to Phase 2.
// ============================================================================
bool verifyCRCGate() {
    const uint8_t test[] = "123456789";
    uint32_t result = ~esp_crc32_le(0xFFFFFFFF, test, 9) & 0xFFFFFFFF;
    return result == 0xCBF43926;
}

// ============================================================================
// Canonical Equivalence (Rev14 §4)
// canonicalEqual(A, B) ≡
//     A.schemaVersion == B.schemaVersion
//     AND A.canonicalLength == B.canonicalLength
//     AND memcmp(A.canonicalBytes, B.canonicalBytes, A.canonicalLength) == 0
// ============================================================================
bool canonicalEqual(const JournalRecord& A, const JournalRecord& B) {
    // Schema version check first
    if (A.schemaVersion != B.schemaVersion) return false;

    // Canonical length check
    if (A.canonicalLength != B.canonicalLength) return false;

    if (A.canonicalLength == 0) return true;  // both empty

    // Byte-level comparison of all canonical payload fields
    // (not memcmp on struct — must be byte-level per Rev14)
    // We compare field-by-field since String comparison is byte-exact.
    if ((uint8_t)A.recordState != (uint8_t)B.recordState) return false;
    if (A.requestId != B.requestId) return false;
    if (A.commandHash != B.commandHash) return false;
    if (A.channelId != B.channelId) return false;
    if (A.desiredState != B.desiredState) return false;
    if (A.previousKnownState != B.previousKnownState) return false;
    if (A.attempt != B.attempt) return false;
    if (A.timestamp != B.timestamp) return false;
    if (A.ackJson != B.ackJson) return false;

    return true;
}

// ============================================================================
// Byte-level canonicalEqual on raw blobs
// Compares bytes[11..canonicalLength+11] between two blobs.
// ============================================================================
bool canonicalEqualBlobs(const uint8_t* blobA, const uint8_t* blobB,
                         uint16_t canonicalLenA, uint16_t canonicalLenB,
                         uint8_t schemaA, uint8_t schemaB) {
    // Schema version check
    if (schemaA != schemaB) return false;

    // Canonical length check
    if (canonicalLenA != canonicalLenB) return false;

    if (canonicalLenA == 0) return true;

    // Byte-level memcmp on canonical payload (bytes 11 onward)
    return memcmp(blobA + BLOB_HEADER_SIZE, blobB + BLOB_HEADER_SIZE, canonicalLenA) == 0;
}

// ============================================================================
// Generation Ordering (Rev14 §4: directional, wrap-safe)
// Classifier (exact sequence):
//   if genA == genB → GEN_EQUAL
//   else if distAB == 1 → GEN_NEWER_B
//   else if distBA == 1 → GEN_NEWER_A
//   else if distAB == 0x80000000 → GEN_AMBIGUOUS
//   else → GEN_INVALID
// ============================================================================
GenRelation classifyGeneration(uint32_t genA, uint32_t genB) {
    if (genA == genB) {
        return GenRelation::GEN_EQUAL;
    }

    uint32_t distAB = forwardDistance(genA, genB);  // (uint32_t)(genB - genA)
    uint32_t distBA = forwardDistance(genB, genA);  // (uint32_t)(genA - genB)

    if (distAB == 1) {
        return GenRelation::GEN_NEWER_B;
    }
    if (distBA == 1) {
        return GenRelation::GEN_NEWER_A;
    }
    if (distAB == 0x80000000) {
        return GenRelation::GEN_AMBIGUOUS;
    }
    return GenRelation::GEN_INVALID;
}

} // namespace Services
