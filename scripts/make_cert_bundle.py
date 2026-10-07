#!/usr/bin/env python3
"""Build the Arduino-ESP32 compact CA bundle (subject DER + SPKI DER).

Requires cryptography only when refreshing certificates, not for firmware builds.
Format matches Arduino-ESP32 WiFiClientSecure/src/esp_crt_bundle.c.
"""
from pathlib import Path
import struct
from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

root = Path(__file__).resolve().parents[1]
certificates = x509.load_pem_x509_certificates((root / "certs/mozilla-roots.pem").read_bytes())
entries = sorted((cert.subject.public_bytes(),
                  cert.public_key().public_bytes(Encoding.DER, PublicFormat.SubjectPublicKeyInfo))
                 for cert in certificates)
bundle = struct.pack(">H", len(entries))
for subject, key in entries:
    bundle += struct.pack(">HH", len(subject), len(key)) + subject + key
(root / "certs/roots.bundle").write_bytes(bundle)
print(f"CA bundle: {len(entries)} roots, {len(bundle)} bytes")
