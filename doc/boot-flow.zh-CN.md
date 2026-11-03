**[中文](boot-flow.zh-CN.md) · [English](boot-flow.md)**

# Quilla 启动流程（复位后校验与跳转 APP）

本文描述每次上电/复位之后，QBOOT 如何决定「进升级 / 留 QBOOT shell / 校验并跳转 APP」，以及跳转交接的每一步。

- 升级（烧录）流程见 [`upgrade-flow.md`](upgrade-flow.md)。
- 协议规范见 [`qboot-app-architecture.md`](qboot-app-architecture.md)。

---

## 1. 总览

```
                ┌──────────── 上电 / 复位 ────────────┐
                │  Reset_Handler → QBOOT 的 main()   │
                └──────────────────┬──────────────────┘
                                   │
                   clock_init() / flag_init() / shellport_init()
                                   │
                        flag = quilla_qboot_flag_get()
                                   │
        ┌──────────────────────────┼──────────────────────────┐
        │                          │                          │
   flag == QIAP(2)            flag == QBOOT(3)          flag == APP1(1)
        │                          │                   或交互标志空白/损坏
        v                          v                          │
  qboot_qiap_run()         qboot_shell_run()                 v
  （直接进升级，           （留 QBOOT shell：qiap/swapp）      qboot_app_image_valid()
   不再返回）               不再返回                           │
                                                        ┌─────┴─────┐
                                                     失败           成功
                                                        │            │
                                                        v            v
                                                 qboot_shell_run()  qboot_secure_boot_ok()
                                                                      │
                                                              ┌───────┴───────┐
                                                         拒绝(打印原因)       通过
                                                              │               │
                                                              v               v
                                                       qboot_shell_run()  jump_to_app()
```

**两条并列的闸门**：机械完好性（向量表）+ 密码学真实性（签名）。
`main()` 中为 `if(qboot_app_image_valid() && qboot_secure_boot_ok())`，任一不通过即落入 QBOOT shell。

---

## 2. 阶段 1：最小化初始化

`os/services/quilla-qboot/qboot-main.c: main()`（共享入口，跨端口）：

```c
clock_init();            /* QBOOT_BUILD 下只有 SysTick，不引入 LPTIM/lpm */
quilla_qboot_flag_init();
shellport_init();        /* USART + DMA；shell writer 就位 */
shell_set_partition_tag("qboot");
```

`clock_init()` 在 `QBOOT_BUILD` 下被裁剪：只维护 `HAL_IncTick()`，不引入 `rtimer` / LPTIM / 低功耗模块。否则由 `Error_Handler` 引出的依赖链会把 APP 侧驱动拖入 24 KB 的 QBOOT。

---

## 3. 阶段 2：读标志分派

`quilla_qboot_flag_get()` 从最新有效 META 记录缓存标志值（见第 5 节与 `quilla-qboot-meta.c`）。交互标志是磨损均衡 META 记录的一个字段（meta 块 magic `QMTL`，`0x514D544C`），META 未编程（全 0xFF）时返回安全默认 `APP1`：

| 缓存标志 | 判定 | 动作 |
| --- | --- | --- |
| `QIAP` (2) | `QIAP` | `qboot_qiap_run()` —— 直接进升级，**不返回** |
| `QBOOT` (3) | `QBOOT` | `qboot_shell_run()` —— 留 QBOOT shell，**即使 APP 完好也不跳转** |
| `APP1` (1)、或全 FF/未知 | `APP1`（安全默认） | 落到阶段 3 |

两项设计约束：

- **标志读不出 → 默认运行 APP**。META 存储会扫描两个槽、取序列号最高的有效记录，单次 Flash 位翻转（或记录撕裂）都不会把设备锁在 bootloader。
- **`swqboot` 设置的是 `QBOOT` 而非 `QIAP`**：停在 shell 等待命令，不会在进入 QBOOT 时即变为待烧录态。

---

## 4. 阶段 3：向量表机械校验

`qboot_app_image_valid()` 只读 APP 区前 8 字节：

```c
quilla_qboot_hal_flash_read(QUILLA_QBOOT_APP_START, vec, 8);

vec[0] ∈ [RAM_START, RAM_START + RAM_SIZE)        /* 0x20000000 + 16 KB（PY32F040xB） */
vec[1] ∈ [APP_START, APP_END] 且 (vec[1] & 1) == 1 /* 复位向量在 APP 区内且 Thumb */
```

已擦除的 Flash 全为 `0xFF`：SP 落在 RAM 之外、复位向量越界——判定必然失败。

该步骤仅保证「跳转后不会立即进入 HardFault」，**不代表镜像可信**；真实性由阶段 4 判定。

---

## 5. 阶段 4：安全启动闸门

`qboot_secure_boot_ok()` → `quilla_qboot_secure_verify_app()`（`quilla-qboot-secure.c`）。

### 5.1 完整性优先于真实性

`verify_app()` 先无条件执行 §5.2–§5.4 的完整性检查（读 manifest、比对长度、算 SHA-256）。**manifest 缺失或摘要不符，任何变体都拒绝启动**（落入 QBOOT shell）。只有完整性通过后，才按钥匙与启动模式进入真实性判定：

```c
/* 无钥匙（pubkey 全 0xFF / 0x00）：完整性通过即放行，回报 QUILLA_SB_INACTIVE */
if(!quilla_qboot_secure_pubkey_active()) return QUILLA_SB_INACTIVE;
```

密钥区全 `0xFF`（擦空）或全 `0x00` 视为「未启用」。未启用时设备行为与未接入安全启动时一致，
可逐台启用——但**仍要求 manifest 存在且摘要匹配**（见 `doc/secure-boot.md` §4 的写入路径）。

### 5.2 读 manifest

`quilla_qboot_meta_get()` 返回从最新有效 META 记录缓存出的 manifest；`QMF1` manifest 块布局（偏移相对块内，定义于 `quilla-qboot-meta.h`）：

| 偏移 | 字段 | 长度 |
| --- | --- | --- |
| 0 | magic `QMF1` | 4 |
| 4 | version | 4 |
| 8 | image_len | 4 |
| 12 | SHA-256(image) | 32 |
| 44 | Ed25519 签名 | 64 |
| 108 | CRC-32 | 4 |

若无有效 manifest（`has_manifest == 0`，或 `QMF1` magic 缺失 / 其 CRC 失败）→ `QUILLA_SB_ERR_NO_MANIFEST`。

### 5.3 长度范围

```c
image_len == 0 或 > QUILLA_SECURE_BOOT_MAX_IMAGE_LEN（= `APP_END − APP_START + 1`，即整个 APP 区；PY32F040xB 为 `0x19000`，GD32F502VE 为 `0x79000`）→ ERR_LEN
```

manifest 描述的长度必须落在 APP 区内，否则后续哈希会越界读到 manifest 页。

### 5.4 实时算摘要

```c
quilla_qboot_secure_hash_range(QUILLA_QBOOT_APP_START, image_len, hash);
```

**从 Flash 当前内容**流式计算 SHA-256（64 B 一块）。这是整套机制的核心：即使攻击者同时改写镜像与 manifest 中的 hash 字段，验签所用的描述符由**刚算出**的 hash 现拼，签名必然不匹配。

manifest 中存放的 hash 仍参与比较，仅用于区分失败原因：

- 实时 hash ≠ manifest hash → `ERR_HASH`（镜像被改 / 传输出错）
- 签名不通过 → `ERR_SIG`（镜像来源不可信）

### 5.5 验签

```c
quilla_qboot_secure_verify_descriptor(image_len, hash, mf + 44)
```

描述符 = `image_len(4,LE) || SHA-256(image)(32)`，共 36 字节，由一次 `quilla_ed25519_verify()` 完成。固件安全版本**不在**此签名描述符内：它位于 `image_len` 之内、固定地址 `QUILLA_QBOOT_APP_START + 0x400`，属于被签名镜像的一部分，受摘要与签名覆盖——无私钥无法伪造（见 `doc/secure-boot.md` §1.1）。上位机与 QBOOT 都不发明它。

启动路径与升级时的 SIGN 帧**共用该函数**，两处判定不会漂移。

### 5.6 防回滚

```c
if(version < quilla_qboot_secure_min_version_get()) → ERR_ROLLBACK
```

下限是磨损均衡 META 记录的一个字段（QMTL meta 块，`QUILLA_META_OFF_FLOOR`），从未写过则为 0。旧版签名镜像即使签名合法也会被拒绝运行。此处比较的 `version` 是**烧录在已签名镜像内的固件安全版本**（非 QBOOT 发明），绝不来自上位机。QBOOT 只把下限抬到它，因此下限只升不降；带更低固件版本的降级（已签名）镜像被拒。
### 5.7 抬升下限

```c
(void)quilla_qboot_secure_min_version_set(version);
return QUILLA_SB_OK;
```

棘轮只升不降；写失败**不影响启动**（签名才是主闸门），避免单次 Flash 故障导致设备不可用。

### 5.8 结果码与 shell 输出

| 返回 | 打印 |
| --- | --- |
| `QUILLA_SB_OK` / `QUILLA_SB_OK_SEALED` | 允许跳转（已认证） |
| `QUILLA_SB_OK_INTEGRITY_ONLY` | 允许跳转（仅完整性，CHECK 模式） |
| `QUILLA_SB_INACTIVE` | 允许跳转（无钥匙，完整性通过） |
| `ERR_NO_MANIFEST` | `no manifest` |
| `ERR_LEN` | `image length out of range` |
| `ERR_HASH` | `image content altered` |
| `ERR_SIG` | `signature invalid` |
| `ERR_ROLLBACK` | `version rollback attempted` |

`QUILLA_SB_OK` 与 `QUILLA_SB_OK_SEALED` 均表示已认证：后者表示镜像此前已通过 Ed25519 校验，在 SEALED 模式下命中 seal 时跳过完整 Ed25519 验签。`QUILLA_SB_OK_INTEGRITY_ONLY` 仅出现在 CHECK 模式：完整性通过但**未做真实性验证**，不声称 secure boot。

拒绝时打印 `[BOOT] secure boot: APP REFUSED - <原因>`，**不复位、不擦除**，落入 QBOOT shell——`qiap` 依然可达，设备可恢复。

---

## 6. 阶段 5：跳转交接

`quilla_qboot_hal_jump_to_app(QUILLA_QBOOT_APP_START)`（`arch/cpu/arm/<mcu>/quilla-qboot-hal.c`）。
顺序**不可调换**：

```c
__disable_irq();

/* (1) 停掉 QBOOT 自己开过的外设 */
NVIC_DisableIRQ(USART2_IRQn);
NVIC_DisableIRQ(DMA1_Channel4_5_6_7_IRQn);
NVIC_ClearPendingIRQ(USART2_IRQn);
NVIC_ClearPendingIRQ(DMA1_Channel4_5_6_7_IRQn);
USART2->CR3        &= ~USART_CR3_DMAR;   /* 停 USART2 的 RX DMA 请求 */
DMA1_Channel6->CCR &= ~DMA_CCR_EN;       /* 停通道本身              */

/* (2) 接管向量表与栈 */
SCB->VTOR = app_start;
sp        = *(uint32_t *)app_start;
app_reset = (void (*)(void))(*(uint32_t *)(app_start + 4));
__set_MSP(sp);

/* (3) 开中断后再跳 */
__enable_irq();
app_reset();
```

各步骤的必要性：

| 步骤 | 省略的后果 |
| --- | --- |
| 关/清 USART2 与 DMA 中断 | 残留 pending IRQ 在 APP 半初始化时触发，跳入默认 handler |
| 停 DMA 请求与通道 | DMA 继续向「现已属于 APP .data/.bss」的低地址 RAM 写入，破坏 APP 状态 |
| 设 `VTOR` | 异常仍指向 QBOOT 的向量表 |
| **`__enable_irq()` 后再跳** | Cortex-M0+ 上 `PRIMASK=1` 会连同 SysTick 一起屏蔽 → `clock_time()` 冻结、etimer 永不到期，APP 表现为「初始化打印正常，随后 RTOS 停摆」 |

外设编号随 MCU 不同（上例为 PY32F040xB 的 USART2 / DMA1 通道 6）。APP 侧也会在 `system_<mcu>.c` 中设置 `VECT_TAB_OFFSET = QUILLA_QBOOT_APP_OFFSET`（当前 `0x7000`，由 `QUILLA_QBOOT_APP_START 0x08007000` 推导）；QBOOT 先行设置一次，以保证分支后第一条指令起向量即正确。

---

## 7. 阶段 6：落入 QBOOT shell

机械校验失败、签名拒绝、或标志为 `QBOOT` 时进入 `qboot_shell_run()`：

```c
shell_register_cmd(&qboot_qiap_cmd);    /* qiap   : 进入升级 */
shell_register_cmd(&qboot_swapp_cmd);   /* swapp  : 设 flag=APP1 并复位 */
shell_puts("\r\n[BOOT] Quilla V" QUILLA_VERSION_STRING "\r\n> ");
while(1) process_run();
```

- `qiap` → 进入升级循环。
- `swapp` → 写 `flag=APP1` 并复位；若当前 `flag==QIAP`（升级进行中）会**拒绝**并打印 `upgrade in progress, switch refused`，避免在半烧录镜像上强行跳转。
- 内置 `part` 命令此时回答 `qboot`——上位机据此判定设备停在 QBOOT（另一判定依据是 banner 的 `[BOOT]` 行首标记）。

该函数为 `while(1)`：**其后任何代码均不可达**。

---

## 8. 状态判定速查表

| 交互标志 | 向量表 | 公钥 | manifest / 签名 | 结果 |
| --- | --- | --- | --- | --- |
| QIAP | — | — | — | 直接进升级 |
| QBOOT | — | — | — | 留 QBOOT shell（不跳 APP） |
| APP1 | 非法 | — | — | 留 QBOOT shell |
| APP1 | 合法 | 未烧 | — | **跳 APP**（`INACTIVE`） |
| APP1 | 合法 | 已烧 | 无 manifest | 留 QBOOT shell（`NO_MANIFEST`） |
| APP1 | 合法 | 已烧 | hash 不符 | 留 QBOOT shell（`HASH`） |
| APP1 | 合法 | 已烧 | 签名不符 | 留 QBOOT shell（`SIG`） |
| APP1 | 合法 | 已烧 | 版本低于下限 | 留 QBOOT shell（`ROLLBACK`） |
| APP1 | 合法 | 已烧 | 全部通过 | **跳 APP**（并抬升下限） |
| 空白/损坏 | 合法 | 任意 | 同上 | 按 `APP1` 处理 |

---

## 9. 开销与占用

启动路径的开销为 SHA-256 扫描镜像（`image_len` 字节）+ 一次 Ed25519 验签（仅 36 字节描述符），二者仅在**启动**与 **SIGN 帧**各执行一次，APP 运行期不执行，不占用实时链路。SHA-256 耗时与**实际写入的 APP 镜像大小成正比**：在 PY32F040xB（Cortex-M0+ @ 48 MHz）上对 ~18 KB 的 QAPP 镜像约 0.1 s（≈ 180 KB/s，估计）。Ed25519 验签与镜像大小无关：PY32F040xB 上约 3.3 s——为 48 MHz 上板实测的 ≈3.5 s 完整 Ed25519 启动（复位→APP banner，8 MHz 时约 20 s）的主导项，GD32F502VE（M33 @ 160 MHz）实测 < 500 ms。（PY32F040xB 计时为 48 MHz 上板实测；以上为开发过程数据，见第 9 节注。）

QBOOT 24 KB 预算实测占用：

> **注意**：本节所有数据均为**开发过程实测值**（联调期取得），会随软件版本与工具链（版本 / 优化等级 / LTO）变化而不同，**不构成交付规格**；在据此下容量结论前，请在自己的构建上复测。

| 工具链 / 配置 | ROM（text） | 对 24 KB 余量 |
| --- | --- | --- |
| Keil `-O2` + microlib（含 `kern-stack.c`）—— 2026-10-11 实测，CR-0042/0043 之前 | 15,192 B | 约 38% |
| Keil `-O1` —— 2026-10-11 实测，CR-0042/0043 之前 | 19,636 B | 约 20% |
| Keil `-O0` | 溢出 | — |
| GNU `-Os`，QBOOT_CHECK —— 2026-11-03 实测 | 21,208 B | 约 14% |
| GNU `-Os`，QBOOT_SEALED —— 2026-11-03 实测 | 21,308 B | 约 13% |
| GNU `-Os`，QBOOT_SEC —— 2026-11-03 实测 | 21,260 B | 约 13% |

`-O0` 会溢出，单步调试 QBOOT 时须注意。
启用 `KERN_CONF_STACK_SHELL=1` 时 GNU 构建约增加 456 B（2026-10-11 实测）。

版本说明：2026-11-03 的 GNU 数据为分变体（CHECK/SEALED/SEC）实测，工具链
`arm-none-eabi-gcc` 13.2.1，已含 CR-0042 入口单源化与 CR-0043 shell 格式化器改造
（后者移除 newlib printf/malloc 族，每镜像约 -1.6 K）。较 2026-10-11 单一数值
（19,412 B）偏大，主要是期间落了 META 双槽存储与防回滚特性——占用随特性增长、
又随 CR-0043 回落。CH32V305 用 WCH GCC12 工具链实测：三变体 20,987–21,095 B（text）。

---

## 10. 常见故障定位

| 现象 | 原因 |
| --- | --- |
| 反复进升级态 | `flag` 停在 `QIAP`（升级未提交）；重跑升级或执行 `swapp` |
| 一直留 QBOOT shell，无 REFUSED 打印 | 向量表机械校验不通过（APP 未烧 / 烧错地址 / APP 未按 `0x08007000` 链接） |
| 打印 `no signed manifest` | 升级时未发 SIGN 帧，或未指定 `--key` |
| 打印 `image content altered` | Flash 内容与 manifest 描述不符；重烧 |
| 打印 `signature invalid` | 镜像与密钥不匹配（更换过密钥，或镜像长度被改） |
| 打印 `version rollback attempted` | 固件安全版本低于 QBOOT 防回滚下限（下限在每次认证安装后棘轮抬升、从不由上位机复位）；说明设备已接受过更新的镜像，或 META 下限从其他单元带过来。用携带更高 `QUILLA_APP_SECURITY_VERSION` 的镜像重跑升级。版本属已签名镜像的一部分，非 QBOOT 赋值 |
| 跳转后初始化打印正常、随后停摆 | 缺少跳转前的 `__enable_irq()`；检查 `jump_to_app()` |
| APP 中中断不进 | QBOOT 跳转前未清 pending IRQ / 未停 DMA |
