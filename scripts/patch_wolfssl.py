from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* InkPointX wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* Public roots may use RSA-4096. Keeping the fast-math ceiling at 8192
   halves wolfSSL's small-stack bignum temporaries compared with its default. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
"""


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    base = text
    if MARKER in text:
        base = text.split(MARKER, 1)[0].rstrip()
    patched = base + OVERRIDES + "\n"
    if patched == text:
        return
    path.write_text(patched)
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)
