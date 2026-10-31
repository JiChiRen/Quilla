**[English](cmsis-layout.md) · [中文](cmsis-layout.zh-CN.md)**

# CMSIS layout for arch/cpu/arm

This document is the **current specification** for the `arch/cpu/arm` directory structure, using the CMSIS-Pack layout (core / device / vendor library layering).

## Directory tree

```
arch/cpu/arm/
├── CMSIS/
│   ├── Core/Include/                 Core and compiler abstraction layer (CMSIS-Core(M) 5.4.0)
│   │                                 core_cm0plus.h / core_cm33.h / mpu_armv8.h /
│   │                                 cmsis_gcc.h / cmsis_armclang.h / cmsis_iccarm.h …
│   ├── DSP/{Include,Source}/         CMSIS-DSP (not enabled in this project, kept as pack material)
│   ├── RTOS/Include/cmsis_os.h       CMSIS-RTOS v1 API (not enabled)
│   └── Device/<Vendor>/<Device>/
│       ├── Include/                  Device headers + system_<device>.h
│       └── Source/
│           ├── system_<device>.c
│           ├── ARM/startup_<device>.s    (armasm)
│           ├── GCC/startup_<device>.S    (GNU as)  + Ld/*.ld
│           └── IAR/startup_<device>.s             + Ld/*.icf
└── vendor/<Vendor>/<Library>/        Vendor firmware library (not part of CMSIS, placed separately)
```

Devices currently integrated:

| Vendor | Device | Core | Status |
|---|---|---|---|
| Puya | PY32F040xB | Cortex-M0+ | In build (APP / QBOOT) |
| GigaDevice | GD32F50x | Cortex-M33 | In build (APP / QBOOT; EIDE + Keil); QBOOT_CHECK / QBOOT_SEC verified on hardware |

## Notes on adding / moving device files

When adding a new MCU, if the vendor firmware library ships its own copy of CMSIS, the core headers such as `core_cm*.h` and `mpu_armv8.h` must be promoted to the unified `Core/Include/`; discard all older-version `cmsis_*.h` files to avoid downgrading the unified core headers to an older version.

`cmsis_armclang.h` does not exist in CMSIS 5.4.0 (that version stops at `cmsis_armcc.h`), yet armclang must have it to compile `core_cm*.h`. It is therefore copied from CMSIS pack 5.8.0 into `Core/Include/`, so that the include paths of both Keil projects are **entirely repo-relative**, not depending on which pack happens to be installed on a given machine. The still-missing `cmsis_armclang_ltm.h` is only included when `__ARMCC_VERSION < 6100100`, which the current V6.16 does not need.

## Places to sync when changing a path

After adding or moving device files, the following **five** places per MCU must be kept in sync (when modifying Keil projects, **Keil must not be open** — it writes back default values for project settings such as `<IROM>` on exit):

1. `user/<mcu>/EIDE/Makefile` (APP, GNU)
2. `user/<mcu>/EIDE/Makefile.qboot` (QBOOT, GNU)
3. `user/<mcu>/EIDE/.eide/eide.json` (EIDE, APP target only)
4. `user/<mcu>/MDK-ARM/APP.uvprojx`
5. `user/<mcu>/MDK-ARM/QBOOT.uvprojx`

Currently in sync for both `py32f040xb` and `gd32f502ve`.

## Verification

After layout changes, rebuild to verify (GNU / armclang each produce an elf; confirm link rc 0).
