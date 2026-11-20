#!/usr/bin/env python3
"""Check the embedded AI HTTPS trust store without contacting any provider."""

from pathlib import Path
import ssl

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/util/AiTranslationRoots.h").read_text(encoding="utf-8")
start = 'R"AIROOT('
stop = ')AIROOT"'
assert HEADER.count(start) == HEADER.count(stop) == 1
pem = HEADER.split(start, 1)[1].split(stop, 1)[0]
expected = {
    "GTS Root R1",
    "GTS Root R2",
    "GTS Root R3",
    "GTS Root R4",
    "ISRG Root X1",
    "DigiCert Global Root G2",
}
assert pem.count("-----BEGIN CERTIFICATE-----") == len(expected)
assert pem.count("-----END CERTIFICATE-----") == len(expected)

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.load_verify_locations(cadata=pem)
loaded = {
    val
    for cert in ctx.get_ca_certs()
    for rdn in cert["subject"]
    for key, val in rdn
    if key == "commonName"
}
assert loaded == expected, f"Unexpected or missing trusted roots: {loaded ^ expected}"
print("AI trust bundle: 6 valid root certificates, includes GTS R4")
