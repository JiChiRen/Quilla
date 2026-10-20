# Quilla

<p align="center">
  <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-3--clause%20BSD-brightgreen.svg" alt="3-clause BSD license"></a>
  <a href="https://github.com/contiki-ng/contiki-ng"><img src="https://img.shields.io/badge/derived%20from-Contiki--NG-00599C.svg" alt="derived from Contiki-NG"></a>
  <img src="https://img.shields.io/badge/MCU-PY32F040xB%20(Cortex--M0%2B)%20%7C%20GD32F502VE%20(Cortex--M33)-orange.svg" alt="MCU: PY32F040xB (Cortex-M0+) | GD32F502VE (Cortex-M33)">
  <img src="https://img.shields.io/badge/secure%20boot-Ed25519%20(RFC%208032)-blueviolet.svg" alt="secure boot: Ed25519 (RFC 8032)">
  <img src="https://img.shields.io/badge/MISRA--C-2012-informational.svg" alt="MISRA C:2012">
</p>
<!-- Repo-dependent badges below: replace `jichiren/quilla` with the real GitHub OWNER/REPO once the repo exists. -->
<p align="center">
  <a href="https://github.com/jichiren/quilla/actions"><img src="https://github.com/jichiren/quilla/actions/workflows/build.yml/badge.svg" alt="build"></a>
  <a href="https://quilla.readthedocs.io"><img src="https://readthedocs.org/projects/quilla/badge/?version=latest" alt="documentation"></a>
  <img src="https://img.shields.io/github/v/release/jichiren/quilla.svg" alt="latest release">
</p>

**English** · [中文](README.zh-CN.md)

Quilla is a lightweight, **secure-boot–capable, OTA-ready** embedded operating system for resource-constrained microcontrollers, derived from [Contiki-NG](https://github.com/contiki-ng/contiki-ng). It keeps the simplicity of Contiki's protothread process model and adds a dual-partition bootloader, **Ed25519 verified secure boot**, and observability facilities — so product firmware can be developed quickly *and* ship with controlled security properties.

It targets cost-effective **Arm Cortex-M** microcontrollers that mainstream RTOSes rarely support out of the box. Two positioning profiles are ported today — the **ultra-low-power entry** profile (PY32F040xB, Cortex-M0+) and the **security-rich** profile (GD32F502VE, Cortex-M33), spanning the low end and the TrustZone-equipped mainstream of the Cortex-M class. Additional Arm Cortex-M parts and RISC-V cores require further porting.

<p align="center">
  <img src="doc/images/quilla-demo.gif" alt="Quilla demo: flashing a signed APP over UART — QBOOT verifies it with Ed25519 and boots the new image" width="640">
</p>

## Why Quilla

- **Secure boot & anti-rollback** — RFC 8032 Ed25519 image verification, SHA-256 integrity, and a version floor. Three build variants:
  - `QBOOT_CHECK` — SHA-256 integrity only (no key);
  - `QBOOT_SEALED` — seal fast-path (verified images skip full verification, ~0.6 s on PY32F040xB);
  - `QBOOT_SEC` — full Ed25519 verification on every boot.
- **Native OTA (QIAP)** — dual-partition **QBOOT + APP**; upgrade the APP over UART from a host tool, no dedicated programmer. Power-loss–resumable flag + manifest handshake.
- **Deterministic, no-heap kernel** — cooperative protothreads, a single rtimer IRQ, zero dynamic allocation (heap disabled by design).
- **Observability & safety facilities** — stack watermark, control-flow checkpoints, independent watchdog with timeout bounds, clock cross-check, unified status codes.
- **CMSIS-Pack layout, multi-MCU** — APP code is decoupled from the MCU behind a single public API; change silicon without touching the application layer.
- **Safety-oriented engineering (self-assessed, not certified)** — change control, locked toolchain, MISRA-C checks, reproducible builds, independent review. **No third-party certification has been submitted**; all safety-level statements here are internal assessment only.

## Supported hardware

| MCU | Core / Clock | Flash / SRAM | Status |
| --- | --- | --- | --- |
| PY32F040xB | Cortex-M0+ @ 8 MHz | 128 KB / 16 KB | On-target verified |
| GD32F502VE | Cortex-M33 @ 160 MHz | 512 KB / 64 KB | On-target verified (QBOOT_CHECK / QBOOT_SEC) |

## Quick start

Build the bootloader with the GNU `arm-none-eabi` toolchain (per-MCU makefiles under `user/<mcu>/EIDE/`, or the Keil projects under `user/<mcu>/MDK-ARM/`), then flash the APP over UART:

```bash
# Flash an APP image over the serial port (QBOOT must be present on target)
python tools/quilla_host_upgrader.py <COM> app.bin -b 115200

# Generate your own keypair and sign an image (replace the in-repo sample key before shipping)
python tools/quilla_ed25519_sign.py keygen
python tools/quilla_ed25519_sign.py sign --key private.hex app.bin
```

See [`doc/keil-qboot-app-build.md`](doc/keil-qboot-app-build.md) for full build steps and [`doc/quilla-host-upgrader-guide.md`](doc/quilla-host-upgrader-guide.md) for the upgrade tool.

## Documentation

Entry point and reader navigation: [`doc/Home.md`](doc/Home.md) (English) ·
[`doc/Home.zh-CN.md`](doc/Home.zh-CN.md) (中文).

- **Application developers** (architecture / protocols / secure boot / flows / API contract):
  `doc/qboot-app-architecture.md`, `doc/secure-boot.md`, `doc/boot-flow.md`, `doc/upgrade-flow.md`,
  `doc/quilla-host-upgrader-guide.md`, `doc/app-api-reference.md`
- **Porters / APP developers** (MCU adaptation / calling OS APIs):
  `doc/port-layout.md`, `doc/cmsis-layout.md`, `doc/keil-qboot-app-build.md`

## License & attribution

Quilla is derived from Contiki-NG. Unless explicitly stated otherwise, sources are distributed under the [3-clause BSD license](LICENSE.md), retaining the upstream copyright notice; derived files also carry `Copyright (c) 2026, JiChiRen Info. Tech.`

The original upstream README is preserved as [`CONTIKI-README.md`](CONTIKI-README.md) for Contiki-NG attribution and reference.
