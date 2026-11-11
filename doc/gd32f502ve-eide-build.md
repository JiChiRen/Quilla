**[English](gd32f502ve-eide-build.md) · [中文](gd32f502ve-eide-build.zh-CN.md)**

# Quilla on GD32F502VE — EIDE / GNU Build Guide

- **Status**: Active — on-target verified
- **Scope**: building the QBOOT bootloader and QAPP application for the **GD32F502VE** (Cortex-M33) with the **GNU Arm Embedded toolchain** through the EIDE project under `user/gd32f502ve/EIDE/`. For the Keil/MDK projects see `doc/keil-qboot-app-build.md`; for the multi-MCU layout see `doc/port-layout.md`.
- **Related**: `doc/secure-boot.md` (CHECK / SEALED / SECURED variants and keys), `doc/upgrade-flow.md` (QIAP host upgrade).

---

## 1. Toolchain

| Item | Value |
|---|---|
| Compiler | **Arm GNU Toolchain** `arm-none-eabi-gcc` (this project is built and verified with **13.2.1**; any recent 12/13 series works) |
| Arch flags | `-mcpu=cortex-m33 -mthumb`, soft-float (no `-mfpu`/`-mfloat-abi`; the bundled startup uses `.fpu softvfp`) |
| C library | newlib-nano (`-specs=nano.specs`); syscall stubs come from `-lnosys` |
| Build system | GNU Make via the EIDE project (`user/gd32f502ve/EIDE/`); the same Makefiles also run standalone |

Put the toolchain `bin/` directory on `PATH`, **or** pass `GCC_PATH` per build (the absolute path is not recorded in the repo):

```bash
cd user/gd32f502ve/EIDE
make -f Makefile            GCC_PATH=/path/to/gcc-arm-none-eabi/bin   # QAPP
make -f Makefile.qboot      GCC_PATH=...                               # QBOOT_CHECK
```

On Debian/Ubuntu the toolchain is the `gcc-arm-none-eabi` package; on macOS `brew install --cask gcc-arm-embedded`; on Windows the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) installer provides it.

## 2. Device and clock

GD32F502VE — Arm Cortex-M33, **512 KiB Flash @ `0x08000000`**, 64 KiB SRAM @ `0x20000000`. The build define is `GD32F502` (with `GD32F50X`). System clock: **8 MHz HXTAL → PLL → 160 MHz** (`__SYSTEM_CLOCK_160M_PLL_HXTAL`); `SystemCoreClock` = 160 MHz.

## 3. Flash / partition map

All QBOOT/APP addresses are `0x0800`-based. `SOC_FLASH_END` in `arch/cpu/arm/gd32f502ve/soc.h` is the single source for the APP end; erase/program page is **2 KiB** (bank0).

| Region | Address | Length | Notes |
|---|---|---|---|
| **QBOOT** | `0x08000000` | 24 KiB | bootloader; linker region limited to 24 K (overflow = link error) |
| **META** | `0x08006000` | 4 KiB | wear-levelled flag + secure-boot manifest (dual 2 KiB slots); neither image occupies it |
| **APP** | `0x08007000` | to `0x08007FFFF` | application; `SOC_FLASH_END = 0x08007FFFF` |
| **secver** | `0x08007400` | 8 B | anti-rollback firmware security version (`APP_START + 0x400`), read by QBOOT |

- **QAPP vector relocation**: `SCB->VTOR = 0x08007000` is set in `main()` (`QAPP_BUILD`), so linking the APP at `0x08007000` is valid; `quilla_qboot_hal_jump_to_app()` sets VTOR again before the branch.
- **QBOOT jump to APP**: `quilla_qboot_hal_jump_to_app()` in `arch/cpu/arm/gd32f502ve/quilla-qboot-hal.c` reads the APP initial SP / reset vector, sets `SCB->VTOR`, loads MSP and branches.

## 4. Console

USART0, **TX = PB6**, **RX = PB7**, `GPIO_AF_0`, 115200 8-N-1. RX is DMA-driven: DMA0 channel 4 (DMAMUX request `DMA_REQUEST_USART0_RX`) in CIRCULAR mode fills a 512-byte ring; the USART IDLE interrupt (frame delimiter) and DMA half/full-transfer interrupts only poll the shell process, and `shellport_rx_harvest()` copies new bytes out by reading the live DMA counter. TX is polling so the last byte is on the wire before the QBOOT hands over to the APP.

## 5. Build targets

```bash
make -f Makefile              # -> QAPP/QAPP.{elf,hex,bin}
make -f Makefile.qboot        # -> QBOOT_CHECK   (SHA-256 integrity only)
make -f Makefile.qboot.sealed # -> QBOOT_SEALED  (default secure variant)
make -f Makefile.qboot.secured    # -> QBOOT_SECURED     (Ed25519 on every boot, production)
```

The three QBOOT variants differ only in `C_DEFS` (`QUILLA_SB_BOOT_MODE` plus the key define; SECURED also sets `QUILLA_PRODUCTION_BUILD`). The banner tag `QUILLA_QBOOT_VARIANT_STR` is derived from the mode in `quilla-qboot-secure.h`, not passed with `-D`. SEALED and SECURED ship with the **same default public key** — replace it with your own before production (see `doc/secure-boot.md` §3.1 and the header comments of `quilla-qboot-pubkey.h`).

## 6. Outputs and flashing

Each target produces `.elf`, `.hex` and `.bin` under its build directory. Flash QBOOT to `0x08000000` and the APP to `0x08007000` (the host updater reads the exact APP range from the device at runtime via the `pinfo` shell command, so it needs no per-MCU hard-coding). Over UART the APP is upgraded with `tools/quilla_host_updater.py` — see `doc/quilla-host-updater-guide.md`.

> When flashing QBOOT and APP separately with a debugger, **erase by sector, never a full-chip erase** — a full-chip erase also wipes the META region (flag + manifest).

## 7. Using the EIDE IDE

Open `user/gd32f502ve/EIDE/` as an EIDE project (`.eide/eide.json`). EIDE builds the **QAPP** target only (the PY32F040xB/GD32 convention); the QBOOT and its variants are built with `make -f Makefile.qboot[.sealed|.secured]` from a terminal, or with the Keil `QBOOT.uvprojx`. The build tasks under `.vscode/tasks.json` mirror the Makefiles.

## 8. Measured sizes (development-process, not a delivery spec)

Measured with GNU Arm Embedded `arm-none-eabi-gcc` **13.2.1** (see §1). The QBOOT region is hard-limited to 24 KiB by the linker script; any overflow is a link error, not a runtime fault.

| Image | text | data | bss |
|---|---|---|---|
| QAPP | 17 908 | 152 | 11 496 |
| QBOOT_CHECK | 21 236 | 116 | 18 344 |
| QBOOT_SEALED | 21 336 | 116 | 18 344 |
| QBOOT_SECURED | 21 288 | 116 | 18 344 |

> Sizes vary with toolchain and software version. Keil (armclang) produces a smaller QBOOT than GNU (see `doc/keil-qboot-app-build.md`); the figures above are GNU only. The QBOOT `bss` figure includes the linker-reserved 8 KiB stack and heap; the actual uninitialized data is much smaller.

## 9. Compiler warnings (expected, benign)

A clean build prints a small number of warnings. They are expected and safe to ignore:

- **Default public-key `#warning`** (SEALED / SECURED only): *"QBOOT: repository default public key in use - provision your own keypair before shipment"* (SECURED with `QUILLA_PRODUCTION_BUILD` uses a production-worded variant) — emitted once per QBOOT build from `quilla-qboot-secure.c`, not from the header. An intentional reminder. The repository ships a real, working Ed25519 keypair so a fresh clone can build and boot immediately; its private key is publicly known (`tools/_keytest/private.hex`), so **you must provision your own key before shipping**. See `doc/secure-boot.md` §3.1.
- **newlib `_write` / `_read` / `_close` / … "not implemented"**: the `nosys` syscall stubs; the firmware does not use a filesystem.
- **`-Wmaybe-uninitialized` in the GigaDevice vendor RCU driver**: a vendor-code warning, not in Quilla code.

## 10. Third-party components

The peripheral drivers and startup under `arch/cpu/arm/vendor/GigaDevice/` and `arch/cpu/arm/CMSIS/` are vendor-provided (GigaDevice / Arm) and carry their own vendor copyright headers; they are redistributed under the repository's third-party terms. Quilla's own code is BSD-3-Clause (see `LICENSE`).
