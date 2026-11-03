**[English](boot-flow.md) · [中文](boot-flow.zh-CN.md)**

# Quilla Boot Flow (Post-Reset Validation and Handover to the APP)

This document describes how, after every power-on / reset, QBOOT decides between "enter upgrade / stay in shell / validate and jump to the APP", and every step of the jump handover.

- Upgrade (flashing) flow: see [`upgrade-flow.md`](upgrade-flow.md).
- Protocol specification: see [`qboot-app-architecture.md`](qboot-app-architecture.md).

---

## 1. Overview
```
                ┌────────── Power-on / Reset ─────────┐
                │  Reset_Handler → QBOOT's main()     │
                └──────────────────┬──────────────────┘
                                   │
                   clock_init() / flag_init() / shellport_init()
                                   │
                        flag = quilla_qboot_flag_get()
                                   │
        ┌──────────────────────────┼──────────────────────────┐
        │                          │                          │
   flag == QIAP(2)            flag == QBOOT(3)          flag == APP1(1)
        │                          │              or interaction flag blank/corrupt
        v                          v                          │
  qboot_qiap_run()         qboot_shell_run()                  v
  (enter upgrade,          (stay in QBOOT shell: qiap/swapp)   qboot_app_image_valid()
   does not return)         does not return                   │
                                                        ┌─────┴─────┐
                                                     Failure      Success
                                                        │            │
                                                        v            v
                                                 qboot_shell_run()  qboot_secure_boot_ok()
                                                                      │
                                                              ┌───────┴───────┐
                                                    Refuse (print reason)   Pass
                                                              │               │
                                                              v               v
                                                       qboot_shell_run()  jump_to_app()
```

**Two parallel gates**: mechanical integrity (vector table) + cryptographic authenticity (signature). In `main()` this reads `if(qboot_app_image_valid() && qboot_secure_boot_ok())`; if either check fails, fall into the QBOOT shell.

---

## 2. Phase 1: Minimal Initialization

`os/services/quilla-qboot/qboot-main.c: main()` (shared across all ports):

```c
clock_init();            /* Under QBOOT_BUILD: SysTick only, no LPTIM/lpm */
quilla_qboot_flag_init();
shellport_init();        /* USART + DMA; shell writer in place */
shell_set_partition_tag("qboot");
```

`clock_init()` is trimmed under `QBOOT_BUILD`: it only maintains `HAL_IncTick()` and does not pull in `rtimer` / LPTIM / low-power modules. Otherwise, the dependency chain reachable from `Error_Handler` would drag APP-side drivers into the 24 KB QBOOT.

---

## 3. Phase 2: Flag Read and Dispatch

`quilla_qboot_flag_get()` returns the flag value cached from the latest valid META record (see §5 and `quilla-qboot-meta.c`). The flag is no longer a standalone "FLAG page": it is one field of the wear-levelled META record (meta block magic `QMTL`, `0x514D544C`), so an unprogrammed (all-0xFF) META reads back as the safe default `APP1`:

| Cached flag | Decision | Action |
| --- | --- | --- |
| `QIAP` (2) | `QIAP` | `qboot_qiap_run()` — enter upgrade directly, **does not return** |
| `QBOOT` (3) | `QBOOT` | `qboot_shell_run()` — stay in the QBOOT shell, **no jump even if the APP is intact** |
| `APP1` (1), or unprogrammed / unknown | `APP1` (safe default) | Fall through to phase 3 |

Two design constraints:

- **Flag unreadable → default to running the APP**. The META store scans both slots and picks the
  highest-sequence valid record, so a single Flash bit flip (or a torn record) cannot lock the
  device into the bootloader.
- **`swqboot` sets `QBOOT`, not `QIAP`**: it stops in the shell to await commands, and does not
  enter a to-be-flashed state when QBOOT starts.

---

## 4. Phase 3: Vector Table Mechanical Validation

`qboot_app_image_valid()` reads only the first 8 bytes of the APP region:

```c
quilla_qboot_hal_flash_read(QUILLA_QBOOT_APP_START, vec, 8);

vec[0] ∈ [RAM_START, RAM_START + RAM_SIZE)        /* 0x20000000 + 16 KB (PY32F040xB) */
vec[1] ∈ [APP_START, APP_END] and (vec[1] & 1) == 1 /* Reset vector within the APP region and Thumb */
```

An erased Flash reads as all `0xFF`: SP falls outside RAM and the reset vector is out of range — the check necessarily fails.

This step only guarantees "no immediate HardFault after the jump"; it **does not mean the image is trustworthy**. Authenticity is decided in phase 4.

---

## 5. Phase 4: Secure Boot Gate

`qboot_secure_boot_ok()` → `quilla_qboot_secure_verify_app()` (`quilla-qboot-secure.c`).

### 5.1 Integrity Before Authenticity

`verify_app()` first unconditionally performs the integrity checks of §5.2–§5.4 (read manifest, compare length, compute SHA-256). **A missing manifest or digest mismatch refuses boot in every variant** (fall into the QBOOT shell). Only after integrity passes does it proceed to authenticity verification according to the key and boot mode:
```c
/* No key (pubkey all 0xFF / 0x00): pass once integrity is OK, return QUILLA_SB_INACTIVE */
if(!quilla_qboot_secure_pubkey_active()) return QUILLA_SB_INACTIVE;
```

A key region that is all `0xFF` (erased) or all `0x00` is treated as "not enabled". When not enabled, the device behaves as if secure boot were not wired in, so it can be enabled unit by unit. A **manifest must still exist and its digest must match** even then (for the write path, see §4 of `doc/secure-boot.md`).

### 5.2 Read the Manifest

`quilla_qboot_meta_get()` returns the manifest cached from the latest valid META record; the `QMF1` manifest block layout (offsets relative to the block, defined in `quilla-qboot-meta.h`):

| Offset | Field | Length |
| --- | --- | --- |
| 0 | magic `QMF1` | 4 |
| 4 | version | 4 |
| 8 | image_len | 4 |
| 12 | SHA-256(image) | 32 |
| 44 | Ed25519 signature | 64 |
| 108 | CRC-32 | 4 |

If no valid manifest is present (`has_manifest == 0`, or the `QMF1` magic is absent / its CRC fails) → `QUILLA_SB_ERR_NO_MANIFEST`.

### 5.3 Length Range

```c
image_len == 0 or > QUILLA_SECURE_BOOT_MAX_IMAGE_LEN (= `APP_END − APP_START + 1`, i.e. the whole APP region; `0x19000` on PY32F040xB, `0x79000` on GD32F502VE) → ERR_LEN
```

The length described in the manifest must fall within the APP region; otherwise the subsequent hash would read out of bounds into the manifest page.

### 5.4 Compute the Digest On the Fly

```c
quilla_qboot_secure_hash_range(QUILLA_QBOOT_APP_START, image_len, hash);
```

Stream-compute SHA-256 (64 B at a time) over the **current Flash contents**. This is the core of the whole mechanism: even if an attacker rewrites both the image and the hash field in the manifest, the descriptor used for signature verification is assembled on the fly from the **just-computed** hash, so the signature is guaranteed to mismatch.

The hash stored in the manifest is still compared, solely to distinguish failure reasons:
- Live hash ≠ manifest hash → `ERR_HASH` (image altered / transfer error)
- Signature verification fails → `ERR_SIG` (image source untrustworthy)

### 5.5 Verify the Signature

```c
quilla_qboot_secure_verify_descriptor(image_len, hash, mf + 44)
```

Descriptor = `image_len(4,LE) || SHA-256(image)(32)`, 36 bytes in total, completed by a single `quilla_ed25519_verify()` call. The firmware security version is **not** in this signed descriptor: it lives inside `image_len` (at the fixed address `QUILLA_QBOOT_APP_START + 0x400`) as part of the signed image, so it is covered by the digest and the signature — it cannot be forged without the private key (see `doc/secure-boot.md` §1.1). Neither the host nor QBOOT invents it.

The boot path and the SIGN frame during upgrade **share this function**, so the two decisions cannot drift apart.

### 5.6 Anti-Rollback

```c
if(version < quilla_qboot_secure_min_version_get()) → ERR_ROLLBACK
```

The floor is a field of the wear-levelled META record (QMTL meta block, `QUILLA_META_OFF_FLOOR`); if it was never written, it is 0. An older signed image is refused execution even if its signature is valid. The `version` compared here is the **firmware security version baked into the signed image** (not invented by QBOOT); it is never taken from the host. QBOOT only ratchets its floor up to it, so the floor can only move upward. A signed-but-older image (lower firmware version) is refused.

### 5.7 Raise the Floor

```c
(void)quilla_qboot_secure_min_version_set(version);
return QUILLA_SB_OK;
```

The ratchet only moves up; a write failure **does not affect boot** (the signature is the main gate), so a single Flash fault cannot make the device unusable.

### 5.8 Result Codes and Shell Output

| Return | Print |
| --- | --- |
| `QUILLA_SB_OK` / `QUILLA_SB_OK_SEALED` | Allow jump (authenticated) |
| `QUILLA_SB_OK_INTEGRITY_ONLY` | Allow jump (integrity only, CHECK mode) |
| `QUILLA_SB_INACTIVE` | Allow jump (no key, integrity passed) |
| `ERR_NO_MANIFEST` | `no manifest` |
| `ERR_LEN` | `image length out of range` |
| `ERR_HASH` | `image content altered` |
| `ERR_SIG` | `signature invalid` |
| `ERR_ROLLBACK` | `version rollback attempted` |

Both `QUILLA_SB_OK` and `QUILLA_SB_OK_SEALED` mean authenticated: the latter means the image previously passed Ed25519 verification, and in SEALED mode a seal hit skips the full Ed25519 verification. `QUILLA_SB_OK_INTEGRITY_ONLY` appears only in CHECK mode: integrity passed but **no authenticity verification was performed**, so secure boot is not claimed.

On refusal it prints `[BOOT] secure boot: APP REFUSED - <reason>`, **without resetting or erasing**, and falls into the QBOOT shell — `qiap` remains reachable and the device can recover.

---

## 6. Phase 5: Jump Handover

`quilla_qboot_hal_jump_to_app(QUILLA_QBOOT_APP_START)` (`arch/cpu/arm/<mcu>/quilla-qboot-hal.c`). The order **must not be changed**:

```c
__disable_irq();

/* (1) Stop the peripherals QBOOT itself enabled */
NVIC_DisableIRQ(USART2_IRQn);
NVIC_DisableIRQ(DMA1_Channel4_5_6_7_IRQn);
NVIC_ClearPendingIRQ(USART2_IRQn);
NVIC_ClearPendingIRQ(DMA1_Channel4_5_6_7_IRQn);
USART2->CR3        &= ~USART_CR3_DMAR;   /* Stop USART2 RX DMA request */
DMA1_Channel6->CCR &= ~DMA_CCR_EN;       /* Stop the channel itself   */

/* (2) Take over the vector table and stack */
SCB->VTOR = app_start;
sp        = *(uint32_t *)app_start;
app_reset = (void (*)(void))(*(uint32_t *)(app_start + 4));
__set_MSP(sp);

/* (3) Enable IRQs before jumping */
__enable_irq();
app_reset();
```

Why each step is necessary:

| Step | Consequence if omitted |
| --- | --- |
| Disable/clear USART2 and DMA IRQs | Residual pending IRQs fire while the APP is half-initialized, jumping into the default handler |
| Stop DMA requests and channels | DMA keeps writing to low-address RAM that "now belongs to the APP .data/.bss", corrupting APP state |
| Set `VTOR` | Exceptions still point at QBOOT's vector table |
| **`__enable_irq()` before jumping** | On Cortex-M0+, `PRIMASK=1` masks SysTick along with everything else → `clock_time()` freezes, etimers never expire, and the APP shows "init prints fine, then the RTOS stalls" |

Peripheral numbering varies by MCU (the example above is the PY32F040xB's USART2 / DMA1 channel 6). The APP side also sets `VECT_TAB_OFFSET = QUILLA_QBOOT_APP_OFFSET` in `system_<mcu>.c` (currently `0x7000`, derived from `QUILLA_QBOOT_APP_START 0x08007000`); QBOOT sets it once beforehand, so that vectors are correct from the first instruction after the branch.

---

## 7. Phase 6: Falling into the QBOOT Shell

Enter `qboot_shell_run()` when the mechanical check fails, the signature is refused, or the flag is `QBOOT`:

```c
shell_register_cmd(&qboot_qiap_cmd);    /* qiap   : enter upgrade */
shell_register_cmd(&qboot_swapp_cmd);   /* swapp  : set flag=APP1 and reset */
shell_puts("\r\n[BOOT] Quilla V" QUILLA_VERSION_STRING "\r\n> ");
while(1) process_run();
```

- `qiap` → enter the upgrade loop.
- `swapp` → write `flag=APP1` and reset; if the current `flag==QIAP` (upgrade in progress), it is **refused** and prints `upgrade in progress, switch refused`, avoiding a forced jump onto a half-flashed image.
- The built-in `part` command now answers `qboot` — the host uses this to determine that the device is stopped in QBOOT (another cue is the `[BOOT]` prefix marker in the banner).

The function is `while(1)`: **any code after it is unreachable**.

---

## 8. State Decision Quick Reference

| Interaction flag | Vector table | Public key | Manifest / signature | Result |
| --- | --- | --- | --- | --- |
| QIAP | — | — | — | Enter upgrade directly |
| QBOOT | — | — | — | Stay in the QBOOT shell (no APP jump) |
| APP1 | Invalid | — | — | Stay in the QBOOT shell |
| APP1 | Valid | Not programmed | — | **Jump to APP** (`INACTIVE`) |
| APP1 | Valid | Programmed | No manifest | Stay in the QBOOT shell (`NO_MANIFEST`) |
| APP1 | Valid | Programmed | Hash mismatch | Stay in the QBOOT shell (`HASH`) |
| APP1 | Valid | Programmed | Signature mismatch | Stay in the QBOOT shell (`SIG`) |
| APP1 | Valid | Programmed | Version below floor | Stay in the QBOOT shell (`ROLLBACK`) |
| APP1 | Valid | Programmed | All checks pass | **Jump to APP** (and raise the floor) |
| Blank / corrupt | Valid | Any | Same as above | Treat as `APP1` |

---

## 9. Cost and Footprint

The cost of the boot path is a SHA-256 scan of the image (`image_len` bytes) plus one Ed25519 verification (only the 36-byte descriptor). Each runs exactly once — at **boot** and on the **SIGN frame** respectively — and neither runs during APP operation, so the real-time path is not burdened. SHA-256 time is **proportional to the actually written APP image size**: on PY32F040xB (Cortex-M0+ @ 48 MHz), a ~18 KB QAPP image takes about 0.1 s (≈ 180 KB/s, estimate). Ed25519 verification is independent of image size: about 3.3 s on PY32F040xB — the dominant term of the ≈3.5 s full-Ed25519 boot measured reset→APP banner on hardware at 48 MHz (vs ≈20 s at 8 MHz) — and under 500 ms on GD32F502VE (M33 @ 160 MHz, measured). (PY32F040xB timing measured on target at 48 MHz; see the note in §9 below.)

Measured QBOOT footprint against the 24 KB budget:

> **Note**: all figures in this section are **development-process measurements** taken during bring-up. They vary with software version and toolchain (version / optimization / LTO) changes and are **not a delivery specification**; re-measure on your own build before drawing capacity conclusions.

| Toolchain / config | ROM (text) | Headroom vs 24 KB |
| --- | --- | --- |
| Keil `-O2` + microlib (incl. `kern-stack.c`) — measured 2026-10-11, pre-CR-0042/0043 | 15,192 B | ~38% |
| Keil `-O1` — measured 2026-10-11, pre-CR-0042/0043 | 19,636 B | ~20% |
| Keil `-O0` | Overflow | — |
| GNU `-Os`, QBOOT_CHECK — measured 2026-11-03 | 21,208 B | ~14% |
| GNU `-Os`, QBOOT_SEALED — measured 2026-11-03 | 21,308 B | ~13% |
| GNU `-Os`, QBOOT_SEC — measured 2026-11-03 | 21,260 B | ~13% |

`-O0` overflows; be careful when single-stepping QBOOT. Enabling `KERN_CONF_STACK_SHELL=1` adds about 456 B to the GNU build (2026-10-11 measurement).

Version note: the 2026-11-03 GNU figures are per-variant (CHECK/SEALED/SEC) on `arm-none-eabi-gcc` 13.2.1 after the CR-0042 entry single-sourcing and the CR-0043 shell formatter change (which removed the newlib printf/malloc family, about -1.6 K per image). They are larger than the single 2026-10-11 figure (19,412 B) mainly because the META dual-slot store and anti-rollback features landed in between — the footprint grew with features, then shrank with CR-0043. CH32V305 measured with the WCH GCC12 toolchain: 20,987–21,095 B text (all three variants).

---

## 10. Common Troubleshooting

| Symptom | Cause |
| --- | --- |
| Repeatedly entering the upgrade state | `flag` stuck at `QIAP` (upgrade not committed); rerun the upgrade or run `swapp` |
| Always staying in the QBOOT shell, no REFUSED print | Vector table mechanical check failed (APP not flashed / flashed to the wrong address / APP not linked for `0x08007000`) |
| Prints `no signed manifest` | No SIGN frame sent during upgrade, or `--key` not specified |
| Prints `image content altered` | Flash contents do not match the manifest description; re-flash |
| Prints `signature invalid` | Image and key do not match (key was changed, or the image length was modified) |
| Prints `version rollback attempted` | Firmware security version is below the QBOOT anti-rollback floor (the floor ratchets up on every authenticated install and is never reset by the host); this means the device already accepted a newer image, or the META floor was carried over from another unit. Re-run the upgrade with an image carrying a higher `QUILLA_APP_SECURITY_VERSION`. The version is part of the signed image, not assigned by QBOOT |
| Init prints fine after the jump, then it stalls | Missing `__enable_irq()` before the jump; check `jump_to_app()` |
| IRQs do not fire in the APP | Pending IRQs not cleared / DMA not stopped before the QBOOT jump |
