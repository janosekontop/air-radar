"""Build Espressif's compressed public CA bundle from a Mozilla/certifi PEM file.

Format: https://github.com/espressif/esp-idf/blob/v4.4.7/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py
Requires cryptography only when regenerating; the ready bundle is included.
"""
import argparse
from pathlib import Path
import re
import struct
from cryptography import x509
from cryptography.hazmat.primitives import serialization

parser = argparse.ArgumentParser()
parser.add_argument('pem', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
certificates = [x509.load_pem_x509_certificate(block.encode()) for block in
                re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----',
                           args.pem.read_text(), re.S)]
entries = []
for cert in certificates:
    subject = cert.subject.public_bytes()
    key = cert.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    entries.append((subject, key))
bundle = bytearray(struct.pack('>H', len(entries)))
for subject, key in sorted(entries):
    bundle.extend(struct.pack('>HH', len(subject), len(key)) + subject + key)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(bundle)
print(f'{len(entries)} public roots; {len(bundle)} bytes')
