**[English](keil-qboot-app-build.md) · [中文](keil-qboot-app-build.zh-CN.md)**

# Quilla QBOOT / APP Dual-System — Keil Project Build File Organization and Flash Address Mapping

> Applicable chips: PY32F040xB (Cortex-M0+) and GD32F502VE (Cortex-M33), each with its own `user/<mcu>/MDK-ARM/` projects; partition addresses are defined by the constants in `quilla-qboot-hal.h` (PY32F040xB values shown below).
> For the companion design see `doc/qboot-app-architecture.md` (QIAP protocol, interaction flags, and boot flow are detailed there).
> This document focuses on: **which files QBOOT / APP each compile in the Keil project**, and **the compilation and mapping of the Flash addresses used for system flashing**.

---

## 1. Project Structure

Under `user/py32f040xb/MDK-ARM/` there are **two independent Keil project files**, each with its own Target:

| Project file | Target name | Image | Load region (IROM1) | Macros | Output |
| --- | --- | --- | --- | --- | --- |
| `QAPP.uvprojx` | `QAPP` | Application firmware QAPP | `0x08007000 / 0x19000` | `USE_HAL_DRIVER,PY32F040xB,QAPP_BUILD` | `QAPP.axf/.hex` (`.\Output\`) |
| `QBOOT.uvprojx` | `QBOOT_CHECK` / `QBOOT_SEALED` / `QBOOT_SEC` | bootloader | `0x08000000 / 0x6000` | See §7 (varies by variant) | `QBOOT_CHECK.axf/.hex`, etc. (`.\QBOOT_Output\`) |

Both use `IRAM1 = 0x20000000 / 0x4000` for RAM (QBOOT only uses less; RAM is not split further).
Both projects enable `Use Memory Layout from Target Dialog` (`umfTarg=1`); the actual layout is determined by IROM1/IRAM1 on the Target page, and the `.sct` is an automatic artifact.

> Backup: `QAPP.uvprojx.bak`, `QBOOT.uvprojx.bak` (plus an older `Project.uvprojx.bak`). Restore them if Keil fails to open properly.

---

## 2. Memory / Flash Address Mapping (both compilation and flashing follow this)

PY32F040xB: Flash 128 KB = `0x08000000 – 0x0801FFFF`, page size 256 B (512 pages total). GD32F502VE: Flash 512 KB = `0x08000000 – 0x0807FFFF`, erase page 2 KB. Both follow the same three-region layout below; only the APP end address and page size differ.

| Region | Link / flash address | Length | Description |
| --- | --- | --- | --- |
| **QBOOT** | `0x08000000` | 24KB (pages 0–95) | bootloader; load region limited to 24K, exceeding it is a link error |
| **APP** | `0x08007000` | `0x19000` (102,400 bytes ≈ 95.5KB, pages 112–511) | application; `APP_START` |
| **META region** | `0x08006000` (between QBOOT and APP) | 4 KB (dual 2 KiB slots) | Wear-levelled QBOOT/APP flag + secure-boot manifest log; **neither image may occupy it**, appended at runtime by code (never whole-page-erased by the flashing tool) |

> The APP load region `0x08007000`–`0x0801FFFF` lies after the META region (`0x08006000`–`0x08006FFF`), so the META region will not be mistakenly linked into the APP image.

- **QAPP vector table relocation**: in `SystemInit()` of `system_<device>.c`, the `QAPP_BUILD` macro sets `VECT_TAB_OFFSET` to `0x7000U`, which is what makes linking QAPP at `0x08007000` valid. QBOOT is at `0x08000000`, so no VTOR change is needed.
- **QBOOT jump to APP**: `quilla_qboot_hal_jump_to_app()` in `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` also sets `SCB->VTOR = APP_START` before jumping — a double safeguard.
- **GNU/EIDE side**: QBOOT uses `py32f040xb_qboot.ld` (`ORIGIN=0x08000000, LENGTH=24K`), APP uses `py32f040xb_app.ld` (`ORIGIN=0x08007000, LENGTH=0x19000`), consistent with Keil's IROM1. GD32F502VE uses its equivalent linker scripts.

---

## 3. Keil Target Options Quick Reference (verify / replicate against this)

`Project → Options for Target`:

| Page | Item | QAPP (`QAPP.uvprojx`) | QBOOT (`QBOOT.uvprojx`) |
| --- | --- | --- | --- |
| **Target** | Device | `PY32F040xB` | Same |
| | IROM1 | `Start 0x08007000` `Size 0x19000` | `Start 0x08000000` `Size 0x6000` |
| | IRAM1 | `Start 0x20000000` `Size 0x5000` | Same |
| **C/C++** | Define | `USE_HAL_DRIVER,PY32F040xB,QAPP_BUILD` | `USE_HAL_DRIVER,PY32F040xB,QBOOT_BUILD` (per variant, also add `QUILLA_SB_BOOT_MODE` / `QUILLA_QBOOT_DEFAULT_KEY` / `QUILLA_PRODUCTION_BUILD` / `QUILLA_QBOOT_VARIANT_STR`, see §7) |
| | Optimization | Keep original setting | `-O2` (project default, microlib 15.4 KB / 24 KB); temporarily use `-O1` when size is tight |
| | One ELF Section per Function | ✓ | ✓ (critical: otherwise the UART/Flash HAL enters the image in full, and it will not fit in 24K) |
| **Linker** | Use Memory Layout from Target Dialog | ✓ | ✓ |
| | Remove Unused Sections | ✓ | ✓ |
| **Output** | Name of Executable | `QAPP` | `QBOOT_CHECK` / `QBOOT_SEALED` / `QBOOT_SEC` (by selected Target) |
| | Create HEX File | ✓ | ✓ |

> **"One ELF Section per Function + Remove Unused Sections" is a hard requirement for QBOOT**: the `py32f040_hal_uart.c` source file is about 120KB; without section removal it cannot possibly fit into 24KB.

The QBOOT partition budget is **24 KB** (`0x08000000`–`0x08005FFF` = 24,576 B); partition boundaries only need page alignment, not sector alignment.

> **microlib is a hard prerequisite**: linking the same source against the standard libc leaves only 3% remaining at `-O2`. Target → `Use MicroLIB`
> must be checked, otherwise `-O1` and above immediately fail with `L6406E: No space in Execution Region ... ER_IROM1`.

> **Link-Time Optimization (LTO) is enabled or disabled by the linked image size**: Whether LTO (armclang `--lto`, the `v6Lto` field in uvprojx) is on depends on the final QBOOT image size, not on a fixed setting.
> - **GD32F502VE (Cortex-M33)**: armclang's LTO inlining cost model underestimates 32-bit Thumb-2, aggressively inlining the whole Ed25519 verification chain (e.g. `ge_add`, `verify_descriptor`). This inflates the cryptographic `QBOOT_SEALED` / `QBOOT_SEC` variants by ~47%, overflowing the 24 KB partition; disable LTO (`v6Lto = 0`) with `-O1`/`-O2` to fit within 24 KB. The non-cryptographic `QBOOT_CHECK` is unaffected and may keep LTO.
> - **PY32F040xB (Cortex-M0+)**: the GNU toolchain does not use `-flto`; under Keil, LTO has limited inlining impact and can normally stay enabled.
> - **Decision order**: build with the default configuration first; if the linker reports `L6406E` (insufficient space), adjust LTO and the optimization level before dropping non-essential commands (e.g. `stack`) or temporarily widening the partition.

**If the actual QBOOT size approaches or exceeds 24KB, handle it in the following order — do not change the partition first:**

1. **Lower the optimization level to `-O1`** (`-O1` leaves plenty of headroom in a 24 KB partition; `-O0` will most likely link as well).
2. **If `L6406E` still occurs**: add `-DKERN_CONF_STACK_SHELL=0` to the QBOOT target alone, dropping the `stack`
   command — saves about **484 B** (19,772 → 19,288 B).
3. **Only if it is still not enough, temporarily widen the partition** (after the change you **must change it back**):
   - QBOOT `IROM1 Size 0x6000` → `0x8000` (32 KB, the next 8 KB boundary); APP `IROM1 Start` correspondingly `0x08007000` → `0x08008000`;
   - **The new boundary must still be page-aligned**: `quilla-qboot-hal.h` has a compile-time guard (`QUILLA_QBOOT_APP_OFFSET % PAGE_SIZE == 0`); a non-page-aligned value will directly trigger `#error`;
   - Check item by item against the §7 registration checklist: the two `.ld` files, the two uvprojx files, the generated scatter, the 3 `baseAddr` entries in `eide.json`,
     and `Makefile.qboot`. `APP_END` is the Flash end address and comes from `SOC_FLASH_END` in `arch/cpu/arm/<mcu>/soc.h`; when changing platforms edit soc.h rather than the build command;
   - The host tool (`tools/quilla_host_upgrader.py`) no longer hardcodes the APP end: it queries the device's `pinfo` shell command for `APP_START`/`APP_END` before transferring, so it follows the new boundary automatically.

---

## 4. QBOOT Build File Organization (minimal set: 30 files)

QBOOT includes only the necessary files, excluding the full Contiki network stack, display/SPI-DMA, ctimer/stimer/mutex/autostart/rtimer, LPTIM low-power, IWDG, watchdog, BSP, etc.

| Keil group | Files |
| --- | --- |
| `CMSIS/MSP` (2) | `startup_py32f040xx.s`, `system_py32f040.c` |
| `CMSIS/PY32F040_HAL_Driver` (9) | `py32f040_hal.c`, `hal_rcc.c`, `hal_rcc_ex.c`, `hal_gpio.c`, `hal_dma.c`, `hal_pwr.c`, `hal_uart.c`, `hal_cortex.c`, **`hal_flash.c`** |
| `os/sys` (5) | `atomic.c`, `process.c`, `kern-guard.c`, `etimer.c`, `timer.c` |
| `os/lib` (6) | `assert.c`, `crc16.c`, `list.c`, `memb.c`, `ringbuf.c`, `ringbufindex.c` |
| `os/services` (1) | `quilla-shell.c` |
| `mounter` (4) | `shellport.c`, `clock.c`, `int-master.c`, **`quilla-qboot-hal.c`** |
| `quilla-qboot` (4) | **`quilla-qboot-flag.c`**, **`quilla-qboot-meta.c`**, **`quilla-qiap.c`**, **`qboot-main.c`** |

> `qboot-main.c` (QBOOT entry, contains `main()`, now shared across all ports at `os/services/quilla-qboot/qboot-main.c`) is a hardware-independent OS module like the other QBOOT services; `quilla-qboot-hal.c` (hardware implementation) remains in the arch layer; `quilla-qboot-flag.c` (thin delegation to meta), `quilla-qboot-meta.c` (wear-levelled META store), and `quilla-qiap.c` are hardware-independent OS modules, see §4.1.

**Explicitly NOT compiled into QBOOT**: `main.c`, `py32f040_hal_msp.c`, `py32f040_it.c`, `hal_lptim.c`, `hal_iwdg.c`, `py32f040xx_Start_Kit.c`, `soc.c`, `platform.c`, `lpm.c`, `rtimer*.c`, `dbg.c`, `watchdog.c`, network stack, display/SPI-DMA.

Rationale and closure basis (checked symbol by symbol):
- `shellport.c` provides its own `USART2_IRQHandler` / `DMA1_Channel4_5_6_7_IRQHandler`, so `py32f040_it.c` is **not needed**; it also provides its own `HAL_UART_MspInit`/`HAL_DMA_MspInit`, so `py32f040_hal_msp.c` is **not needed**.
- The `SysTick_Handler` in `py32f040_it.c` is disabled by `#if 0`; `clock.c` is the sole provider.
- `qboot-main.c` (now `os/services/quilla-qboot/qboot-main.c`) calls `clock_init()` directly rather than `platform_init_stage_one()`, thus not pulling in `soc_init → rtimer_init → rtimer_arch_init → HAL_LPTIM` nor `Error_Handler()` (the latter is provided only by `main.c`, which QBOOT does not include).
- `QBOOT_BUILD` is used to isolate `clock_update_ticks()` in `clock.c`'s `SysTick_Handler` (that path would pull in `RTIMER_NOW()` → rtimer/lpm). QBOOT keeps only `HAL_IncTick()`.
- `clock.c` unconditionally calls `etimer_pending()`/`etimer_request_poll()` → `etimer.c` + `timer.c` are mandatory; `process.c` calls `kern_guard_*` → `kern-guard.c` is mandatory.
- `lpm_exit()` is an empty macro when `LPM_CONF_ENABLE=0` (`contiki-conf.h`), so `lpm.c` is not needed.
- **The bootloader has zero libc dependency**: `quilla-qiap.c` uses its own `key_match()` to compare keys, not `memcmp`.
- `int-master.c` is mandatory: `atomic.c` calls `int_master_*` via `sys/critical.h`; the implementation is in `arch/cpu/arm/int-master.c` (not `os/sys/`, which holds only headers).

### 4.1 OS / Hardware Layering (quilla-qboot)

The boot-related modules are split as "OS declares the interface, platform implements it", the same pattern as `int-master.h`/`clock.h`:

| Layer | File | Contents |
| --- | --- | --- |
| OS (hardware-independent) | `os/services/quilla-qboot/quilla-qboot-meta.{h,c}` | Wear-levelled META store (dual-slot append-only log) |
| OS (hardware-independent) | `os/services/quilla-qboot/quilla-qboot-flag.{h,c}` | Interaction flag read/write (delegates to the META store) |
| OS (hardware-independent) | `os/services/quilla-qboot/quilla-qiap.{h,c}` | QIAP protocol state machine (pure byte stream + CRC) |
| OS (hardware-independent) | `os/services/quilla-qboot/quilla-app-qboot.{h,c}` | APP-side `swqboot` command |
| OS (interface declaration) | `os/services/quilla-qboot/quilla-qboot-hal.h` | Interfaces the platform must implement + partition layout constants |
| Platform implementation | `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` | Per-MCU Flash erase/write/read, system reset, VTOR jump, serial transmit |
| Platform entry | `os/services/quilla-qboot/qboot-main.c` (shared) | `main()`, serial ring, shell loop, boot policy; calls per-MCU `clock_init()` / `shellport_init()` / `quilla_qboot_hal_app_image_valid()` |

The OS-side modules **do not include any HAL/CMSIS headers**; all machine operations go through `quilla_qboot_hal_*()`:

```
quilla_qboot_hal_flash_read(addr, dst, len)
quilla_qboot_hal_flash_write_page(page_base, src)    /* full-page erase + write */
quilla_qboot_hal_reboot()                            /* disable IRQs + system reset */
quilla_qboot_hal_jump_to_app(app_start)              /* VTOR + MSP + jump */
quilla_qboot_hal_transmit(data, len)                 /* upgrade-link transmit (via shellport under QBOOT) */
quilla_qboot_hal_int_disable() / _int_restore(st)    /* forward int_master_* */
```

In the partition layout, `APP_START`/`PAGE_SIZE`/`RAM_*` are given PY32F040xB default values via `#ifndef` in `quilla-qboot-hal.h`; override them with `-D` when changing platforms. Among them, **`APP_END` (Flash end address, upper bound of the APP region) is a pure MCU hardware property and is no longer passed in via the build command `-D`**; it is instead defined in the `SOC_FLASH_END` macro in `arch/cpu/arm/<mcu>/soc.h` — `quilla-qboot-hal.h` takes it directly. Porting to a new MCU only requires adding `SOC_FLASH_END` to the corresponding `soc.h`; no build-command changes and no OS header changes are needed.

---

## 5. APP Build File Organization (original full set + new files)

APP = all files of the original project (full Contiki, display, SPI DMA, application, etc.) **plus the new additions**:

| New file | Target group | Purpose |
| --- | --- | --- |
| `arch/cpu/arm/vendor/Puya/PY32F040_HAL_Driver/Src/py32f040_hal_flash.c` | `CMSIS/PY32F040_HAL_Driver` | HAL FLASH needed to write the interaction flag |
| `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` | `mounter` | Interaction flag erase/write, reset (the APP side needs only these two) |
| `os/services/quilla-qboot/quilla-qboot-meta.c` | `quilla-qboot` | Wear-levelled META store (flag/floor/seal/manifest, shared by QBOOT/APP) |
| `os/services/quilla-qboot/quilla-qboot-flag.c` | `quilla-qboot` (new group) | Read/write interaction flags (delegates to META; shared by QBOOT/APP) |
| `os/services/quilla-qboot/quilla-app-qboot.c` | `quilla-qboot` | APP-side `swqboot` command (APP only, OS layer) |

APP-side source integration (`user/<mcu>/Src/`):
- `main.c` calls `shell_set_partition_tag("app1")` and `quilla_app_qboot_init()` after `shellport_init()` (the header uses the source-relative path `../../../os/services/quilla-qboot/quilla-app-qboot.h` to ensure all three build systems can resolve it);
- `system_<device>.c` sets `VECT_TAB_OFFSET` to `0x7000U` under `QAPP_BUILD`.

> QBOOT-exclusive files `qboot-main.c` (`os/services/quilla-qboot`) / `quilla-qiap.c` are **not included in the APP build**. `quilla-qboot-hal.c` is compiled on both sides, but its `quilla_qboot_hal_transmit()` is guarded by `QBOOT_BUILD` and is an empty implementation under APP.

---

## 6. System Flashing (Flash Programming) Address Mapping and Procedure

### 6.1 Flash Boundaries of the Three Regions
- **QBOOT.hex**: base address `0x08000000`, occupies the first 24KB. Flash only once (unless upgrading the bootloader).
- **APP.hex**: base address `0x08007000`. Routine application updates flash only this one.
- **META region `0x08006000`–`0x08006FFF` (4 KB)**: appended at runtime by code (`quilla_qboot_flag_set` / the QIAP manifest write). **The flashing tool must never chip-erase/overwrite it**, otherwise the flag / manifest state is cleared and a full re-flash is required. Keil by default only writes the blocks covered by the HEX and will not touch the META region.

### 6.2 Recommended Flashing Procedure
1. First mass production / recovery: first flash `QBOOT.hex` (`0x08000000`), then `APP.hex` (`0x08007000`). Different base addresses, no overlap, so the order does not matter.
2. Routine application upgrade: flash only `APP.hex`.
3. Field upgrade: APP running → `swqboot` command → flag written to `qboot` → reset → QBOOT enters the **interactive shell** (not a direct upgrade); the host first sends the `qiap` command to switch to serial upgrade, then sends ENTER/DATA/END frames (protocol: see §8 of the architecture doc); only a valid ENTER frame writes the flag to `qiap` (a mid-way power loss can automatically re-enter upgrade), a successful END writes back `app1` and resets into the new APP. If you enter QBOOT by mistake, `swapp` in the shell forces a return to APP.
4. Merged flashing: use `srec_cat` or similar to merge the two HEX files by their respective base addresses, then flash the whole chip; when merging, **do not include the META region** (`0x08006000`–`0x08006FFF`).

### 6.3 Misflash Protection (optional)
- Option Byte write protection (WRP) protects the QBOOT region (pages 0–95), preventing a QBOOT bug from destroying the bootloader itself.

---

## 7. Registration Status of the Three Build Entry Points ("iron rule" check)

| New `.c` | Keil | EIDE `.eide/eide.json` | GNU `Makefile` |
| --- | --- | --- | --- |
| `os/services/quilla-qboot/qboot-main.c` (QBOOT-only, shared) | `QBOOT.uvprojx` ✓ | (EIDE: `make -f Makefile.qboot`) | `Makefile.qboot` ✓ |
| `arch/cpu/arm/py32f040xb/quilla-qboot-hal.c` (shared) | `QBOOT.uvprojx` ✓ + `QAPP.uvprojx` ✓ | `QAPP` (eide.json) ✓ | `Makefile`(QAPP) ✓ + `Makefile.qboot` ✓ |
| `os/services/quilla-qboot/quilla-qboot-flag.c` (shared) | `QBOOT.uvprojx` ✓ + `QAPP.uvprojx` ✓ | `QAPP` (eide.json) ✓ | `Makefile`(QAPP) ✓ + `Makefile.qboot` ✓ |
| `os/services/quilla-qboot/quilla-qboot-meta.c` (shared) | `QBOOT.uvprojx` ✓ + `QAPP.uvprojx` ✓ | `QAPP` (eide.json) ✓ | `Makefile`(QAPP) ✓ + `Makefile.qboot` ✓ |
| `os/services/quilla-qboot/quilla-qiap.c` (QBOOT-only) | `QBOOT.uvprojx` ✓ | — | `Makefile.qboot` ✓ |
| `os/services/quilla-qboot/quilla-app-qboot.c` (QAPP-only) | `QAPP.uvprojx` ✓ | `QAPP` (eide.json) ✓ | `Makefile`(QAPP) ✓ |
| `py32f040_hal_flash.c` (needed by both) | `QBOOT.uvprojx` ✓ + `QAPP.uvprojx` ✓ | `QAPP` (eide.json) ✓ | `Makefile`(QAPP) ✓ + `Makefile.qboot` ✓ |

> The boot-related modules live in `os/services/quilla-qboot/`; all three build entry points (Keil / EIDE / GNU) are registered (including `Makefile.qboot`'s `-I../../../os/services/quilla-qboot`).

> **The GNU side has three secure-boot variants** (1:1 with the three Keil Targets):
>
> | Variant | Makefile | Output | Description |
> |------|----------|------|------|
> | `QBOOT_CHECK` | `Makefile.qboot` | `./QBOOT_CHECK/QBOOT_CHECK.{elf,hex,bin}` | Integrity only, no key (`QUILLA_SB_BOOT_MODE=CHECK`) |
> | `QBOOT_SEALED` | `Makefile.qboot.sealed` | `./QBOOT_SEALED/QBOOT_SEALED.{elf,hex,bin}` | Development state, default key (`QUILLA_QBOOT_DEFAULT_KEY` + `SEALED`) |
> | `QBOOT_SEC` | `Makefile.qboot.sec` | `./QBOOT_SEC/QBOOT_SEC.{elf,hex,bin}` | Production state, SECURE mode verifies signature on every power-on (`QUILLA_QBOOT_DEFAULT_KEY` + `SECURE` + `QUILLA_PRODUCTION_BUILD`); same key as SEALED, see the makefile header comments for replacement |
>
> The three variants differ only in `C_DEFS` (macro list: see `doc/secure-boot.md` §3.1). SEALED and SEC compile in the **same default key**; before shipment both must replace it with their own public key (via `-DQUILLA_SECURE_BOOT_PUBKEY` or the product-macro layer — the branch template is in the header comments of `quilla-qboot-pubkey.h`; whether to replace is the customer's decision, and while still using the default key the header emits a compile warning); the `EIDE/` directories of both MCUs already provide these three makefiles.

Macros and linking: the two Keil projects use `QAPP_BUILD`/`QBOOT_BUILD` respectively and set the corresponding IROM1; EIDE adds `QAPP_BUILD` + `py32f040xb_app.ld`; `Makefile` adds `-DQAPP_BUILD` + `py32f040xb_app.ld`; `Makefile.qboot` uses `-DQBOOT_BUILD` + `py32f040xb_qboot.ld`.

---

## 8. Notes

- **Flash page programming granularity = 256B**: `FLASH_Program_Page()` writes 64 words; `HAL_FLASH_Program()` enforces `Address % FLASH_PAGE_SIZE == 0` (otherwise it returns `HAL_ERROR`). The vendor header comment "Program 128bytes" is outdated text; go by `FLASH_PAGE_SIZE = 0x100`. The code buffers a full 256B page + aligns the page base address (`page_base &= ~(FLASH_PAGE_SIZE-1)`).
- **Erase must explicitly set `TypeErase` and `NbPages`**: the loop condition of `HAL_FLASH_Erase` is `NbPages * FLASH_PAGE_SIZE`; `NbPages=0` silently does nothing. The implementation is in `arch/cpu/arm/py32f040xb/quilla-qboot-hal.c`, setting `FLASH_TYPEERASE_PAGEERASE` + `NbPages=1`.
- **The HAL main header is `py32f0xx_hal.h`** (not `py32f040_hal.h`); `py32f040_hal_flash.h` / `py32f040_hal_cortex.h` do exist.
- **QBOOT link closure**: no undefined external symbols in the minimal set; `process-mutex.c`, `lpm.c`, `autostart.c` are not needed (`int-master.c` is needed and already included).
- **Both projects have "One ELF Section per Function + Remove Unused Sections" enabled** (the prerequisite for squeezing QBOOT into 24K): `TargetArmAds → Cads → <OneElfS>1</OneElfS>`, linker `LDads → <Misc>--remove</Misc>`.
- **Note**: the APP's IROM element name is `<IROM>` (no suffix), not `<IROM1>`; when changing the link address, be sure to use the tag's actual name — changing the wrong element has no effect.
