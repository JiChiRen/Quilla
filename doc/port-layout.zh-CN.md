**[中文](port-layout.zh-CN.md) · [English](port-layout.md)**

# 多 MCU 移植布局规范

- **状态**：现行（2026-10-08 起）
- **关联**：`doc/cmsis-layout.md`（CMSIS-Pack 目录规范）
- **适用范围**：`arch/cpu/arm/`（含其 `platform/`）与 `user/`

> **RISC-V 移植（CH32V305VB，`ch32v305rb`）**：同样的三层边界适用于
> `arch/cpu/riscv/`、`arch/platform/riscv/` 与 `user/ch32v305rb/`，但构建为
> **仅 EIDE / GNU GCC12**（无 Keil），且启动镜像链在 `0x00000000` Flash 别名区，
> 故整套分区方案为 `0x0` 基址而非 `0x0800` 基址。见 `doc/riscv-ch32v305-build.md`。

---

## 1. 目标

同一仓库里并存多颗 MCU，且**任何一颗的改动不碰另一颗的源清单**。
三层分界：

| 层 | 内容 | 是否随 MCU 变 |
|---|---|---|
| `os/` | 内核、shell 服务、QIAP 协议与安全启动 | 否（`os/` 全仓零引用 `arch/` 与厂商头，已实测） |
| `arch/cpu/arm/` 通用层 | `cpu.h`、`arm-def.h`、`int-master.c`、`dbg.c` | 否 |
| `arch/cpu/arm/<mcu>/` | clock / rtimer-arch / soc / shellport / lpm / dev / qboot-hal / qboot-main | **是** |
| `arch/platform/arm/<mcu>/` | `contiki-conf.h`、`platform.c` | **是** |
| `user/<mcu>/` | 该 MCU 的工程树（EIDE / MDK-ARM / Src / Inc） | **是** |
| `arch/cpu/arm/CMSIS/`、`arch/cpu/arm/vendor/` | 器件包与厂商固件库（所有 MCU 共用） | 共享 |

`arch/platform/arm/inttypes.h` 是唯一的例外：它与 MCU 无关，留在
`arch/platform/arm/` 下，因此该目录与 `arch/platform/arm/<mcu>/`
**都要**在 include 路径里。

---

## 2. 目录

```
arch/cpu/arm/
├── cpu.h  arm-def.h  int-master.c  dbg.c      # ARM 通用
├── CMSIS/                                     # 见 doc/cmsis-layout.md
├── vendor/<Vendor>/<Library>/
└── <mcu>/                                     # 每颗 MCU 一份
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

`<mcu>` 取值：`py32f040xb`、`gd32f502ve`（小写、与器件包名对应）。

---

## 3. 新增一颗 MCU 的检查清单

按顺序做，每步都假定前一步已就位。

1. **器件包**：`arch/cpu/arm/CMSIS/Device/<Vendor>/<Device>/`
   含 `Include/`、启动文件（`Source/{ARM,GCC,IAR}/`）、`system_<device>.c`、
   链接脚本（`Source/GCC/Ld/`、`Source/IAR/Ld/`）。
2. **厂商库**：`arch/cpu/arm/vendor/<Vendor>/<Library>/`，库内自带 CMSIS 副本必须删除。
3. **MCU 层**：`arch/cpu/arm/<mcu>/` 实现上表八个文件（+ `dev/`）。
   源内相对引用 `os/` 时注意深度 —— 在 `<mcu>/` 下是 `../../../../os/`。
4. **平台层**：`arch/platform/arm/<mcu>/{contiki-conf.h,platform.c}`。
5. **工程树**：`user/<mcu>/`，两个 GNU Makefile（APP / QBOOT）+ `eide.json`
   + 两个 Keil `.uvprojx`。目录深度必须保持两级（`user/<mcu>/EIDE`），
   否则 `../../../` 形式的路径全部失效。
6. **构建登记五处**：`Makefile`、`Makefile.qboot`、`.eide/eide.json`、
   `APP.uvprojx`、`QBOOT.uvprojx`。改完**逐组 grep 复核落盘值**。
7. **描述文件校验**：确认三份 Keil 描述（`APP.uvprojx` / `QBOOT.uvprojx`）与 Makefile 的源文件 / include / 宏完全一致。自动化的 `tools/verify_gd32_keil_project.py` 是**内部开发工具，不随公开版本发布**（仅私有镜像内含）。
8. **验证**：`make -f Makefile` 与 `make -f Makefile.qboot` 各出 elf；再用 Keil 以 armclang/armasm/armlink 真编译链接一遍确认工程。`tools/verify_gd32_keil_build.py` 同样是**内部开发工具，不公开**。
   > 注意：Keil 工程修改后，IDE 在退出时会把部分设置回写默认值，建议用文本方式修改
   > `.uvprojx`（`ElementTree.write()` 不起效）。

---

## 4. 约束与注意事项

- **改 Keil 工程时 UV4 必须关闭。** 它在退出时会把 `.uvprojx` 写回，可能把 `<IROM>` 改回默认值；也可能锁住 `MDK-ARM/` 目录导致无法重命名。文件被锁就停下来让人处理，不要用变通路径绕。
- **include 深度**：文件从 `arch/cpu/arm/` 移到 `arch/cpu/arm/<mcu>/` 后，所有 `"../../../os/..."` 要改成 `"../../../../os/..."`。
- **Makefile 续行**：`C_INCLUDES` 里除最后一项外每行必须以 ` \` 结尾，插入新行时漏掉会静默吞掉后续所有 include。
- **单颗 MCU 的改动不得出现在通用层**：`arch/cpu/arm/` 根目录下只允许放通用文件，出现 MCU 专属 `.c` 就是违规。
- **通用层不得 `#include` 任何厂商头**：`cpu.h` 只能按 MCU 宏分派（`#if defined(PY32F040xB) … #elif defined(GD32F50X) … #else #error`）。硬编码一个厂商 HAL 会让第二颗 MCU 直接编译不过。
- **不要依赖"偶然的" include 传递链**：通用 `.c` 用到 CMSIS 内建函数（`__get_PRIMASK` 等）就必须自己 `#include "cpu.h"`。例如 `int-master.c` 靠 `rtimer-arch.h` → `cpu.h` 拿到，GD32 的 `rtimer-arch.h` 写法不同就断了，报的是 `undefined reference`，看调用点看不出原因。
- **`mkdir $@` 要写成 `mkdir -p $@`**：否则目标目录已存在时 `make -B` 全量重编直接失败。
- **同一个文件连续多次 Edit 会写回覆盖**。改描述文件优先用脚本一次写完，写完立刻 grep 复核落盘值，不要用 Edit 反复抠同一个文件。
- **`<Define>` / `<IncludePath>` 在 `.uvprojx` 里不止一处**：`<TargetCommonOption>` 下也有同名空标签。只改 `<Cads>` 块内的那一对，否则改动不生效。
- **`<Groups>` 与分组里的 `FilePath` 必须和 Makefile 的 `C_SOURCES` 逐项对齐**。生成器容易漏掉"只此一个、落进空组"的文件 —— 例如漏了 `system_gd32f50x.c`，后果是 `SystemInit()` 不进镜像。所以第 7 步的一致性校验不是可选项。
- **两个工具链的栈深度必须一样**：`Source/ARM/startup_<device>.s` 的 `Stack_Size` 要等于 `Source/GCC/Ld/*.ld` 的 `_Min_Stack_Size`。厂商默认值通常是不加分析的 1 KiB。armlink 还需要这个启动文件导出 `Stack_Mem` / `Stack_Limit`（`kern-stack.c` 用），但**不能**同时导出 `__initial_sp`（`L6915E`）。
- **armasm 要带 `--pd="__MICROLIB SETA 1"`**（当 `useUlib=1`）。厂商启动文件普遍用 `IF :DEF:__MICROLIB` 二选一，漏了这个预定义会报 `Undefined symbol __use_two_region_memory`，看上去像 C 库坏了，其实是汇编预定义的事。
- **`Source/GCC/startup_*.S` 与 `Source/ARM/startup_*.s` 是同一逻辑文件的两个变体**，目录不同还可能只差大小写。做跨工具链清单比对时要先把二者视为等价，否则检查永远红。
- **CMSIS 5.4.0 没有 `cmsis_armclang.h`**。缺它 armclang 就要引一个绝对 pack 路径；把该文件复制进 `arch/cpu/arm/CMSIS/Core/Include/` 才能让两个 Keil 工程只用仓库相对路径。

---

## 5. 示例：GD32F502VE 移植参数

| 项 | 取值 |
|---|---|
| 内核 | Cortex-M33（`-mcpu=cortex-m33`，无 FPU 编译选项，库内启动文件用 `.fpu softvfp`） |
| 时钟 | HXTAL 8 MHz → PLL0P ×40/2 = 160 MHz（`-DGD32F502` 即选中 `__SYSTEM_CLOCK_160M_PLL_HXTAL`） |
| 串口 | USART0，PB6=TX / PB7=RX，`GPIO_AF_0`，115200 8-N-1；RX 走 DMA0 CH4 + IDLE 定帧（CIRCULAR 环），TX 轮询 |
| rtimer | TIMER0（APB2 160 MHz），预分频 159 → 1 MHz，`RTIMER_ARCH_SECOND` 写在本 MCU 的 `rtimer-arch.h` 内 |
| 分区 | QBOOT 24 K @`0x08000000`、META 4 K @`0x08006000`（磨损均衡 flag + manifest）、APP @`0x08007000`–`0x0807FFFF`、擦除页 2 KB（bank0） |
| `libopt.h` | rcu/gpio/usart/timer/fmc/pmu **+ misc + syscfg**（后两项是编译强制） |

新增 MCU 时若厂商库自带 CMSIS 副本比仓库旧，**只提升用到的 core 头**，
不要整包覆盖。

---

## 6. GD32F502VE 的构建描述

| 描述文件 | 工具链 | 关键值 |
|---|---|---|
| `user/gd32f502ve/MDK-ARM/APP.uvprojx` | armclang V6.16 / armlink | `GD32F502VE` + `GigaDevice.GD32F50x_DFP.1.0.1`，`<Cpu>` 含 `CPUTYPE("Cortex-M33") DSP` **不含 FPU**，`FlashDriverDll` 用 `UL2V8M`（V8-M 专用），`TargetDllName` `SARMV8M.DLL -pCM33`，ROM `0x08007000/0x79000` |
| `user/gd32f502ve/MDK-ARM/QBOOT.uvprojx` | 同上 | ROM `0x08000000/0x6000`，宏换 `-DQBOOT_BUILD` |
| `user/gd32f502ve/EIDE/.eide/eide.json` | arm-none-eabi-gcc | `cpuType Cortex-M33` / `floatingPointHardware none` / `$float-abi-type soft`，scatter `gd32f502xE_app.ld` |

逐步的 GNU/EIDE 构建（工具链、分区表、构建目标、烧录、实测尺寸）见 `doc/gd32f502ve-eide-build.md`（及 `doc/gd32f502ve-eide-build.zh-CN.md`）。

**FPU 决定（重要）**：虽然在硬件能力上 `Dfpu=1`，但两侧都**不使用浮点**。
一旦启用 `FPU3(SFPU)` + `-mfloat-abi=hard`，异常帧多出 S0–S15 + FPSCR 共 68 字节，
`kern-stack.c` 的 watermark 峰值就不再是同一个数。将来要开，必须同时改
`<Cpu>`、两个 Makefile 的 `-mfpu/-mfloat-abi`，并重测三条深路径栈峰值。

**EIDE 只有 APP 一个 target**（沿用 PY32F040xB 做法）：QBOOT 走 `make -f Makefile.qboot` 或 Keil `QBOOT.uvprojx`。

**下载/调试配置**：烧录走 Keil UV4，`.vscode/tasks.json` 只给三条构建任务。

体积（armclang 实测）：APP **13,792 B** / 446,464 B；QBOOT **15,816 B** / 24,576 B（余 ~35.6%）。
同配置 GNU 是 14,368 B / 19,524 B —— Keil QBOOT 比 GNU 小 3,708 B。

> **注意**：以上数据均为**开发过程实测值**，会随软件版本与工具链变化而不同，**不构成交付规格**。
