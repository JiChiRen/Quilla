**[English](riscv-ch32v305-build.md) · [中文](riscv-ch32v305-build.zh-CN.md)**

# Quilla 在 CH32V305VB（RISC-V）上 — EIDE / GNU GCC12 构建

- **状态**：Active —— 首次 bring-up（2026-10-21）：已用 GCC12 全程构建通过，**尚未上板实测**
- **范围**：仅 `ch32v305rb` 移植。Cortex-M 端口（PY32F040xB、GD32F502VE）用 Keil + GNU 构建，见 `doc/keil-qboot-app-build.md`；**本 MCU 仅 EIDE / GNU Makefile 构建——没有也不计划 Keil/MDK 工程。**
- **相关**：`doc/port-layout.md`（多 MCU 布局）、`doc/secure-boot.md`、`doc/upgrade-flow.md`。

---

## 1. 工具链

| 项 | 值 |
|---|---|
| 编译器 | **xPack GNU RISC-V Embedded GCC 12.2.0**（MounRiver Studio 2 自带的 "RISC-V Embedded GCC12"），前缀 `riscv-wch-elf-` |
| 架构 / ABI | `-march=rv32imac -mabi=ilp32`（软浮点；不用 FPU，与 GD32"不用浮点"决策一致） |
| C 库 | newlib-nano（`-specs=nano.specs`）+ 系统调用桩（`-specs=nosys.specs`）；shell 的 `vsnprintf` 否则会拖入全量 printf |
| 启动 | `-nostartfiles` —— WCH 启动文件已提供 `_start` / `handle_reset`；不加会与工具链自带 `crt0` 的 `_start` 冲突 |
| 中断属性 | 每个覆盖 WCH 启动弱符号的句柄都用 `__attribute__((interrupt("WCH-Interrupt-fast")))` |

工具链 `bin/` 目录**不记录进仓库**。每次构建时用 `PATH` 或 `GCC_PATH` 指定：

```bash
make -C user/ch32v305rb/EIDE -f Makefile              GCC_PATH=/path/to/RISC-V\ Embedded\ GCC12/bin
make -C user/ch32v305rb/EIDE -f Makefile.qboot        GCC_PATH=...
```

> 目录名含空格（"RISC-V Embedded GCC12"）；Makefile 已对 `$(CC)/$(AS)/$(CP)/$(SZ)` 加引号，故可正常工作。用无空格的 `PATH` 可完全避开。

### CI 验证：绿灯意味着什么、不意味着什么

GitHub Actions 无法运行 WCH 工具链——它不可再分发——所以 `firmware-riscv` 这个
job 用标准 `riscv-none-elf` 工具链（xPack GCC 13.2）构建本移植，构建前先把唯一一处
厂商专有写法就地改写：

```
__attribute__((interrupt("WCH-Interrupt-fast")))   ->   __attribute__((interrupt("machine")))
```

该 job 捕获的是共用代码的通用回归——语法、类型、符号缺失、链接脚本漂移。它**不**能
证明镜像能跑：快速中断入口路径只在 WCH 工具链下存在，且 bring-up 仍未完成（§8）。
CI 绿灯与实板状态无关。

## 2. 器件

CH32V305VB = WCH **CH32V30x_D8C**（RV32IMAC，青稞 QingKe V4F 内核 + WCH PFIC 中断控制器）。它是以 Cortex-M 价位提供的通用 RISC-V；最突出的工业特性是**同等低价位下的 USBHS 与工业级温度支持**，引脚与外围兼容的 CH32V307 同属 CH32V30x 家族，额外提供 1G MAC + 10M PHY，可快速衍生网络产品。**128 KiB Flash 链在 `0x00000000` 别名区**，32 KiB SRAM @ `0x20000000`。构建宏为 `CH32V30x_D8C`。

> 与 Cortex-M 端口不同，本 MCU 的启动镜像位于 **`0x00000000`** Flash 别名区（WCH 启动/链接惯例），**不是** `0x08000000`。因此本端口的整套 QBOOT/APP 方案、`SOC_FLASH_END`、`-DQUILLA_QBOOT_*` 宏都是 `0x0` 基址。`0x08000000` 仍是同一物理 Flash 的读别名。

> **厂商 SDK（WCH）**：本端口的一部分——`arch/cpu/riscv/ch32v305rb/` 下的 `Peripheral/`（外设驱动）、`Core/`、`Startup/`、`system_ch32v30x.*` 与 `Debug/`——为 **WCH 厂商 SDK**，保留 WCH 自带的上游头文件原样收录（Copyright Nanjing Qinheng Microelectronics Co., Ltd.）。**使用范围 / 许可（本项目裁定）**：WCH 源代码**仅用于 WCH 芯片**（此处即 CH32V305VB）；其未设 copyleft / 传染性许可，故收录不影响 Quilla 自有 BSD-3-Clause 代码。**本项目只引用这些文件，不对其另加版权或许可主张。** 已核实：任何 Cortex-M（PY32 / GD32）构建均不引用 `arch/cpu/riscv/` 或 `arch/platform/riscv/` 下任何文件。Quilla 自有代码为 BSD-3-Clause（见 `LICENSE`）。

## 3. 时钟与时基

- **系统时钟**：HSE 8 MHz → PLL → **96 MHz**（`system_ch32v30x.c` 的 `SYSCLK_FREQ_96MHz_HSE`）；HCLK = 96 MHz，APB2 = 96 MHz，APB1 = 48 MHz。`SystemCoreClock` = 96 000 000。
- **rtimer**：TIM1（APB2，96 MHz）预分频 96 → **1 MHz**（1 µs/tick）；16 位计数 + 软件溢出合成（同 GD32 TIMER0 方案）。
- **软件时钟**：WCH RV SysTick（`0xE000F000`）按 `CLOCK_SECOND` 触发；64 位 tick 由 rtimer 派生，同 GD32。
- **看门狗**：IWDG，LSI（~40 kHz）时钟；`dev/qclk.c` 用 RTC 分频器测 LSI 对 rtimer 的比值以收窄超时带——CH32V305 上对应 GD32 的 FWDGT/IRC40K。

## 4. 分区（0x0 基址）

| 区域 | 地址 | 长度 | 说明 |
|---|---|---|---|
| **QBOOT** | `0x00000000` | 24 KiB | bootloader；链接区限 24 K（超出即链接错误） |
| **META** | `0x00006000` | 4 KiB | 磨损均衡 flag + 安全启动 manifest（双 2 KiB 槽）；两镜像均不占用 |
| **APP** | `0x00007000` | 至 `0x0001FFFF` | 应用；`SOC_FLASH_END = 0x0001FFFF` |
| **secver** | `0x00007400` | 8 B | 防回滚固件安全版本（`APP_START + 0x400`），QBOOT 读取 |

Flash 擦/写粒度（页）为 **256 B**（`-DQUILLA_QBOOT_PAGE_SIZE=256`）。

## 5. 控制台

USART2，**TX = PA2（GPIO16）**，**RX = PA3（GPIO17）**，115200 8-N-1。RX 由 DMA 驱动：DMA1 通道 6 CIRCULAR 模式填 512 字节环；USART IDLE 中断（帧定界）与 DMA 传输完成中断仅轮询 shell 进程，`shellport_rx_harvest()`（shell 线程）读实时 DMA 计数拷出新字节。TX 轮询，保证 QBOOT 跳 APP 前最后一字节已上线。与 GD32 shellport 同模型。

## 6. 镜像与构建目标

```bash
make -f Makefile              # -> QAPP/QAPP.{elf,hex,bin}
make -f Makefile.qboot        # -> QBOOT_CHECK  （仅 SHA-256 完整性）
make -f Makefile.qboot.sealed # -> QBOOT_SEALED （默认安全变体）
make -f Makefile.qboot.secured    # -> QBOOT_SECURED     （每次 Ed25519 验签）
```

`QUILLA_SB_BOOT_MODE` 由各 Makefile 设置（CHECK / SEALED / SECURED），与 GD32 变体一致；banner 标签 `QUILLA_QBOOT_VARIANT_STR` 由它在 `quilla-qboot-secure.h` 内派生。

## 7. 实测尺寸（开发过程数据，非交付规格）

| 镜像 | text | data | bss |
|---|---|---|---|
| QAPP | 18 453 | 192 | 10 120 |
| QBOOT_CHECK | 23 004 | 160 | 5 456 |
| QBOOT_SEALED | 23 108 | 160 | 5 456 |
| QBOOT_SECURED | 23 060 | 160 | 5 456 |

三个 QBOOT 变体均入 24 KiB 区（约 1.4 KiB 余量，与吃紧的 GD32 SEALED 相当）。数字随工具链/软件版本变化。

> **编译警告**：干净构建本身无警告。SEALED 与 SECURED 变体另会触发故意的「默认公钥」`#warning`（见 `doc/gd32f502ve-eide-build.md` §9）——提醒量产前将仓库默认 Ed25519 密钥替换为你自己的，属预期。

## 8. 需上板验证的 bring-up 项

以下为本首版承载的 RISC-V 特有风险；均不阻碍编译/链接，均需硅片确认：

1. **Flash 页 = 256 B** —— 取自 CH32V30x 家族；依赖前请对照 CH32V305VB 数据手册确认。
2. **Flash 写例程从 Flash 运行**（非 GD32 那样的 SRAM 常驻）。QBOOT 只擦/写 META/APP（从不写自己所在页）且看门狗仅在 APP 侧，故每次操作屏蔽中断。若出现读-写竞争，把擦/写内核迁到 SRAM（WCH 为此提供 `FLASH_ROM_ERASE` / `FLASH_ROM_WRITE`）。
3. **SysTick 周期重载** —— 句柄清 CNTIF 并依赖硬件自动重载；软件时钟由 rtimer 派生，故时序滑移只影响轮询率，不影响 `clock_time()`。
4. **`qclk` 的 RTC-on-LSI 上电**（备份域访问）；测失败按设计降级到编译期假设。
5. **`QUILLA_APP_SECVER_SELFCHECK` 在本端口编译期关闭** —— APP 侧自检用 Cortex-M 内联 asm（`cpsid i`），无 RISC-V 等价物。它只是混淆层；真正的防回滚闸门是 QBOOT 的签名 + floor，不受影响。
