**[中文](keil-qboot-app-build.zh-CN.md) · [English](keil-qboot-app-build.md)**

# Quilla QBOOT / APP 双系统 — Keil 工程编译文件整理与烧录地址映射

> 适用芯片：PY32F040xB（Cortex-M0+）与 GD32F502VE（Cortex-M33），各有自己的 `user/<mcu>/MDK-ARM/` 工程；分区地址由 `quilla-qboot-hal.h` 的常量定义（下文取 PY32F040xB 值为例）。
> 配套设计见 `doc/qboot-app-architecture.md`（QIAP 协议、交互标志、启动流程详见该文）。
> 本文件聚焦：**Keil 工程里 QBOOT / APP 各自编译哪些文件**，以及**系统烧录对应的 Flash 地址编译与映射**。

---

## 1. 工程形态

`user/py32f040xb/MDK-ARM/` 下是**两个独立的 Keil 工程文件**，各含一个 Target：

| 工程文件 | Target 名 | 镜像 | 链接区域（IROM1） | 宏 | 产物 |
| --- | --- | --- | --- | --- | --- |
| `QAPP.uvprojx` | `QAPP` | 业务固件 QAPP | `0x08007000 / 0x19000` | `USE_HAL_DRIVER,PY32F040xB,QAPP_BUILD` | `QAPP.axf/.hex`（`.\Output\`） |
| `QBOOT.uvprojx` | `QBOOT_CHECK` / `QBOOT_SEALED` / `QBOOT_SECURED` | bootloader | `0x08000000 / 0x6000` | 见 §7（按变体不同） | `QBOOT_CHECK.axf/.hex` 等（`.\QBOOT_Output\`） |

两者 RAM 均为 `IRAM1 = 0x20000000 / 0x4000`（QBOOT 只少用、不另分 RAM）。
两个工程都启用 `Use Memory Layout from Target Dialog`（`umfTarg=1`），实际布局由 Target 页的 IROM1/IRAM1 决定，`.sct` 是自动产物。

> 备份：`QAPP.uvprojx.bak`、`QBOOT.uvprojx.bak`（另有早期 `Project.uvprojx.bak`）。Keil 打开异常时可还原。

---

## 2. 内存 / 烧录地址映射（编译与烧写都按此）

Flash 128KB = `0x08000000 – 0x0801FFFF`，页 256B（共 512 页）。

| 区域 | 链接 / 烧录地址 | 长度 | 说明 |
| --- | --- | --- | --- |
| **QBOOT** | `0x08000000` | 24KB（页 0–95） | bootloader；链接区限定 24K，超界即链接报错 |
| **APP** | `0x08007000` | `0x19000`（102,400 字节 ≈95.5KB，页 112–511） | 业务；`APP_START` |
| **META 区** | `0x08006000`（QBOOT 与 APP 之间） | 4 KB（双 2KB 槽） | 磨损均衡的 QBOOT/APP 标志与安全启动 manifest 日志；两个镜像均不占用，由运行时追加写入（烧录工具切勿整片擦除） |

> APP 链接区 `0x08007000`–`0x0801FFFF` 落在 META 区（`0x08006000`–`0x08006FFF`）之后，META 区不会被误链进 APP 镜像。

- **QAPP 向量表重定位**：`system_<device>.c` 的 `SystemInit()` 内以 `QAPP_BUILD` 宏把 `VECT_TAB_OFFSET` 设为 `0x7000U`，QAPP 链接到 `0x08007000` 才成立。QBOOT 在 `0x08000000`，无需改 VTOR。
- **QBOOT 跳 APP**：`arch/cpu/arm/<mcu>/quilla-qboot-hal.c` 的 `quilla_qboot_hal_jump_to_app()` 跳转前也设 `SCB->VTOR = APP_START`，双保险。
- **GNU/EIDE 侧**：QBOOT 用 `py32f040xb_qboot.ld`（`ORIGIN=0x08000000, LENGTH=24K`），APP 用 `py32f040xb_app.ld`（`ORIGIN=0x08007000, LENGTH=0x19000`），与 Keil 的 IROM1 一致；GD32F502VE 用各自对应的链接脚本。
- **GNU/EIDE 侧**：QBOOT 用 `py32f040xb_qboot.ld`（`ORIGIN=0x08000000, LENGTH=24K`），APP 用 `py32f040xb_app.ld`（`ORIGIN=0x08007000, LENGTH=0x19000`），与 Keil 的 IROM1 一致。

---

## 3. Keil Target Option 速查（照此核对 / 复刻）

`Project → Options for Target`：

| 页面 | 项 | QAPP（`QAPP.uvprojx`） | QBOOT（`QBOOT.uvprojx`） |
| --- | --- | --- | --- |
| **Target** | Device | `PY32F040xB` | 同左 |
| | IROM1 | `Start 0x08007000` `Size 0x19000` | `Start 0x08000000` `Size 0x6000` |
| | IRAM1 | `Start 0x20000000` `Size 0x5000` | 同左 |
| **C/C++** | Define | `USE_HAL_DRIVER,PY32F040xB,QAPP_BUILD` | `USE_HAL_DRIVER,PY32F040xB,QBOOT_BUILD`（按变体追加 `QUILLA_SB_BOOT_MODE` / `QUILLA_QBOOT_DEFAULT_KEY` / `QUILLA_PRODUCTION_BUILD`，见 §7；`QUILLA_QBOOT_VARIANT_STR` banner 标签由模式派生，不进 Define） |
| | Optimization | 沿用原设置 | `-O2`（工程默认，microlib 15.4 KB / 24 KB）；体积偏紧时可临时 `-O1` |
| | One ELF Section per Function | ✓ | ✓（关键：否则 UART/Flash HAL 全量进镜像，24K 也装不下） |
| **Linker** | Use Memory Layout from Target Dialog | ✓ | ✓ |
| | 回收未用段（Remove Unused Sections） | ✓ | ✓ |
| **Output** | Name of Executable | `QAPP` | `QBOOT_CHECK` / `QBOOT_SEALED` / `QBOOT_SECURED`（按所选 Target） |
| | Create HEX File | ✓ | ✓ |

> **"每函数一段 + 回收未用段"对 QBOOT 是硬需求**：`py32f040_hal_uart.c` 源文件约 120KB，不做段回收不可能塞进 24KB。

QBOOT 分区预算为 **24 KB**（`0x08000000`–`0x08005FFF` = 24,576 B），分区边界只需页对齐、无需 sector 对齐。

> **microlib 是硬前提**：同一份源码改用标准 libc 链接，`-O2` 就只剩 3%。Target → `Use MicroLIB`
> 必须勾上，否则 `-O1` 及以上直接 `L6406E: No space in Execution Region ... ER_IROM1`。

> **Link-Time Optimization（LTO）按链接后实际体积决定开关**：LTO（armclang `--lto`，uvprojx 的 `v6Lto`）是否开启取决于 QBOOT 最终镜像体积，而非固定开启。
> - **GD32F502VE（Cortex-M33）**：armclang 的 LTO 内联成本模型对 32 位 Thumb-2 估值偏低，会激进内联整条 Ed25519 验签链（如 `ge_add`、`verify_descriptor`），使含密码学的 `QBOOT_SEALED` / `QBOOT_SECURED` 变体体积膨胀约 47%，超出 24 KB 分区；此时应关闭 LTO（`v6Lto = 0`）并配合 `-O1`/`-O2`，即可落回 24 KB 内。不含密码学的 `QBOOT_CHECK` 不受影响，可保留 LTO。
> - **PY32F040xB（Cortex-M0+）**：GNU 工具链不使用 `-flto`，Keil 构建下 LTO 对内联影响有限，通常可保持开启。
> - **决策顺序**：先按默认配置构建；若链接器报 `L6406E`（空间不足），优先调整 LTO 与优化等级，再考虑摘除非必需命令（如 `stack`）或临时放宽分区。

**若 QBOOT 实际体积接近或超过 24KB，按以下顺序处置，不要一上来就动分区：**

1. **降低优化等级到 `-O1`**（`-O1` 在 24 KB 分区里余量充足；`-O0` 也大概率能链过）。
2. **若仍报 `L6406E`**：给 QBOOT target 单独加 `-DKERN_CONF_STACK_SHELL=0`，摘掉 `stack`
   命令——可省约 **484 B**（19,772 → 19,288 B）。
3. **仍不够才临时放宽分区**（改完**必须改回来**）：
   - QBOOT `IROM1 Size 0x6000` → `0x8000`（32 KB，下一个 8 KB 边界）；APP `IROM1 Start` 同步 `0x08007000` → `0x08008000`；
   - **新边界必须仍是页对齐**：`quilla-qboot-hal.h` 里有编译期守卫（`QUILLA_QBOOT_APP_OFFSET % PAGE_SIZE == 0`），非页对齐会直接 `#error`；
   - 按 §7 的登记清单逐项核对：两个 `.ld`、两个 uvprojx、生成的 scatter、`eide.json` 3 处 `baseAddr`、
     以及 `Makefile.qboot`。`APP_END` 是 Flash 末址，取自 `arch/cpu/arm/<mcu>/soc.h` 的 `SOC_FLASH_END`，换平台改 soc.h 而非构建命令；
   - 上位机（`tools/quilla_host_updater.py`）已不再写死 APP 末址：传输前它查询设备的 `pinfo` shell 命令取得 `APP_START`/`APP_END`，因此新边界会被自动跟随。

---

## 4. QBOOT 编译文件整理（最小集 30 件）

QBOOT 只纳必要文件，剔除整包 Contiki 网络栈、显示/SPI-DMA、ctimer/stimer/mutex/autostart/rtimer、LPTIM 低功耗、IWDG、看门狗、BSP 等。

| Keil 分组 | 文件 |
| --- | --- |
| `CMSIS/MSP`（2） | `startup_py32f040xx.s`、`system_py32f040.c` |
| `CMSIS/PY32F040_HAL_Driver`（9） | `py32f040_hal.c`、`hal_rcc.c`、`hal_rcc_ex.c`、`hal_gpio.c`、`hal_dma.c`、`hal_pwr.c`、`hal_uart.c`、`hal_cortex.c`、**`hal_flash.c`** |
| `os/sys`（5） | `atomic.c`、`process.c`、`kern-guard.c`、`etimer.c`、`timer.c` |
| `os/lib`（6） | `assert.c`、`crc16.c`、`list.c`、`memb.c`、`ringbuf.c`、`ringbufindex.c` |
| `os/services`（1） | `quilla-shell.c` |
| `mounter`（4） | `shellport.c`、`clock.c`、`int-master.c`、**`quilla-qboot-hal.c`** |
| `quilla-qboot`（4） | **`quilla-qboot-flag.c`**、**`quilla-qboot-meta.c`**、**`quilla-qiap.c`**、**`qboot-main.c`** |

> `qboot-main.c`（QBOOT 入口，含 `main()`，现跨端口共享于 `os/services/quilla-qboot/qboot-main.c`）与其余 QBOOT 服务一样是硬件无关的 OS 模块；`quilla-qboot-hal.c`（硬件实现）仍留在 arch 层；`quilla-qboot-flag.c`（对 meta 的薄封装）、`quilla-qboot-meta.c`（磨损均衡 META 存储）、`quilla-qiap.c` 是硬件无关的 OS 模块，见第 4.1 节。

**QBOOT 明确不编入**：`main.c`、`py32f040_hal_msp.c`、`py32f040_it.c`、`hal_lptim.c`、`hal_iwdg.c`、`py32f040xx_Start_Kit.c`、`soc.c`、`platform.c`、`lpm.c`、`rtimer*.c`、`dbg.c`、`watchdog.c`、网络栈、显示/SPI-DMA。

原因与闭包依据（逐符号核对）：
- `shellport.c` 自带 `USART2_IRQHandler` / `DMA1_Channel4_5_6_7_IRQHandler`，故**不需要** `py32f040_it.c`；它也自带 `HAL_UART_MspInit`/`HAL_DMA_MspInit`，故**不需要** `py32f040_hal_msp.c`。
- `py32f040_it.c` 里的 `SysTick_Handler` 被 `#if 0` 关闭，`clock.c` 是唯一提供者。
- `qiap_main.c` 直接 `clock_init()` 而非 `platform_init_stage_one()`，从而不拉 `soc_init → rtimer_init → rtimer_arch_init → HAL_LPTIM` 与 `Error_Handler()`（后者只由 `main.c` 提供，而 QBOOT 不含 `main.c`）。
- 已用 `QBOOT_BUILD` 把 `clock.c` 的 `SysTick_Handler` 中 `clock_update_ticks()` 隔离掉（该路径会引 `RTIMER_NOW()` → rtimer/lpm）。QBOOT 只保留 `HAL_IncTick()`。
- `clock.c` 无条件调用 `etimer_pending()`/`etimer_request_poll()` → 必须带 `etimer.c` + `timer.c`；`process.c` 调用 `kern_guard_*` → 必须带 `kern-guard.c`。
- `lpm_exit()` 在 `LPM_CONF_ENABLE=0`（`contiki-conf.h`）时是空宏，故不需 `lpm.c`。
- **bootloader 零 libc 依赖**：`quilla-qiap.c` 用自带 `key_match()` 比密钥，未用 `memcmp`。
- `int-master.c` 必须带：`atomic.c` 经 `sys/critical.h` 调用 `int_master_*`；实现在 `arch/cpu/arm/int-master.c`（不是 `os/sys/`，那里只有头文件）。

### 4.1 OS / 硬件分层（quilla-qboot）

boot 相关模块按"OS 声明接口、平台实现"拆分，与 `int-master.h`/`clock.h` 同范式：

| 层 | 文件 | 内容 |
| --- | --- | --- |
| OS（硬件无关） | `os/services/quilla-qboot/quilla-qboot-meta.{h,c}` | 磨损均衡 META 存储（双槽 append-only 日志） |
| OS（硬件无关） | `os/services/quilla-qboot/quilla-qboot-flag.{h,c}` | 交互标志读写（委派 META 存储） |
| OS（硬件无关） | `os/services/quilla-qboot/quilla-qiap.{h,c}` | QIAP 协议状态机（纯字节流 + CRC） |
| OS（硬件无关） | `os/services/quilla-qboot/quilla-app-qboot.{h,c}` | APP 侧 `swqboot` 命令 |
| OS（接口声明） | `os/services/quilla-qboot/quilla-qboot-hal.h` | 平台需实现的接口 + 分区布局常量 |
| 平台实现 | `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` | 各 MCU 的 Flash 擦写/读、系统复位、VTOR 跳转、串口发送 |
| 平台入口 | `os/services/quilla-qboot/qboot-main.c`（共享） | `main()`、串口 ring、shell 循环、启动策略；调用各 MCU `clock_init()` / `shellport_init()` / `quilla_qboot_hal_app_image_valid()` |

OS 侧模块**不 include 任何 HAL/CMSIS 头**，机器操作全部经 `quilla_qboot_hal_*()`：

```
quilla_qboot_hal_flash_read(addr, dst, len)
quilla_qboot_hal_flash_write_page(page_base, src)   /* 整页擦+写 */
quilla_qboot_hal_reboot()                            /* 关中断 + 系统复位 */
quilla_qboot_hal_jump_to_app(app_start)              /* VTOR + MSP + 跳转 */
quilla_qboot_hal_transmit(data, len)                 /* 升级链路发送（QBOOT 下走 shellport） */
quilla_qboot_hal_int_disable() / _int_restore(st)    /* 转发 int_master_* */
```

分区布局中 `APP_START`/`PAGE_SIZE`/`RAM_*` 在 `quilla-qboot-hal.h` 里以 `#ifndef` 给出 PY32F040xB 默认值，换平台时用 `-D` 覆盖。其中 **`APP_END`（Flash 末址、APP 区上界）是纯 MCU 硬件属性，不再由构建命令 `-D` 传入**，而是定义在 `arch/cpu/arm/<mcu>/soc.h` 的 `SOC_FLASH_END` 宏里——`quilla-qboot-hal.h` 直接取它。新 MCU 移植只需在对应 `soc.h` 补 `SOC_FLASH_END`，无需动构建命令，也无需改 OS 头文件。

---

## 5. APP 编译文件整理（原全量 46 件 + 新增 3 件）

APP = 原工程的全部文件（完整 Contiki、显示、SPI DMA、业务等）**再新增**：

| 新增文件 | 落点分组 | 作用 |
| --- | --- | --- |
| `arch/cpu/arm/vendor/Puya/PY32F040_HAL_Driver/Src/py32f040_hal_flash.c` | `CMSIS/PY32F040_HAL_Driver` | 写交互标志需要 HAL FLASH |
| `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` | `mounter` | 交互标志擦写、复位（APP 侧只需这两项） |
| `os/services/quilla-qboot/quilla-qboot-meta.c` | `quilla-qboot` | 磨损均衡 META 存储（flag/floor/seal/manifest，QBOOT/APP 共用） |
| `os/services/quilla-qboot/quilla-qboot-flag.c` | `quilla-qboot`（新分组） | 交互标志读写（委派 META；QBOOT/APP 共用） |
| `os/services/quilla-qboot/quilla-app-qboot.c` | `quilla-qboot` | APP 侧 `swqboot` 命令（仅 APP，OS 层） |

APP 侧源码接入（`user/<mcu>/Src/`）：
- `main.c` 在 `shellport_init()` 后调用 `shell_set_partition_tag("app1")` 与 `quilla_app_qboot_init()`（头文件用源相对路径 `../../../os/services/quilla-qboot/quilla-app-qboot.h`，保证三套构建都能解析）；
- `system_<device>.c` 在 `QAPP_BUILD` 下把 `VECT_TAB_OFFSET` 设为 `0x7000U`。

> QBOOT 专属文件 `qboot-main.c`（`os/services/quilla-qboot`）/ `quilla-qiap.c` **不进 APP 构建**。`quilla-qboot-hal.c` 两侧都要编，但其中 `quilla_qboot_hal_transmit()` 由 `QBOOT_BUILD` 守卫，APP 下为空实现。

---

## 6. 系统烧录（Flash 编程）地址映射与步骤

### 6.1 三个区域的烧写边界
- **QBOOT.hex**：基址 `0x08000000`，占前 24KB。只烧一次（除非升级 bootloader）。
- **APP.hex**：基址 `0x08007000`。日常业务更新只烧这个。
- **META 区 `0x08006000`–`0x08006FFF`（4 KB）**：由运行时追加写入（`quilla_qboot_flag_set` / QIAP manifest 写）。**烧录工具切勿整片擦除或覆盖**，否则标志与 manifest 状态丢失，需重新全量烧录。Keil 默认仅写入 HEX 覆盖区块，不触及 META 区。

### 6.2 推荐烧录流程
1. 首次量产 / 恢复：先烧 `QBOOT.hex`（`0x08000000`），再烧 `APP.hex`（`0x08007000`）。基址不同、不重叠，顺序无所谓。
2. 日常业务升级：只烧 `APP.hex`。
3. 现场升级：APP 运行 → `swqboot` 命令 → 标志写 `qboot` → 复位 → QBOOT 进**交互 shell**（非直接升级）；上位机先发 `qiap` 命令切到串口升级，再发 ENTER/DATA/END 帧（协议见架构文档 §8）；合法 ENTER 帧才把标志写 `qiap`（中途掉电可自动重进升级），END 成功写回 `app1` 并复位进新 APP。若误入 QBOOT，shell 里 `swapp` 可强制回 APP。
4. 合并烧录：可用 `srec_cat` 等把两个 HEX 按各自基址合并后整片烧；合并时**不要包含 META 区**（`0x08006000`–`0x08006FFF`）。

### 6.3 防误刷（可选）
- Option Byte 写保护（WRP）保护 QBOOT 区（页 0–95），防止 QBOOT bug 自毁 bootloader。

---

## 7. 三套构建入口登记状态（"铁律"核对）

| 新增 `.c` | Keil | EIDE `.eide/eide.json` | GNU `Makefile` |
| --- | --- | --- | --- |
| `os/services/quilla-qboot/qboot-main.c`（QBOOT 专属，共享） | `QBOOT.uvprojx` ✓ | （EIDE 无 QBOOT 配置，用 `make -f Makefile.qboot`） | `Makefile.qboot` ✓ |
| `arch/cpu/arm/py32f040xb/quilla-qboot-hal.c`（共用） | `QBOOT.uvprojx` ✓ + `QAPP.uvprojx` ✓ | `QAPP`（eide.json） ✓ | `Makefile`(QAPP) ✓ + `Makefile.qboot` ✓ |
| `os/services/quilla-qboot/quilla-qboot-flag.c`（共用） | `QBOOT.uvprojx` ✓ + `QAPP.uvprojx` ✓ | `QAPP`（eide.json） ✓ | `Makefile`(QAPP) ✓ + `Makefile.qboot` ✓ |
| `os/services/quilla-qboot/quilla-qboot-meta.c`（共用） | `QBOOT.uvprojx` ✓ + `QAPP.uvprojx` ✓ | `QAPP`（eide.json） ✓ | `Makefile`(QAPP) ✓ + `Makefile.qboot` ✓ |
| `os/services/quilla-qboot/quilla-qiap.c`（QBOOT 专属） | `QBOOT.uvprojx` ✓ | — | `Makefile.qboot` ✓ |
| `os/services/quilla-qboot/quilla-app-qboot.c`（QAPP 专属） | `QAPP.uvprojx` ✓ | `QAPP`（eide.json） ✓ | `Makefile`(QAPP) ✓ |
| `py32f040_hal_flash.c`（两者均需） | `QBOOT.uvprojx` ✓ + `QAPP.uvprojx` ✓ | `QAPP`（eide.json） ✓ | `Makefile`(QAPP) ✓ + `Makefile.qboot` ✓ |

> boot 相关模块位于 `os/services/quilla-qboot/`；三套构建入口（Keil / EIDE / GNU）均已登记（含 `Makefile.qboot` 的 `-I../../../os/services/quilla-qboot`）。

> **GNU 侧三个 secure-boot 变体**（与 Keil 三 Target 1:1）：
>
> | 变体 | Makefile | 产物 | 说明 |
> |------|----------|------|------|
> | `QBOOT_CHECK` | `Makefile.qboot` | `./QBOOT_CHECK/QBOOT_CHECK.{elf,hex,bin}` | 仅完整性，无钥匙（`QUILLA_SB_BOOT_MODE=CHECK`） |
> | `QBOOT_SEALED` | `Makefile.qboot.sealed` | `./QBOOT_SEALED/QBOOT_SEALED.{elf,hex,bin}` | 开发态，默认钥匙（`QUILLA_QBOOT_DEFAULT_KEY` + `SEALED`） |
> | `QBOOT_SECURED` | `Makefile.qboot.secured` | `./QBOOT_SECURED/QBOOT_SECURED.{elf,hex,bin}` | 量产态，SECURE 模式每电验签（`QUILLA_QBOOT_DEFAULT_KEY` + `SECURE` + `QUILLA_PRODUCTION_BUILD`）；钥匙与 SEALED 同一把，替换见 makefile 头注释 |
>
> 三个变体差异仅在 `C_DEFS`（宏清单见 `doc/secure-boot.md` §3.1）。SEALED 与 SECURED 编译进的是**同一把默认钥匙**，出货前都须替换为自有公钥：GNU 在 `C_DEFS` 加 `-DQUILLA_SECURE_BOOT_PUBKEY={...}`，Keil 须在 `quilla-qboot-pubkey.h` 自行添加产品分支并在 Define 里选它的 token（仓库未预置该分支，模板见 `tools/_keytest/README.md` §3.2）；是否替换由客户决定，仍用默认钥匙时 `quilla-qboot-secure.c` 会发一条编译警告。两个 MCU 的 `EIDE/` 目录均已提供这三个 makefile。

宏与链接：Keil 两工程分别 `QAPP_BUILD`/`QBOOT_BUILD` 并设对应 IROM1；EIDE 加 `QAPP_BUILD` + `py32f040xb_app.ld`；`Makefile` 加 `-DQAPP_BUILD` + `py32f040xb_app.ld`；`Makefile.qboot` 用 `-DQBOOT_BUILD` + `py32f040xb_qboot.ld`。

---

## 8. 备注

- **Flash 页编程粒度 = 256B**：`FLASH_Program_Page()` 写 64 words；`HAL_FLASH_Program()` 强制 `Address % FLASH_PAGE_SIZE == 0`（否则返回 `HAL_ERROR`）。厂商头注释"Program 128bytes"是过时文案，以 `FLASH_PAGE_SIZE = 0x100` 为准。代码按 256B 整页暂存 + 页基址对齐（`page_base &= ~(FLASH_PAGE_SIZE-1)`）。
- **擦除必须显式置 `TypeErase` 与 `NbPages`**：`HAL_FLASH_Erase` 的循环条件是 `NbPages * FLASH_PAGE_SIZE`，`NbPages=0` 会静默空转。实现位于 `arch/cpu/arm/py32f040xb/quilla-qboot-hal.c`，设 `FLASH_TYPEERASE_PAGEERASE` + `NbPages=1`。
- **HAL 主头名是 `py32f0xx_hal.h`**（不是 `py32f040_hal.h`）；而 `py32f040_hal_flash.h` / `py32f040_hal_cortex.h` 确实存在。
- **QBOOT 链接闭包**：最小集内无未定义外部符号；不需要 `process-mutex.c`、`lpm.c`、`autostart.c`（`int-master.c` 需要，已纳入）。
- **两工程均已启用「每函数一段 + 回收未用段」**（QBOOT 压进 24K 的前提）：`TargetArmAds → Cads → <OneElfS>1</OneElfS>`，链接器 `LDads → <Misc>--remove</Misc>`。
- **注意**：APP 的 IROM 元素名是 `<IROM>`（无后缀），不是 `<IROM1>`；改链接地址时务必按标签实际名改，改错元素不会生效。
