#!/usr/bin/env python3
"""
Ed25519 KAT using RFC 8032 Section 7.1 standard test vectors.
Per independent audit D-001: script was missing from repo (existed only
in local workspace). Now committed to satisfy PG-11 requirement.

Uses INDEPENDENT published RFC 8032 test vectors — NOT self-generated.
Verifies:
  1. Load RFC private key → derive public key → MUST match RFC published public key
  2. Sign RFC message → signature MUST verify
  3. Wrong message → MUST reject
  4. Tampered signature → MUST reject
  5. OTA flow (sign SHA-256(firmware), verify + tampered reject)
"""
import sys, hashlib

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    from cryptography.exceptions import InvalidSignature
except ImportError:
    print("ERROR: pip install cryptography"); sys.exit(2)

FAILS = []
def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}: {name}" + (f" — {detail}" if not cond else ""))
    if not cond: FAILS.append(name)

# RFC 8032 Section 7.1 Test 1 (INDEPENDENT published standard vector)
RFC_TEST_1 = {
    "name": "RFC 8032 §7.1 Test 1 (empty message)",
    "private_key_hex": "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
    "public_key_hex":  "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
    "message": b"",
}

def run_rfc_kat():
    print("\n=== Ed25519 KAT — RFC 8032 §7.1 Standard Test Vectors ===")
    print("  Per PG-11: uses INDEPENDENT published test vectors.\n")

    for i, tv in enumerate([RFC_TEST_1], 1):
        print(f"  [{i}] {tv['name']}")
        priv_bytes = bytes.fromhex(tv["private_key_hex"])
        check(f"    private key = 32 bytes", len(priv_bytes) == 32, f"got {len(priv_bytes)}")

        priv_key = Ed25519PrivateKey.from_private_bytes(priv_bytes)
        pub_key = priv_key.public_key()
        pub_bytes = pub_key.public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw
        )
        rfc_pub = bytes.fromhex(tv["public_key_hex"])
        check(f"    derived public key == RFC public key",
              pub_bytes == rfc_pub,
              f"derived={pub_bytes.hex()[:16]}... rfc={rfc_pub.hex()[:16]}...")

        msg = tv["message"]
        sig = priv_key.sign(msg)
        check(f"    signature = 64 bytes", len(sig) == 64, f"got {len(sig)}")

        try:
            pub_key.verify(sig, msg)
            check(f"    verify(RFC_pub, sig, RFC_msg) -> PASS", True)
        except InvalidSignature:
            check(f"    verify(RFC_pub, sig, RFC_msg) -> PASS", False, "valid signature rejected")

        wrong_msg = b"WRONG"
        try:
            pub_key.verify(sig, wrong_msg)
            check(f"    verify(wrong_msg) -> REJECT", False, "invalid signature accepted (CRITICAL)")
        except InvalidSignature:
            check(f"    verify(wrong_msg) -> REJECT", True)

        tampered = bytearray(sig); tampered[0] ^= 0x01
        try:
            pub_key.verify(bytes(tampered), msg)
            check(f"    verify(tampered_sig) -> REJECT", False, "tampered accepted (CRITICAL)")
        except InvalidSignature:
            check(f"    verify(tampered_sig) -> REJECT", True)

    # OTA flow simulation
    print("\n  [OTA flow — sign SHA-256(firmware), verify]")
    fw = b"\xE9\x00" * 512
    h = hashlib.sha256(fw).digest()
    fw_sig = priv_key.sign(h)
    try:
        pub_key.verify(fw_sig, h)
        check(f"    verify(SHA-256(fw), sig) -> PASS", True)
    except InvalidSignature:
        check(f"    verify(SHA-256(fw), sig) -> PASS", False, "rejected")

    fw_t = bytearray(fw); fw_t[0] ^= 0xFF
    h_bad = hashlib.sha256(bytes(fw_t)).digest()
    try:
        pub_key.verify(fw_sig, h_bad)
        check(f"    verify(SHA-256(tampered_fw)) -> REJECT", False, "accepted!")
    except InvalidSignature:
        check(f"    verify(SHA-256(tampered_fw)) -> REJECT", True)

    total = len(FAILS)
    print(f"\n=== Summary: {total} failure(s) ===")
    if FAILS:
        for f in FAILS: print(f"  - {f}")
        sys.exit(1)
    else:
        print("\nRFC 8032 Ed25519 KAT: ALL PASS")
        print("\nThis KAT uses INDEPENDENT published RFC 8032 test vectors.")
        print("Target ESP32 KAT (same vectors on device) remains")
        print("NOT EXECUTED — HARDWARE REQUIRED.")
        sys.exit(0)

if __name__ == "__main__":
    run_rfc_kat()
