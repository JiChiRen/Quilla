# tools/

**[English](README.md) · [中文](README.zh-CN.md)**

Quilla toolset. Only the tools below ship in this public repository, and they cover the full external workflow — flash your own APP image, and sign it for secure boot. (Internal build / verification / audit tooling is not part of this public repository.)

## Provided for external use

| Tool | Purpose |
|------|---------|
| `quilla_host_updater.py` | Flash a QAPP over UART: `python quilla_host_updater.py <COM> app.bin -b 115200` |
| `quilla_ed25519_sign.py` | Secure-boot signing / key management: `keygen`, `pubkey`, `sign`, `--self-test` |
| `quilla_app_uart_flash.bat` | Windows one-shot flash helper |

### Key management (important)

`tools/_keytest/` holds the repository's **default sample keypair**. Its private key (`private.hex`) is intentionally public and ships with the repo so a fresh clone works out of the box: the default `QBOOT_SEALED` / `QBOOT_SECURED` builds compile in the matching default public key, and the upgrade flow (`quilla_host_updater.py --key tools/_keytest/private.hex`) needs the matching private key to sign — otherwise the device rejects the SIGN frame.

**For production, generate your own keypair offline.** The default key must NOT ship in a product — its private half is public, so anyone could sign an image the device accepts:

```bash
python tools/quilla_ed25519_sign.py keygen --dir <offline-secure-dir>
```

- Keep your production `private.hex` only on the signing station — never in firmware, never in the repo.
- Compile your `public.hex` into QBOOT. `quilla_ed25519_sign.py keygen` prints both procedures: GNU/EIDE adds `-DQUILLA_SECURE_BOOT_PUBKEY={...}` to `C_DEFS`; Keil's comma-separated Define field cannot carry the list, so add a product branch in `os/services/quilla-qboot/quilla-qboot-pubkey.h` (template: `tools/_keytest/README.md` §3.2) and select its token there.
- You may add `-DQUILLA_PRODUCTION_BUILD`; if the repository default public key is still compiled in, `quilla-qboot-secure.c` emits one `#warning` reminding you to replace it (no longer a hard `#error` — see `os/services/quilla-qboot/quilla-qboot-pubkey.h`).

## Reference

- Ed25519 — RFC 8032 (the standard text is not vendored here; use the official copy):
  https://www.rfc-editor.org/rfc/rfc8032.txt
