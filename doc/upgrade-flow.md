**[English](upgrade-flow.md) · [中文](upgrade-flow.zh-CN.md)**

# Quilla Online Upgrade Flow (Host → QBOOT → Reset)

This document describes a complete serial-port upgrade: entering QBOOT, per-frame processing and Flash writes, END transport-layer verification, SIGN signature verification and manifest persistence, and finally the reset into the new APP.

- The boot-time verification and jump-to-APP procedure **after** reset is described in [`boot-flow.md`](boot-flow.md).
- The protocol specification and the complete error-code table are in [`qboot-app-architecture.md`](qboot-app-architecture.md).

---

## 1. Participants and Source Index

| Component | Code |
| --- | --- |
| Host tool | `tools/quilla_host_updater.py` |
| Host signing | `tools/quilla_ed25519_sign.py` |
| QBOOT entry (main / serial loop / shell) | `os/services/quilla-qboot/qboot-main.c` (shared) |
| QIAP frame state machine + Flash writing | `os/services/quilla-qboot/quilla-qiap.c` |
| Secure boot policy (signature verification / anti-rollback / manifest) | `os/services/quilla-qboot/quilla-qboot-secure.c` |
| Ed25519 verification | `os/services/quilla-qboot/quilla-ed25519.c` |
| Interaction flag | `os/services/quilla-qboot/quilla-qboot-flag.c` |
| Hardware operations (Flash / reset / jump / transmit) | `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` |
| Partition constants (single source of truth) | `os/services/quilla-qboot/quilla-qboot-hal.h` |

---

## 2. Partition Layout (the coordinate system for flashing and verification)

| Region | Address Range | Length | Description |
| --- | --- | --- | --- |
| QBOOT | `0x08000000` – `0x08005FFF` | 24 KB (PY32F040xB pages 0–95) | Includes SHA-256/512 + Ed25519 verification |
| APP (flashable region) | `0x08007000` – Flash end | PY32F040xB `0x19000` = 102,400 B; GD32 `0x79000` = 495,616 B | DATA frames are accepted only within this range |
| META region | `0x08006000` – `0x08006FFF` | 4 KB | Wear-levelled flag + secure-boot manifest log (dual 2 KiB slots, see below) |

Design constraints:

- **The META region (which holds the manifest) is not inside the APP region** (`QUILLA_QBOOT_APP_END` = `SOC_FLASH_END`: PY32F040xB `0x0801FFFF`, GD32F502VE `0x0807FFFF`); the upgrade protocol hard-rejects any write into the META region, and the linker never places code there.
- **META store layout**: the 4 KiB META region is split into two 2 KiB slots — SLOT_A (`0x08006000`) and SLOT_B (`0x08006800`). Each record is a 256 B stride = 64 B meta block (magic `QMTL`, `0x514D544C`) + 128 B manifest block (magic `QMF1`, `0x514D4631`) + 64 B padding, and is **appended** with program-only writes — the live (active) slot is never erased in place. A slot is only erased when it is the standby (the slot that does NOT hold the latest record), and it is pre-erased before the active slot fills. On read, both slots are scanned and the record with the highest sequence is authoritative, so a power loss at any point leaves a fully readable record.
  - The **meta block** carries `flag` (`APP1`/`QIAP`/`QBOOT`), the anti-rollback `floor`, and the sealed-image record (`seal_hash`/`seal_len`).
  - The **manifest block** carries `version` (assigned by QBOOT from its internal anti-rollback floor at install — never host-supplied and never taken from the firmware image), `image_len`, `hash` and `sig`; it is written only when an image is installed (the QIAP SIGN / integrity-only END path via `quilla_qboot_meta_set_manifest`), never in `QBOOT_CHECK` mode. When no manifest exists the block holds just the `QMF1` magic + `version` with `image_len = 0` and the rest `0xFF`.
  - Both blocks carry independent magic + CRC-32, so a torn write to one block does not invalidate the other ("independent logs").

The single source of truth for all offsets is `QUILLA_QBOOT_APP_START` in `quilla-qboot-hal.h`.

---

## 3. Frame Format

```
+------+------+-----------+---------+------------+
| SOF  | CMD  | LEN(2,LE) | PAYLOAD | CRC16(2,LE)|
| 0xA5 | 1B   | 2B        | LEN B   | 2B         |
+------+------+-----------+---------+------------+
         └────────── CRC16 covers CMD+LEN+PAYLOAD ────┘
```

- The CRC initial value is 0; the algorithm is `crc16_data()` (`os/lib/crc16.c`).
- The per-frame payload limit is `QUILLA_QIAP_MAX_PAYLOAD = PAGE_SIZE + 8 = 264` bytes; oversized frames are discarded while the parser waits for the next SOF.

### Command Table

| Direction | CMD | Name | payload |
| --- | --- | --- | --- |
| H→M | `0x01` | ENTER | `app_start(4)` + `unlock_key(8)` |
| H→M | `0x02` | DATA | `start(4)` + `end(4)` + `data(≤256)` |
| H→M | `0x03` | END | `total_len(4)` + `total_crc16(2)` |
| H→M | `0x04` | SIGN | `signature(64)` (64 B total) |
| M→H | `0x81` | ACK_ENTER | `status(1)` |
| M→H | `0x82` | ACK_DATA | `start_addr(4)` (echo) |
| M→H | `0x83` | ACK_END | `status(1)` |
| M→H | `0x84` | ACK_SIGN | `status(1)` |
| M→H | `0xC0\|phase` | NAK | `reason(1)`; phase: ENTER=1, DATA=2, END/other=3, SIGN=4 |

### NAK reason

| Value | Meaning |
| --- | --- |
| `0x01` | Address out of range (not in the APP region) |
| `0x02` | unlock key mismatch |
| `0x03` | Frame CRC error (**retransmittable**) |
| `0x04` | Page erase failure |
| `0x05` | Programming/read-back verification failure |
| `0x06` | Total length or total CRC mismatch |
| `0x07` | Length field does not match the actual data length |
| `0x08` | Signature verification failed |
| `0x09` | Image has no usable security-version region (missing `.quilla_secver` / magic mismatch) |
| `0x0A` | Firmware security version is below the device anti-rollback floor (downgrade rejected) |

---

## 4. Overall Sequence

```
Host                                    MCU (QBOOT)
  |                                            |
  |-- (0) Keep the device in QBOOT ------------->|
  |      'swqboot' in APP / leftover flag==QIAP |
  |                                            |
  |-- (1) Read banner, send 'qiap' if needed --->|  qboot_qiap_run()
  |                                            |
  |-- ENTER 0x01 ------------------------------->|  Verify addr+key, flag=QIAP
  |<- ACK 0x81 ---------------------------------|
  |                                            |
  |-- DATA 0x02 xN (256B/chunk) --------------->|  Per-page read-modify-erase-write + read-back verify
  |<- ACK 0x82 (echo start addr) xN -------------|  Accumulate total_len / total_crc
  |                                            |
  |-- END 0x03 --------------------------------->|  Compare total length + total CRC
  |<- ACK 0x83 ---------------------------------|  (no commit here when a public key is present)
  |                                            |
  |-- SIGN 0x04 -------------------------------->|  Real-time SHA-256 + Ed25519 verify
  |                                            |  + anti-rollback + write manifest
  |                                            |  + raise lower bound + flag=APP1
  |<- ACK 0x84 ---------------------------------|
  |                                            |
  |                                            |-- quilla_qboot_hal_reboot()
  |                                            |   (disable IRQs + NVIC_SystemReset)
```

---

## 5. Stage 0: Keep the MCU in QBOOT

An upgrade must take place inside QBOOT. There are three ways to enter it:

| Method | Trigger | Result |
| --- | --- | --- |
| Active switch from APP | Run `swqboot` in the APP shell (`quilla-app-qboot.c`) | `flag=QBOOT` → reset → QBOOT shell (**no automatic upgrade**) |
| Host request | The QBOOT shell receives the text `qiap\n` | Enter the QIAP receive loop |
| Power-loss recovery | The flag is already `QIAP` (recorded in the META store) | Power-on goes directly into the upgrade state (a partially flashed image is recoverable) |

`swqboot` only parks the device at the QBOOT shell and does not start an upgrade — this is a deliberate design choice to avoid accidentally entering the flashing state. Starting an upgrade still requires sending `qiap`.

---

## 6. Stage 1: Entering the QIAP Receive Loop

`os/services/quilla-qboot/qboot-main.c: qboot_qiap_cmd_handler()` → `qboot_qiap_run()`:

```c
shellport_set_raw_sink(qboot_rx_sink);   /* bytes from the DMA ISR now go to the ring */
quilla_qiap_init();                      /* state machine reset: done=0, session=0 */
shell_puts("\r\n[QIAP] enter upgrade mode. Send ENTER frame.\r\n");
while(1) {
  if(quilla_qiap_is_done()) {            /* upgrade success -> reboot */
    shell_puts("\r\n[QIAP] flash complete. Rebooting into APP...\r\n");
    quilla_qboot_hal_reboot();
  }
  if(qboot_rx_pop(&b) == 0) quilla_qiap_feed(b);
}
```

**Flash erase/write is not performed in the ISR.** The DMA interrupt only pushes bytes into a 512 B ring buffer (`qboot_rx_sink`); the main loop then feeds them one by one to `quilla_qiap_feed()`. If the buffer is full, bytes are dropped, and the host's retransmission provides the fallback.

On the host side, `ensure_upgrade_mode()` first reads the banner:

- Contains `enter upgrade mode` → already in upgrade mode;
- Contains `[BOOT]` → send `qiap\n` and wait up to 3 seconds for `enter upgrade mode`;
- Neither → report an error and exit.

Note: `[BOOT]` comes from the line-start marker of the QBOOT banner; the echo of the `part` command contains `qboot`, which can serve as a second confirmation.

---

## 7. Stage 2: Frame Parsing State Machine

`quilla-qiap.c: quilla_qiap_feed()`, advanced byte by byte:

```
S_SOF ─(0xA5)─> S_CMD ─> S_LEN1 ─> S_LEN2 ─> S_PAYLOAD ─> S_CRC1 ─> S_CRC2
                                      │                                 │
                            len>264: drop frame, return to S_SOF   CRC mismatch: send NAK 0xC3/0x03
                                                                   CRC match: dispatch()
                                                                   afterwards always return to S_SOF
```

The CRC is accumulated as bytes arrive (each byte via `crc16_data`) and compared after both CRC bytes have been received. On mismatch a NAK is sent and the parser resynchronizes; no partial parsing is performed.

---

## 8. Stage 3: ENTER — Handshake and Setting the QIAP Flag

The `QUILLA_QIAP_CMD_ENTER` branch of `dispatch()`:

1. `len < 12` → NAK `0xC1/0x07`
2. `app_start != QUILLA_QBOOT_APP_START` (i.e. `0x08007000`) → NAK `0xC1/0x01`
3. Compare the key byte by byte (no `memcmp`; a branch-free XOR accumulation is used to avoid pulling in libc) → on mismatch NAK `0xC1/0x02`
4. Clear `rx_total_len` / `rx_total_crc` / `end_seen`, set `session_active=1`
5. `quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_QIAP)`
6. ACK `0x81`

The purpose of step 5: from then on, even after a mid-upgrade power loss or disconnection, the flag read at the next power-on is `QIAP`, so QBOOT re-enters the upgrade flow instead of jumping to a half-flashed APP.

---

## 9. Stage 4: DATA — Chunk-by-Chunk Writes with Read-Back Verification

Each chunk is one page (PY32F040xB: 256 B; GD32: 2 KB); payload = `start(4) + end(4) + data`.

Validation order of the `DATA` branch of `dispatch()`:

1. `session_active` is false → NAK `0xC2/0x07`
2. `len < 8` → NAK `0xC2/0x07`
3. `start < APP_START` or `end > APP_END` or `end < start` or `(end-start+1) != datalen` → NAK `0xC2/0x01`
   - The upper bound of `end` is `APP_END` (= `SOC_FLASH_END`: PY32F040xB `0x0801FFFF`, GD32F502VE `0x0807FFFF`), so the META region is inherently excluded and the host cannot write it by mistake. The device's own bound is authoritative; the host updater learns it via the `pinfo` shell command before transferring.
4. `flash_write_range()` → on failure NAK `0xC2/0x04` (erase) or `0xC2/0x05` (program/read-back)
5. Accumulate `rx_total_len`, chain-accumulate `rx_total_crc`
6. ACK `0x82` + echo `start` (the host verifies the address chunk by chunk to prevent misalignment)

### Read-Modify-Erase-Write in `flash_write_range()`

```c
while(off < datalen) {
  page_base = (addr + off) & ~(PAGE_SIZE - 1);
  page_off  = (addr + off) - page_base;
  n = min(PAGE_SIZE - page_off, datalen - off);

  quilla_qboot_hal_flash_read(page_base, staging, PAGE_SIZE);   /* read whole page */
  memcpy(staging + page_off, data + off, n);                    /* modify RAM */
  quilla_qboot_hal_flash_write_page(page_base, staging);        /* erase + write */
  quilla_qboot_hal_flash_read(page_base + page_off, staging, n);/* read back */
  if(memcmp(staging, data + off, n)) return -4;                 /* verify */
  off += n;
}
```

- `staging` uses `QUILLA_QBOOT_ALIGNED` (4-byte alignment) — unaligned word accesses on Cortex-M0+ trigger a HardFault.
- Chunked writes are therefore safe: a frame smaller than one page will not erase existing same-page content to `0xFF`.
- The underlying `quilla_qboot_hal_flash_write_page()`: `HAL_FLASH_Unlock` → erase 1 page → `HAL_FLASH_PageProgram` → `HAL_FLASH_Lock`.

---

## 10. Stage 5: END — Transport-Layer Total Verification

payload = `total_len(4) + total_crc16(2)`.

1. `session_active` is false → NAK `0xC3/0x07`
2. `len < 6` → NAK `0xC3/0x07`
3. `rx_total_len != total_len || rx_total_crc != total_crc` → NAK `0xC3/0x06` (the whole image can be resent)
4. `end_seen = 1`
5. Branch:
   - **No public key burned** (`quilla_qboot_secure_pubkey_active()==0`) → `commit_app()`: write `flag=APP1` and `done=1`, then ACK `0x83`.
     This supports devices and development flows that have not enabled secure boot.
   - **Public key burned** → **ACK only, no commit**. An unsigned image is never marked runnable; SIGN must pass first.
6. ACK `0x83`

This branch is the core change introduced by the secure-boot integration: END is downgraded from a "commit point" to a "transfer-complete acknowledgment".

---

## 11. Stage 6: SIGN — Full Verification + Writing the Manifest + Commit

payload = `signature(64)`; the length must be exactly 64 B.

### 11.1 Pre-checks

| Condition | Result |
| --- | --- |
| `!session_active \|\| !end_seen` | NAK `0xC4/0x07` |
| `len != 64` | NAK `0xC4/0x07` |
| No public key burned | `commit_app()` directly + ACK `0xC4` (prevents SIGN frames from rendering devices unbootable when secure boot is absent) |
| `rx_total_len == 0` or `> MAX_IMAGE_LEN` (`APP_END − APP_START + 1`; `0x19000` on PY32F040xB) | NAK `0xC4/0x07` |

### 11.2 Computing the Digest On the Fly

```c
quilla_qboot_secure_hash_range(QUILLA_QBOOT_APP_START, rx_total_len, hash);
```

The SHA-256 is computed in a streaming fashion from the **actual current contents of Flash** (fed to `sha256_update` 64 B at a time). What the signature binds is "what is in Flash at this moment", not what the host claims to have sent.

### 11.3 Signature Verification

The object being signed is a fixed 36-byte descriptor:

```
image_len(4, LE) || SHA-256(image)(32)
```

The version is **not** in the signed descriptor: it lives inside `image_len` (at `QUILLA_QBOOT_APP_START + 0x400`) as part of the signed image, covered by the digest and signature, so it cannot be forged without the private key. Neither the host nor QBOOT invents it. `quilla_qboot_secure_verify_descriptor()` reassembles the descriptor using the **just-computed** `hash` and calls `quilla_ed25519_verify()`.

- On failure → NAK `0xC4/0x08`; the image remains in Flash but is **not marked runnable**.

### 11.4 Version assignment (anti-rollback)

The anti-rollback version is **baked into the signed firmware image** (at the fixed Flash address `QUILLA_QBOOT_APP_START + 0x400`, as `QUILLA_APP_SECURITY_VERSION`), not owned by QBOOT or the host. On a successful, authenticated SIGN, QBOOT reads the firmware version and ratchets the floor up to it (refusing a lower one):

```c
fw_version = quilla_qboot_secure_fw_version();  /* from signed image, never host-supplied */
if(fw_version == 0U) {
    send_nak(4U, QUILLA_QIAP_RC_VER);      return;  /* no usable security-version region */
}
if(fw_version < quilla_qboot_secure_min_version_get()) {
    send_nak(4U, QUILLA_QIAP_RC_ROLLBACK); return;  /* downgrade below the floor */
}
nv = fw_version;
quilla_qboot_meta_set_manifest(nv, rx_total_len, hash, payload);
quilla_qboot_secure_min_version_set(nv);
```

The two rejection reasons above use **distinct** reason codes (`0x09` VER, `0x0A` ROLLBACK) rather than reusing `0x08` SIGN, so the host updater reports the *true* cause (a missing version region vs. a downgrade) instead of mislabeling both as "signature failed". A real signature mismatch still returns `0x08`.

The host cannot influence this value, and it is not taken from the firmware image, so neither an operator nor a malicious build can forge a lower version. The actual rollback rejection (`version < floor`) is enforced at boot by `quilla_qboot_secure_verify_app()`; see `boot-flow.md`.

### 11.5 Writing the Manifest

The manifest is no longer a standalone page: it is the 128 B `QMF1` block inside the wear-levelled META record, written by `quilla_qboot_meta_set_manifest()` (which appends a new META record rather than erasing in place). The block layout:

| Offset | Field | Length |
| --- | --- | --- |
| 0 | magic `QMF1` (`0x514D4631`) | 4 |
| 4 | version | 4 |
| 8 | image_len | 4 |
| 12 | SHA-256(image) | 32 |
| 44 | Ed25519 signature | 64 |
| 108 | CRC-32 | 4 |
| — | Total | **128 B** (rest reserved / `0xFF`) |

The manifest is written only when an image is installed — the SIGN path (`version`/`image_len`/`hash`/`sig`) and the integrity-only END path (`version`/`image_len`/`hash`, signature left as `0xFF`). In `QBOOT_CHECK` mode no manifest is produced; the block then carries only the `QMF1` magic + `version` with `image_len = 0` and the rest `0xFF`.

### 11.6 Committing

```c
if(commit_app() != 0) → NAK 0xC4/0x05;              /* flag = APP1, done = 1 */
send_ack(QUILLA_QIAP_ACK_SIGN, 0U);
```

The anti-rollback floor has already been raised in §11.4 (QBOOT-owned). Raising
it is a best-effort operation: a write failure does not block booting (the
signature is the main gate), so a single Flash fault does not render the device
unusable.

---

## 12. Stage 7: Reset

`quilla_qiap_is_done()` returns 1 → the main loop prints `[QIAP] flash complete. Rebooting into APP...` → `quilla_qboot_hal_reboot()`:

```c
__disable_irq();
HAL_NVIC_SystemReset();
while(1) {}
```

Interrupts are disabled first, so that no ISR can interrupt access to the just-rewritten interaction flag.

For the procedure after reset, see [`boot-flow.md`](boot-flow.md).

---

## 13. Host Commands

```bash
# Regular upgrade with secure boot disabled (no public key)
python tools/quilla_host_updater.py COM3 app.bin

# With secure boot enabled: send the SIGN frame using the production private key generated by keygen.
# The key must come from your own keygen: the repository's default (public) key has its private
# key published with the repo, so a leak is equivalent to disclosure — switch to your own key
# and keep it offline.
python tools/quilla_host_updater.py COM3 app.bin \
       --key keys/prod/private.hex

# Generate a production key pair (outputs private.hex + public.hex; keep the private key offline)
python tools/quilla_ed25519_sign.py keygen --dir keys/prod
```

The actual order in `run()`: `ensure_upgrade_mode()` → `enter()` → `send_data()` → `end()` → `sign()` (only when `--key` is specified).

Host retransmission policy (`_transact`): on timeout or `reason==0x03` (CRC), retry up to 3 times; for any other reason, abort immediately.

If the device has a public key burned but the host did not specify `--key`, the tool first warns that "the upgrade will be rejected at SIGN" and then stops after END: the device remains at `flag=QIAP`, and a correctly signed SIGN frame can be resent, or the upgrade can simply be rerun.

For the full parameter list, see `doc/quilla-host-updater-guide.md` §10.4; for signing-tool usage, see `tools/README.md`.

---

## 14. Fault Matrix and Recovery

| Fault point | Device state | Recovery |
| --- | --- | --- |
| Disconnect before ENTER | `flag=QBOOT` or APP1 | Simply rerun the tool |
| Power loss during data transfer | **`flag=QIAP`** | Power-on automatically re-enters QBOOT; resend ENTER to continue flashing |
| CRC error on a chunk | The chunk was not written | The host automatically retransmits that chunk after the NAK |
| Read-back verification failure | The page may be corrupted | NAK `0x05`; rerun the full-image upgrade |
| END total length/CRC mismatch | Not committed | NAK `0x06`; resend END or the whole image |
| SIGN verification failure | Image is in Flash, not committed | NAK `0x08`; the device stays at `flag=QIAP` and a correct signature can be resent |
| SIGN rejected — no security-version region in the image | Image is in Flash, not committed | NAK `0x09`; rebuild the APP so `.quilla_secver` is emitted (Keil: `.ARM.__at_0x08007400`), then resend |
| SIGN rejected — downgrade (version below floor) | Image is in Flash, not committed | NAK `0x0A`; the device already accepted a newer version — flash an image whose `QUILLA_APP_SECURITY_VERSION` ≥ the floor |
| Power loss after commit, before reset | `flag=APP1`, image intact | The new APP boots normally at power-on |
| manifest write failure | Not committed | NAK `0x05`; rerun the upgrade |

Any uncommitted state leaves `flag` at `QIAP`, so QBOOT automatically returns to an interactive upgrade state at power-on; therefore a failed upgrade never renders the device unusable.

---

## 15. One-Page Cheat Sheet

```
'swqboot' in APP ──> flag=QBOOT ──> reset ──> QBOOT shell
                                                    │ 'qiap'
                                                    v
                                              QIAP receive loop
ENTER ──> flag=QIAP (power-loss recovery anchor)
DATA  ──> per-page read-modify-erase-write + read-back check, chain-accumulate len/crc
END   ──> compare total len/crc; commit if no public key, ACK only if key present
SIGN  ──> real-time SHA-256 → Ed25519 verify → anti-rollback → write manifest
          → raise lower bound → flag=APP1 → done=1
Reset ──> see boot-flow.md
```
