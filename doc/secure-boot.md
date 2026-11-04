**[English](secure-boot.md) · [中文](secure-boot.zh-CN.md)**

# Quilla Secure Boot Design Specification

This document describes QBOOT's boot-time trust establishment strategy. It helps build / release / certification leads choose among three **build variants**, and gives corresponding recommendations for IEC 62443 compliance levels.

> Audience: developers / security leads responsible for firmware build, release, and certification.
> Key management (default public key, own public key, private key storage) see `tools/README.md`.
> Hard constraints: the default public key (publicly shipped with the repository) must not be delivered with products; the private key must be stored offline, kept only at the signing station, and never enter firmware or the repository;
> production builds may define `QUILLA_PRODUCTION_BUILD` — if the repository default public key is still compiled in, the header will emit a compile warning reminding replacement (no longer hard-blocked with `#error`).

---

## 1. What Happens at Boot

Before QBOOT hands control over to the APP, it establishes trust in the following order (step 1 always executes; steps 2 and 3 depend on the build variant):

1. **Integrity (always executed)**: compute SHA-256 over the APP image (`[APP_START, APP_START + image_len)`, where `image_len` is the actual written firmware length recorded in the manifest) and compare it against the digest, detecting "image corrupted / partially flashed".
   The cost is **proportional to the actually written APP image size**: on **PY32F040xB (Cortex-M0+, 48 MHz)**, a typical ~18 KB QAPP image takes about **0.1 s** (≈ 180 KB/s), i.e., an N KB APP image takes about N × 0.0055 s.
   Missing manifest or digest mismatch → refuse to boot, fall into the QBOOT shell.
2. **Authenticity**: perform an **Ed25519 signature verification** on the 36-byte descriptor `image_len(4) || SHA-256(32)`, with the public key compiled into the QBOOT image (not modifiable). This proves the image "was indeed signed by a party holding the private key".
   The verification target is fixed at the 36-byte descriptor and is **independent of image size**: on **PY32F040xB (Cortex-M0+, 48 MHz; no 32×32→64 multiplier, no crypto acceleration, pure C bignum arithmetic)** it takes about **3.3 s**. GD32F502VE (Cortex-M33 @ 160 MHz, with 32×32→64 multiplier) is measured at **under 500 ms** on hardware (~18 KB image).
   **Only "keyed" build variants execute this step.**
3. **Anti-rollback**: refuse to boot when the APP image's **firmware security version** is below the monotonic "minimum version floor" persisted in the META record.
   The security version is **not owned by QBOOT and is never supplied by the host**: it is baked into the signed image region at the fixed address `QUILLA_QBOOT_APP_START + 0x400` (`0x08007400`) as part of the signed `image_len`, so it is covered by the SHA-256 digest and the Ed25519 signature — it cannot be forged without the private key. On a successful authenticated install the floor is raised to that firmware version.
   See **§1.1 Dual anti-rollback mechanism** for the full design, including the non-security APP-side obfuscation layer.
   **Only "keyed" build variants execute this** (QBOOT_CHECK does not, because it has no key and cannot trust the floor).

### 1.1 Dual anti-rollback mechanism

The anti-rollback gate is enforced by **two cooperating layers**. Only the first is a security control; the second is honest defense-in-depth and is explicitly **not** a cryptographic boundary.

**Layer 1 — firmware-owned security version (the real gate).**
The APP image carries a 32-bit security version (`QUILLA_APP_SECURITY_VERSION`) baked at the fixed Flash address `QUILLA_QBOOT_APP_START + 0x400` (`0x08007400`), inside the signed `image_len`. It is therefore covered by the SHA-256 digest and the Ed25519 signature: an attacker cannot change it without the private key. QBOOT reads it at boot (`quilla_qboot_secure_fw_version()`) and at the QIAP SIGN stage, compares it to the persisted floor, and rejects a lower value (`QUILLA_SB_ERR_ROLLBACK`). A downgraded image signed by the real key simply carries a lower version and is refused. The version is **never owned by QBOOT and never supplied by the host** — only a genuine (signed) release fixes its value. Bump `QUILLA_APP_SECURITY_VERSION` on every firmware release that must not be rolled back.

**Layer 2 — APP-side obfuscation (`QUILLA_APP_SECVER_SELFCHECK`, default ON).**
The APP additionally self-checks the in-Flash region at startup (`quilla_app_secver_selfcheck()`) and halts if the magic / version is internally inconsistent. **Honest scope — read before relying on it:** this is **OBFUSCATION, not a security control**. Its only purpose is to raise the bar for an **authorized-key end user** who would otherwise locate the explicit security-version bytes in the binary, bump them, and re-sign — defeating the QBOOT gate with a one-line edit. It does **not** stop a determined attacker who holds the private key: both the region and the baked constant live in the signed image and are covered by the signature, so a key holder can always produce a consistent (`magic`, `version`) pair and re-sign. The real gate remains Layer 1 (signature + floor); production still requires **disciplined private-key authorization**, and a **hardware monotonic counter (eFuse/OTP)** is the only control that stops a key holder from rolling back. Disable Layer 2 with `-DQUILLA_APP_SECVER_SELFCHECK=0` if it interferes with a debug workflow.

> **Key-management consequence:** because the version is part of the signed image, the host updater (`quilla_host_updater.py`) no longer transmits a version — it sends only the image; the version is whoever signed it. A no-key (QBOOT_CHECK) install still writes an integrity-only manifest and treats the version as a floor placeholder, but anti-rollback is not enforced there (no key ⇒ no trusted floor).

**No public key compiled at all** (pubkey all 0xFF) → steps 2 and 3 are automatically skipped, leaving only the integrity check of step 1.
This is the historical "keyless" state, now corresponding to the build variant **QBOOT_CHECK**.

---

## 2. The Three Build Variants

Variants are determined by build-time macros and produce different QBOOT images (distinguished in the banner by `[CHECK]` / `[SEALED]` / `[SEC]`).
Their boot policies and applicable scenarios are as follows:

| Variant | Key (same one for SEALED / SEC) | Boot policy (boot mode) | banner | Anti-rollback | Claims secure boot | Use case |
|------|----------------------------|----------------------|--------|--------|----------------------|------|
| **QBOOT_CHECK** | None | Integrity only (CHECK mode, step 1) | `[CHECK]` | No | No (integrity only) | Development / internal / production lines not requiring authenticity |
| **QBOOT_SEALED** | Default key (replaceable with your own, see §2.5) | SEALED mode: SHA-256 (~0.6 s, PY32F040xB) compares the seal; verification is skipped on a hit; falls back to Ed25519 once on first boot / image change, then re-seals | `[SEALED]` | Yes (firmware version, §1.1) | Yes (relies on RDP locking SWD) | R&D bring-up + mass production (RDP on, SL1/SL2) |
| **QBOOT_SEC** | Default key (same one as SEALED, replaceable with your own, see §2.5) | SECURE mode: Ed25519 verification on every boot (~20 s, PY32F040xB) | `[SEC]` | Yes (firmware version, §1.1) | Yes (re-verified every power cycle) | Mass production (certification profile requires per-cycle re-authentication, SL3/SL4) |

### 2.1 QBOOT_CHECK (Transport Integrity Only)

- Boot performs only the SHA-256 comparison: no signature verification, no origin authentication, no anti-rollback.
- **Does not claim secure boot**: it can only guarantee that "the image was not corrupted during transfer / flashing", not that "the image comes from a trusted party".
- Requires no key management at all and works out of the box; suitable for development debugging and internal / closed scenarios with no authenticity requirement.
- During upgrade, the integrity manifest is written by QIAP's END frame (see `doc/quilla-host-updater-guide.md` §10); this manifest contains no trusted signature; the signature field is 0.

### 2.2 QBOOT_SEALED (SEALED boot mode, default (public) key)

- Boot uses the **SEALED mode**: in steady state, each boot only recomputes SHA-256 (~0.6 s on PY32F040xB, varying linearly with image size) and compares it against the seal in the META record, **skipping the Ed25519 verification**; only on first boot / image change (SWD re-flash, seal lost or expired) does it run the full Ed25519 (~20 s on PY32F040xB), re-sealing after it passes.
- The compiled **key is exactly the same as QBOOT_SEC's**: by default it is the repository-public "default public key" (`QUILLA_QBOOT_DEFAULT_KEY`, with the paired private key `tools/_keytest/private.hex` shipped publicly with the repository — anyone can sign images that it will accept).
- **"Whether it can ship" depends on whether the key is still the default public key, not on the SEALED variant name**: after switching to your own key, the SEALED mode itself (together with RDP locking SWD) is a legitimate mass-production configuration (SL1/SL2). If the default key is still used, it is equivalent to handing over the private key, and **shipping is prohibited**. For key replacement see §2.5.

### 2.3 QBOOT_SEC (SECURE boot mode, default (public) key)

- Boot uses the **SECURE mode**: full Ed25519 verification on every power cycle (~20 s on PY32F040xB, independent of image size), **not using the seal fast path**. Suitable for products whose certification profile explicitly requires "re-verifying authenticity on every boot" (SL3/SL4, or threat models that include runtime compromise / insufficient RDP trust in scope).
- The compiled **key is exactly the same as QBOOT_SEALED's**: by default it is the repository-public "default public key", which must be replaced with your own public key before shipping. **The only difference between the two is the boot verification policy (SEALED fast path vs SECURE per-cycle verification), not the key** — the replacement procedure is identical for both, see §2.5.
- `QUILLA_PRODUCTION_BUILD` may be defined — this only makes the "still compiled with the default public key" compile warning more prominent and no longer triggers a compile failure; whether to replace is up to the customer.

### 2.4 Boot Modes (SECURE / SEALED / CHECK) and Their Relation to Variants

"Boot mode" is another orthogonal compile-time knob (`QUILLA_SB_BOOT_MODE`):

| Mode | Meaning | Ed25519 at boot | Boot time (PY32F040xB, M0+/48 MHz) |
|------|------|-------------------|----------|
| `SECURE` (=0) | Verify signature on every power cycle | **Yes**, once per cycle | ~3.5 s (measured: SHA-256 + Ed25519 + overhead; dominated by Ed25519) |
| `SEALED` (=1, default) | Verify signature at most once per image (seal fast path) | **No** (when the seal hits) | ~0.1 s (SHA-256, varying linearly with image size) |
| `CHECK` (=2) | Integrity only (no signature verification) | No | ~0.1 s (SHA-256, varying linearly with image size) |

> **PY32F040xB clock**: the device runs at 48 MHz (HSI 24 MHz + PLL×2, internal RC, no external crystal). With an ~18 KB QAPP image as the baseline, the SHA-256 portion scales linearly with the actually written APP image size. Available APP partitions: PY32F040xB ≈ 100 KB (`0x08007000`–`0x0801FFFF`), GD32F502VE ≈ 484 KB (`0x08007000`–`0x0807FFFF`); SHA-256 only computes the actually written `image_len` bytes and does not scan the entire region.
> Measured on hardware at 48 MHz: a full-Ed25519 boot (SHA-256 + Ed25519 + boot overhead) from reset to the APP banner is ≈ 3.5 s (down from ≈ 20 s at 8 MHz) — consistent with the ≈3.3 s Ed25519 + ≈0.1 s SHA-256 breakdown above. Ed25519 is the dominant, image-size-independent term; the SHA-256 figure is an estimate (~18–20 KB image, ≈180 KB/s). Captured as end-to-end reset→APP-banner timing on target.
> GD32F502VE (Cortex-M33 @ 160 MHz) on hardware: Ed25519 measured **under 500 ms** (~18 KB image); SHA-256 still estimated at ~0.03 s/18 KB (not separately measured).
>
> **Note**: all timing figures in this document are **development-process measurements**. They vary with software version and toolchain changes and are **not a delivery specification**; re-measure on your own build.

The factory boot modes of the three variants are: **QBOOT_CHECK = CHECK mode**; **QBOOT_SEALED = SEALED mode**; **QBOOT_SEC = SECURE mode**. If further adjustment is needed, change the corresponding variant's `QUILLA_SB_BOOT_MODE` to `SECURE`.

> **Mass-production hard prerequisites for QBOOT_SEALED / QBOOT_SEC to claim security (Non-Bypassability):**
> 1. **Lock the debug port (RDP / lock SWD):** if SWD is not locked, an attacker can rewrite the APP, manifest, and the seal in the META record, bypassing the seal comparison.
> 2. **Do not expose any unauthenticated flashing interface:** in shipped SEALED / SEC builds, no firmware entry point that can write Flash *without* signature verification may be exposed — this includes the debug port (SWD / JTAG), DFU / ROM bootloader, and any serial / USB / on-board command that writes Flash directly. Any such unauthenticated write-to-Flash path bypasses the Ed25519 gate outright, rendering the variant name and verification policy meaningless. The only legitimate signed-upgrade path is the QIAP serial upgrade (§4), which writes and commits the manifest only after the SIGN frame passes verification.
> 3. These requirements map to **NIST SP 800-193 §4.2.1.3 (Non-Bypassability)**: an authenticated update mechanism must prevent firmware from being bypassed or rolled back to an unauthorized state. **For QBOOT_SEALED / QBOOT_SEC to claim security, mass production must satisfy BOTH "lock the debug port" AND "do not expose any unauthenticated flashing interface"; locking SWD alone does not cover other entry points such as DFU or direct-write commands.**

### 2.5 Default Key Replacement (common to SEALED / SEC)

The SEALED and SEC variants compile in the **same key**: by default it is the "default key" shipped publicly with the repository. Regardless of which variant is chosen for mass production, it must be replaced with the customer's own key pair (the replacement procedure is exactly the same for both):

1. **Offline generation**: `tools/quilla_ed25519_sign.py keygen` → produces `private.hex` (private key) + `public.hex` (public key).
2. **Public key compiled into QBOOT**: compile your own public key via `-DQUILLA_SECURE_BOOT_PUBKEY={...}` or a product macro, replacing the default `QUILLA_QBOOT_DEFAULT_KEY` branch; the branch template is in the header comments of `os/services/quilla-qboot/quilla-qboot-pubkey.h` and in `tools/README.md`.
3. **Private key stays only at the signing station**, stored offline, **never entering firmware or the repository**; during flashing, point to it with `quilla_host_updater.py --key` (see `doc/quilla-host-updater-guide.md` §10.3).

> If the default public key is still compiled in, the header will emit a `#warning` (more prominent with `QUILLA_PRODUCTION_BUILD`) reminding replacement —
> **whether to replace is up to the customer**, but leaking the default key is equivalent to handing the private key to anyone (anyone can sign images accepted by the device),
> which constitutes a false security claim.

---

## 3. Compile-Time Selection (GNU / EIDE / Keil)

### 3.1 GNU / EIDE (`C_DEFS` in `user/<mcu>/EIDE/Makefile.qboot*`)

| Variant | Makefile | Key `C_DEFS` (excerpt) |
|------|----------|------------------------|
| QBOOT_CHECK | `Makefile.qboot` | `-DQUILLA_SB_BOOT_MODE=QUILLA_SB_BOOT_MODE_CHECK -DQUILLA_QBOOT_VARIANT_STR=\"CHECK\"` |
| QBOOT_SEALED | `Makefile.qboot.sealed` | `-DQUILLA_QBOOT_DEFAULT_KEY -DQUILLA_SB_BOOT_MODE=QUILLA_SB_BOOT_MODE_SEALED -DQUILLA_QBOOT_VARIANT_STR=\"SEALED\"` |
| QBOOT_SEC | `Makefile.qboot.sec` | `-DQUILLA_PRODUCTION_BUILD -DQUILLA_QBOOT_DEFAULT_KEY -DQUILLA_SB_BOOT_MODE=QUILLA_SB_BOOT_MODE_SECURE -DQUILLA_QBOOT_VARIANT_STR=\"SEC\"` (for key injection see the comments at the top of that makefile) |

In `quilla-qboot-secure.h`, `#ifndef QUILLA_SB_BOOT_MODE` defaults to `SEALED`. If undefined, it is SEALED (full verification when a key is present, degrading to integrity only when no key is present).

### 3.2 Keil (Define field in `user/<mcu>/MDK-ARM/QBOOT.uvprojx`)

Each `<mcu>` project contains three Targets: `QBOOT_CHECK` / `QBOOT_SEALED` / `QBOOT_SEC`, with Defines corresponding one-to-one with 3.1.
The `{...}` public key list containing commas gets split in the Keil Define, so the public key goes through the "product macro" layer (see `tools/README.md`),
and is not written directly in Define.

---

## 4. Serial Upgrade (manifest writing)

Regardless of variant, APP installation / recovery must go through the **QIAP serial upgrade** (do not use pure SWD direct flashing of the APP), because the manifest (integrity digest; SEALED/SEC also include a signature) is written by the upgrade path:

- **Keyed** (QBOOT_SEALED / QBOOT_SEC): END only ACKs; the manifest is written and committed only after the SIGN frame passes verification;
- **Keyless** (QBOOT_CHECK): the END frame directly writes the integrity manifest and commits.

For SEALED / SEC, this is also part of the §2.4 mass-production security prerequisite: beyond using QIAP, mass production must ensure no other direct-write-Flash entry point that bypasses the signature exists.

For the reference implementation and commands see `doc/quilla-host-updater-guide.md` §10.

---

## 5. Mapping to IEC 62443 Compliance Levels (developer self-assessment)

> **Disclaimer**: The following is **engineering advice** based on general IEC 62443-4-2 practice, not a certification conclusion.
> Which SL a specific product falls into, and whether "per-boot re-authentication" is required, depends on the **component security capability profile
> and threat model of the target component**, with the final ruling resting with the certification assessment body.
> Do not overstate claims on your own.

- **IEC 61508 SIL (functional safety) is orthogonal to this**: SIL2 concerns "corrupted images caused by random hardware faults / system errors
  being detected before they run", which is guaranteed by **integrity (SHA-256)**, provided by all three variants.
  **Authenticity / tamper resistance is a matter of IEC 62443 (security), not SIL.**

- **IEC 62443-4-2 (component security requirements)**: secure boot is typically a requirement item at SL2 and above.

| Variant / mode | Applicability tendency | Notes |
|------|----------|------|
| **QBOOT_CHECK** | Products with no 62443 secure-boot requirements, purely internal / development | Integrity only, **does not claim secure boot** |
| **QBOOT_SEALED (SEALED mode)** | **SL1 / SL2** most product profiles: install-time authentication + per-boot integrity + locked SWD (RDP) | Satisfies "verify authenticity/integrity before boot"; prerequisite is RDP enabled |
| **SECURE mode** | **SL3 / SL4** or stricter profiles: requires **re-verification on every boot**, or the threat model includes "runtime compromise / insufficient RDP trust" in scope | Provides per-cycle re-authentication, at the cost of ~3.5 s per power cycle (PY32F040xB, M0+/48 MHz, measured reset→APP banner; GD32F502VE under 500 ms (measured)) |

**Pragmatic advice**: the vast majority of end products can use **QBOOT_SEALED (SEALED mode + RDP on)** to meet SL1/SL2, with fast boot. Only when the target certification profile **explicitly requires re-verification on every boot** should you choose **QBOOT_SEC (SECURE mode)**, accepting the ~3.5 s per power cycle cost (PY32F040xB, M0+/48 MHz, measured; GD32F502VE under 500 ms, measured). Regardless of mode, **the public key must be compiled into QBOOT** (standard practice for secure boot), and **the private key stays only at the signing station, never entering firmware / the repository**. Product builds may add `QUILLA_PRODUCTION_BUILD`; if the repository default public key is still compiled in, the header emits a compile warning reminding replacement (no longer hard-blocked with `#error`).

---

## 6. Authenticity Responsibility Allocation When Integrated as a Slave MCU

**Scenario**: the Quilla MCU is attached as a slave component under an SoC that has already passed functional safety certification, with the SoC responsible for upgrading the MCU firmware over serial (or an on-board bus). Does the MCU still need to perform its own Ed25519 verification?

> **Standards boundary (critical)**: IEC 61508 / ISO 26262 are **functional safety** standards, concerned with accidental corruption from random / systematic faults, and **explicitly exclude security from scope** (pointing to IEC 62443). Ed25519 verification addresses **firmware authenticity (against deliberate tampering)**, which falls under **security (intentional fault)**. Therefore: **SoC compliance with 61508/26262 is almost irrelevant to "whether the MCU should verify Ed25519"** — SoC compliance provides no protection whatsoever against malicious firmware. What actually makes "firmware updates must be authentic" a hard requirement is **ISO/SAE 21434 + UN R155/R156 (automotive)** and **IEC 62443 (industrial control)**, not 61508/26262.

### 6.1 Whether the MCU Needs Independent Verification: Depends on Three Things

| Criterion | If satisfied → authenticity can be delegated to the SoC, and the MCU need not verify independently | If not satisfied → the MCU must verify independently |
|--------|------------------------------------------|--------------------------|
| ① Is the SoC→MCU upgrade link unreachable by adversaries | The link is secure if inside a sealed single board, with no external wiring / debug ports, and the SoC itself is not compromised | The link is physically accessible / has debug headers / the SoC may be compromised |
| ② Are there other firmware entry points on the MCU | Only this one SoC-mediated upgrade path, none other | Any of JTAG / SWD / USB / OTA entry points also exist |
| ③ Does the system need to meet 21434 / 62443 / R155 | A purely closed safety system with no cybersecurity compliance requirements | Automotive / industrial control; end-to-end authenticity is mandatory |

### 6.2 Is It Valid to "Allocate" Authenticity to the SoC?

It can be valid, but conditionally. In a system safety / cybersecurity argument, "MCU firmware authenticity" can be **allocated to the SoC**: the SoC completes verification before sending (or the signature is guaranteed on the SoC side), and the MCU only receives and flashes. IEC 61508 / ISO 26262 only care about the **outcome** (the running software is authorized and complete), and do not prescribe **which level the mechanism must reside at**; the standards also emphasize safety function independence — if the SoC is a certified trust anchor and the link is controlled, this delegation is accepted.

Prerequisites for validity: all three items in 6.1 must hold. If any one is not satisfied, "delegating to the SoC" degrades to **single-point trust** — once the SoC is bypassed, the entire authenticity argument fails.

### 6.3 Practical Advice (defense in depth)

Even if the SoC is architecturally the anchor, it is **not recommended** to remove Ed25519 from the MCU for this reason:

- MCU-side verification **costs almost nothing** (it only stores the public key, the private key is always offline, and it introduces no key exposure surface);
- It makes the MCU **independently self-protecting**: even if the SoC↔MCU link is later exposed, the SoC is compromised, or someone on the production line / repair bench connects directly to the MCU to flash, authenticity still holds;
- It also simplifies the safety / cybersecurity argument (each component verifies independently; independence is exactly what IEC 61508 emphasizes).

**Bottom line**: the MCU side should at least retain the **hash / CRC integrity check** (catching transfer corruption, a legitimate diagnostic means). Whether Ed25519 is mandatory depends on the three items in 6.1 and whether the system needs to pass cybersecurity standards — **not** on whether the SoC holds a 61508/26262 certificate.

> Distinction: if the product does not require secure boot in the first place, **QBOOT_CHECK** should be used (bypassing verification, retaining integrity only); this is an explicit declaration of "secure boot not needed"; it is a matter of a different nature from "MCU as a slave component, with authenticity handled by the SoC" — do not conflate the two.

---

## 7. Serial Observability (debugging)

The QBOOT boot serial port (`[qb]` tag, DEBUG ONLY, shipping prohibited) can distinguish two paths:

```
[qb] m-sig-in / m-sig-out    full Ed25519 verification window (~20 s)
[qb] m-sealed                took the seal fast path (Ed25519 skipped)
[qb] sb-sealed               seal fast path hit, skipping full verification
[qb] sb-out=00000000         QUILLA_SB_OK (full Ed25519 verification passed)
[qb] sb-out=00000002         QUILLA_SB_OK_INTEGRITY_ONLY (integrity only, CHECK mode)
[qb] sb-out=00000003         QUILLA_SB_OK_SEALED (passed via seal)
[qb] sb-out=FFFFFFFC         QUILLA_SB_ERR_SIG (verification failed / key mismatch)
[qb] sb-out=FFFFFFFD         QUILLA_SB_ERR_HASH (integrity mismatch, image tampered / transfer error)
[qb] sb-out=FFFFFFFE         QUILLA_SB_ERR_NO_MANIFEST (no manifest)
```

> To verify "the full verification chain itself" rather than just looking at the seal fast path: clear the META record seal record, or directly build once in `SECURE` mode and run it on hardware.
