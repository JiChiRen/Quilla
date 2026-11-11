**[English](riscv-ch32v305-build.md) · [中文](riscv-ch32v305-build.zh-CN.md)**

# Quilla on CH32V305VB (RISC-V) — EIDE / GNU GCC12 Build

- **Status**: Active — first bring-up (2026-10-21): builds end-to-end with GCC12, **not yet on-target verified**
- **Scope**: the `ch32v305rb` port only. The Cortex-M ports (PY32F040xB, GD32F502VE) build with Keil + GNU and are documented in `doc/keil-qboot-app-build.md`; **this MCU is EIDE / GNU-Makefile only — there is no Keil/MDK project and none is planned.**
- **Related**: `doc/port-layout.md` (multi-MCU layout), `doc/secure-boot.md`, `doc/upgrade-flow.md`.

---

## 1. Toolchain

| Item | Value |
|---|---|
| Compiler | **xPack GNU RISC-V Embedded GCC 12.2.0** (the "RISC-V Embedded GCC12" bundled with MounRiver Studio 2), prefix `riscv-wch-elf-` |
| Arch / ABI | `-march=rv32imac -mabi=ilp32` (soft-float; no FPU is used, matching the GD32 "no floating point" decision) |
| C library | newlib-nano (`-specs=nano.specs`) + syscall stubs (`-specs=nosys.specs`); the shell's `vsnprintf` otherwise pulls in the full printf |
| Startup | `-nostartfiles` — the WCH startup provides `_start` / `handle_reset`; without it the toolchain's own `crt0` `_start` collides |
| Interrupt attribute | `__attribute__((interrupt("WCH-Interrupt-fast")))` on every handler that overrides a WCH startup weak symbol |

The toolchain `bin/` directory is **not recorded in the repository**. Supply it per build either by putting it on `PATH` or by passing `GCC_PATH`:

```bash
# EIDE / command line
make -C user/ch32v305rb/EIDE -f Makefile              GCC_PATH=/path/to/RISC-V\ Embedded\ GCC12/bin
make -C user/ch32v305rb/EIDE -f Makefile.qboot        GCC_PATH=...
```

> The directory name contains a space ("RISC-V Embedded GCC12"); the Makefiles quote `$(CC)/$(AS)/$(CP)/$(SZ)` so this works. A `PATH` without spaces avoids the issue entirely.

### CI verification (what a green run does and does not mean)

GitHub Actions cannot run the WCH toolchain — it is not redistributable — so the
`firmware-riscv` job builds this port with a stock `riscv-none-elf` toolchain
(xPack GCC 13.2) after rewriting the single vendor-only construct in place:

```
__attribute__((interrupt("WCH-Interrupt-fast")))   ->   __attribute__((interrupt("machine")))
```

That job catches generic regressions in shared code — syntax, types, missing
symbols, linker-script drift. It is **not** evidence that the image boots: the
fast-interrupt entry path only exists with the WCH toolchain, and bring-up
remains open (§8). A green CI run says nothing about on-target status.

## 2. Device

CH32V305VB = WCH **CH32V30x_D8C** (RV32IMAC, QingKe V4F-class core with the WCH PFIC interrupt controller). It is a mainstream, general-purpose RISC-V offered at a Cortex-M price; its headline industrial features are **USBHS and industrial-temperature support at low cost**, and the pin- and peripheral-compatible CH32V307 in the same CH32V30x family adds a 1 G MAC + 10 M PHY for networked products. **128 KiB Flash linked at the `0x00000000` alias**, 32 KiB SRAM at `0x20000000`. The build define is `CH32V30x_D8C`.

> Unlike the Cortex-M ports, the boot images for this MCU live at the **`0x00000000`** Flash alias (the WCH startup/linker convention), **not** `0x08000000`. The whole QBOOT/APP scheme, `SOC_FLASH_END`, and the `-DQUILLA_QBOOT_*` macros are therefore `0x0`-based for this port. `0x08000000` remains a read alias of the same physical Flash.

> **Vendor SDK (WCH)**: a portion of this port — `Peripheral/` (device drivers), `Core/`, `Startup/`, `system_ch32v30x.*` and `Debug/` under `arch/cpu/riscv/ch32v305rb/` — is **WCH's vendor SDK**, kept with WCH's own upstream headers untouched (Copyright Nanjing Qinheng Microelectronics Co., Ltd.). **Usage scope / licensing (project ruling)**: the WCH source is used **only for WCH chips** (here the CH32V305VB); it carries no copyleft / viral licence, so including it does not affect Quilla's own BSD-3-Clause code. **Quilla only references these files and adds no copyright or licence claim of its own over them.** Verified: no Cortex-M (PY32 / GD32) build references any file under `arch/cpu/riscv/` or `arch/platform/riscv/`. Quilla's own code is BSD-3-Clause (see `LICENSE`).

## 3. Clock and time bases

- **System clock**: HSE 8 MHz → PLL → **96 MHz** (`SYSCLK_FREQ_96MHz_HSE` in `system_ch32v30x.c`); HCLK = 96 MHz, APB2 = 96 MHz, APB1 = 48 MHz. `SystemCoreClock` = 96 000 000.
- **rtimer**: TIM1 (APB2, 96 MHz) prescaled by 96 → **1 MHz** (1 µs/tick); 16-bit counter + software overflow count (same model as the GD32 TIMER0 port).
- **Software clock**: the WCH RV SysTick (`0xE000F000`) fires at `CLOCK_SECOND`; the 64-bit tick is derived from the rtimer, exactly as on GD32.
- **Watchdog**: IWDG clocked by LSI (~40 kHz), with `dev/qclk.c` measuring LSI against the rtimer (RTC divider) to narrow the timeout band — the CH32V305 equivalent of the GD32 FWDGT/IRC40K pair.

## 4. Partitions (0x0-based)

| Region | Address | Length | Notes |
|---|---|---|---|
| **QBOOT** | `0x00000000` | 24 KiB | bootloader; linker region limited to 24 K (overflow = link error) |
| **META** | `0x00006000` | 4 KiB | wear-levelled flag + secure-boot manifest (dual 2 KiB slots); neither image occupies it |
| **APP** | `0x00007000` | to `0x0001FFFF` | application; `SOC_FLASH_END = 0x0001FFFF` |
| **secver** | `0x00007400` | 8 B | anti-rollback firmware security version (`APP_START + 0x400`), read by QBOOT |

Flash erase/program granularity (page) is **256 B** (`-DQUILLA_QBOOT_PAGE_SIZE=256`).

## 5. Console

USART2, **TX = PA2 (GPIO16)**, **RX = PA3 (GPIO17)**, 115200 8-N-1. RX is DMA-driven: DMA1 channel 6 in CIRCULAR mode fills a 512-byte ring; the USART IDLE interrupt (frame delimiter) and the DMA transfer-complete interrupt only poll the shell process, and `shellport_rx_harvest()` (shell thread) copies new bytes out by reading the live DMA counter. TX is polling so the last byte is on the wire before the QBOOT branches to the APP. This is the same model as the GD32 shellport.

## 6. Images and build targets

```bash
make -f Makefile              # -> QAPP/QAPP.{elf,hex,bin}
make -f Makefile.qboot        # -> QBOOT_CHECK  (SHA-256 integrity only)
make -f Makefile.qboot.sealed # -> QBOOT_SEALED (default secure variant)
make -f Makefile.qboot.secured    # -> QBOOT_SECURED     (Ed25519 every boot)
```

`QUILLA_SB_BOOT_MODE` is set per Makefile (CHECK / SEALED / SECURED), matching the GD32 variants; the `QUILLA_QBOOT_VARIANT_STR` banner tag is derived from it in `quilla-qboot-secure.h`.

## 7. Measured sizes (development-process, not a delivery spec)

| Image | text | data | bss |
|---|---|---|---|
| QAPP | 18 453 | 192 | 10 120 |
| QBOOT_CHECK | 23 004 | 160 | 5 456 |
| QBOOT_SEALED | 23 108 | 160 | 5 456 |
| QBOOT_SECURED | 23 060 | 160 | 5 456 |

All three QBOOT variants fit the 24 KiB region (≈1.4 KiB headroom, comparable to the tight GD32 SEALED). Figures vary with toolchain/software version.

> **Compiler warnings**: a clean build is otherwise warning-free. The SEALED and SECURED variants additionally emit the intentional default public-key `#warning` (see `doc/gd32f502ve-eide-build.md` §9) — it reminds you to replace the repository default Ed25519 key with your own before shipping, and is expected.

## 8. Bring-up items that need on-hardware validation

These are the RISC-V-specific risks this first cut carries; none block compilation/link, all need silicon confirmation:

1. **Flash page = 256 B** — taken from the CH32V30x family; confirm against the CH32V305VB datasheet before relying on it.
2. **Flash writer runs from Flash** (not SRAM-resident like the GD32 port). QBOOT only erases/programs META/APP (never its own page) and the watchdog is APP-side, so IRQs are masked per operation. If a read-while-write hazard appears, migrate the erase/program cores to SRAM (WCH exposes `FLASH_ROM_ERASE` / `FLASH_ROM_WRITE` for this).
3. **SysTick periodic reload** — the handler clears CNTIF and relies on hardware auto-reload; the software clock is rtimer-derived so a timing slip only affects the poll rate, not `clock_time()`.
4. **RTC-on-LSI bring-up** for `qclk` (backup-domain access); a failed measurement degrades the watchdog to its compile-time assumption by design.
5. **`QUILLA_APP_SECVER_SELFCHECK` is compiled out** on this port — the APP-side self-check uses Cortex-M inline asm (`cpsid i`) with no RISC-V equivalent. It is obfuscation only; the real anti-rollback gate is QBOOT's signature + floor, which is unaffected.
