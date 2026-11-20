from pathlib import Path
import sys


# The SDK previously ignored wolfSSL_CTX_load_verify_buffer()'s result.
# With a user-supplied AI provider this hid malformed/unloadable CA bundles,
# making every failure look like ASN_NO_SIGNER_E (-188). Log only TLS hostname,
# status and PEM length: never the API key, book text or HTTP headers.
SECURECLIENT_LOAD_OLD = """    wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(_rootCA),
                                   strlen(_rootCA), WOLFSSL_FILETYPE_PEM);"""
SECURECLIENT_LOAD_NEW = """    /* CrossPoint: validated CA loading for AI provider diagnostics. */
    const int certLoadStatus = wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(_rootCA),
                                                              strlen(_rootCA), WOLFSSL_FILETYPE_PEM);
    if (Serial) Serial.printf("[SecureClient] CA load: host=%s result=%d pem_bytes=%u\\n", host, certLoadStatus,
                              static_cast<unsigned>(strlen(_rootCA)));
    if (certLoadStatus != WOLFSSL_SUCCESS) {
      stop();
      return 0;
    }"""


def patch_secure_client_source(source: str) -> str:
    if SECURECLIENT_LOAD_NEW in source:
        return source  # Idempotent for incremental builds.
    count = source.count(SECURECLIENT_LOAD_OLD)
    if count != 1:
        raise RuntimeError(f"Unsupported pinned FreeInk SecureClient.cpp (CA load matches={count})")
    return source.replace(SECURECLIENT_LOAD_OLD, SECURECLIENT_LOAD_NEW)


if "--self-test" in sys.argv:
    probe = "before\\n" + SECURECLIENT_LOAD_OLD + "\\nafter"
    patched = patch_secure_client_source(probe)
    assert "CA load: host=%s result=%d" in patched
    assert patch_secure_client_source(patched) == patched
    try:
        patch_secure_client_source("unknown SDK")
        raise AssertionError("mismatch must fail closed")
    except RuntimeError:
        pass
    print("SecureClient CA load diagnostic patch checks passed")
    raise SystemExit(0)


Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* MEMFIX-PORT: 8192 handles up to RSA-4096 keys (the public-CA maximum,
   ISRG Root X1 included) with half the per-bignum heap of 16384: with
   WOLFSSL_SMALL_STACK each fast-math temp is FP_MAX_BITS/8 * 2 bytes on the
   heap, and TLS cert verification allocates dozens at once. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
/* Arduino-wolfSSL always defines DEBUG_WOLFSSL, but SecureClient turns the
   trace on only in FREEINK_WOLFSSL_DEBUG builds. Drop it otherwise so the trace
   code and strings are not linked. Set the flag for all sources (build_flags),
   not just SecureNet, to keep the trace. */
#ifndef FREEINK_WOLFSSL_DEBUG
#undef DEBUG_WOLFSSL
#endif
"""


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)

# Apply a narrowly scoped downstream SDK patch during the pre-build step.
# The submodule SHA remains pinned; the patch is local to the build tree.
secure_client_file = PROJECT_DIR / "freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp"
if secure_client_file.is_file():
    contents = secure_client_file.read_text(encoding="utf-8")
    amended = patch_secure_client_source(contents)
    if amended != contents:
        secure_client_file.write_text(amended, encoding="utf-8")
        print("Patched SecureClient CA loading diagnostics")
else:
    raise RuntimeError("Missing pinned FreeInk SDK SecureClient.cpp submodule")
