**[中文](cmsis-layout.zh-CN.md) · [English](cmsis-layout.md)**

# arch/cpu/arm 的 CMSIS 布局

本文是 `arch/cpu/arm` 目录结构的**现行规范**，采用 CMSIS-Pack 布局（内核 / 器件 / 厂商库分层）。

## 目录树

```
arch/cpu/arm/
├── CMSIS/
│   ├── Core/Include/                 内核与编译器抽象层（CMSIS-Core(M) 5.4.0）
│   │                                 core_cm0plus.h / core_cm33.h / mpu_armv8.h /
│   │                                 cmsis_gcc.h / cmsis_armclang.h / cmsis_iccarm.h …
│   ├── DSP/{Include,Source}/         CMSIS-DSP（本项目未启用，作为 pack 素材保留）
│   ├── RTOS/Include/cmsis_os.h       CMSIS-RTOS v1 API（未启用）
│   └── Device/<Vendor>/<Device>/
│       ├── Include/                  器件头 + system_<device>.h
│       └── Source/
│           ├── system_<device>.c
│           ├── ARM/startup_<device>.s    (armasm)
│           ├── GCC/startup_<device>.S    (GNU as)  + Ld/*.ld
│           └── IAR/startup_<device>.s             + Ld/*.icf
└── vendor/<Vendor>/<Library>/        厂商固件库（不属于 CMSIS，单独放）
```

当前已纳入的两颗器件：

| Vendor | Device | 内核 | 状态 |
|---|---|---|---|
| Puya | PY32F040xB | Cortex-M0+ | 已在构建（APP / QBOOT） |
| GigaDevice | GD32F50x | Cortex-M33 | 已在构建（APP / QBOOT；EIDE + Keil）；QBOOT_CHECK / QBOOT_SECURED 已上板验证 |

## 新增 / 移动器件文件的注意事项

新增 MCU 时若厂商固件库自带 CMSIS 副本，须把其中的 `core_cm*.h`、`mpu_armv8.h` 等内核头提升到统一的 `Core/Include/`；版本较旧的 `cmsis_*.h` 全部丢弃，避免把统一内核头回退到旧版本。

`cmsis_armclang.h` 在 CMSIS 5.4.0 里没有（该版本止于 `cmsis_armcc.h`），而 armclang 编译 `core_cm*.h` 时必须拿到它。因此将它从 CMSIS pack 5.8.0 复制进 `Core/Include/`，让两个 Keil 工程的 include 路径**全是仓库相对路径**，不依赖某台机器上恰好装着哪个 pack。仍缺的 `cmsis_armclang_ltm.h` 只在 `__ARMCC_VERSION < 6100100` 时才被 include，当前 V6.16 用不到。

## 改动一处路径，需要同步的地方

新增或移动器件文件后，每颗 MCU 的以下**五处**必须同步（改 Keil 工程时 **Keil 不能开着**，它会在退出时把 `<IROM>` 等项目设置写回默认值）：

1. `user/<mcu>/EIDE/Makefile`（APP，GNU）
2. `user/<mcu>/EIDE/Makefile.qboot`（QBOOT，GNU）
3. `user/<mcu>/EIDE/.eide/eide.json`（EIDE，仅 APP target）
4. `user/<mcu>/MDK-ARM/APP.uvprojx`
5. `user/<mcu>/MDK-ARM/QBOOT.uvprojx`

当前 `py32f040xb` 与 `gd32f502ve` 两颗均已同步。

## 验证

布局改动后请重新构建验证（GNU / armclang 各出 elf，确认链接 rc 0）。
