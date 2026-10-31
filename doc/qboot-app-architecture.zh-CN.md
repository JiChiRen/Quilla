**[中文](qboot-app-architecture.zh-CN.md) · [English](qboot-app-architecture.md)**

# Quilla QBOOT / APP 双系统拆分与 QIAP 升级协议设计

> 适用芯片：PY32F040xB（Cortex-M0+）与 GD32F502VE（Cortex-M33）
> PY32F040xB 资源：Flash 128 KB（0x08000000–0x0801FFFF），RAM 16 KB（0x20000000），Flash 页 **256 B**，页擦除。
> GD32F502VE 资源：Flash 512 KB（0x08000000–0x0807FFFF），擦除页 2 KB（bank0）。
> 共用代码：本仓库 `arch/`、`os/`（Contiki-NG 派生 + Quilla 自有服务）。
> 状态：设计文档 + 参考实现。协议与算法部分已在 PC 侧用 clang 编译并跑通 RFC 8032 测试向量；ARM 侧由 Keil 工程编译验证。
>
> 配套流程文档：
> - [`upgrade-flow.md`](upgrade-flow.md) — 完整在线升级流程（上位机下发 → 切 QBOOT → 帧处理 → 校验 → 写入 → 复位）
> - [`boot-flow.md`](boot-flow.md) — 复位后的启动校验与切换 APP 流程

---

## 1. 目标与约束

- 上电先进入 **QBOOT**（In-Application Programming，bootloader），QBOOT 读 `[QBOOT/APP 交互标志]`：
  - 标志为默认 `app1` → **先校验 APP 向量表**（初始 SP 落在 RAM 区间、复位向量落在 APP 区间且 Thumb 位为 1），合法则跳转到 APP 起始地址运行；
  - 标志为 `qiap` → 进入串口升级流程；
  - APP 区空白或损坏（已擦除 / 向量非法）→ 留在 QBOOT 交互 shell，串口链路保活（`qiap` 可重新升级）。
- APP 收到 shell 命令 `swqboot` 时，把标志改写为 `qboot`（停在 QBOOT 交互 shell，不进升级）并复位；复位后 QBOOT 起来进交互 shell，可 `swapp` 回 APP 或 `qiap` 开始升级。`swqboot` **不再**直接置 `qiap`——真正的升级标志在 QBOOT 收到合法 ENTER 帧时才落盘（见 §3/§8），既让误入 QBOOT 可脱困，又保证烧录中途掉电能自动重进升级。
- **QBOOT 体积要小**：编译/链接只纳入必要文件，不拉入整包 Contiki 网络栈、显示、SPI DMA 等。
- QBOOT 与 APP 都提供 shell 命令 `part`：QBOOT 回 `qboot`，APP 回 `app1`。`part` 的分区标记**不能写死在 shell 里**，由各系统运行时传入。

---

## 2. 系统划分

| 系统 | 职责 | 是否用 OS 调度 | 关键源文件（建议） |
| --- | --- | --- | --- |
| **QBOOT** | 判断标志、跳转 APP、串口 QIAP 接收与烧写 | 复用最小 OS（仅为跑 shell 进程） | `arch/cpu/arm/<mcu>/qboot-main.c`、`arch/cpu/arm/<mcu>/quilla-qboot-hal.c`、`os/services/quilla-qboot/{quilla-qiap.c,quilla-qboot-meta.c,quilla-qboot-flag.c}`、`quilla-shell.c`、`shellport.c`、`arch/cpu/arm` 最小 HAL（rcc/gpio/uart/dma/pwr/cortex/flash）、`os/sys/{process,clock,...}`、`os/lib/{crc16,ringbuf,...}` |
| **APP** | 业务（显示、指令解析、SPI DMA 等完整功能） | 完整 Contiki | 现有 `main.c` 全量 + `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` + `os/services/quilla-qboot/{quilla-qboot-meta.c,quilla-qboot-flag.c,quilla-app-qboot.c}` |

两系统**共用** `arch/`、`os/` 同一份源码，差异只在于：

1. **链接脚本不同**（Flash 区域切分 + APP 的 VTOR 重定位）；
2. **构建源文件清单不同**（QBOOT 用最小集，见第 4 节）；
3. **入口与少量初始化不同**（QBOOT 用 `qboot-main.c`，APP 用 `main.c`）；
4. **运行期各自调用 `shell_set_partition_tag()`**（见第 6 节）。

> 注意：QBOOT 也使用 shell（为了 `part` 命令和可选的人机交互），因此必须保留让 shell 进程能跑起来的最小 OS 子集（process/clock 等）。所谓"减少 QBOOT 体积"主要是**剔除网络栈、显示、SPI DMA、CoAP/SNMP 等 APP 专属模块**，而非彻底裸机。若希望 QBOOT 进一步瘦身到裸机轮询（连 shell 进程调度都不要），需要把 shell 改成轮询版，工作量更大；本文档默认采用"最小 OS + shell"方案。

---

## 3. 内存映射（现行值）

PY32F040xB：Flash 128 KB = 0x20000 字节，页 256 B（共 512 页）。布局为 **QBOOT – META – APP** 三段，跨 MCU 字节级同址；GD32F502VE 仅 Flash 末址（`0x0807FFFF`）与擦除页大小（2 KB）不同。

| 区域 | 地址范围 | 长度 | 说明 |
| --- | --- | --- | --- |
| **QBOOT** | `0x08000000` – `0x08005FFF` | **24 KB**（PY32F040xB 页 0–95） | bootloader；含 SHA-256/512 + Ed25519 验签，链接脚本限定 `LENGTH = 0x6000`，超界即链接报错 |
| **META** | `0x08006000` – `0x08006FFF` | **4 KB**（PY32F040xB 页 96–111） | 安全启动元数据区，位于 QBOOT 与 APP 之间；含交互标志与 manifest |
| └─ META 存储 | `0x08006000`（SLOT_A）/ `0x08006800`（SLOT_B） | 2 × 2 KB 槽 | 磨损均衡的标志 + manifest 日志：256 B 记录（64 B `QMTL` meta 块 + 128 B `QMF1` manifest 块 + 64 B 填充），只编程追加；活动槽永不在原地擦除（见第 5 节） |
| **APP** | `0x08007000` – Flash 末址 | **PY32F040xB `0x19000`（102,400 B ≈ 95.5 KB）**；GD32 `0x79000`（495,616 B） | 业务固件；`ORIGIN = 0x08007000` |

- **APP 起始地址 `APP_START = 0x08007000`**（`QUILLA_QBOOT_APP_START`，下文协议与跳转均以此为基准）。
- **APP 可烧录上限随 MCU 而异**：PY32F040xB = `0x19000`（APP 区止于 `0x0801FFFF`）；GD32F502VE 一直延伸到 `0x0807FFFF`（512 KiB Flash → `0x79000`）。META 在 APP 之前，不再占用 Flash 末页。上位机不写死它：升级前查询设备的 `pinfo` 命令（见 §6），从运行中固件的 `SOC_FLASH_END` 推导可烧录上限。
- **META 区放在 QBOOT 与 APP 之间**（而非末页）：FLAG 与 manifest 不再占用 Flash 尾页，APP 区可一直延伸到 Flash 末尾，空间最大化；升级协议硬性拒绝任何写入 META 区的地址（见第 8 节），QBOOT 永不触碰自身以上的 META/APP 区。
- **META = 4 KB 是跨 MCU 统一的最小可行值**：GD32F502VE 的 bank0 最小擦除粒度是 2 KB 页，META 因此取 4 KB（两个 2 KB 页）让 FLAG 与 manifest 各占一个擦除页，并保持 4 KB 对齐以满足 PY32F040xB 的 4 KB 扇区边界；PY32F040xB 页仅 256 B，4 KB 纯属对齐填充（FLAG ≈ 16 B + manifest ≈ 108 B 实际只占 META 的一小部分）。
- 上表为现行值。`APP_START` 与 QBOOT 长度可按实际体积微调，但必须保证三区域无重叠，且 **META 区不被 QBOOT 或 APP 的代码/数据占用**（链接脚本不要给任一方分配 `0x08006000`–`0x08006FFF`）。
- 分区边界的**唯一来源**是 `os/services/quilla-qboot/quilla-qboot-hal.h`（`QUILLA_QBOOT_META_BASE` / `QUILLA_QBOOT_META_SIZE` / `QUILLA_QBOOT_APP_START`）；`QUILLA_QBOOT_APP_OFFSET` 由它推导，APP 的 `VECT_TAB_OFFSET` 再用该宏，改分区只需改这一处。

### APP 向量表重定位（易错点）
APP 不能再用默认的 `VTOR = 0x08000000`。必须在 APP 构建里把 `SCB->VTOR` 设成 `APP_START`：
- 在 `system_<device>.c` 的 `SystemInit()` 内加（受 `QAPP_BUILD` 宏控制）：
  ```c
  #ifdef QAPP_BUILD
    SCB->VTOR = APP_START;   /* 0x08007000 */
  #endif
  ```
- 或在 APP 的复位处理最开头手写 `SCB->VTOR = FLASH_BASE + 0x7000;`。
- QBOOT 不需要改 VTOR（它本来就位于 0x08000000，是芯片复位向量所在地）。

---

## 4. 构建分离方案

本仓库每个 MCU 有五处构建登记点（`Makefile`、`Makefile.qboot`、`.eide/eide.json`、`APP.uvprojx`、`QBOOT.uvprojx`）。按"铁律"，每新增 `.c` 必须在五处全部登记。**QBOOT 专用文件（qboot-main.c / quilla-qiap.c）只进 QBOOT 构建，不进现有 APP 构建**，避免回归。

### 4.1 现有 APP 构建（改动最小、保持可用）
为让 APP 具备 `swqboot` 能力，向现有 APP 构建**追加**（不删任何现有项）：
- 源文件：`arch/cpu/arm/<mcu>/quilla-qboot-hal.c`、`os/services/quilla-qboot/quilla-qboot-meta.c`、`os/services/quilla-qboot/quilla-qboot-flag.c`、`os/services/quilla-qboot/quilla-app-qboot.c`（以及该 MCU 尚未编入的 flash HAL）；
- 宏：`-DQAPP_BUILD`（驱动 VTOR 重定位）；
- `main.c` 启动期调用 `shell_set_partition_tag("app1")` 与 `quilla_app_qboot_init()`（注册 `swqboot` 命令）；
- 链接脚本换成 APP 变体（`py32f040xb_app.ld` / GD32 对应脚本）。

> 需在 Keil/EIDE 五处构建入口登记上述追加项。

### 4.2 QBOOT 构建（新增 `Makefile.qboot` + QBOOT 链接脚本）
新增 `user/<mcu>/EIDE/Makefile.qboot`（`TARGET = QBOOT`）与 QBOOT 链接脚本（FLASH `LENGTH = 24K`）。`make -f Makefile.qboot` 即产出 `QBOOT/QBOOT.{elf,hex,bin}`。QBOOT 的最小源文件集（**已按真实链接闭包逐一符号核对**，非估算）：

- **启动/系统**：`startup_<device>xx.s`（芯片复位向量 + 弱 ISR）、`user/<mcu>/Src/system_<device>.c`（提供 `SystemInit()`，复位后由 startup 调用，配置 `SystemCoreClock`）。
  - **QBOOT 不走 `platform_init_stage_one()`**：`qboot-main.c` 直接调用 `clock_init()`（仅配置 SysTick/系统节拍）。原因：`platform_init_stage_one → soc_init` 会拉起 `lpm_init()` 与 `rtimer_init() → rtimer_arch_init()`，后者**强制依赖该 MCU 的 LPTIM HAL 与 `Error_Handler()`**（仅 `main.c` 提供），既增体积又引入 APP 专属符号。QBOOT 只需"系统时钟正确（保证串口波特率）+ SysTick + UART"，`clock_init()` 已足够。
- **HAL（最小，全部用仓库内 vendor HAL 副本，不依赖外部 SDK）**：该 MCU 的 `hal.c`、`hal_rcc.c`、`hal_rcc_ex.c`、`hal_gpio.c`、`hal_dma.c`、`hal_pwr.c`、`hal_uart.c`、`hal_cortex.c`、`hal_flash.c`。
  - `shellport.c` 自带 `HAL_UART_MspInit` / `HAL_DMA_MspInit`（USART 引脚与时钟配置），因此**无需**平台侧的 `*_hal_msp.c`。
- **QBOOT 业务**：`arch/cpu/arm/<mcu>/qboot-main.c`（入口）、`arch/cpu/arm/<mcu>/quilla-qboot-hal.c`（硬件）、`os/services/quilla-qboot/quilla-qiap.c`、`os/services/quilla-qboot/quilla-qboot-meta.c`（磨损均衡 META 存储）、`os/services/quilla-qboot/quilla-qboot-flag.c`（对 meta 的薄封装）。
- **shell**：`os/services/shell/quilla-shell.c`、`arch/cpu/arm/<mcu>/shellport.c`（复用 shell 串口，直接拿到 `part` 命令）。
- **OS 最小子集（符号闭包必需）**：`os/sys/process.c`、`os/sys/kern-guard.c`、`os/sys/clock.c`、`os/sys/etimer.c`、`os/sys/timer.c`、`os/sys/atomic.c`、`os/lib/crc16.c`、`os/lib/ringbuf.c`、`os/lib/assert.c`。
  - 依赖链（已逐一核对）：`process.c` 调 `kern_guard_register` / `kern_raise`（宏，展开为 `kern_error_hook_default`，定义于 `kern-guard.c`）→ `kern-guard.c` 必需；`clock.c` 在 `clock_update_ticks()` 中**无条件**调 `etimer_pending()` / `etimer_request_poll()` → `etimer.c` 必需；`etimer.c` 用 `timer_*` → `timer.c` 必需；shell 用 `strcmp`/`memset`、shellport 用 `strlen` → 由 `-specs=nano.specs` 的 `libc_nano.a` 提供，而链接脚本的 `/DISCARD/ { libc.a }` 仅丢弃**完整** libc，不影响这些小函数。
  - `quilla-qiap.c` 的密钥比较**不依赖 libc**：原 `memcmp` 改为自带的 `key_match()` 逐字节比较（去除 `<string.h>`），保持 bootloader 零 libc 依赖。
  - **未引入**：`list.c`/`memb.c`（QBOOT 源未引用）、`autostart.c`、`ctimer.c`、`stimer.c`、`rtimer.c`/`rtimer-arch.c`、`lpm.c`、`kern-nomalloc.c`、`platform.c`/`soc.c`、`watchdog.c`、网络栈、显示/SPI DMA 等。
- **宏**：`-DUSE_HAL_DRIVER -D<device> -DQBOOT_BUILD`；**优化 `-Os`**；QBOOT 链接脚本。
- **剔除（APP 专属，严禁进 QBOOT）**：网络栈、显示、SPI DMA、CoAP/SNMP、RPL、TSCH、CTIMER/STIMER、AUTOSTART，以及 LPTIM/低功耗/RTIMER 链路（`rtimer-arch.c`、`hal_lptim.c`、`lpm.c`）。

> QBOOT 体积预算 24 KB。闭包经静态符号核对完整；若 `-Os` 链接后超 24K，按以下顺序再砍：`assert.c`（若未被引用，`--gc-sections` 已丢弃）；把 `clock.c` 对 `etimer` 的调用摘掉（需改 `clock.c`，工作量大，不推荐）；仍超则回调第 3 节把 `APP_START` 后移（牺牲 APP 空间）。GD32F502VE（M33）注意：armclang LTO 会内联 Ed25519 验签链，使 SEALED/SEC 超出预算——须关闭 LTO（`v6Lto=0`），见 `doc/keil-qboot-app-build.md`。

---

## 5. QBOOT / APP 交互标志与 META 存储

交互标志与安全启动元数据不再以独立页存放，而是统一存于磨损均衡 META 存储（`quilla-qboot-meta.c`）：
4 KB META 区划分为两个 2 KB 槽（SLOT_A `0x08006000` / SLOT_B `0x08006800`），以只编程不擦除的方式追加写入 256 B 记录——每记录含 64 B `QMTL` meta 块（flag、防回滚下限 floor、密封记录 seal）与 128 B `QMF1` manifest 块。活动槽从不原地擦除，standby 槽在活动槽写满前预擦。启动时扫描双槽、取序列号最高的有效记录为权威，任意时刻掉电均保留一条完整可读记录，标志与元数据因此获得同等的掉电安全性。

标志值（不变）：

```c
#define QUILLA_QBOOT_FLAG_APP1   0x00000001U   /* 默认：进 APP */
#define QUILLA_QBOOT_FLAG_QIAP    0x00000002U   /* 升级进行中（ENTER 后落盘） */
#define QUILLA_QBOOT_FLAG_QBOOT 0x00000003U   /* 停在 QBOOT shell（swqboot 置） */
```

**为什么用 Flash 而非 RAM/备份寄存器**：上电（掉电）后 RAM 内容不保；备份寄存器在 PY32F040xB 上受 VBAT/复位影响且非所有封装可用。META 区（位于 QBOOT 与 APP 之间）是跨复位最稳的落点，且天然独立于两系统代码区。

**读**：`quilla_qboot_flag_get()` —— 若 `magic` 不匹配或值为 `QIAP` 以外，一律返回 `APP1`（默认安全态）。

**写**（APP 调 `swqboot`）：`quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_QBOOT)`（停在 QBOOT shell）；升级真正开始时（合法 ENTER 帧）由 `quilla-qiap.c` 写 `QUILLA_QBOOT_FLAG_QIAP`；END 成功写回 `APP1`。所有写入都走 META 追加路径：新记录编程进 standby 槽（需要时先预擦），下次读取按最高序列号取用——不存在"读页→擦页→重写整页"循环，写中途掉电也不会损坏活动记录。QBOOT 自身启动成功后再把标志写回 `APP1`，然后复位/跳转。

实现见 `os/services/quilla-qboot/quilla-qboot-flag.{h,c}`（两系统共用，已在 APP 构建追加列表中；对 `quilla-qboot-meta.c` 的薄封装）。

---

## 6. `part` 命令与分区标记的传递（回答"不要写死在 shell"）

shell 是公用代码，且遵循"核心不反向引用应用"的既定设计。**分区标记由每个系统自己的入口在运行期传入**，shell 只负责存储与回显：

- `quilla-shell.h` 新增：
  ```c
  /* 由 QBOOT/APP 各自在启动期调用，告知 shell 当前分区身份。shell 本身不判断。 */
  void shell_set_partition_tag(const char *tag);
  ```
- `quilla-shell.c` 内增加静态 `const char *shell_part_tag = "app1";`（默认安全值），新增内置命令 `part`：
  ```c
  static void part_handler(const char *args) {
    (void)args;
    shell_output("%s\r\n", shell_part_tag ? shell_part_tag : "unknown");
  }
  ```
  并在 `shell_init()` 注册该命令。
- **QBOOT**（`qboot-main.c`）启动后调用 `shell_set_partition_tag("qboot");`
- **APP**（`main.c`）启动后调用 `shell_set_partition_tag("app1");`

这样：判定 `qboot`/`app1` 的逻辑完全在各自的 `main`，shell 始终保持中立、零硬编码；将来新增第三分区也只需改对应入口，不碰 shell。

QBOOT 另注册一个 `pinfo` 命令（`qboot-main.c`），回显真实分区布局 `[PART] APP_START=0x… APP_END=0x… APP_SIZE=0x…`，取编译期宏（`QUILLA_QBOOT_APP_END` = `SOC_FLASH_END`，随 MCU 而异）。上位机读取它，使预检时的可烧录上限始终与设备一致，而非某个写死的 MCU 值。`pinfo` 仅 QBOOT 有（APP shell 无），且与 `part` 一样，QIAP raw sink 接管后即被吞掉。

---

## 7. QBOOT 启动流程（bootloader boot policy）

```
上电 → 芯片复位向量(0x08000000) → QBOOT 的 Reset_Handler
  → 时钟/Flash 初始化 → shell 初始化 → shell_set_partition_tag("qboot")
  → quilla_qboot_flag_get()
       ├─ == QIAP    → 直接进入 QBOOT 串口升级收帧（crash recovery，第 8 节）
       │             （升级中途掉电/复位会停留在此状态，避免跳半残 APP）
       ├─ == QBOOT → 进入 QBOOT 交互 shell，**不跳 APP**（即使 APP 向量表合法）：
       │             · 可 `swapp` 回 APP；可 `qiap` 开始升级
       └─ == APP1 或 交互标志空白/损坏（读回均视为 APP1）：
            ├─ qboot_app_image_valid() 通过
            │    └─ 且 qboot_secure_boot_ok() 通过 → 跳转 APP（app_jump 见下）
            └─ 任一不通过 → 进入 QBOOT 交互 shell（保留 `qiap` 可达）

app_jump:
  __disable_irq();
  SCB->VTOR = APP_START;            /* 保险：APP 自身也会设 */
  sp  = *(uint32_t*)APP_START;
  pc  = *(uint32_t*)(APP_START+4);
  ((void(*)(void))pc)();            /* 跳 APP 复位向量 */
```

**如何进入升级模式（无"停留窗口"）**：当前实现**没有** 300 ms 停留窗口，也不存在 `0x1A` 触发字节——这是早期草案，已被删除。进入 QBOOT 串口升级只有两条路径：
- **冷启动 flag==QIAP**：升级中途掉电/复位后，交互标志仍是 `QIAP`，QBOOT 上电直接进升级收帧（避免跳半残 APP）；
- **`qiap` 命令**：QBOOT 交互 shell 中（flag==QBOOT 强制停留、或 flag==APP1 且 APP 损坏落入），上位机发送文本命令 `qiap`（回车结尾）即切到升级收帧。

APP 区损坏时落 QBOOT shell、`swqboot` 后强制停留 QBOOT shell，正是为了让 `qiap` 命令始终可达；想回 APP 则用 `swapp`。详见第 8 节。

---

## 8. QBOOT 串口升级协议（上位机 ↔ QBOOT）

### 8.0 如何进入升级模式（重要，上位机协议已变）
QBOOT 上电**默认不**直接收二进制升级帧，而是先判断标志（见第 7 节）：
- flag==QIAP（升级中途掉电/复位遗留）→ 自动进入升级收帧；
- 否则进入 QBOOT 交互 shell（flag==QBOOT 强制停留；flag==APP1 且 APP 损坏也会落入）。

在 shell 中，**上位机必须先发送文本命令 `qiap`（回车结尾）**，设备才会把串口链路从"shell 行解析"切换为"原始字节流"，之后所有字节直接喂给 `quilla_qiap_feed()` 解析。
> ⚠️ 在 `qiap` 命令之前发送的任何二进制帧（含 ENTER/DATA/END）都会被 shell 当作命令解析或静默丢弃，升级无法启动。务必先敲 `qiap`。
> 典型上位机序列（经 `swqboot` 进入 QBOOT 的场景）：`swqboot`（APP 内，置 QBOOT 标志并复位）→ 复位进 QBOOT shell → 发 `qiap` → 发 ENTER/DATA/END → 成功复位进新 APP。

链路：复用现有 shell 串口（PY32F040xB：USART2 / PA2/PA3 / DMA；GD32F502VE：USART0 / PB6/PB7 / DMA0 CH4 + IDLE 定帧；均为 115200-N-8-1，与 APP 一致）。

**协议为二进制帧**，与 shell 的行解析互不干扰：`qiap` 命令切换后，串口字节流直接喂给 `quilla_qiap_feed()` 状态机（`quilla-qiap.c`），不再走 shell 行解析。

### 8.1 通用帧格式（小端传输多字节）

```
SOF    1B   固定 0xA5
CMD    1B   命令/响应码（见下表）
LEN    2B   后续 PAYLOAD 长度（小端）
PAYLOAD LEN 命令参数
CRC16  2B   CRC-16/CCITT，覆盖 [CMD][LEN][PAYLOAD]（即 SOF 之后的全部内容）
```

**CRC-16/CCITT 算法（上位机必须与此一致，复用 `os/lib/crc16.c`）**：
```c
uint16_t crc16_add(uint8_t b, uint16_t acc) {
  acc ^= b;
  acc  = (acc >> 8) | (acc << 8);
  acc ^= (acc & 0xff00) << 4;
  acc ^= (acc >> 8) >> 4;
  acc ^= (acc & 0xff00) >> 5;
  return acc;
}
/* 初始 acc = 0；逐字节 crc16_add；最终结果与帧内 CRC16 比较 */
```

### 8.2 命令码

| 方向 | CMD | 名称 | PAYLOAD |
| --- | --- | --- | --- |
| H→M | `0x01` | ENTER_OTA_REQ | `app_start_addr(4)` + `unlock_key(N)` |
| M→H | `0x81` | ACK_ENTER | `status(1)`（0=OK） |
| H→M | `0x02` | DATA_FRAME | `start_addr(4)` + `end_addr(4)` + `data(LEN-8)` |
| M→H | `0x82` | ACK_DATA | `start_addr(4)`（回显本帧起始，便于续传/重传） |
| H→M | `0x03` | END_FRAME | `total_len(4)` + `total_crc16(2)` |
| M→H | `0x83` | ACK_END | `status(1)`（0=OK；**已烧公钥时不再提交**） |
| H→M | `0x04` | SIGN_FRAME | `signature(64)` |
| M→H | `0x84` | ACK_SIGN（QBOOT SUCCESS） | `status(1)`（0=OK） |
| M→H | `0xCx` | NAK | `reason(1)`（见错误码） |

> 否定响应统一用 `0xC1/0xC2/0xC3/0xC4`（对应 enter/data/end/sign 阶段失败），`reason` 字节区分原因（地址越界、密钥错、CRC 错、擦写失败、长度不符、签名无效等）。

### 8.3 流程

1. **握手（确认目标 + 解锁）**
   - 上位机发 `ENTER_OTA_REQ`：`app_start_addr = 0x08007000` + 约定的 `unlock_key`（建议 8 字节，出厂/配置写入）。
   - QBOOT 校验 `app_start_addr == APP_START` 且 `unlock_key == QBOOT_UNLOCK_KEY`：
     - 均正确 → 回 `0x81 ACK_ENTER`，进入"接收数据"状态；
     - 校验通过即把交互标志写为 `QIAP`（`quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_QIAP)`），使升级中途掉电/复位后下次自动重进升级，不会跳半残 APP。
     - 任一不符 → 回 `0xC1 NAK`（reason），可重试有限次，超时退出/回 APP。

2. **逐帧烧写（带偏移与校验）**
   - 上位机发 `DATA_FRAME`：`start_addr`、`end_addr`、`data`（`end_addr - start_addr + 1 == data长度`）。
   - QBOOT **地址校验**（关键）：`start_addr >= APP_START` 且 `end_addr <= APP_END`，且落入合法页边界；否则 `0xC2 NAK`。
     - 因为 APP 区相对整片 Flash 有 `APP_START` 偏移，所有收到的地址都已是**绝对 APP 区地址**（上位机按 APP 实际链接地址下发），QBOOT 直接按该绝对地址写 Flash；"偏移"的含义体现在：QBOOT 必须拒绝任何 `< APP_START` 或 `> APP_END` 的地址，防止误写 QBOOT 自身或交互标志。
   - QBOOT 对该帧覆盖到的每个新页，先擦除（记录"已擦页"避免重复擦），再按页编程写 `data`（PY32F040xB：256 B 页；GD32：2 KB 页）；写后**回读校验**字节一致。
   - 累加全局 `rx_total_len += data长度`、`rx_total_crc = crc16_data(data, len, rx_total_crc)`。
   - 正确 → 回 `0x82 ACK_DATA`（带 `start_addr`）；失败 → 回 `0xC2 NAK`，上位机重传本帧。

3. **收尾校验**
   - 全部帧发完后，上位机发 `END_FRAME`：`total_len(4)` + `total_crc16(2)`。
   - QBOOT 比较 `rx_total_len == total_len` 且 `rx_total_crc == total_crc16`：
     - 不一致 → 回 `0xC3 NAK`，上位机可重发 END_FRAME 或重新整包升级。
     - 一致 → 回 `0x83 ACK_END`。**是否就此提交取决于是否烧入验签公钥**：
       - **未烧公钥（安全启动未启用）**：END 直接把交互标志写为 `APP1` 并复位，行为与历史版本一致，老上位机无需改动。
       - **已烧公钥**：END **只回 ACK，不提交**，必须等下面的 SIGN 帧校验通过。

4. **签名并提交（安全启动启用时必需）**
   - 上位机发 `SIGN_FRAME`：`signature(64)`。
   - 设备端按顺序做：
     1. 对 Flash 中 `[APP_START, APP_START+total_len)` 实时算 SHA-256；
     2. 校验 `Ed25519(pubkey, image_len||sha256, signature)`；
     3. **从已签名镜像取固件安全版本**：读 `quilla_qboot_secure_fw_version()`（固定地址 `QUILLA_QBOOT_APP_START + 0x400`）；若为 `0`（区域缺失）或低于防回滚下限则拒。版本**属于已签名镜像**，绝不来自上位机；
     4. 写 manifest（magic/version=nv/image_len/sha256/sig）到 META 记录；
     5. 抬高防回滚下限到 `nv`；
     6. 落 seal 并写标志 `APP1`，随后**系统复位**。
   - 任一步失败 → 回 `0xC4 NAK`（`0x08` 签名无效），**不提交、不复位**，设备停在升级态等待重试。

> **签名对象约定**（两端必须一致）：
> `image_len(4B LE) || SHA-256(image)(32B)`，共 36 字节。
> 版本号**不在**签名对象内：它位于 `image_len` 之内、固定地址 `QUILLA_QBOOT_APP_START + 0x400`，属于被签名镜像的一部分，受摘要与签名覆盖，无私钥无法伪造。上位机与 QBOOT 都不发明它。
> 设备端用**自己实时算出的**摘要参与验签，而不是 manifest 里存的那份，
> 因此篡改 Flash 镜像会改变签名输入从而直接验签失败。

### 8.4 错误码（reason）建议
- `0x01` 地址越界（< APP_START 或 > APP_END）
- `0x02` 解锁密钥错误
- `0x03` 帧内 CRC 错误
- `0x04` Flash 擦除失败
- `0x05` Flash 编程/回读失败
- `0x06` 总长度/总 CRC 不符
- `0x07` 长度字段与 data 不符
- `0x08` 签名无效（未签名、签名不匹配，或镜像长度非法）

---

## 8.5 安全启动（Ed25519 验签 + 防回滚）

烧入验签公钥后，QBOOT 只执行**签名合法且版本未回退**的 APP。未烧公钥时整套机制自动旁路（`QUILLA_SB_INACTIVE`），保持历史行为，因此产线可按需逐台启用。

### 签名对象

```
image_len(4B LE) || SHA-256(image)(32B)     共 36 字节
```

不直接签整个镜像：Ed25519 只需一次 36 字节的验签调用，唯一需要流式处理镜像的是 SHA-256，代价小得多。版本号**不在**签名对象内：它位于 `image_len` 之内、固定地址 `QUILLA_QBOOT_APP_START + 0x400`，属于被签名镜像的一部分，受摘要与签名覆盖，无私钥无法伪造。设备端用**自己实时算出的**摘要参与验签，而非 manifest 里存的那份，所以篡改 Flash 镜像必然改变签名输入而验签失败。

### manifest（META 记录内的 `QMF1` 块，128B）

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 4 | magic `QMF1`（`0x514D4631`） |
| 4 | 4 | version |
| 8 | 4 | image_len |
| 12 | 32 | SHA-256(image) |
| 44 | 64 | Ed25519 签名 |
| 108 | 4 | CRC-32 |

manifest 不是独立页，而是磨损均衡 META 记录内的 128 B `QMF1` 块（见第 5 节），由 `quilla_qboot_meta_set_manifest()` 追加写入；无整页擦除，因此写 manifest 不会破坏镜像尾部。仅在安装镜像时写入（`QBOOT_CHECK` 模式不写入）；无 manifest 时该块仅含 `QMF1` magic 与 `version`，`image_len = 0`，其余为 `0xFF`。

### 启动校验（`quilla_qboot_secure_verify_app()`）

```
读 manifest → magic 不对          → ERR_NO_MANIFEST（拒跳）
image_len 越界                    → ERR_LEN（拒跳）
算 SHA-256(image) ≠ manifest 摘要  → ERR_HASH（拒跳）
Ed25519 验签失败                   → ERR_SIG（拒跳）
version < 防回滚下限               → ERR_ROLLBACK（拒跳）
全部通过                          → 抬下限到 version，返回 OK，跳转 APP
```

拒跳时 QBOOT **不复位、不跳 APP**，打印原因后落交互 shell，`qiap` 仍可达——设备保持可救活而不是变砖。

### 防回滚下限

下限是磨损均衡 META 记录的一个字段（QMTL meta 块，`QUILLA_META_OFF_FLOOR`），单调递增。升级提交与启动校验通过均将下限抬至当前版本。版本**属于已签名的固件镜像**（烧录在 `QUILLA_QBOOT_APP_START + 0x400`，受签名覆盖），绝不接受上位机注入，QBOOT 只把下限抬到它，因此携带更低固件版本的降级（已签名）镜像被拒。抬升失败按尽力而为处理：签名已认可的镜像不应因下限写失败而变砖，签名才是主闸门。（APP 另带对该区域的自我检查 `QUILLA_APP_SECVER_SELFCHECK`——混淆、非安全控制，见 `doc/secure-boot.md` §1.1。）

### 密钥与工具

```bash
python tools/quilla_ed25519_sign.py keygen --dir keys
#   -> keys/private.hex（产线保管，绝不外泄）
#   -> keys/public.hex（烧进 QBOOT）
#   并打印：  -DQUILLA_SECURE_BOOT_PUBKEY={0x..,...,0x..}
```

把该宏加入 QBOOT 构建（Keil `Define` / `Makefile.qboot` `C_DEFS`）即启用安全启动。
私钥只留在产线，烧录时：

```bash
python tools/quilla_host_upgrader.py COM3 app.bin --key keys/private.hex
```

---

## 9. 可靠性与安全

- **断电/中途失败**：升级未完成（如断电）时，交互标志自合法 ENTER 帧被接受之时起即读作 `QIAP`（见第 5 节），下次上电直接进升级，不会跳半残 APP。QBOOT 跳转 APP 前仍会做**最小完整性检查**（如校验 APP 复位向量非全 0xFF、栈指针在 RAM 范围内）；不通过则停留 QBOOT 等待重刷。
- **防误刷**：`unlock_key` + `app_start_addr` 双重确认；地址越界硬拒绝；交互标志永不参与烧写。
- **写保护（可选）**：可用 Option Byte 的 WRP 把 QBOOT 区（PY32F040xB 页 0–95，即 24 KB）写保护，防止 QBOOT bug 自毁 bootloader。
- **看门狗**：QBOOT 期间若启用 IWDG，需在擦写循环中及时喂狗或临时关闭，避免擦写耗时触发复位。
- **回滚**：当前设计单一 APP 槽，无 A/B 备份；如需回滚需再规划第二个 APP 槽与选择逻辑（超出本设计范围）。

---

## 相关源文件

| 文件 | 作用 | 归属构建 |
| --- | --- | --- |
| `doc/qboot-app-architecture.md` | 本设计文档 | 文档 |
| `os/services/shell/quilla-shell.h` | `shell_set_partition_tag()` | 共用 |
| `os/services/shell/quilla-shell.c` | `part` 命令 + 标记变量 | 共用 |
| `os/services/quilla-qboot/quilla-qboot-meta.h/.c` | 磨损均衡 META 存储（flag/floor/seal/manifest） | 共用 |
| `os/services/quilla-qboot/quilla-qboot-flag.h/.c` | 交互标志读写（委派 META） | 共用 |
| `os/services/quilla-qboot/quilla-app-qboot.h/.c` | APP 侧 `swqboot` 命令 | APP |
| `os/services/quilla-qboot/quilla-qiap.h/.c` | QBOOT 侧 QIAP 状态机 | QBOOT |
| `os/services/quilla-qboot/quilla-qboot-hal.h` | 平台需实现的硬件接口 + 布局常量 | 声明 |
| `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` | 上述接口的各 MCU 实现（Flash/复位/跳转/发送） | 共用 |
| `arch/cpu/arm/<mcu>/qboot-main.c` | QBOOT 入口（`main()`、串口 ring、启动策略） | QBOOT |

> **分层约定**：`os/services/quilla-qboot/*` 全部硬件无关，不 include 任何 HAL/CMSIS 头；机器操作经 `quilla_qboot_hal_*()` 下发到 `arch/cpu/arm/<mcu>/quilla-qboot-hal.c`（与 `int-master.h`/`clock.h` 同范式）。分区布局常量在 `quilla-qboot-hal.h` 中以 `#ifndef` 给出默认值，换平台用 `-D` 覆盖。
| `user/<mcu>/EIDE/py32f040xb_qboot.ld`（GD32 为对应 QBOOT 脚本） | QBOOT 链接脚本 | QBOOT |
| `user/<mcu>/EIDE/py32f040xb_app.ld`（GD32 为对应 APP 脚本） | APP 链接脚本（`ORIGIN=0x08007000`，PY32F040xB `LENGTH=0x19000`） | APP |
| `user/<mcu>/EIDE/Makefile.qboot` | QBOOT 构建参考（`make -f Makefile.qboot`） | QBOOT |
