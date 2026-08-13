#!/usr/bin/env python3
"""
Timer Digital Relay v4.0 — Firmware Signing Tool
=================================================

Generates SHA-256 hash + Ed25519 signature for OTA firmware binary.

CONTRACT (audit round 10B):
  signature = ed25519_sign(sha256(firmware.bin), private_key)
  ESP32 verifies: ed25519_verify(signature, sha256(firmware.bin), public_key)

NOT: ed25519_sign(firmware.bin, private_key)  ← WRONG, will be rejected by ESP32

Usage:
  python3 sign_firmware.py firmware.bin

Output:
  firmware.bin.sha256     — 64 hex chars
  firmware.bin.sig        — 128 hex chars (Ed25519 signature)
  firmware.bin.ota.json   — OTA command payload (paste into PWA or MQTT publish)

Prerequisites:
  pip install cryptography

Generate keypair (one-time):
  python3 sign_firmware.py --gen-keys
  → creates firmware_signing_private.pem + firmware_signing_public.pem
  → paste PUBLIC key (64 hex chars) into Config.h OTA_ED25519_PUBLIC_KEY_HEX
  → keep PRIVATE key secure (signing machine only, NEVER in firmware)
"""

import sys
import os
import json
import hashlib
from pathlib import Path

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
    from cryptography.hazmat.primitives import serialization
except ImportError:
    print("ERROR: Install cryptography library:")
    print("  pip install cryptography")
    sys.exit(1)


PRIVATE_KEY_PATH = "firmware_signing_private.pem"
PUBLIC_KEY_PATH = "firmware_signing_public.pem"


def generate_keypair():
    """Generate Ed25519 keypair. Run once, keep private key secure."""
    private_key = Ed25519PrivateKey.generate()
    public_key = private_key.public_key()

    # Save private key (PEM, no passphrase — protect via filesystem permissions)
    private_pem = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption()
    )
    with open(PRIVATE_KEY_PATH, "wb") as f:
        f.write(private_pem)
    os.chmod(PRIVATE_KEY_PATH, 0o600)  # owner read/write only

    # Save public key (PEM)
    public_pem = public_key.public_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PublicFormat.SubjectPublicKeyInfo
    )
    with open(PUBLIC_KEY_PATH, "wb") as f:
        f.write(public_pem)

    # Print raw public key hex (for Config.h)
    raw_public = public_key.public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw
    )
    print("=" * 70)
    print("Ed25519 keypair generated")
    print("=" * 70)
    print(f"Private key: {PRIVATE_KEY_PATH} (KEEP SECURE — signing machine only)")
    print(f"Public key:  {PUBLIC_KEY_PATH}")
    print()
    print("Paste this PUBLIC KEY into Config.h (OTA_ED25519_PUBLIC_KEY_HEX):")
    print()
    print(f'constexpr const char* OTA_ED25519_PUBLIC_KEY_HEX = "{raw_public.hex()}";')
    print()
    print("⚠️  NEVER commit the private key to git. Add to .gitignore:")
    print(f"  echo '{PRIVATE_KEY_PATH}' >> .gitignore")


def sign_firmware(firmware_path: str, version: str = ""):
    """Sign firmware binary. Returns (sha256_hex, signature_hex, size)."""
    if not os.path.exists(PRIVATE_KEY_PATH):
        print(f"ERROR: Private key not found at {PRIVATE_KEY_PATH}")
        print("Generate keypair first: python3 sign_firmware.py --gen-keys")
        sys.exit(1)

    if not os.path.exists(firmware_path):
        print(f"ERROR: Firmware binary not found: {firmware_path}")
        sys.exit(1)

    # Load private key
    with open(PRIVATE_KEY_PATH, "rb") as f:
        private_key = serialization.load_pem_private_key(f.read(), password=None)

    # Read firmware binary
    with open(firmware_path, "rb") as f:
        firmware_bytes = f.read()

    size = len(firmware_bytes)
    print(f"Firmware: {firmware_path} ({size} bytes)")

    # Compute SHA-256
    sha256_hash = hashlib.sha256(firmware_bytes).digest()
    sha256_hex = sha256_hash.hex()
    print(f"SHA-256:  {sha256_hex}")

    # R10B-1 CONTRACT: sign the SHA-256 hash (32 bytes), NOT the full binary
    # This matches firmware's Utils::ed25519VerifyHash() which verifies
    # signature over 32-byte SHA-256 hash
    signature = private_key.sign(sha256_hash)
    signature_hex = signature.hex()
    print(f"Signature (Ed25519 over SHA-256): {signature_hex}")

    # Write artifacts
    base = firmware_path
    with open(f"{base}.sha256", "w") as f:
        f.write(sha256_hex + "\n")
    with open(f"{base}.sig", "w") as f:
        f.write(signature_hex + "\n")

    # OTA command payload (for PWA / MQTT publish)
    ota_payload = {
        "action": "update",
        "url": "",  # Fill in: HTTPS URL where firmware.bin is hosted
        "version": version or "4.0.0",  # Fill in: new version (must be > current)
        "size": size,
        "sha256": sha256_hex,
        "signature": signature_hex,
        "requestId": ""  # PWA fills this automatically via sendCommandWithAck
    }
    with open(f"{base}.ota.json", "w") as f:
        json.dump(ota_payload, f, indent=2)

    print()
    print("Artifacts written:")
    print(f"  {base}.sha256       — SHA-256 hash (64 hex chars)")
    print(f"  {base}.sig          — Ed25519 signature (128 hex chars)")
    print(f"  {base}.ota.json     — OTA command payload (fill url + version)")
    print()
    print("Next steps:")
    print(f"  1. Upload {firmware_path} to HTTPS host (e.g., GitHub Release)")
    print(f"  2. Edit {base}.ota.json: set 'url' to HTTPS download URL")
    print(f"  3. Edit {base}.ota.json: set 'version' to new version (e.g., '4.1.0')")
    print(f"  4. PWA: publish OTA command via MQTT (url+version+size+sha256+signature)")
    print(f"  5. ESP32 downloads → verifies SHA-256 → verifies Ed25519 → installs → reboots")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    if sys.argv[1] == "--gen-keys":
        generate_keypair()
    elif sys.argv[1] in ("-h", "--help"):
        print(__doc__)
    else:
        firmware_path = sys.argv[1]
        version = sys.argv[2] if len(sys.argv) > 2 else ""
        sign_firmware(firmware_path, version)


if __name__ == "__main__":
    main()
