**[English](qboot-app-architecture.md) · [中文](qboot-app-architecture.zh-CN.md)**

# Quilla QBOOT / APP Dual-System Split and QIAP Upgrade Protocol Design

> Target chips: PY32F040xB (Cortex-M0+) and GD32F502VE (Cortex-M33)
> PY32F040xB resources: Flash 128 KB (0x08000000–0x0801FFFF), RAM 16 KB (0x20000000), Flash page **256 B**, page erase.
> GD32F502VE resources: Flash 512 KB (0x08000000–0x0807FFFF), Flash erase page 2 KB (bank0).
> Shared code: this repository's `arch/`, `os/` (Contiki-NG derivative + Quilla's own services).
> Status: design document + reference implementation. The protocol and algorithm portions have been compiled on the PC side with clang and run through the RFC 8032 test vectors; the ARM side is compile-verified via the Keil project.
>
> Companion process documents:
> - [`upgrade-flow.md`](upgrade-flow.md) — complete online upgrade flow (host upload → switch to QBOOT → frame processing → verification → write → reset)
> - [`boot-flow.md`](boot-flow.md) — boot verification and APP switching flow after reset

---

## 1. Goals and Constraints

- On power-up the device first enters **QBOOT** (In-Application Programming, bootloader), which reads the `[QBOOT/APP interaction flag]`:
  - Flag is the default `app1` → **validate the APP vector table first** (initial SP within the RAM range, reset vector within the APP region with the Thumb bit set); if valid, jump to the APP start address and run.
  - Flag is `qiap` → enter the UART upgrade flow.
  - APP region blank or corrupted (erased / invalid vectors) → stay in the QBOOT interactive shell, keeping the UART link alive (`part` → loader, `qiap` can restart the upgrade).
- On the shell command `swqboot`, APP rewrites the flag to `qboot` (stay in the QBOOT interactive shell, do not enter the upgrade) and resets. After the reset, QBOOT comes up in the interactive shell, where `swapp` returns to APP and `qiap` starts the upgrade. `swqboot` **no longer** sets `qiap` directly: the real upgrade flag is persisted only when QBOOT receives a valid ENTER frame (§3/§8). This both lets a device that accidentally entered QBOOT escape, and guarantees that a power loss mid-flashing re-enters the upgrade on the next boot.
- **QBOOT must be small**: compilation/linking includes only the necessary files, not the full Contiki network stack, display, SPI DMA, etc.
- Both QBOOT and APP provide the shell command `part`: QBOOT returns `qboot`, APP returns `app1`. The partition tag **must not be hardcoded in the shell**; each system passes it in at runtime.

---

## 2. System Division

| System | Responsibility | Uses OS Scheduling | Key Source Files (Suggested) |
| --- | --- | --- | --- |
| **QBOOT** | Flag evaluation, jumping to APP, UART QIAP reception and flashing | Reuses minimal OS (only to run the shell process) | `os/services/quilla-qboot/qboot-main.c` (shared boot entry), `arch/cpu/arm/<mcu>/quilla-qboot-hal.c`, `os/services/quilla-qboot/{quilla-qiap.c,quilla-qboot-meta.c,quilla-qboot-flag.c}`, `quilla-shell.c`, `shellport.c`, minimal `arch/cpu/arm` HAL (rcc/gpio/uart/dma/pwr/cortex/flash), `os/sys/{process,clock,...}`, `os/lib/{crc16,ringbuf,...}` |
| **APP** | Business logic (display, command parsing, SPI DMA and other full features) | Full Contiki | existing full `main.c` + `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` + `os/services/quilla-qboot/{quilla-qboot-meta.c,quilla-qboot-flag.c,quilla-app-qboot.c}` |

The two systems **share** the same `arch/`, `os/` source tree. The only differences are:

1. **Different linker scripts** (Flash region partitioning + APP VTOR relocation);
2. **Different build source lists** (QBOOT uses the minimal set, see §4);
3. **Different entry points and a small amount of initialization** (QBOOT uses the shared `os/services/quilla-qboot/qboot-main.c`, APP uses `main.c`);
4. **Each calls `shell_set_partition_tag()` at runtime** (see §6).

> Note: QBOOT also uses the shell (for the `part` command and optional human interaction), so the minimal OS subset that lets the shell process run (process/clock, etc.) must be retained. "Reducing QBOOT size" means **stripping APP-specific modules such as the network stack, display, SPI DMA, and CoAP/SNMP**, not going fully bare-metal. Slimming QBOOT further to bare-metal polling (no shell process scheduling) would require rewriting the shell as a polling version, a larger effort; this document defaults to "minimal OS + shell".

---

## 3. Memory Map (Current Values)

PY32F040xB: Flash 128 KB = 0x20000 bytes, 256 B pages (512 pages). The layout is a **QBOOT – META – APP** three-segment layout, byte-level identical across MCUs; GD32F502VE differs only in the Flash end address (`0x0807FFFF`) and the erase page size (2 KB).

| Region | Address Range | Length | Description |
| --- | --- | --- | --- |
| **QBOOT** | `0x08000000` – `0x08005FFF` | **24 KB** (PY32F040xB pages 0–95) | bootloader; includes SHA-256/512 + Ed25519 signature verification; the linker script limits `LENGTH = 0x6000`, and exceeding the bound is a link error |
| **META** | `0x08006000` – `0x08006FFF` | **4 KB** (PY32F040xB pages 96–111) | secure boot metadata region, between QBOOT and APP; contains the interaction flag and manifest |
| └─ META store | `0x08006000` (SLOT_A) / `0x08006800` (SLOT_B) | 2 × 2 KiB slots | Wear-levelled flag + manifest log: 256 B records (64 B `QMTL` meta block + 128 B `QMF1` manifest block + 64 B pad), appended program-only; the live slot is never erased in place (see §5) |
| **APP** | `0x08007000` – Flash end | **PY32F040xB `0x19000` (102,400 B ≈ 95.5 KB)**; GD32 `0x79000` (495,616 B) | business firmware; `ORIGIN = 0x08007000` |

- **APP start address `APP_START = 0x08007000`** (`QUILLA_QBOOT_APP_START`; the protocols and jumps below all use this as the base).
- **APP programmable upper bound is MCU-specific**: PY32F040xB = `0x19000` (APP ends at `0x0801FFFF`); GD32F502VE runs to `0x0807FFFF` (512 KiB flash → `0x79000`). META comes before APP and no longer occupies the Flash end page. The host updater does not hardcode the bound: it queries the device's `pinfo` command (§6) and derives the flashable cap from the running firmware's `SOC_FLASH_END`.
- **The META region sits between QBOOT and APP** (rather than at the end page): FLAG and manifest no longer occupy the Flash tail page, so the APP region extends to the end of Flash, maximizing space. The upgrade protocol hard-rejects any address that writes into the META region (§8), and QBOOT never touches the META/APP regions above itself.
- **META = 4 KB is the unified minimal feasible value across MCUs**: GD32F502VE erases in 2 KiB pages (bank0), so META is sized 4 KiB (two 2 KiB pages) to give the flag record and manifest each their own erase page; it stays 4 KiB aligned for the PY32F040xB 4 KiB-sector boundary. The PY32F040xB page is only 256 B, so the 4 KB is pure alignment padding (FLAG ≈ 16 B + manifest ≈ 108 B actually occupy only a small fraction of META).
- The table above lists current values. `APP_START` and the QBOOT length may be fine-tuned to actual sizes, but the three regions must not overlap, and **the META region must not be occupied by QBOOT or APP code/data** (do not allocate `0x08006000`–`0x08006FFF` to either side in the linker scripts).
- The **single source of truth** for partition boundaries is `os/services/quilla-qboot/quilla-qboot-hal.h` (`QUILLA_QBOOT_META_BASE` / `QUILLA_QBOOT_META_SIZE` / `QUILLA_QBOOT_APP_START`). `QUILLA_QBOOT_APP_OFFSET` is derived from it, and APP's `VECT_TAB_OFFSET` uses that macro in turn, so changing the partition requires editing only this one place.

### APP Vector Table Relocation (Common Pitfall)
APP can no longer use the default `VTOR = 0x08000000`. The APP build must set `SCB->VTOR` to `APP_START`:
- Add the following inside `SystemInit()` in `system_py32f040.c` (guarded by the `APP_BUILD` macro):
  ```c
  #ifdef APP_BUILD
    SCB->VTOR = APP_START;   /* 0x08007000 */
  #endif
  ```
- Or hand-write `SCB->VTOR = FLASH_BASE + 0x7000;` at the very beginning of APP's reset handler.
- QBOOT does not need to change VTOR (it is already located at 0x08000000, where the chip reset vector resides).

---

## 4. Build Separation Approach

This repository has five build registration points per MCU (`Makefile`, `Makefile.qboot`, `.eide/eide.json`, `APP.uvprojx`, `QBOOT.uvprojx`). Per the "iron rule", every newly added `.c` must be registered in all five. **QBOOT-only files (`os/services/quilla-qboot/qboot-main.c` / `quilla-qiap.c`) go only into the QBOOT build, not into the existing APP build**, to avoid regressions.

### 4.1 Existing APP Build (Minimal Changes, Stays Usable)

To give APP the `swqboot` capability, **append** to the existing APP build (do not remove any existing items):

- Source files: `arch/cpu/arm/<mcu>/quilla-qboot-hal.c`, `os/services/quilla-qboot/quilla-qboot-meta.c`, `os/services/quilla-qboot/quilla-qboot-flag.c`, `os/services/quilla-qboot/quilla-app-qboot.c` (plus the MCU's flash HAL where not already compiled);
- Macros: `-DQAPP_BUILD` (drives VTOR relocation);
- `main.c` calls `shell_set_partition_tag("app1")` and `quilla_app_qboot_init()` during startup (registers the `swqboot` command);
- Switch the linker script to the APP variant (`py32f040xb_app.ld` / the GD32 equivalent).

> The above additions must be registered in all five Keil/EIDE build entry points.

### 4.2 QBOOT Build (New `Makefile.qboot` + the QBOOT linker script)

Add `user/<mcu>/EIDE/Makefile.qboot` (`TARGET = QBOOT`) and the QBOOT linker script (Flash `LENGTH = 24K`). `make -f Makefile.qboot` then produces `QBOOT/QBOOT.{elf,hex,bin}`. The minimal QBOOT source set (**verified symbol by symbol against the real link closure**, not estimated):

- **Startup/system**: `startup_<device>xx.s` (chip reset vector + weak ISRs), `user/<mcu>/Src/system_<device>.c` (provides `SystemInit()`, called by startup after reset, configures `SystemCoreClock`).
  - **QBOOT does not go through `platform_init_stage_one()`**: `os/services/quilla-qboot/qboot-main.c` calls `clock_init()` directly (only configures SysTick/system tick). Reason: `platform_init_stage_one → soc_init` pulls up `lpm_init()` and `rtimer_init() → rtimer_arch_init()`, and the latter **hard-depends on the MCU's LPTIM HAL and `Error_Handler()`** (provided only by `main.c`), adding both size and APP-specific symbols. QBOOT only needs "correct system clock (to guarantee the UART baud rate) + SysTick + UART", for which `clock_init()` is sufficient.
- **HAL (minimal; all use the in-repository vendor HAL copies, no external SDK dependency)**: the MCU's `hal.c`, `hal_rcc.c`, `hal_rcc_ex.c`, `hal_gpio.c`, `hal_dma.c`, `hal_pwr.c`, `hal_uart.c`, `hal_cortex.c`, `hal_flash.c`.
  - `shellport.c` brings its own `HAL_UART_MspInit` / `HAL_DMA_MspInit` (USART pin and clock configuration), so the platform's `*_hal_msp.c` is **not needed**.
- **QBOOT business logic**: `os/services/quilla-qboot/qboot-main.c` (shared entry point), `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` (hardware), `os/services/quilla-qboot/quilla-qiap.c`, `os/services/quilla-qboot/quilla-qboot-meta.c` (wear-levelled META store), `os/services/quilla-qboot/quilla-qboot-flag.c` (thin delegation to meta).
- **shell**: `os/services/shell/quilla-shell.c`, `arch/cpu/arm/<mcu>/shellport.c` (reuses the shell UART, directly obtains the `part` command).
- **Minimal OS subset (required by symbol closure)**: `os/sys/process.c`, `os/sys/kern-guard.c`, `os/sys/clock.c`, `os/sys/etimer.c`, `os/sys/timer.c`, `os/sys/atomic.c`, `os/lib/crc16.c`, `os/lib/ringbuf.c`, `os/lib/assert.c`.
  - Dependency chain (verified one by one): `process.c` calls `kern_guard_register` / `kern_raise` (macros expanding to `kern_error_hook_default`, defined in `kern-guard.c`) → `kern-guard.c` is required; `clock.c` **unconditionally** calls `etimer_pending()` / `etimer_request_poll()` in `clock_update_ticks()` → `etimer.c` is required; `etimer.c` uses `timer_*` → `timer.c` is required; the shell uses `strcmp`/`memset` and shellport uses `strlen` → provided by `libc_nano.a` via `-specs=nano.specs`, while the linker script's `/DISCARD/ { libc.a }` only discards the **full** libc, not affecting these small functions.
  - The key comparison in `quilla-qiap.c` **does not depend on libc**: the original `memcmp` was replaced with the built-in byte-by-byte `key_match()` (removing `<string.h>`), keeping the bootloader free of libc dependencies.
  - **Not introduced**: `list.c`/`memb.c` (not referenced by QBOOT sources), `autostart.c`, `ctimer.c`, `stimer.c`, `rtimer.c`/`rtimer-arch.c`, `lpm.c`, `kern-nomalloc.c`, `platform.c`/`soc.c`, `watchdog.c`, the network stack, display/SPI DMA, etc.
- **Macros**: `-DUSE_HAL_DRIVER -D<device> -DQBOOT_BUILD`; **optimization `-Os`**; the QBOOT linker script.
- **Excluded (APP-specific, strictly forbidden in QBOOT)**: network stack, display, SPI DMA, CoAP/SNMP, RPL, TSCH, CTIMER/STIMER, AUTOSTART, and the LPTIM/low-power/RTIMER chain (`rtimer-arch.c`, `hal_lptim.c`, `lpm.c`).

> QBOOT size budget is 24 KB. The closure is complete per static symbol verification. If the `-Os` link result exceeds 24 KB, cut in this order: `assert.c` (if unreferenced, `--gc-sections` already discards it); removing `clock.c`'s calls to `etimer` (requires modifying `clock.c`, a large effort, not recommended); if still over, move `APP_START` back per §3 (sacrificing APP space). On GD32F502VE (M33) note that armclang LTO inlines the Ed25519 verification chain and inflates SEALED/SECURED past the budget — disable LTO (`v6Lto=0`); see `doc/keil-qboot-app-build.md`.

---

## 5. QBOOT / APP Interaction Flag and META Store

The flag is no longer a standalone "FLAG page". It is one field of the wear-levelled META store (`quilla-qboot-meta.c`), which splits the 4 KiB META region into two 2 KiB slots and appends 256 B records — each carrying a 64 B `QMTL` meta block (flag, anti-rollback floor, sealed-image record) and a 128 B `QMF1` manifest block — with program-only writes. The live slot is never erased in place; the standby slot is pre-erased before the active slot fills. On read, both slots are scanned and the record with the highest sequence is authoritative, so a power loss leaves a fully readable record. The flag itself (and the floor / seal / manifest) enjoys the same power-loss safety as the rest of the log.

Flag values (unchanged):

```c
#define QUILLA_QBOOT_FLAG_APP1   0x00000001U   /* default: enter APP */
#define QUILLA_QBOOT_FLAG_QIAP   0x00000002U   /* upgrade in progress (persisted after ENTER) */
#define QUILLA_QBOOT_FLAG_QBOOT  0x00000003U   /* stay in QBOOT shell (set by swqboot) */
```

**Why Flash instead of RAM/backup registers**: after power-up (power loss), RAM contents are not retained; backup registers on the PY32F040xB are affected by VBAT/reset and are not available on all packages. The META region (between QBOOT and APP) is the most stable location across resets and is naturally independent of both systems' code regions.

**Read**: `quilla_qboot_flag_get()` — if `magic` does not match, or the value is anything other than `QIAP`, it always returns `APP1` (the default safe state).

**Write** (APP calls `swqboot`): `quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_QBOOT)` (stay in the QBOOT shell). When the upgrade truly begins (valid ENTER frame), `quilla-qiap.c` writes `QUILLA_QBOOT_FLAG_QIAP`; on END success, `APP1` is written back. All writes go through the META append path: the new record is programmed into the standby slot (pre-erased if needed), and the next read picks it up by highest sequence — there is no read-page/erase-page/rewrite-page cycle, so a power loss mid-write can never corrupt the live record. QBOOT itself writes the flag back to `APP1` after a successful boot, then resets/jumps.

Implementation: `os/services/quilla-qboot/quilla-qboot-flag.{h,c}` (shared by both systems, already in the APP build addition list; a thin delegation to `quilla-qboot-meta.c`).

---

## 6. The `part` Command and Partition Tag Propagation (Answering "Don't Hardcode It in the Shell")

The shell is common code and follows the established design of "the core does not reference applications backwards". **The partition tag is passed in at runtime by each system's own entry point**; the shell is only responsible for storing and echoing it:

- New in `quilla-shell.h`:
  ```c
  /* Called by QBOOT and APP respectively during startup, to tell the shell the current partition identity. The shell itself does not decide. */
  void shell_set_partition_tag(const char *tag);
  ```
- In `quilla-shell.c`, add the static `const char *shell_part_tag = "app1";` (default safe value) and add the built-in command `part`:
  ```c
  static void part_handler(const char *args) {
    (void)args;
    shell_output("%s\r\n", shell_part_tag ? shell_part_tag : "unknown");
  }
  ```
  and register this command in `shell_init()`.
- **QBOOT** (`qboot-main.c`) calls `shell_set_partition_tag("qboot");` after startup
- **APP** (`main.c`) calls `shell_set_partition_tag("app1");` after startup

This way: the logic for determining `qboot`/`app1` lives entirely in each system's own `main`, and the shell always stays neutral with zero hardcoding; adding a third partition in the future also only requires editing the corresponding entry point, without touching the shell.

QBOOT additionally registers a `pinfo` command (`os/services/quilla-qboot/qboot-main.c`) that prints the real partition layout `[PART] APP_START=0x… APP_END=0x… APP_SIZE=0x…` from the compiled-in macros (`QUILLA_QBOOT_APP_END` = `SOC_FLASH_END`, which is MCU-specific). The host updater reads it, so its pre-flight flashable-size cap always matches the device instead of a hardcoded per-MCU value. `pinfo` is QBOOT-only (not in the APP shell) and, like `part`, is swallowed once the QIAP raw sink takes over.

---

## 7. QBOOT Boot Flow (bootloader boot policy)

```
Power-on → chip reset vector (0x08000000) → QBOOT's Reset_Handler
  → clock/Flash init → shell init → shell_set_partition_tag("qboot")
  → quilla_qboot_flag_get()
       ├─ == QIAP    → enter QBOOT UART upgrade frame reception directly (crash recovery, §8)
       │             (a power loss/reset mid-upgrade stays in this state, avoiding a jump to a half-broken APP)
       ├─ == QBOOT   → enter the QBOOT interactive shell, **do not jump to APP** (even if the APP vector table is valid):
       │             · `swapp` can return to APP; `qiap` can start the upgrade
       └─ == APP1, or interaction flag blank/corrupted (read back as APP1 in either case):
            ├─ qboot_app_image_valid() passes
            │    └─ and qboot_secure_boot_ok() passes → jump to APP (app_jump below)
            └─ either check fails → enter the QBOOT interactive shell (keeping `qiap` reachable)

app_jump:
  __disable_irq();
  SCB->VTOR = APP_START;            /* safety: APP sets it itself too */
  sp  = *(uint32_t*)APP_START;
  pc  = *(uint32_t*)(APP_START+4);
  ((void(*)(void))pc)();            /* jump to the APP reset vector */
```

**How to enter upgrade mode (no "dwell window")**: the current implementation has **no** 300 ms dwell window, and there is no `0x1A` trigger byte — this was an early draft and has been deleted. There are only two paths to enter the QBOOT UART upgrade:

- **Cold boot with flag==QIAP**: after a power loss/reset mid-upgrade, the interaction flag still reads `QIAP`, so QBOOT enters upgrade frame reception directly on power-up (avoiding a jump to a half-broken APP);
- **`qiap` command**: in the QBOOT interactive shell (forced stay when flag==QBOOT, or falling in when flag==APP1 and APP is corrupted), the host sends the text command `qiap` (terminated by Enter) to switch to upgrade frame reception.

Falling into the QBOOT shell when the APP region is corrupted, and the forced QBOOT-shell stay after `swqboot`, exist precisely to keep the `qiap` command always reachable; to return to APP, use `swapp`. See §8 for details.

---

## 8. QBOOT UART Upgrade Protocol (Host ↔ QBOOT)

### 8.0 How to Enter Upgrade Mode (Important: the Host Protocol Has Changed)

On power-up, QBOOT does **not** receive binary upgrade frames directly by default; it first evaluates the flag (§7):

- flag==QIAP (leftover from a power loss/reset mid-upgrade) → automatically enter upgrade frame reception;
- otherwise enter the QBOOT interactive shell (forced stay when flag==QBOOT; also falls in when flag==APP1 and APP is corrupted).

In the shell, **the host must first send the text command `qiap` (terminated by Enter)**. The device then switches the UART link from "shell line parsing" to a "raw byte stream", after which all bytes are fed directly to `quilla_qiap_feed()` for parsing.

> ⚠️ Any binary frame sent before the `qiap` command (including ENTER/DATA/END) is parsed as a shell command or silently discarded, and the upgrade cannot start. Be sure to type `qiap` first.
> Typical host sequence (entering QBOOT via `swqboot`): `swqboot` (inside APP, sets the QBOOT flag and resets) → reset into the QBOOT shell → send `qiap` → send ENTER/DATA/END → on success, reset into the new APP.

Link: reuses the existing shell UART (PY32F040xB: USART2 / PA2/PA3 / DMA; GD32F502VE: USART0 / PB6/PB7 / DMA0 CH4 + IDLE framing; CH32V305RB: USART2 / PA2/PA3 / DMA1 CH6 + IDLE framing; all 115200-N-8-1, same as APP).

**The protocol is binary frames**, non-interfering with the shell's line parsing: after the `qiap` command switches modes, the UART byte stream is fed directly to the `quilla_qiap_feed()` state machine (`quilla-qiap.c`) and no longer goes through shell line parsing.

### 8.1 Common Frame Format (Multi-Byte Fields Are Little-Endian)

```
SOF    1B   fixed 0xA5
CMD    1B   command/response code (see the table below)
LEN    2B   length of the following PAYLOAD (little-endian)
PAYLOAD LEN  command parameters
CRC16  2B   CRC-16/CCITT, covering [CMD][LEN][PAYLOAD] (i.e. everything after SOF)
```

**CRC-16/CCITT algorithm (the host must match this exactly; reuses `os/lib/crc16.c`)**:
```c
uint16_t crc16_add(uint8_t b, uint16_t acc) {
  acc ^= b;
  acc  = (acc >> 8) | (acc << 8);
  acc ^= (acc & 0xff00) << 4;
  acc ^= (acc >> 8) >> 4;
  acc ^= (acc & 0xff00) >> 5;
  return acc;
}
/* initial acc = 0; call crc16_add byte by byte; compare the final result against the CRC16 in the frame */
```

### 8.2 Command Codes

| Direction | CMD | Name | PAYLOAD |
| --- | --- | --- | --- |
| H→M | `0x01` | ENTER_OTA_REQ | `app_start_addr(4)` + `unlock_key(N)` |
| M→H | `0x81` | ACK_ENTER | `status(1)` (0=OK) |
| H→M | `0x02` | DATA_FRAME | `start_addr(4)` + `end_addr(4)` + `data(LEN-8)` |
| M→H | `0x82` | ACK_DATA | `start_addr(4)` (echoes this frame's start address, for continuation/retransmission) |
| H→M | `0x03` | END_FRAME | `total_len(4)` + `total_crc16(2)` |
| M→H | `0x83` | ACK_END | `status(1)` (0=OK; **no longer commits once the public key has been flashed**) |
| H→M | `0x04` | SIGN_FRAME | `signature(64)` |
| M→H | `0x84` | ACK_SIGN (QBOOT SUCCESS) | `status(1)` (0=OK) |
| M→H | `0xCx` | NAK | `reason(1)` (see error codes) |

> Negative responses uniformly use `0xC1/0xC2/0xC3/0xC4` (corresponding to failures at the enter/data/end/sign stages), and the `reason` byte distinguishes the cause (address out of bounds, wrong key, CRC error, erase/write failure, length mismatch, invalid signature, etc.).

### 8.3 Flow

1. **Handshake (target confirmation + unlock)**
   - The host sends `ENTER_OTA_REQ`: `app_start_addr = 0x08007000` + the agreed `unlock_key` (8 bytes recommended, written at factory/configuration).
   - QBOOT verifies `app_start_addr == APP_START` and `unlock_key == QBOOT_UNLOCK_KEY`:
     - Both correct → replies `0x81 ACK_ENTER`, entering the "receiving data" state;
     - Once verification passes, the interaction flag is written as `QIAP` (`quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_QIAP)`), so that after a power loss/reset mid-upgrade, the next boot automatically re-enters the upgrade instead of jumping to a half-broken APP.
     - Any mismatch → replies `0xC1 NAK` (reason); limited retries are allowed, and on timeout it exits/returns to APP.

2. **Frame-by-frame flashing (with offset and verification)**
   - The host sends `DATA_FRAME`: `start_addr`, `end_addr`, `data` (`end_addr - start_addr + 1 == data length`).
   - QBOOT **address validation** (critical): `start_addr >= APP_START` and `end_addr <= APP_END`, and the range must fall on legal page boundaries; otherwise `0xC2 NAK`.
     - Because the APP region has an `APP_START` offset relative to the whole Flash, all received addresses are **absolute APP-region addresses** (the host sends them according to the APP's actual link addresses), and QBOOT writes Flash directly at that absolute address. "Offset" here means: QBOOT must reject any address `< APP_START` or `> APP_END`, to prevent accidentally writing QBOOT itself or the interaction flag.
   - For each new page covered by the frame, QBOOT first erases it (recording "erased pages" to avoid repeated erases), then programs `data` page by page (PY32F040xB: 256 B; GD32: 2 KB); after writing, it **reads back and verifies** that the bytes match.
   - Accumulates the global `rx_total_len += data length` and `rx_total_crc = crc16_data(data, len, rx_total_crc)`.
   - Correct → replies `0x82 ACK_DATA` (with `start_addr`); failure → replies `0xC2 NAK`, and the host retransmits this frame.

3. **Final verification**
   - After all frames are sent, the host sends `END_FRAME`: `total_len(4)` + `total_crc16(2)`.
   - QBOOT compares `rx_total_len == total_len` and `rx_total_crc == total_crc16`:
     - Mismatch → replies `0xC3 NAK`; the host may resend END_FRAME or restart the full-package upgrade.
     - Match → replies `0x83 ACK_END`. **Whether to commit at this point depends on whether the verification public key has been flashed**:
       - **Public key not flashed (secure boot not enabled)**: END writes the interaction flag as `APP1` directly and resets; the behavior matches historical versions, and old hosts need no changes.
       - **Public key flashed**: END **only replies ACK and does not commit**; it must wait for the SIGN frame verification below to pass.

4. **Sign and commit (required when secure boot is enabled)**
   - The host sends `SIGN_FRAME`: `signature(64)`.
   - The device does the following, in order:
     1. Compute SHA-256 in real time over `[APP_START, APP_START+total_len)` in Flash;
     2. Verify `Ed25519(pubkey, image_len||sha256, signature)`;
     3. **Take the firmware security version from the signed image**: read `quilla_qboot_secure_fw_version()` at the fixed address `QUILLA_QBOOT_APP_START + 0x400`; refuse if it is `0` (region missing) or below the anti-rollback floor. The version is **part of the signed image**, never host-supplied;
     4. Write the manifest (magic/version=nv/image_len/sha256/sig) to the META record;
     5. Raise the anti-rollback lower bound to `nv`;
     6. Persist the seal and write the flag `APP1`, then **system reset**.
   - Any step failing → replies `0xC4 NAK` (`0x08` invalid signature), **no commit, no reset**; the device stays in the upgrade state awaiting retry.

> **Signed object convention** (both ends must agree):
> `image_len(4B LE) || SHA-256(image)(32B)`, 36 bytes total.
> The version is **not** in the signed object: it lives inside `image_len` (fixed address `QUILLA_QBOOT_APP_START + 0x400`) as part of the signed image, covered by the digest and the signature, so it cannot be forged without the private key. Neither the host nor QBOOT invents it.
> The device uses the **digest it computes in real time** for signature verification, not the copy stored in the manifest,
> so tampering with the Flash image changes the signature input and thus fails verification directly.

### 8.4 Error Codes (reason) Recommendations
- `0x01` Address out of bounds (< APP_START or > APP_END)
- `0x02` Wrong unlock key
- `0x03` In-frame CRC error
- `0x04` Flash erase failure
- `0x05` Flash program/read-back failure
- `0x06` Total length/total CRC mismatch
- `0x07` Length field inconsistent with data
- `0x08` Invalid signature (unsigned, signature mismatch, or illegal image length)

---

## 8.5 Secure Boot (Ed25519 Signature Verification + Anti-Rollback)

Once the verification public key has been flashed, QBOOT only executes APP images that are **validly signed and not rolled back in version**. When no public key has been flashed, the whole mechanism is automatically bypassed (`QUILLA_SB_INACTIVE`), preserving historical behavior, so the production line can enable it per unit as needed.

### Signed Object

```
image_len(4B LE) || SHA-256(image)(32B)     36 bytes total
```

The whole image is not signed directly: Ed25519 needs only a single 36-byte verification call; the only part that streams over the image is SHA-256, which costs far less. The version number is **not** in the signed object: it lives inside `image_len` (fixed address `QUILLA_QBOOT_APP_START + 0x400`) as part of the signed image, covered by the digest and the signature, so it cannot be forged without the private key. The device verifies the signature against the **digest it computes in real time**, not the copy stored in the manifest, so tampering with the Flash image necessarily changes the signature input and fails verification.

### manifest (`QMF1` block inside the META record, 128B)

| Offset | Length | Field |
| --- | --- | --- |
| 0 | 4 | magic `QMF1` (`0x514D4631`) |
| 4 | 4 | version |
| 8 | 4 | image_len |
| 12 | 32 | SHA-256(image) |
| 44 | 64 | Ed25519 signature |
| 108 | 4 | CRC-32 |

The manifest is no longer a dedicated page: it is the 128 B `QMF1` block of the wear-levelled META record (see Section 5), appended with `quilla_qboot_meta_set_manifest()` — no whole-page erase, so the image tail can never be corrupted by a manifest write. It is written only when an image is installed (never in `QBOOT_CHECK` mode); when absent the block holds just the `QMF1` magic + `version` with `image_len = 0` and the rest `0xFF`.

### Boot Verification (`quilla_qboot_secure_verify_app()`)

```
read manifest → bad magic                 → ERR_NO_MANIFEST (refuse to jump)
image_len out of bounds                   → ERR_LEN (refuse to jump)
computed SHA-256(image) ≠ manifest digest → ERR_HASH (refuse to jump)
Ed25519 verification failed               → ERR_SIG (refuse to jump)
version < anti-rollback lower bound       → ERR_ROLLBACK (refuse to jump)
all checks passed                         → raise the lower bound to version, return OK, jump to APP
```

When refusing to jump, QBOOT **does not reset and does not jump to APP**; after printing the reason it enters the interactive shell, where the `qiap` command remains reachable — the device stays recoverable rather than unbootable.

### Anti-Rollback Lower Bound

The lower bound is a field of the wear-levelled META record (QMTL meta block, `QUILLA_META_OFF_FLOOR`), only ever increasing. Both a successful upgrade and a passed boot verification raise the lower bound to the current version. The version is **part of the signed firmware image** (baked at `QUILLA_QBOOT_APP_START + 0x400`, covered by the signature); it is never accepted from the host, and QBOOT only ratchets its floor up to it, so a downgraded (signed) image carrying a lower firmware version is refused. A failure to raise it is handled as best-effort: **an image already accepted by the signature must not become unbootable merely because the lower bound cannot be written**; the signature check is the primary gate. (The APP also carries a non-cryptographic self-check of the region, `QUILLA_APP_SECVER_SELFCHECK` — obfuscation, not a security control; see `doc/secure-boot.md` §1.1.)

### Keys and Tools

```bash
python tools/quilla_ed25519_sign.py keygen --dir keys
#   -> keys/private.hex (kept on the production line, never leaked)
#   -> keys/public.hex (flashed into QBOOT)
#   and prints toolchain-specific provisioning instructions
```

Follow the printed instructions: GNU adds `-DQUILLA_SECURE_BOOT_PUBKEY={...}`
to `C_DEFS`; Keil adds a product branch in `quilla-qboot-pubkey.h` and selects
its token in the Define box (the comma-separated Define field cannot carry a
`{...}` list).
The private key stays only on the production line; when flashing:

```bash
python tools/quilla_host_updater.py COM3 app.bin --key keys/private.hex
```

---

## 9. Reliability and Security

- **Power loss / mid-process failure**: if the upgrade is incomplete (e.g. power loss), the interaction flag reads `QIAP` from the moment a valid ENTER frame is accepted (§5), so the next power-up enters the upgrade directly instead of jumping to a half-broken APP. Before jumping to APP, QBOOT still performs a **minimal integrity check** (e.g. the APP reset vector is not all 0xFF and the stack pointer is within the RAM range); if it fails, it stays in QBOOT awaiting reflashing.
- **Anti-misflashing**: `unlock_key` + `app_start_addr` dual confirmation; hard rejection of out-of-bounds addresses; the interaction flag never participates in flashing.
- **Write protection (optional)**: Option Byte WRP can write-protect the QBOOT region (PY32F040xB pages 0–95 for 24 KB), preventing a QBOOT bug from destroying the bootloader itself.
- **Watchdog**: if IWDG is enabled during QBOOT, it must be fed in time during the erase/write loops or temporarily disabled, to avoid erase/write latency triggering a reset.
- **Rollback**: the current design has a single APP slot with no A/B backup; rollback would require planning a second APP slot and selection logic (out of scope for this design).

---

## Related Source Files

| File | Purpose | Owning Build |
| --- | --- | --- |
| `doc/qboot-app-architecture.md` | This design document | docs |
| `os/services/shell/quilla-shell.h` | `shell_set_partition_tag()` | shared |
| `os/services/shell/quilla-shell.c` | `part` command + tag variable | shared |
| `os/services/quilla-qboot/quilla-qboot-meta.h/.c` | Wear-levelled META store (flag/floor/seal/manifest) | shared |
| `os/services/quilla-qboot/quilla-qboot-flag.h/.c` | Interaction flag read/write (delegates to META) | shared |
| `os/services/quilla-qboot/quilla-app-qboot.h/.c` | APP-side `swqboot` command | APP |
| `os/services/quilla-qboot/quilla-qiap.h/.c` | QBOOT-side QIAP state machine | QBOOT |
| `os/services/quilla-qboot/quilla-qboot-hal.h` | Platform hardware interface to implement + layout constants | declarations |
| `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` | Per-MCU implementation of the above interface (Flash/reset/jump/transmit) | shared |
| `os/services/quilla-qboot/qboot-main.c` | QBOOT entry (`main()`, UART ring, boot policy; shared across ports) | QBOOT |

> **Layering convention**: `os/services/quilla-qboot/*` is entirely hardware-independent and does not `include` any HAL/CMSIS headers; machine operations are dispatched via `quilla_qboot_hal_*()` down to `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` (same pattern as `int-master.h`/`clock.h`). Partition layout constants are given as default values in `quilla-qboot-hal.h` via `#ifndef` and overridden with `-D` on other platforms.
| `user/<mcu>/EIDE/py32f040xb_qboot.ld` (GD32: the equivalent QBOOT script) | QBOOT linker script | QBOOT |
| `user/<mcu>/EIDE/py32f040xb_app.ld` (GD32: the equivalent APP script) | APP linker script (`ORIGIN=0x08007000`, PY32F040xB `LENGTH=0x19000`) | APP |
| `user/<mcu>/EIDE/Makefile.qboot` | QBOOT build reference (`make -f Makefile.qboot`) | QBOOT |
