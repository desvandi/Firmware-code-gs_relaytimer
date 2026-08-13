// =============================================================================
// Utils/Crypto.cpp — Cryptographic helpers (SHA-256, PBKDF2, HMAC, JWT)
// =============================================================================
#include "Crypto.h"
#include "RtcDriver.h"
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <string.h>

namespace Utils {

bool constantTimeMemEquals(const volatile uint8_t* a, const volatile uint8_t* b, size_t len) {
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) {
    diff |= a[i] ^ b[i];
  }
  return diff == 0;
}

void generateRandomBytes(uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    buf[i] = (uint8_t)(esp_random() & 0xFF);
  }
}

void bytesToHex(const uint8_t* in, size_t len, char* out) {
  static const char hexchars[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2]     = hexchars[(in[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hexchars[in[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
  size_t hexLen = strlen(hex);
  if (hexLen != outLen * 2) return false;
  for (size_t i = 0; i < outLen; i++) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    uint8_t b = 0;
    if (hi >= '0' && hi <= '9') b = (hi - '0') << 4;
    else if (hi >= 'a' && hi <= 'f') b = (hi - 'a' + 10) << 4;
    else if (hi >= 'A' && hi <= 'F') b = (hi - 'A' + 10) << 4;
    else return false;
    if (lo >= '0' && lo <= '9') b |= (lo - '0');
    else if (lo >= 'a' && lo <= 'f') b |= (lo - 'a' + 10);
    else if (lo >= 'A' && lo <= 'F') b |= (lo - 'A' + 10);
    else return false;
    out[i] = b;
  }
  return true;
}

String sha256Hex(const String& data) {
  uint8_t hash[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) { mbedtls_md_free(&ctx); return String(); }
  if (mbedtls_md_setup(&ctx, info, 0) != 0) { mbedtls_md_free(&ctx); return String(); }
  if (mbedtls_md_starts(&ctx) != 0) { mbedtls_md_free(&ctx); return String(); }
  if (mbedtls_md_update(&ctx, (const unsigned char*)data.c_str(), data.length()) != 0) {
    mbedtls_md_free(&ctx); return String();
  }
  if (mbedtls_md_finish(&ctx, hash) != 0) { mbedtls_md_free(&ctx); return String(); }
  mbedtls_md_free(&ctx);
  char buf[65];
  bytesToHex(hash, 32, buf);
  return String(buf);
}

bool hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* msg, size_t msgLen,
                uint8_t* outHash) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_starts(&ctx, key, keyLen) != 0) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_update(&ctx, msg, msgLen) != 0) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_finish(&ctx, outHash) != 0) { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_free(&ctx);
  return true;
}

bool pbkdf2HmacSha256(const char* pass, size_t passLen,
                      const uint8_t* salt, size_t saltLen,
                      uint16_t iterations, uint8_t* outHash) {
  if (pass == nullptr || salt == nullptr || outHash == nullptr || iterations == 0) {
    return false;
  }
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) return false;

  // Block = salt || INT(1) (big-endian, 4 bytes)
  uint8_t saltBlock[64 + 4];
  if (saltLen > sizeof(saltBlock) - 4) return false;
  memcpy(saltBlock, salt, saltLen);
  saltBlock[saltLen] = 0; saltBlock[saltLen + 1] = 0;
  saltBlock[saltLen + 2] = 0; saltBlock[saltLen + 3] = 1;

  // U1 = HMAC(pass, saltBlock)
  uint8_t u[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_starts(&ctx, (const unsigned char*)pass, passLen) != 0) {
    mbedtls_md_free(&ctx); return false;
  }
  if (mbedtls_md_hmac_update(&ctx, saltBlock, saltLen + 4) != 0) {
    mbedtls_md_free(&ctx); return false;
  }
  if (mbedtls_md_hmac_finish(&ctx, u) != 0) { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_free(&ctx);

  // T = U1
  uint8_t t[32];
  memcpy(t, u, 32);

  // T ^= Ui for i = 2..iterations
  for (uint16_t i = 1; i < iterations; i++) {
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1) != 0) {
      mbedtls_md_free(&ctx);
      memset(u, 0, sizeof(u)); memset(t, 0, sizeof(t));
      return false;
    }
    if (mbedtls_md_hmac_starts(&ctx, (const unsigned char*)pass, passLen) != 0) {
      mbedtls_md_free(&ctx);
      memset(u, 0, sizeof(u)); memset(t, 0, sizeof(t));
      return false;
    }
    if (mbedtls_md_hmac_update(&ctx, u, 32) != 0) {
      mbedtls_md_free(&ctx);
      memset(u, 0, sizeof(u)); memset(t, 0, sizeof(t));
      return false;
    }
    if (mbedtls_md_hmac_finish(&ctx, u) != 0) {
      mbedtls_md_free(&ctx);
      memset(u, 0, sizeof(u)); memset(t, 0, sizeof(t));
      return false;
    }
    mbedtls_md_free(&ctx);
    for (int j = 0; j < 32; j++) t[j] ^= u[j];
  }
  memcpy(outHash, t, 32);
  memset(u, 0, sizeof(u)); memset(t, 0, sizeof(t));
  return true;
}

String base64urlEncode(const uint8_t* data, size_t len) {
  // base64url: same as base64 but '-' instead of '+', '_' instead of '/', no '='
  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
  // mbedtls returns required length including null terminator
  uint8_t* buf = (uint8_t*)malloc(outLen);
  if (!buf) return String();
  int ret = mbedtls_base64_encode(buf, outLen, &outLen, data, len);
  if (ret != 0) { free(buf); return String(); }
  // Translate + -> -, / -> _, strip =
  String s;
  s.reserve(outLen);
  for (size_t i = 0; i < outLen; i++) {
    char c = (char)buf[i];
    if (c == '+') s += '-';
    else if (c == '/') s += '_';
    else if (c == '=') continue;
    else s += c;
  }
  free(buf);
  return s;
}

String base64urlEncode(const String& s) {
  return base64urlEncode((const uint8_t*)s.c_str(), s.length());
}

String jwtSign(const String& username, const String& secret, uint32_t ttlSeconds) {
  // Header
  String headerJson = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
  String headerB64 = base64urlEncode(headerJson);

  // Payload
  uint32_t iat = Drivers::rtc.getUnixTime();
  if (iat == 0) iat = millis() / 1000;  // fallback if RTC not ready
  uint32_t exp = iat + ttlSeconds;
  String payloadJson = "{\"sub\":\"" + username + "\",\"iat\":" + String(iat) +
                       ",\"exp\":" + String(exp) + "}";
  String payloadB64 = base64urlEncode(payloadJson);

  // Signature
  String signingInput = headerB64 + "." + payloadB64;
  uint8_t sig[32];
  if (!hmacSha256((const uint8_t*)secret.c_str(), secret.length(),
                  (const uint8_t*)signingInput.c_str(), signingInput.length(), sig)) {
    return String();
  }
  String sigB64 = base64urlEncode(sig, 32);
  return signingInput + "." + sigB64;
}

bool jwtVerify(const String& token, const String& secret, String& outUsername) {
  int firstDot = token.indexOf('.');
  int lastDot = token.lastIndexOf('.');
  if (firstDot <= 0 || lastDot <= firstDot) return false;
  String headerB64 = token.substring(0, firstDot);
  String payloadB64 = token.substring(firstDot + 1, lastDot);
  String sigB64 = token.substring(lastDot + 1);
  String signingInput = headerB64 + "." + payloadB64;

  // Recompute signature
  uint8_t expectedSig[32];
  if (!hmacSha256((const uint8_t*)secret.c_str(), secret.length(),
                  (const uint8_t*)signingInput.c_str(), signingInput.length(), expectedSig)) {
    return false;
  }
  String expectedSigB64 = base64urlEncode(expectedSig, 32);
  if (!constantTimeMemEquals(
        (const volatile uint8_t*)sigB64.c_str(),
        (const volatile uint8_t*)expectedSigB64.c_str(),
        sigB64.length())) {
    return false;
  }
  if (sigB64.length() != expectedSigB64.length()) return false;

  // Decode payload (base64url -> JSON string)
  // Translate - -> +, _ -> /, add padding
  String b64 = payloadB64;
  b64.replace('-', '+');
  b64.replace('_', '/');
  while (b64.length() % 4) b64 += '=';

  size_t outLen = 0;
  mbedtls_base64_decode(nullptr, 0, &outLen, (const unsigned char*)b64.c_str(), b64.length());
  if (outLen == 0 || outLen > 512) return false;
  uint8_t* buf = (uint8_t*)malloc(outLen + 1);
  if (!buf) return false;
  int ret = mbedtls_base64_decode(buf, outLen, &outLen,
                                  (const unsigned char*)b64.c_str(), b64.length());
  if (ret != 0) { free(buf); return false; }
  buf[outLen] = '\0';
  String payload((const char*)buf);
  free(buf);

  // Extract "sub" and "exp"
  int subIdx = payload.indexOf("\"sub\":\"");
  if (subIdx < 0) return false;
  subIdx += 7;
  int subEnd = payload.indexOf("\"", subIdx);
  if (subEnd < 0) return false;
  outUsername = payload.substring(subIdx, subEnd);

  int expIdx = payload.indexOf("\"exp\":");
  if (expIdx >= 0) {
    expIdx += 6;
    int expEnd = payload.indexOf(',', expIdx);
    if (expEnd < 0) expEnd = payload.indexOf('}', expIdx);
    String expStr = payload.substring(expIdx, expEnd);
    uint32_t exp = (uint32_t)expStr.toInt();
    uint32_t now = Drivers::rtc.getUnixTime();
    if (now == 0) now = millis() / 1000;  // fallback if RTC not ready
    if (now > exp) return false;
  }
  return true;
}

String generateToken(size_t hexChars) {
  size_t byteLen = (hexChars + 1) / 2;
  uint8_t* buf = (uint8_t*)malloc(byteLen);
  if (!buf) return String();
  generateRandomBytes(buf, byteLen);
  char* hex = (char*)malloc(hexChars + 1);
  if (!hex) { free(buf); return String(); }
  bytesToHex(buf, byteLen, hex);
  hex[hexChars] = '\0';
  String s(hex);
  free(buf);
  free(hex);
  return s;
}

} // namespace Utils
