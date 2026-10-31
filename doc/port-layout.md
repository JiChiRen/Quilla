**[English](port-layout.md) · [中文](port-layout.zh-CN.md)**

# Multi-MCU Port Layout Specification

- **Status**: Active (effective 2026-10-08)
- **Related**: `doc/cmsis-layout.md` (CMSIS-Pack directory specification)
- **Scope**: `arch/cpu/arm/` (including its `platform/`) and `user/`

> **RISC-V port (CH32V305VB, `ch32v305rb`)**: the same three-layer boundary applies
> under `arch/cpu/riscv/`, `arch/platform/riscv/` and `user/ch32v305rb/`, but the
> build is **EIDE / GNU GCC12 only** (no Keil) and the boot images are linked at
> the `0x00000000` Flash alias, so the whole partition scheme is `0x0`-based
> rather than `0x0800`-based. See `doc/riscv-ch32v305-build.md`.

---

## 1. Goals

Multiple MCUs coexist in the same repository, and **changes to any one of them never touch another's source list**.
Three-layer boundary:

| Layer | Contents | Varies with MCU |
|---|---|---|
| `os/` | Kernel, shell services, QIAP protocol, and secure boot | No (`os/` has zero repo-wide references to `arch/` or vendor headers, verified) |
| `arch/cpu/arm/` common layer | `cpu.h`, `arm-def.h`, `int-master.c`, `dbg.c` | No |
| `arch/cpu/arm/<mcu>/` | clock / rtimer-arch / soc / shellport / lpm / dev / qboot-hal / qboot-main | **Yes** |
| `arch/platform/arm/<mcu>/` | `contiki-conf.h`, `platform.c` | **Yes** |
| `user/<mcu>/` | That MCU's project tree (EIDE / MDK-ARM / Src / Inc) | **Yes** |
| `arch/cpu/arm/CMSIS/`, `arch/cpu/arm/vendor/` | Device packs and vendor firmware libraries (shared by all MCUs) | Shared |

`arch/platform/arm/inttypes.h` is the only exception: it is MCU-independent and stays under
`arch/platform/arm/`, so both that directory and `arch/platform/arm/<mcu>/`
**must** be in the include path.

---

## 2. Directory

```
arch/cpu/arm/
├── cpu.h  arm-def.h  int-master.c  dbg.c      # ARM common
├── CMSIS/                                     # see doc/cmsis-layout.md
├── vendor/<Vendor>/<Library>/
└── <mcu>/                                     # one per MCU
    ├── clock.c
    ├── rtimer-arch.{c,h}
    ├── soc.{c,h}
    ├── shellport.{c,h}
    ├── lpm.{c,h}
    ├── qboot-main.c
    ├── quilla-qboot-hal.c
    └── dev/{watchdog,qwdt,qclk}.{c,h}

arch/platform/arm/
├── inttypes.h
└── <mcu>/{contiki-conf.h, platform.c}

user/<mcu>/
├── BSP/  Inc/  Src/
├── EIDE/{Makefile, Makefile.qboot, .eide/eide.json}
└── MDK-ARM/{APP.uvprojx, QBOOT.uvprojx}
```

`<mcu>` values: `py32f040xb`, `gd32f502ve` (lowercase, matching the device pack names).

---

## 3. Checklist for Adding a New MCU

Do them in order; each step assumes the previous one is already in place.

1. **Device pack**: `arch/cpu/arm/CMSIS/Device/<Vendor>/<Device>/`
   containing `Include/`, startup files (`Source/{ARM,GCC,IAR}/`), `system_<device>.c`,
   and link scripts (`Source/GCC/Ld/`, `Source/IAR/Ld/`).
2. **Vendor library**: `arch/cpu/arm/vendor/<Vendor>/<Library>/`; the CMSIS copy bundled inside the library must be deleted.
3. **MCU layer**: `arch/cpu/arm/<mcu>/` implements the eight files from the table above (+ `dev/`).
   When referencing `os/` relatively in sources, watch the depth — under `<mcu>/` it is `../../../../os/`.
4. **Platform layer**: `arch/platform/arm/<mcu>/{contiki-conf.h,platform.c}`.
5. **Project tree**: `user/<mcu>/`, two GNU Makefiles (APP / QBOOT) + `eide.json`
   + two Keil `.uvprojx`. The directory depth must stay at two levels (`user/<mcu>/EIDE`),
   otherwise every path of the form `../../../` stops working.
6. **Build registration in five places**: `Makefile`, `Makefile.qboot`, `.eide/eide.json`,
   `APP.uvprojx`, `QBOOT.uvprojx`. After editing, **grep each group to re-verify the on-disk values**.
7. **Descriptor file verification**: confirm the three Keil descriptors (`APP.uvprojx` /
   `QBOOT.uvprojx`) fully match the Makefile's source files / includes / macros. The
   `tools/verify_gd32_keil_project.py` helper that automates this check is an **internal
   development tool and is not part of the public release** (private mirror only).
8. **Validation**: `make -f Makefile` and `make -f Makefile.qboot` each produce an elf;
   a real compile-and-link pass with armclang/armasm/armlink via Keil confirms the project.
   The `tools/verify_gd32_keil_build.py` automation is likewise an **internal dev tool**, not
   shipped publicly.
   > Note: after a Keil project is modified, the IDE writes some settings back to their defaults on exit; text-based editing of
   > `.uvprojx` is recommended (`ElementTree.write()` does not take effect).

---

## 4. Constraints and Notes

- **UV4 must be closed when editing Keil projects.** On exit it writes `.uvprojx` back, possibly reverting `<IROM>` to its default value; it may also lock the `MDK-ARM/` directory and prevent renaming. If files are locked, stop and let a person handle it; do not work around it with an alternate path.
- **Include depth**: after files are moved from `arch/cpu/arm/` to `arch/cpu/arm/<mcu>/`, all `"../../../os/..."` must be changed to `"../../../../os/..."`.
- **Makefile line continuations**: in `C_INCLUDES`, every line except the last must end with ` \`; omitting it when inserting a new line silently swallows all subsequent includes.
- **Single-MCU changes must not appear in the common layer**: under the `arch/cpu/arm/` root only common files are allowed; an MCU-specific `.c` there is a violation.
- **The common layer must not `#include` any vendor header**: `cpu.h` may only dispatch on MCU macros (`#if defined(PY32F040xB) … #elif defined(GD32F50X) … #else #error`). Hardcoding one vendor's HAL will make the second MCU fail to compile outright.
- **Do not rely on "accidental" include transitive chains**: if a common `.c` uses CMSIS intrinsics (`__get_PRIMASK`, etc.), it must `#include "cpu.h"` itself. For example, `int-master.c` gets it via `rtimer-arch.h` → `cpu.h`; if GD32's `rtimer-arch.h` is written differently, the chain breaks, the error reported is `undefined reference`, and the call site gives no clue why.
- **`mkdir $@` must be written as `mkdir -p $@`**: otherwise, when the target directory already exists, `make -B` full rebuilds fail immediately.
- **Repeated Edits to the same file overwrite each other on write-back**. When modifying descriptor files, prefer finishing them in one pass with a script, then grep to re-verify the on-disk values right away; do not pick at the same file with repeated Edits.
- **`<Define>` / `<IncludePath>` appear in more than one place in `.uvprojx`**: `<TargetCommonOption>` also has same-named empty tags. Only change the pair inside the `<Cads>` block, otherwise the change accomplishes nothing.
- **`<Groups>` and the `FilePath` entries within the groups must align item by item with the Makefile's `C_SOURCES`**. Generators easily omit "one-of-a-kind files that fall into an empty group" — e.g., omitting `system_gd32f50x.c` means `SystemInit()` never makes it into the image. That is why the step-7 consistency check is not optional.
- **The stack depth must be the same across both toolchains**: `Stack_Size` in `Source/ARM/startup_<device>.s` must equal `_Min_Stack_Size` in `Source/GCC/Ld/*.ld`. Vendor defaults are typically an unanalyzed 1 KiB. armlink also requires this startup file to export `Stack_Mem` / `Stack_Limit` (used by `kern-stack.c`), but it must **not** also export `__initial_sp` (`L6915E`).
- **armasm needs `--pd="__MICROLIB SETA 1"`** (when `useUlib=1`). Vendor startup files commonly use the either-or `IF :DEF:__MICROLIB`; omitting this predefine triggers `Undefined symbol __use_two_region_memory`, which looks like a broken C library but is really an assembly-predefine issue.
- **`Source/GCC/startup_*.S` and `Source/ARM/startup_*.s` are two variants of the same logical file**, and the directories may differ only in case. When comparing cross-toolchain inventories, treat the two as equivalent first; otherwise the check is always red.
- **CMSIS 5.4.0 does not have `cmsis_armclang.h`**. Without it, armclang would have to reference an absolute pack path; copying that file into `arch/cpu/arm/CMSIS/Core/Include/` is what lets both Keil projects use only repo-relative paths.

---

## 5. Example: GD32F502VE Port Parameters

| Item | Value |
|---|---|
| Core | Cortex-M33 (`-mcpu=cortex-m33`, no FPU compile option; the bundled startup file uses `.fpu softvfp`) |
| Clock | HXTAL 8 MHz → PLL0P ×40/2 = 160 MHz (`-DGD32F502` selects `__SYSTEM_CLOCK_160M_PLL_HXTAL`) |
| UART | USART0, PB6=TX / PB7=RX, `GPIO_AF_0`, 115200 8-N-1; RX via DMA0 CH4 + IDLE framing (CIRCULAR ring), TX polled |
| rtimer | TIMER0 (APB2 160 MHz), prescaler 159 → 1 MHz, `RTIMER_ARCH_SECOND` defined in this MCU's `rtimer-arch.h` |
| Partitions | QBOOT 24 K @`0x08000000`, META 4 K @`0x08006000` (wear-levelled flag + manifest), APP @`0x08007000`–`0x0807FFFF`, erase page 2 KB (bank0) |
| `libopt.h` | rcu/gpio/usart/timer/fmc/pmu **+ misc + syscfg** (the last two are compile-mandatory) |

When adding a new MCU, if the CMSIS copy bundled with the vendor library is older than the repo's, **upgrade only the core headers actually used**;
do not overwrite with the whole pack.

---

## 6. GD32F502VE Build Descriptors

| Descriptor file | Toolchain | Key values |
|---|---|---|
| `user/gd32f502ve/MDK-ARM/APP.uvprojx` | armclang V6.16 / armlink | `GD32F502VE` + `GigaDevice.GD32F50x_DFP.1.0.1`, `<Cpu>` contains `CPUTYPE("Cortex-M33") DSP` **without FPU**, `FlashDriverDll` uses `UL2V8M` (V8-M specific), `TargetDllName` `SARMV8M.DLL -pCM33`, ROM `0x08007000/0x79000` |
| `user/gd32f502ve/MDK-ARM/QBOOT.uvprojx` | Same as above | ROM `0x08000000/0x6000`, macro changed to `-DQBOOT_BUILD` |
| `user/gd32f502ve/EIDE/.eide/eide.json` | arm-none-eabi-gcc | `cpuType Cortex-M33` / `floatingPointHardware none` / `$float-abi-type soft`, scatter `gd32f502xE_app.ld` |

For the step-by-step GNU/EIDE build (toolchain, partition map, build targets, flashing, measured sizes), see `doc/gd32f502ve-eide-build.md` (and `doc/gd32f502ve-eide-build.zh-CN.md`).

**FPU decision (important)**: although `Dfpu=1` in terms of hardware capability, neither side **uses floating point**.
Once `FPU3(SFPU)` + `-mfloat-abi=hard` is enabled, the exception frame gains an extra S0–S15 + FPSCR, 68 bytes in total,
and the watermark peak in `kern-stack.c` is no longer the same number. To enable it in the future, you must change
`<Cpu>`, the `-mfpu/-mfloat-abi` in both Makefiles, and re-measure the stack peaks of the three deep paths, all at once.

**EIDE has only one target, APP** (following the PY32F040xB practice): QBOOT goes through `make -f Makefile.qboot` or the Keil `QBOOT.uvprojx`.

**Download/debug configuration**: flashing goes through Keil UV4; `.vscode/tasks.json` provides only three build tasks.

Size (measured with armclang): APP **13,792 B** / 446,464 B; QBOOT **15,816 B** / 24,576 B (~35.6% left over).
The same configuration under GNU is 14,368 B / 19,524 B — Keil QBOOT is 3,708 B smaller than GNU.

> **Note**: the figures above are **development-process measurements**; they vary with software version and toolchain changes and are **not a delivery specification**.
