#!/usr/bin/env python3
"""Check the embedded AI HTTPS trust store without contacting any provider."""

from pathlib import Path
import hashlib
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
    "WE1",
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
# Pin the exact Google Trust Services WE1 intermediate used by opencode.ai.
# Do not accept an arbitrary certificate merely because its CN says WE1.
fingerprints = {hashlib.sha256(cert).hexdigest() for cert in ctx.get_ca_certs(binary_form=True)}
assert "1dfc1605fbad358d8bc844f76d15203fac9ca5c1a79fd4857ffaf2864fbebf96" in fingerprints
print("AI trust bundle: 6 roots plus fingerprint-verified WE1 intermediate")
