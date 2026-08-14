// =============================================================================
// JournalRecordTest.cpp — Host-side Phase 1 verification harness
// =============================================================================
// AUDITOR PHASE 1 GATE — Rev26
// This harness proves byte-level correctness of JournalRecord against the
// Rev14 §4 / Rev26 normative contract. It compiles the SAME firmware source
// file (firmware/JournalRecord.cpp) on the host using two minimal shims:
//
//     firmware/test/host/shims/Arduino.h   — minimal String + min()
//     firmware/test/host/shims/esp_crc.h  — pure-C++ esp_crc32_le()
//
// Every test below maps 1:1 to an item in the auditor's Required closure
// table. Exit code is non-zero if ANY test fails.
//
// Build:
//     g++ -std=c++17 -I firmware/test/host/shims -I firmware
//         firmware/test/host/JournalRecordTest.cpp firmware/JournalRecord.cpp
//         -o /tmp/journal_record_test
// Run:
//     /tmp/journal_record_test
// =============================================================================
#include "JournalRecord.h"   // firmware header (under test)
#include "esp_crc.h"         // host shim for esp_crc32_le (used by CRC KAT test)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

using namespace Services;

// --- Minimal test framework ------------------------------------------------

static int g_passCount = 0;
static int g_failCount = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            printf("  [PASS] %s\n", msg);                                    \
            g_passCount++;                                                   \
        } else {                                                             \
            printf("  [FAIL] %s   (line %d)\n", msg, __LINE__);               \
            g_failCount++;                                                   \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected, msg)                                      \
    do {                                                                     \
        auto _a = (actual);                                                  \
        auto _e = (expected);                                                \
        if (_a == _e) {                                                      \
            printf("  [PASS] %s\n", msg);                                    \
            g_passCount++;                                                   \
        } else {                                                             \
            printf("  [FAIL] %s   (got=%llu, expected=%llu, line %d)\n",     \
                   msg,                                                      \
                   (unsigned long long)_a,                                   \
                   (unsigned long long)_e,                                   \
                   __LINE__);                                                \
            g_failCount++;                                                   \
        }                                                                    \
    } while (0)

// ============================================================================
// Helper: build a sample record for round-trip tests
// ============================================================================
static JournalRecord makeSampleRecord() {
    JournalRecord r;
    r.schemaVersion      = JOURNAL_SCHEMA_VERSION;
    r.generation         = 0x00000042;
    r.recordState        = RecordState::COMMITTED;
    r.requestId          = "req-abc-123";
    r.commandHash        = "sha256:deadbeef";
    r.channelId          = 3;
    r.desiredState       = 1;  // ON
    r.previousKnownState = 0;  // OFF
    r.attempt            = 1;
    r.timestamp          = 0xDEADBEEF;
    r.ackJson            = "{\"ok\":true}";
    return r;
}

// ============================================================================
// TEST 1 — Exact byte layout
// ============================================================================
// Verifies that header bytes are at the offsets documented in JournalRecord.h
// and that the canonical payload fields appear in the documented order with
// the documented length-prefix bytes.
// ============================================================================
static void test_exact_byte_layout() {
    printf("\n[TEST 1] Exact byte layout (header + canonical payload)\n");

    JournalRecord r = makeSampleRecord();
    uint8_t blob[BLOB_SIZE];
    uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);

    CHECK(payloadEnd > 0, "serialize succeeded (payloadEnd > 0)");

    // --- Header (bytes 0..10) ---
    CHECK_EQ(blob[0], (uint8_t)BLOB_MAGIC1, "byte 0 = magic1 ('T')");
    CHECK_EQ(blob[1], (uint8_t)BLOB_MAGIC2, "byte 1 = magic2 ('J')");
    CHECK_EQ(blob[2], (uint8_t)JOURNAL_SCHEMA_VERSION, "byte 2 = schemaVersion (4)");

    // generation (bytes 3..6, uint32 LE)
    uint32_t gen_from_blob = (uint32_t)blob[3]
                            | ((uint32_t)blob[4] << 8)
                            | ((uint32_t)blob[5] << 16)
                            | ((uint32_t)blob[6] << 24);
    CHECK_EQ(gen_from_blob, (uint32_t)0x00000042, "bytes 3..6 = generation (LE)");

    // CRC at bytes 7..10 — must be non-zero (record has content)
    uint32_t crc_from_blob = (uint32_t)blob[7]
                            | ((uint32_t)blob[8] << 8)
                            | ((uint32_t)blob[9] << 16)
                            | ((uint32_t)blob[10] << 24);
    CHECK(crc_from_blob != 0, "bytes 7..10 = CRC (non-zero for non-empty record)");

    // --- Canonical payload (byte 11 onward) ---
    uint16_t offset = BLOB_HEADER_SIZE;  // 11

    // byte 11: recordState
    CHECK_EQ(blob[offset], (uint8_t)RecordState::COMMITTED, "byte 11 = recordState (COMMITTED=3)");
    offset += 1;

    // byte 12: requestIdLen
    uint8_t expectedReqIdLen = (uint8_t)r.requestId.length();
    CHECK_EQ(blob[offset], expectedReqIdLen, "byte 12 = requestIdLen");
    offset += 1;

    // bytes 13..13+len-1: requestId
    CHECK(memcmp(&blob[offset], r.requestId.c_str(), expectedReqIdLen) == 0,
          "bytes 13.. = requestId ASCII");
    offset += expectedReqIdLen;

    // next: commandHashLen
    uint8_t expectedHashLen = (uint8_t)r.commandHash.length();
    CHECK_EQ(blob[offset], expectedHashLen, "next byte = commandHashLen");
    offset += 1;

    // commandHash
    CHECK(memcmp(&blob[offset], r.commandHash.c_str(), expectedHashLen) == 0,
          "next bytes = commandHash ASCII");
    offset += expectedHashLen;

    // channelId
    CHECK_EQ(blob[offset], (uint8_t)3, "next byte = channelId (3)");
    offset += 1;

    // desiredState
    CHECK_EQ(blob[offset], (uint8_t)1, "next byte = desiredState (ON=1)");
    offset += 1;

    // previousKnownState
    CHECK_EQ(blob[offset], (uint8_t)0, "next byte = previousKnownState (OFF=0)");
    offset += 1;

    // attempt
    CHECK_EQ(blob[offset], (uint8_t)1, "next byte = attempt (1)");
    offset += 1;

    // timestamp (4 bytes LE)
    uint32_t ts_from_blob = (uint32_t)blob[offset]
                           | ((uint32_t)blob[offset+1] << 8)
                           | ((uint32_t)blob[offset+2] << 16)
                           | ((uint32_t)blob[offset+3] << 24);
    CHECK_EQ(ts_from_blob, (uint32_t)0xDEADBEEF, "next 4 bytes = timestamp (LE)");
    offset += 4;

    // ackLen (2 bytes LE)
    uint16_t ackLen_from_blob = (uint16_t)blob[offset]
                                | ((uint16_t)blob[offset+1] << 8);
    CHECK_EQ(ackLen_from_blob, (uint16_t)r.ackJson.length(),
             "next 2 bytes = ackLen (LE)");
    offset += 2;

    // ackJson
    CHECK(memcmp(&blob[offset], r.ackJson.c_str(), r.ackJson.length()) == 0,
          "next bytes = ackJson ASCII");
    offset += r.ackJson.length();

    CHECK_EQ(offset, payloadEnd, "payloadEnd matches walked offset");

    // Padding (bytes payloadEnd..BLOB_SIZE-1) must be zero
    bool padding_zero = true;
    for (uint16_t i = payloadEnd; i < BLOB_SIZE; i++) {
        if (blob[i] != 0) { padding_zero = false; break; }
    }
    CHECK(padding_zero, "padding bytes are zero");
}

// ============================================================================
// TEST 2 — Serialize → Deserialize round-trip identity
// ============================================================================
static void test_round_trip() {
    printf("\n[TEST 2] serialize → deserialize round-trip identity\n");

    JournalRecord original = makeSampleRecord();
    uint8_t blob[BLOB_SIZE];

    uint16_t payloadEnd = serializeRecord(original, blob, BLOB_SIZE);
    CHECK(payloadEnd > 0, "serialize succeeded");

    JournalRecord restored;
    ParseResult pr = deserializeRecord(blob, BLOB_SIZE, restored);
    CHECK(pr == ParseResult::PARSE_VALID, "deserialize → PARSE_VALID");

    CHECK(restored.schemaVersion == original.schemaVersion, "schemaVersion restored");
    CHECK(restored.generation == original.generation, "generation restored");
    CHECK(restored.recordState == original.recordState, "recordState restored");
    CHECK(restored.requestId == original.requestId, "requestId restored");
    CHECK(restored.commandHash == original.commandHash, "commandHash restored");
    CHECK(restored.channelId == original.channelId, "channelId restored");
    CHECK(restored.desiredState == original.desiredState, "desiredState restored");
    CHECK(restored.previousKnownState == original.previousKnownState, "previousKnownState restored");
    CHECK(restored.attempt == original.attempt, "attempt restored");
    CHECK(restored.timestamp == original.timestamp, "timestamp restored");
    CHECK(restored.ackJson == original.ackJson, "ackJson restored");

    // canonicalLength must equal payloadEnd - BLOB_HEADER_SIZE
    CHECK_EQ(restored.canonicalLength,
             (uint16_t)(payloadEnd - BLOB_HEADER_SIZE),
             "canonicalLength = payloadEnd - 11");

    // CRC must verify
    CHECK(verifyRecordCRC(blob, payloadEnd), "verifyRecordCRC → true");
}

// ============================================================================
// TEST 3 — Boundary tests: max-length fields serialize OK
//          Boundary tests: over-limit fields are REJECTED (P1-1 closure)
// ============================================================================
static std::string makeStringOf(size_t n) {
    return std::string(n, 'x');
}

static void test_boundary_requestId() {
    printf("\n[TEST 3a] requestId boundary (max=64 valid, 65 REJECT)\n");

    uint8_t blob[BLOB_SIZE];

    // --- max=64 → valid ---
    {
        JournalRecord r = makeSampleRecord();
        r.requestId = String(makeStringOf(64).c_str(), 64);
        uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);
        CHECK(payloadEnd > 0, "requestId.length()==64 → serialize OK (no truncation)");

        JournalRecord restored;
        ParseResult pr = deserializeRecord(blob, BLOB_SIZE, restored);
        CHECK(pr == ParseResult::PARSE_VALID, "requestId.length()==64 → deserialize VALID");
        CHECK_EQ(restored.requestId.length(), (unsigned)64,
                 "requestId round-trips at length 64");
        CHECK(restored.requestId == r.requestId,
              "requestId bytes preserved at length 64");
    }

    // --- 65 → REJECT (P1-1: must NOT silently truncate) ---
    {
        JournalRecord r = makeSampleRecord();
        r.requestId = String(makeStringOf(65).c_str(), 65);

        // Pre-fill blob with a sentinel so we can prove the serializer did NOT write.
        memset(blob, 0xAB, BLOB_SIZE);

        uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);
        CHECK(payloadEnd == 0, "requestId.length()==65 → serialize FAILS (returns 0)");

        // Verify blob was NOT modified — caller must not write a half-baked blob.
        bool blob_untouched = true;
        for (uint16_t i = 0; i < BLOB_SIZE; i++) {
            if (blob[i] != 0xAB) { blob_untouched = false; break; }
        }
        CHECK(blob_untouched, "requestId.length()==65 → blob left UNMODIFIED (no truncation)");
    }
}

static void test_boundary_commandHash() {
    printf("\n[TEST 3b] commandHash boundary (max=64 valid, 65 REJECT)\n");

    uint8_t blob[BLOB_SIZE];

    // --- max=64 → valid ---
    {
        JournalRecord r = makeSampleRecord();
        r.commandHash = String(makeStringOf(64).c_str(), 64);
        uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);
        CHECK(payloadEnd > 0, "commandHash.length()==64 → serialize OK");

        JournalRecord restored;
        ParseResult pr = deserializeRecord(blob, BLOB_SIZE, restored);
        CHECK(pr == ParseResult::PARSE_VALID, "commandHash.length()==64 → deserialize VALID");
        CHECK_EQ(restored.commandHash.length(), (unsigned)64,
                 "commandHash round-trips at length 64");
    }

    // --- 65 → REJECT ---
    {
        JournalRecord r = makeSampleRecord();
        r.commandHash = String(makeStringOf(65).c_str(), 65);
        memset(blob, 0xCD, BLOB_SIZE);

        uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);
        CHECK(payloadEnd == 0, "commandHash.length()==65 → serialize FAILS (returns 0)");

        bool blob_untouched = true;
        for (uint16_t i = 0; i < BLOB_SIZE; i++) {
            if (blob[i] != 0xCD) { blob_untouched = false; break; }
        }
        CHECK(blob_untouched, "commandHash.length()==65 → blob left UNMODIFIED");
    }
}

static void test_boundary_ackJson() {
    printf("\n[TEST 3c] ackJson boundary (max=1024 valid, 1025 REJECT)\n");

    uint8_t blob[BLOB_SIZE];

    // --- max=1024 → valid ---
    {
        JournalRecord r = makeSampleRecord();
        r.ackJson = String(makeStringOf(1024).c_str(), 1024);
        uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);
        CHECK(payloadEnd > 0, "ackJson.length()==1024 → serialize OK");

        JournalRecord restored;
        ParseResult pr = deserializeRecord(blob, BLOB_SIZE, restored);
        CHECK(pr == ParseResult::PARSE_VALID, "ackJson.length()==1024 → deserialize VALID");
        CHECK_EQ(restored.ackJson.length(), (unsigned)1024,
                 "ackJson round-trips at length 1024");
    }

    // --- 1025 → REJECT ---
    {
        JournalRecord r = makeSampleRecord();
        r.ackJson = String(makeStringOf(1025).c_str(), 1025);
        memset(blob, 0xEF, BLOB_SIZE);

        uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);
        CHECK(payloadEnd == 0, "ackJson.length()==1025 → serialize FAILS (returns 0)");

        bool blob_untouched = true;
        for (uint16_t i = 0; i < BLOB_SIZE; i++) {
            if (blob[i] != 0xEF) { blob_untouched = false; break; }
        }
        CHECK(blob_untouched, "ackJson.length()==1025 → blob left UNMODIFIED");
    }
}

// ============================================================================
// TEST 4 — Malformed variable-length fields → PARSE_INVALID
// ============================================================================
// Construct valid blobs, then deliberately truncate / corrupt each variable
// length field to ensure the parser rejects them.
// ============================================================================
static void test_malformed_inputs() {
    printf("\n[TEST 4] Malformed variable-length field handling\n");

    JournalRecord r = makeSampleRecord();
    uint8_t blob[BLOB_SIZE];
    uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);
    CHECK(payloadEnd > 0, "baseline serialize OK");

    JournalRecord dummy;

    // 4a: blob too short for header
    {
        ParseResult pr = deserializeRecord(blob, BLOB_HEADER_SIZE - 1, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID, "blobLen < header → INVALID");
    }

    // 4b: bad magic
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        bad[0] = 0x00;
        ParseResult pr = deserializeRecord(bad, BLOB_SIZE, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID, "magic1 wrong → INVALID");
    }
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        bad[1] = 0x00;
        ParseResult pr = deserializeRecord(bad, BLOB_SIZE, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID, "magic2 wrong → INVALID");
    }

    // 4c: bad schemaVersion
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        bad[2] = 99;  // not 4
        ParseResult pr = deserializeRecord(bad, BLOB_SIZE, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID, "schemaVersion != 4 → INVALID");
    }

    // 4d: requestIdLen declared > MAX_REQUEST_ID_LEN
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        bad[BLOB_HEADER_SIZE + 1] = MAX_REQUEST_ID_LEN + 1;  // 65
        ParseResult pr = deserializeRecord(bad, BLOB_SIZE, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID,
              "requestIdLen=65 declared → INVALID");
    }

    // 4e: requestIdLen claims more bytes than the blob has
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        // Set requestIdLen to a value that, added to current offset,
        // exceeds blobLen. We shrink blobLen to force truncation.
        uint8_t reqIdLen = bad[BLOB_HEADER_SIZE + 1];
        // Truncate the blob just after reqIdLen byte but before all reqId bytes
        uint16_t truncatedLen = BLOB_HEADER_SIZE + 2 + (reqIdLen / 2);
        ParseResult pr = deserializeRecord(bad, truncatedLen, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID,
              "requestIdLen > remaining bytes → INVALID");
    }

    // 4f: commandHashLen declared > MAX_COMMAND_HASH_LEN
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        // Walk to commandHashLen position
        uint16_t off = BLOB_HEADER_SIZE + 1 + bad[BLOB_HEADER_SIZE + 1] + 1;
        bad[off] = MAX_COMMAND_HASH_LEN + 1;  // 65
        ParseResult pr = deserializeRecord(bad, BLOB_SIZE, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID,
              "commandHashLen=65 declared → INVALID");
    }

    // 4g: ackLen declared > MAX_ACK_JSON_LEN
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        // ackLen position = payloadEnd - r.ackJson.length() - 2
        uint16_t ackLenPos = payloadEnd - r.ackJson.length() - 2;
        bad[ackLenPos]     = 0xFF;  // low byte
        bad[ackLenPos + 1] = 0x0F;  // high byte → 0x0FFF = 4095 > 1024
        ParseResult pr = deserializeRecord(bad, BLOB_SIZE, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID,
              "ackLen=4095 declared → INVALID");
    }

    // 4h: ackLen claims more bytes than blob has
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        // Declare a length that fits within MAX_ACK_JSON_LEN but exceeds
        // remaining buffer. Truncate the blob just after ackLen.
        uint16_t ackLenPos = payloadEnd - r.ackJson.length() - 2;
        bad[ackLenPos]     = 0x80;  // 128 bytes claimed
        bad[ackLenPos + 1] = 0x00;
        // Truncate blob right after the ackLen field
        uint16_t truncatedLen = ackLenPos + 2 + 4;
        ParseResult pr = deserializeRecord(bad, truncatedLen, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID,
              "ackLen > remaining bytes → INVALID");
    }

    // 4i: recordState has invalid enum value
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        bad[BLOB_HEADER_SIZE] = 0xFE;  // > EXECUTION_FAILED_OUTPUT_MISMATCH (7)
        ParseResult pr = deserializeRecord(bad, BLOB_SIZE, dummy);
        CHECK(pr == ParseResult::PARSE_INVALID,
              "recordState=0xFE (out of enum) → INVALID");
    }
}

// ============================================================================
// TEST 5 — CRC known-answer test: "123456789" → 0xCBF43926
// ============================================================================
// Rev14 §4: "Test vector: '123456789' → 0xCBF43926 ... implementation MUST
// reproduce these expected outcomes with automated unit tests before journal
// integration."
// ============================================================================
static void test_crc_known_answer() {
    printf("\n[TEST 5] CRC-32/ISO-HDLC known-answer test\n");

    // First: invoke the firmware's own gate (uses esp_crc32_le from the shim).
    CHECK(verifyCRCGate(), "firmware verifyCRCGate() returns true");

    // Second: explicit check that the CRC of "123456789" equals 0xCBF43926.
    // This proves the shim's esp_crc32_le matches the spec mathematically.
    const uint8_t vec[] = "123456789";
    uint32_t result = ~esp_crc32_le(0xFFFFFFFF, vec, 9) & 0xFFFFFFFF;
    CHECK_EQ(result, (uint32_t)0xCBF43926, "CRC32(\"123456789\") == 0xCBF43926");
}

// ============================================================================
// TEST 6 — CRC corruption detection
// ============================================================================
// Mutate one byte of a serialized record's canonical payload and confirm
// verifyRecordCRC() returns false. Also mutate a header byte.
// ============================================================================
static void test_crc_corruption_detection() {
    printf("\n[TEST 6] CRC corruption detection\n");

    JournalRecord r = makeSampleRecord();
    uint8_t blob[BLOB_SIZE];
    uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);
    CHECK(payloadEnd > 0, "baseline serialize OK");

    // 6a: corrupt a canonical payload byte
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        bad[BLOB_HEADER_SIZE] ^= 0x01;  // flip 1 bit in recordState
        CHECK(verifyRecordCRC(bad, payloadEnd) == false,
              "1-bit flip in recordState → CRC fails");
    }

    // 6b: corrupt a header byte (covered by CRC)
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        bad[3] ^= 0x80;  // flip a bit in generation (CRC covers header 0..6)
        CHECK(verifyRecordCRC(bad, payloadEnd) == false,
              "1-bit flip in generation byte 3 → CRC fails");
    }

    // 6c: corrupt a magic byte (CRC covers bytes 0..6, magic at 0..1)
    {
        uint8_t bad[BLOB_SIZE];
        memcpy(bad, blob, BLOB_SIZE);
        bad[0] ^= 0x01;
        CHECK(verifyRecordCRC(bad, payloadEnd) == false,
              "1-bit flip in magic1 → CRC fails");
    }

    // 6d: confirm CRC passes for the unmodified blob
    CHECK(verifyRecordCRC(blob, payloadEnd) == true,
          "unmodified blob → CRC verifies");
}

// ============================================================================
// TEST 7 — canonicalEqual: equal and mismatch cases
// ============================================================================
static void test_canonical_equal() {
    printf("\n[TEST 7] canonicalEqual (equal + mismatch)\n");

    JournalRecord a = makeSampleRecord();
    JournalRecord b = makeSampleRecord();  // identical
    JournalRecord c = makeSampleRecord();  // will be mutated

    // Both serialized + deserialized so canonicalLength is set.
    uint8_t blobA[BLOB_SIZE], blobB[BLOB_SIZE], blobC[BLOB_SIZE];
    serializeRecord(a, blobA, BLOB_SIZE);
    serializeRecord(b, blobB, BLOB_SIZE);
    serializeRecord(c, blobC, BLOB_SIZE);
    deserializeRecord(blobA, BLOB_SIZE, a);
    deserializeRecord(blobB, BLOB_SIZE, b);
    deserializeRecord(blobC, BLOB_SIZE, c);

    // 7a: equal records
    CHECK(canonicalEqual(a, b), "two identical records → canonicalEqual=true");

    // 7b: canonicalEqualBlobs on raw bytes
    CHECK(canonicalEqualBlobs(blobA, blobB, a.canonicalLength, b.canonicalLength,
                              a.schemaVersion, b.schemaVersion),
          "canonicalEqualBlobs on identical blobs → true");

    // 7c: mismatch in requestId
    c.requestId = "different-id";
    // Re-serialize c with the new value (need fresh canonicalLength)
    serializeRecord(c, blobC, BLOB_SIZE);
    deserializeRecord(blobC, BLOB_SIZE, c);
    CHECK(!canonicalEqual(a, c), "requestId differs → canonicalEqual=false");
    CHECK(!canonicalEqualBlobs(blobA, blobC, a.canonicalLength, c.canonicalLength,
                                a.schemaVersion, c.schemaVersion),
          "canonicalEqualBlobs: requestId differs → false");

    // 7d: mismatch in commandHash
    JournalRecord d = makeSampleRecord();
    d.commandHash = "different-hash";
    uint8_t blobD[BLOB_SIZE];
    serializeRecord(d, blobD, BLOB_SIZE);
    deserializeRecord(blobD, BLOB_SIZE, d);
    CHECK(!canonicalEqual(a, d), "commandHash differs → canonicalEqual=false");

    // 7e: mismatch in timestamp
    JournalRecord e = makeSampleRecord();
    e.timestamp = 0xCAFEBABE;
    uint8_t blobE[BLOB_SIZE];
    serializeRecord(e, blobE, BLOB_SIZE);
    deserializeRecord(blobE, BLOB_SIZE, e);
    CHECK(!canonicalEqual(a, e), "timestamp differs → canonicalEqual=false");

    // 7f: mismatch in ackJson
    JournalRecord f = makeSampleRecord();
    f.ackJson = "{\"ok\":false}";
    uint8_t blobF[BLOB_SIZE];
    serializeRecord(f, blobF, BLOB_SIZE);
    deserializeRecord(blobF, BLOB_SIZE, f);
    CHECK(!canonicalEqual(a, f), "ackJson differs → canonicalEqual=false");

    // 7g: equal-but-different-canonicalLength → false
    JournalRecord g = makeSampleRecord();
    g.requestId = "short";  // shorter requestId → different canonicalLength
    uint8_t blobG[BLOB_SIZE];
    serializeRecord(g, blobG, BLOB_SIZE);
    deserializeRecord(blobG, BLOB_SIZE, g);
    CHECK(!canonicalEqual(a, g),
          "canonicalLength differs → canonicalEqual=false");

    // 7h: padding-only difference → still equal (padding has no semantic meaning)
    uint8_t blobA_padded[BLOB_SIZE];
    memcpy(blobA_padded, blobA, BLOB_SIZE);
    blobA_padded[BLOB_SIZE - 1] ^= 0xFF;  // flip a padding byte
    JournalRecord a_padded;
    deserializeRecord(blobA_padded, BLOB_SIZE, a_padded);
    CHECK(canonicalEqual(a, a_padded),
          "padding-only difference → canonicalEqual=true");
}

// ============================================================================
// TEST 8 — Schema mismatch
// ============================================================================
// Two records with different schemaVersion must NOT be canonical-equal,
// even if their canonical payloads are byte-identical.
// ============================================================================
static void test_schema_mismatch() {
    printf("\n[TEST 8] Schema mismatch detection\n");

    JournalRecord a = makeSampleRecord();
    JournalRecord b = makeSampleRecord();
    // Forcibly set different schemaVersion. Note: in normal operation,
    // schemaVersion is always JOURNAL_SCHEMA_VERSION (4); this test simulates
    // a future schema upgrade where two records may legitimately have
    // different schemaVersion values stored on the same NVS partition.
    b.schemaVersion = (uint8_t)(JOURNAL_SCHEMA_VERSION + 1);

    // Both records would have identical canonical payload bytes if we
    // serialized them — but schema differs.
    uint8_t blobA[BLOB_SIZE], blobB[BLOB_SIZE];
    // We bypass serializeRecord here (it would write JOURNAL_SCHEMA_VERSION)
    // and craft blobs directly with different byte-2 values.
    JournalRecord a_for_serialize = makeSampleRecord();
    JournalRecord b_for_serialize = makeSampleRecord();
    serializeRecord(a_for_serialize, blobA, BLOB_SIZE);
    serializeRecord(b_for_serialize, blobB, BLOB_SIZE);
    // Mutate byte 2 (schemaVersion) of blobB
    blobB[2] = (uint8_t)(JOURNAL_SCHEMA_VERSION + 1);

    // Parse A normally
    deserializeRecord(blobA, BLOB_SIZE, a);
    // B will be PARSE_INVALID (parser rejects unknown schemaVersion) — so
    // canonicalEqualBlobs is the right API to test schema-mismatch behaviour.

    CHECK(!canonicalEqualBlobs(blobA, blobB,
                                a.canonicalLength, a.canonicalLength,
                                blobA[2], blobB[2]),
          "schemaVersion differs → canonicalEqualBlobs=false");

    // Also confirm that the parser rejects the foreign schemaVersion blob.
    JournalRecord dummy;
    ParseResult pr = deserializeRecord(blobB, BLOB_SIZE, dummy);
    CHECK(pr == ParseResult::PARSE_INVALID,
          "deserialize foreign-schemaVersion blob → PARSE_INVALID");
}

// ============================================================================
// TEST 9 — Generation classifier (Rev14 §4 normative test vectors)
// ============================================================================
// Implements the EXACT normative table from Rev14 §4:
//
// | genA       | genB       | distAB           | distBA           | Result        |
// |------------|------------|-------------------|-------------------|---------------|
// | 0          | 0          | 0                 | 0                 | GEN_EQUAL     |
// | 0          | 1          | 1                 | 0xFFFFFFFF        | GEN_NEWER_B   |
// | 1          | 0          | 0xFFFFFFFF        | 1                 | GEN_NEWER_A   |
// | 0          | 0xFFFFFFFF | 0xFFFFFFFF        | 1                 | GEN_NEWER_A   |
// | 0xFFFFFFFF | 0          | 1                 | 0xFFFFFFFF        | GEN_NEWER_B   |
// | 0          | 5          | 5                 | 0xFFFFFFFB        | GEN_INVALID   |
// | 5          | 0          | 0xFFFFFFFB        | 5                 | GEN_INVALID   |
// | 10         | 20         | 10                | 0xFFFFFFF6        | GEN_INVALID   |
// | 10         | 0x8000000A | 0x80000000        | 0x80000000        | GEN_AMBIGUOUS |
// ============================================================================
static void test_generation_vectors() {
    printf("\n[TEST 9] Generation classifier (Rev14 normative vectors)\n");

    // --- Auditor-required vectors ---
    CHECK(classifyGeneration(0, 1) == GenRelation::GEN_NEWER_B,
          "0 → 1   : GEN_NEWER_B");
    CHECK(classifyGeneration(1, 0) == GenRelation::GEN_NEWER_A,
          "1 → 0   : GEN_NEWER_A");
    CHECK(classifyGeneration(0xFFFFFFFF, 0) == GenRelation::GEN_NEWER_B,
          "0xFFFFFFFF → 0            : GEN_NEWER_B (wrap-safe)");
    CHECK(classifyGeneration(0, 0xFFFFFFFF) == GenRelation::GEN_NEWER_A,
          "0 → 0xFFFFFFFF           : GEN_NEWER_A (wrap-safe)");
    CHECK(classifyGeneration(0, 5) == GenRelation::GEN_INVALID,
          "0 → 5   (distance 5)    : GEN_INVALID");
    // The auditor specified "distance 2^31 → AMBIGUOUS":
    // genA=10, genB=10+0x80000000=0x8000000A gives distAB=distBA=0x80000000.
    CHECK(classifyGeneration(10, 0x8000000A) == GenRelation::GEN_AMBIGUOUS,
          "distance == 2^31          : GEN_AMBIGUOUS");

    // --- Additional normative vectors from Rev14 §4 table ---
    CHECK(classifyGeneration(0, 0) == GenRelation::GEN_EQUAL,
          "0 → 0   : GEN_EQUAL");
    CHECK(classifyGeneration(5, 0) == GenRelation::GEN_INVALID,
          "5 → 0   : GEN_INVALID");
    CHECK(classifyGeneration(10, 20) == GenRelation::GEN_INVALID,
          "10 → 20 : GEN_INVALID");

    // --- Symmetry sanity: AMBIGUOUS must be symmetric ---
    CHECK(classifyGeneration(0x8000000A, 10) == GenRelation::GEN_AMBIGUOUS,
          "AMBIGUOUS is symmetric");

    // --- distBA==1 must yield GEN_NEWER_A even when distAB is huge ---
    CHECK(classifyGeneration(0xFFFFFFFF, 0) == GenRelation::GEN_NEWER_B,
          "0xFFFFFFFF → 0  : GEN_NEWER_B (not INVALID)");

    // --- Conventional increment ---
    CHECK(classifyGeneration(100, 101) == GenRelation::GEN_NEWER_B,
          "100 → 101 : GEN_NEWER_B");
    CHECK(classifyGeneration(101, 100) == GenRelation::GEN_NEWER_A,
          "101 → 100 : GEN_NEWER_A");

    // --- Equal but different value ---
    CHECK(classifyGeneration(7, 7) == GenRelation::GEN_EQUAL,
          "7 → 7   : GEN_EQUAL");
}

// ============================================================================
// TEST 10 — empty-strings serialize and round-trip cleanly
// ============================================================================
// Covers the degenerate case where requestId / commandHash / ackJson are
// all empty strings. Confirms zero-length fields are part of the canonical
// contract and serialize/deserialize identically.
// ============================================================================
static void test_empty_strings() {
    printf("\n[TEST 10] Empty-string fields round-trip\n");

    JournalRecord r;
    r.schemaVersion      = JOURNAL_SCHEMA_VERSION;
    r.generation         = 0;
    r.recordState        = RecordState::EMPTY;
    r.requestId          = "";
    r.commandHash        = "";
    r.channelId          = 0;
    r.desiredState       = 0xFF;
    r.previousKnownState = 0;
    r.attempt            = 0;
    r.timestamp          = 0;
    r.ackJson            = "";

    uint8_t blob[BLOB_SIZE];
    uint16_t payloadEnd = serializeRecord(r, blob, BLOB_SIZE);
    CHECK(payloadEnd > 0, "empty-fields record serializes OK");

    // Replace r with its deserialized form so canonicalLength is set.
    deserializeRecord(blob, BLOB_SIZE, r);

    JournalRecord restored;
    ParseResult pr = deserializeRecord(blob, BLOB_SIZE, restored);
    CHECK(pr == ParseResult::PARSE_VALID, "empty-fields record parses VALID");

    CHECK(restored.requestId.length() == 0, "requestId empty");
    CHECK(restored.commandHash.length() == 0, "commandHash empty");
    CHECK(restored.ackJson.length() == 0, "ackJson empty");
    CHECK(restored.recordState == RecordState::EMPTY, "recordState = EMPTY");

    CHECK(verifyRecordCRC(blob, payloadEnd), "CRC verifies for empty-fields record");

    // canonicalEqual should be true for two identical empty records (both
    // deserialized, both with canonicalLength set).
    CHECK(canonicalEqual(r, restored), "two empty records → canonicalEqual=true");
}

// ============================================================================
// main
// ============================================================================
int main() {
    printf("==========================================================\n");
    printf("JournalRecord Phase 1 Host Test — Rev26 / Rev14 normative\n");
    printf("Compiles firmware/JournalRecord.cpp directly (no mock)\n");
    printf("==========================================================\n");

    test_exact_byte_layout();
    test_round_trip();
    test_boundary_requestId();
    test_boundary_commandHash();
    test_boundary_ackJson();
    test_malformed_inputs();
    test_crc_known_answer();
    test_crc_corruption_detection();
    test_canonical_equal();
    test_schema_mismatch();
    test_generation_vectors();
    test_empty_strings();

    printf("\n==========================================================\n");
    printf("RESULTS: %d passed, %d failed\n", g_passCount, g_failCount);
    printf("==========================================================\n");

    return (g_failCount == 0) ? 0 : 1;
}
