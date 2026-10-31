**[English](gd32f502ve-eide-build.md) · [中文](gd32f502ve-eide-build.zh-CN.md)**

# Quilla 在 GD32F502VE 上 — EIDE / GNU 构建指南

- **状态**：Active —— 已上板实测
- **范围**：用 **GNU Arm Embedded 工具链** 通过 `user/gd32f502ve/EIDE/` 下的 EIDE 工程构建 **GD32F502VE**（Cortex-M33）的 QBOOT bootloader 与 QAPP 应用。Keil/MDK 工程见 `doc/keil-qboot-app-build.md`；多 MCU 布局见 `doc/port-layout.md`。
- **相关**：`doc/secure-boot.md`（CHECK / SEALED / SEC 变体与密钥）、`doc/upgrade-flow.md`（QIAP 主机升级）。

---

## 1. 工具链

| 项 | 值 |
|---|---|
| 编译器 | **Arm GNU Toolchain** `arm-none-eabi-gcc`（本项目以 **13.2.1** 构建并验证；近期的 12/13 系列均可） |
| 架构标志 | `-mcpu=cortex-m33 -mthumb`，软浮点（无 `-mfpu`/`-mfloat-abi`；配套启动文件用 `.fpu softvfp`） |
| C 库 | newlib-nano（`-specs=nano.specs`）；系统调用桩来自 `-lnosys` |
| 构建系统 | 经 EIDE 工程的 GNU Make（`user/gd32f502ve/EIDE/`）；同一套 Makefile 也可独立运行 |

将工具链 `bin/` 目录加入 `PATH`，**或**每次构建用 `GCC_PATH` 传入（仓库不记录绝对路径）：

```bash
cd user/gd32f502ve/EIDE
make -f Makefile            GCC_PATH=/path/to/gcc-arm-none-eabi/bin   # QAPP
make -f Makefile.qboot      GCC_PATH=...                               # QBOOT_CHECK
```

Debian/Ubuntu 上工具链即 `gcc-arm-none-eabi` 包；macOS 用 `brew install --cask gcc-arm-embedded`；Windows 用 [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) 安装包。

## 2. 器件与时钟

GD32F502VE —— Arm Cortex-M33，**512 KiB Flash @ `0x08000000`**，64 KiB SRAM @ `0x20000000`。构建宏为 `GD32F502`（配合 `GD32F50X`）。系统时钟：**8 MHz HXTAL → PLL → 160 MHz**（`__SYSTEM_CLOCK_160M_PLL_HXTAL`）；`SystemCoreClock` = 160 MHz。

## 3. Flash / 分区表

所有 QBOOT/APP 地址均为 `0x0800` 基址。`arch/cpu/arm/gd32f502ve/soc.h` 里的 `SOC_FLASH_END` 是 APP 末地址的唯一来源；擦/写页为 **2 KiB**（bank0）。

| 区域 | 地址 | 长度 | 说明 |
|---|---|---|---|
| **QBOOT** | `0x08000000` | 24 KiB | bootloader；链接区限 24 K（超出即链接错误） |
| **META** | `0x08006000` | 4 KiB | 磨损均衡 flag + 安全启动 manifest（双 2 KiB 槽）；两镜像均不占用 |
| **APP** | `0x08007000` | 至 `0x08007FFFF` | 应用；`SOC_FLASH_END = 0x08007FFFF` |
| **secver** | `0x08007400` | 8 B | 防回滚固件安全版本（`APP_START + 0x400`），QBOOT 读取 |

- **QAPP 向量表重定位**：`main()`（`QAPP_BUILD`）中设置 `SCB->VTOR = 0x08007000`，故 APP 链在 `0x08007000` 合法；`quilla_qboot_hal_jump_to_app()` 在跳转前再次设置 VTOR。
- **QBOOT 跳 APP**：`arch/cpu/arm/gd32f502ve/quilla-qboot-hal.c` 的 `quilla_qboot_hal_jump_to_app()` 读取 APP 初始 SP / 复位向量，设置 `SCB->VTOR`、装载 MSP 并跳转。

## 4. 控制台

USART0，**TX = PB6**，**RX = PB7**，`GPIO_AF_0`，115200 8-N-1。RX 由 DMA 驱动：DMA0 通道 4（DMAMUX 请求 `DMA_REQUEST_USART0_RX`）CIRCULAR 模式填充 512 字节环；USART IDLE 中断（帧定界）与 DMA 半满/全满中断只轮询 shell 进程，`shellport_rx_harvest()` 读实时 DMA 计数拷出新字节。TX 轮询，保证 QBOOT 交棒 APP 前最后一字节已上线。

## 5. 构建目标

```bash
make -f Makefile              # -> QAPP/QAPP.{elf,hex,bin}
make -f Makefile.qboot        # -> QBOOT_CHECK   （仅 SHA-256 完整性）
make -f Makefile.qboot.sealed # -> QBOOT_SEALED  （默认安全变体）
make -f Makefile.qboot.sec    # -> QBOOT_SEC     （每次启动 Ed25519 验签，量产）
```

三个 QBOOT 变体仅在 `C_DEFS` 上不同（`QUILLA_SB_BOOT_MODE` + `QUILLA_QBOOT_VARIANT_STR`；SEC 另设 `QUILLA_PRODUCTION_BUILD`）。SEALED 与 SEC 出厂编译进**同一把默认公钥**——量产前请替换为你自己的（见 `doc/secure-boot.md` §3.1 与 `quilla-qboot-pubkey.h` 头部注释）。

## 6. 产物与烧录

每个目标在其构建目录下产出 `.elf`、`.hex` 与 `.bin`。将 QBOOT 烧到 `0x08000000`、APP 烧到 `0x08007000`（主机升级工具在运行时经 `pinfo` shell 命令从设备读取精确的 APP 区间，无需按 MCU 硬编码）。经串口升级 APP 用 `tools/quilla_host_upgrader.py` —— 见 `doc/quilla-host-upgrader-guide.md`。

> 用调试器分别烧 QBOOT 与 APP 时，**按扇区擦除，切勿整片擦除**——整片擦除会连 META 区（flag + manifest）一起抹掉。

## 7. 使用 EIDE IDE

将 `user/gd32f502ve/EIDE/` 作为 EIDE 工程打开（`.eide/eide.json`）。EIDE 仅构建 **QAPP** 目标（PY32F040xB/GD32 约定）；QBOOT 及其变体用 `make -f Makefile.qboot[.sealed|.sec]` 在终端构建，或用 Keil 的 `QBOOT.uvprojx`。`.vscode/tasks.json` 下的构建任务镜像各 Makefile。

## 8. 实测尺寸（开发过程数据，非交付规格）

以 GNU Arm Embedded `arm-none-eabi-gcc` **13.2.1** 测得（见 §1）。QBOOT 区由链接脚本硬性限制为 24 KiB；任何溢出都是链接错误，而非运行期故障。

| 镜像 | text | data | bss |
|---|---|---|---|
| QAPP | 17 908 | 152 | 11 496 |
| QBOOT_CHECK | 21 236 | 116 | 18 344 |
| QBOOT_SEALED | 21 336 | 116 | 18 344 |
| QBOOT_SEC | 21 288 | 116 | 18 344 |

> 数字随工具链与软件版本变化。Keil（armclang）产出的 QBOOT 比 GNU 小（见 `doc/keil-qboot-app-build.md`）；上表仅为 GNU 数据。QBOOT 的 `bss` 数字包含链接脚本预留的 8 KiB 栈与堆，实际未初始化数据远小于此。

## 9. 编译警告（预期，良性）

干净构建会打印少量警告，均为预期且可忽略：

- **默认公钥 `#warning`**（仅 SEALED / SEC）：*"QUILLA_QBOOT_DEFAULT_KEY is compiled in: this is the REPOSITORY DEFAULT public key … 量产前请替换为你自己的密钥对……"* —— 故意的提醒。仓库随附一把真实可用的 Ed25519 密钥对，使全新 clone 即可构建并启动；其私钥公开已知（`tools/_keytest/private.hex`），故**量产前必须自备密钥**。见 `doc/secure-boot.md` §3.1。
- **newlib `_write` / `_read` / `_close` … "not implemented"**：`nosys` 系统调用桩；固件不使用文件系统。
- **GigaDevice 厂商 RCU 驱动的 `-Wmaybe-uninitialized`**：厂商代码警告，不在 Quilla 代码中。

## 10. 第三方组件

`arch/cpu/arm/vendor/GigaDevice/` 与 `arch/cpu/arm/CMSIS/` 下的外设驱动与启动文件为厂商提供（GigaDevice / Arm），各自带有厂商版权头；按仓库的第三方条款再分发。Quilla 自有代码为 BSD-3-Clause（见 `LICENSE`）。
