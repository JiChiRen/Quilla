**[中文](upgrade-flow.zh-CN.md) · [English](upgrade-flow.md)**

# Quilla 在线升级流程（上位机 → QBOOT → 复位）

本文描述一次完整串口升级：进入 QBOOT、逐帧处理与 Flash 写入、END 传输层校验、SIGN 签名校验与 manifest 落盘，直至复位进入新 APP。

- 复位**之后**的启动校验与跳转 APP 流程见 [`boot-flow.md`](boot-flow.md)。
- 协议规范与错误码总表见 [`qboot-app-architecture.md`](qboot-app-architecture.md)。

---

## 1. 参与方与源码索引

| 环节 | 代码 |
| --- | --- |
| 上位机工具 | `tools/quilla_host_upgrader.py` |
| 上位机签名 | `tools/quilla_ed25519_sign.py` |
| QBOOT 入口（main / 串口环 / shell） | `arch/cpu/arm/<mcu>/qboot-main.c` |
| QIAP 帧状态机 + 写 Flash | `os/services/quilla-qboot/quilla-qiap.c` |
| 安全启动策略（验签 / 防回滚 / manifest） | `os/services/quilla-qboot/quilla-qboot-secure.c` |
| Ed25519 验签 | `os/services/quilla-qboot/quilla-ed25519.c` |
| 交互标志 | `os/services/quilla-qboot/quilla-qboot-flag.c` |
| 硬件操作（Flash / 复位 / 跳转 / 发送） | `arch/cpu/arm/<mcu>/quilla-qboot-hal.c` |
| 分区常量（唯一来源） | `os/services/quilla-qboot/quilla-qboot-hal.h` |

---

## 2. 分区布局（烧录与校验的坐标系）

| 区域 | 地址范围 | 长度 | 说明 |
| --- | --- | --- | --- |
| QBOOT | `0x08000000` – `0x08005FFF` | 24 KB（PY32F040xB 页 0–95） | 含 SHA-256/512 + Ed25519 验签 |
| APP（可烧录区） | `0x08007000` – Flash 末址 | PY32F040xB `0x19000` = 102,400 B；GD32 `0x79000` = 495,616 B | DATA 帧只接受此区间 |
| META 区 | `0x08006000` – `0x08006FFF` | 4 KB | 磨损均衡的标志 + 安全启动 manifest 日志（双 2 KB 槽，见下） |

设计约束：

- **META 区（含 manifest）不在 APP 区内**（`QUILLA_QBOOT_APP_END` = `SOC_FLASH_END`：PY32F040xB `0x0801FFFF`，GD32F502VE `0x0807FFFF`）；升级协议硬性拒绝任何写入 META 区的地址，链接器也不会在该处放置代码。
- **META 存储布局**：4 KB META 区拆成两个 2 KB 槽——SLOT_A（`0x08006000`）与 SLOT_B（`0x08006800`）。每条记录为 256 B 步长 = 64 B meta 块（magic `QMTL`，`0x514D544C`）+ 128 B manifest 块（magic `QMF1`，`0x514D4631`）+ 64 B 填充，并以**只编程不擦除**的方式追加写入——活动槽（active）永不在原地擦除。只有当某槽是 standby（不持有最新记录的槽）时才会被擦除，且会在 active 槽写满之前预先擦好。读取时扫描两个槽、取序列号最高的有效记录为权威，因此任何时刻掉电都留下一条完整可读的记录。
  - **meta 块**携带 `flag`（`APP1`/`QIAP`/`QBOOT`）、防回滚 `floor` 与密封镜像记录（`seal_hash`/`seal_len`）。
  - **manifest 块**携带 `version`（由 QBOOT 在安装时依内部防回滚下限赋值——既不来自上位机，也不取自固件镜像）、`image_len`、`hash` 与 `sig`；只在安装镜像时写入（QIAP 的 SIGN / 仅完整性 END 路径经 `quilla_qboot_meta_set_manifest`），`QBOOT_CHECK` 模式下从不写入。无 manifest 时该块仅含 `QMF1` magic + `version` 且 `image_len = 0`，其余为 `0xFF`。
  - 两个块各自带独立 magic + CRC-32，因此其中一个块写入撕裂不会拖垮另一个（"独立日志"）。

所有偏移量的唯一来源是 `quilla-qboot-hal.h` 的 `QUILLA_QBOOT_APP_START`。

---

## 3. 帧格式

```
+------+------+-----------+---------+------------+
| SOF  | CMD  | LEN(2,LE) | PAYLOAD | CRC16(2,LE)|
| 0xA5 | 1B   | 2B        | LEN B   | 2B         |
+------+------+-----------+---------+------------+
         └────────── CRC16 覆盖 CMD+LEN+PAYLOAD ────┘
```

- CRC 初值 0，算法 `crc16_data()`（`os/lib/crc16.c`）。
- 单帧 payload 上限 `QUILLA_QIAP_MAX_PAYLOAD = PAGE_SIZE + 8 = 264` 字节；超长帧丢弃，等待下一个 SOF。

### 命令表

| 方向 | CMD | 名称 | payload |
| --- | --- | --- | --- |
| H→M | `0x01` | ENTER | `app_start(4)` + `unlock_key(8)` |
| H→M | `0x02` | DATA | `start(4)` + `end(4)` + `data(≤256)` |
| H→M | `0x03` | END | `total_len(4)` + `total_crc16(2)` |
| H→M | `0x04` | SIGN | `signature(64)`（共 64 B） |
| M→H | `0x81` | ACK_ENTER | `status(1)` |
| M→H | `0x82` | ACK_DATA | `start_addr(4)`（回显） |
| M→H | `0x83` | ACK_END | `status(1)` |
| M→H | `0x84` | ACK_SIGN | `status(1)` |
| M→H | `0xC0\|phase` | NAK | `reason(1)`；phase：ENTER=1、DATA=2、END/其它=3、SIGN=4 |

### NAK reason

| 值 | 含义 |
| --- | --- |
| `0x01` | 地址越界（不在 APP 区） |
| `0x02` | unlock key 不符 |
| `0x03` | 帧 CRC 错（**可重传**） |
| `0x04` | 页擦除失败 |
| `0x05` | 编程/回读校验失败 |
| `0x06` | 总长度或总 CRC 不符 |
| `0x07` | 长度字段与实际数据长度不符 |
| `0x08` | 签名校验未通过 |
| `0x09` | 镜像无可用安全版本区（缺 `.quilla_secver` / magic 不符） |
| `0x0A` | 固件安全版本低于设备防回滚下限（拒绝降级） |

---

## 4. 总体时序

```
上位机                                    MCU (QBOOT)
  |                                            |
  |-- (0) 让设备停在 QBOOT -------------------->|
  |      APP 内 'swqboot' / flag==QIAP 遗留     |
  |                                            |
  |-- (1) 读取 banner，必要时发 'qiap' ---------->|  qboot_qiap_run()
  |                                            |
  |-- ENTER 0x01 ------------------------------>|  校验地址+密钥, flag=QIAP
  |<- ACK 0x81 --------------------------------|
  |                                            |
  |-- DATA 0x02 ×N (每片 256B) --------------->|  逐页读-改-擦-写 + 回读校验
  |<- ACK 0x82 (回显起始地址) ×N ---------------|  累加 total_len / total_crc
  |                                            |
  |-- END 0x03 -------------------------------->|  总长度+总 CRC 比对
  |<- ACK 0x83 --------------------------------|  (有公钥时此处不提交)
  |                                            |
  |-- SIGN 0x04 ------------------------------>|  实时 SHA-256 + Ed25519 验签
  |                                            |  + 防回滚 + 写 manifest
  |                                            |  + 抬升下限 + flag=APP1
  |<- ACK 0x84 --------------------------------|
  |                                            |
  |                                            |-- quilla_qboot_hal_reboot()
  |                                            |   (关中断 + NVIC_SystemReset)
```

---

## 5. 阶段 0：让 MCU 停在 QBOOT

升级必须发生在 QBOOT 中。三种进入方式：

| 方式 | 触发 | 结果 |
| --- | --- | --- |
| APP 主动切 | APP shell 执行 `swqboot`（`quilla-app-qboot.c`） | `flag=QBOOT` → 复位 → QBOOT shell（**不自动升级**） |
| 上位机请求 | QBOOT shell 收到文本 `qiap\n` | 进入 QIAP 接收循环 |
| 掉电恢复 | 标志已是 `QIAP`（记录于 META 存储） | 上电直接进入升级态（半烧录可恢复） |

`swqboot` 只把设备停在 QBOOT shell，不启动升级——这是刻意设计，避免误触进入烧录态。
启动升级仍需发送 `qiap`。

---

## 6. 阶段 1：切进 QIAP 接收循环

`qboot-main.c: qboot_qiap_cmd_handler()` → `qboot_qiap_run()`：

```c
shellport_set_raw_sink(qboot_rx_sink);   /* DMA ISR 收到的字节改投 ring */
quilla_qiap_init();                      /* 状态机复位：done=0, session=0 */
shell_puts("\r\n[QIAP] enter upgrade mode. Send ENTER frame.\r\n");
while(1) {
  if(quilla_qiap_is_done()) {            /* 升级成功 -> 复位 */
    shell_puts("\r\n[QIAP] success. Rebooting...\r\n");
    quilla_qboot_hal_reboot();
  }
  if(qboot_rx_pop(&b) == 0) quilla_qiap_feed(b);
}
```

**Flash 擦写不在中断中执行。** DMA 中断只把字节推进 256 B 环形缓冲（`qboot_rx_sink`），主循环再逐个喂给 `quilla_qiap_feed()`。缓冲区满则丢弃，由上位机重传兜底。

上位机侧 `ensure_upgrade_mode()` 先读 banner：

- 含 `enter upgrade mode` → 已在升级态；
- 含 `[BOOT]` → 发 `qiap\n`，3 秒内等待 `enter upgrade mode`；
- 均无 → 报错退出。

注：`[BOOT]` 取自 QBOOT banner 的行首标记；`part` 命令回显含 `qboot`，可作二次确认。

---

## 7. 阶段 2：帧解析状态机

`quilla-qiap.c: quilla_qiap_feed()`，逐字节推进：

```
S_SOF ─(0xA5)─> S_CMD ─> S_LEN1 ─> S_LEN2 ─> S_PAYLOAD ─> S_CRC1 ─> S_CRC2
                                      │                                 │
                            len>264: 弃帧回 S_SOF              CRC 不符: 发 NAK 0xC3/0x03
                                                               CRC 相符: dispatch()
                                                               之后一律回 S_SOF
```

CRC 边收边算（每字节 `crc16_data`），收完两字节 CRC 后比对；不符则发 NAK 并重新同步，不做部分解析。
---

## 8. 阶段 3：ENTER —— 握手并落 QIAP 标志

`dispatch()` 的 `QUILLA_QIAP_CMD_ENTER` 分支：

1. `len < 12` → NAK `0xC1/0x07`
2. `app_start != QUILLA_QBOOT_APP_START`（即 `0x08007000`）→ NAK `0xC1/0x01`
3. 密钥逐字节比对（无 `memcmp`，用无分支 XOR 累积，避免引入 libc）→ 不符 NAK `0xC1/0x02`
4. 清零 `rx_total_len` / `rx_total_crc` / `end_seen`，置 `session_active=1`
5. `quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_QIAP)`
6. ACK `0x81`

第 5 步的作用：此后即使中途掉电或断线，下次上电读到的 flag 为 `QIAP`，QBOOT 会重新进入升级流程，而不是跳转到烧写一半的 APP。

---

## 9. 阶段 4：DATA —— 逐片写入并回读校验

每片一页（PY32F040xB：256 B；GD32：2 KB），payload = `start(4) + end(4) + data`。

`dispatch()` 的 `DATA` 分支校验顺序：

1. `session_active` 为假 → NAK `0xC2/0x07`
2. `len < 8` → NAK `0xC2/0x07`
3. `start < APP_START` 或 `end > APP_END` 或 `end < start` 或 `(end-start+1) != datalen` → NAK `0xC2/0x01`
   - `end` 上界为 `APP_END`（= `SOC_FLASH_END`：PY32F040xB `0x0801FFFF`、GD32F502VE `0x0807FFFF`），META 区天然被排除，上位机无法误写。设备侧边界才是权威；上位机传输前经 `pinfo` shell 命令取得该边界。
4. `flash_write_range()` → 失败 NAK `0xC2/0x04`（擦除）或 `0xC2/0x05`（编程/回读）
5. 累加 `rx_total_len`，链式累加 `rx_total_crc`
6. ACK `0x82` + 回显 `start`（上位机逐片核对地址，防止串位）

### `flash_write_range()` 的读-改-擦-写

```c
while(off < datalen) {
  page_base = (addr + off) & ~(PAGE_SIZE - 1);
  page_off  = (addr + off) - page_base;
  n = min(PAGE_SIZE - page_off, datalen - off);

  quilla_qboot_hal_flash_read(page_base, staging, PAGE_SIZE);   /* 读整页 */
  memcpy(staging + page_off, data + off, n);                    /* 改 RAM  */
  quilla_qboot_hal_flash_write_page(page_base, staging);        /* 擦 + 写 */
  quilla_qboot_hal_flash_read(page_base + page_off, staging, n);/* 回读    */
  if(memcmp(staging, data + off, n)) return -4;                 /* 校验    */
  off += n;
}
```

- `staging` 使用 `QUILLA_QBOOT_ALIGNED`（4 字节对齐）——Cortex-M0+ 非对齐字访问会触发 HardFault。
- 分批写入因此是安全的：不足一页的帧不会把同页已有内容擦成 `0xFF`。
- 底层 `quilla_qboot_hal_flash_write_page()`：`HAL_FLASH_Unlock` → 擦 1 页 → `HAL_FLASH_PageProgram` → `HAL_FLASH_Lock`。

---

## 10. 阶段 5：END —— 传输层总校验

payload = `total_len(4) + total_crc16(2)`。

1. `session_active` 为假 → NAK `0xC3/0x07`
2. `len < 6` → NAK `0xC3/0x07`
3. `rx_total_len != total_len || rx_total_crc != total_crc` → NAK `0xC3/0x06`（可整包重发）
4. `end_seen = 1`
5. 分支：
   - **未烧公钥**（`quilla_qboot_secure_pubkey_active()==0`）→ `commit_app()`：写 `flag=APP1`、`done=1`，再 ACK `0x83`。
     兼容未启用安全启动的设备与开发流程。
   - **已烧公钥** → **仅回 ACK，不提交**。未签名镜像不会被标记为可运行，必须等待 SIGN 通过。
6. ACK `0x83`

该分支是安全启动接入协议的核心改动：END 由「提交点」降级为「传输完成确认」。

---

## 11. 阶段 6：SIGN —— 完整校验 + 写 manifest + 提交

payload = `signature(64)`，长度必须恰好 64 B。

### 11.1 前置检查

| 条件 | 结果 |
| --- | --- |
| `!session_active \|\| !end_seen` | NAK `0xC4/0x07` |
| `len != 64` | NAK `0xC4/0x07` |
| 未烧公钥 | 直接 `commit_app()` + ACK `0xC4`（避免 SIGN 帧使未启用安全启动的设备不可用） |
| `rx_total_len == 0` 或 `> MAX_IMAGE_LEN`（`APP_END − APP_START + 1`；PY32F040xB 为 `0x19000`） | NAK `0xC4/0x07` |

### 11.2 实时算摘要

```c
quilla_qboot_secure_hash_range(QUILLA_QBOOT_APP_START, rx_total_len, hash);
```

从 **Flash 当前实际内容**流式计算 SHA-256（每次 64 B 喂 `sha256_update`）。
签名绑定的是「此刻 Flash 里是什么」，而非上位机声称发送了什么。

### 11.3 验签

签名对象为固定 36 字节描述符：

```
image_len(4, LE) || SHA-256(image)(32)
```

版本号**不在**签名对象内：它位于 `image_len` 之内、固定地址 `QUILLA_QBOOT_APP_START + 0x400`，属于被签名镜像的一部分，受摘要与签名覆盖，无私钥无法伪造。上位机与 QBOOT 都不发明它。`quilla_qboot_secure_verify_descriptor()` 用**刚算出**的 `hash` 重新拼装描述符，调用 `quilla_ed25519_verify()`。

- 不通过 → NAK `0xC4/0x08`，镜像留在 Flash 但**不被标记为可运行**。

### 11.4 版本赋值（防回滚）

防回滚版本**烧录在已签名的固件镜像**内（固定 Flash 地址 `QUILLA_QBOOT_APP_START + 0x400`，即 `QUILLA_APP_SECURITY_VERSION`），既不归 QBOOT 也不归上位机。验签通过且已认证的 SIGN 提交成功后，QBOOT 读取固件版本并把下限抬到它（低于则拒）：

```c
fw_version = quilla_qboot_secure_fw_version();  /* 取自签名镜像，非上位机注入 */
if(fw_version == 0U) {
    send_nak(4U, QUILLA_QIAP_RC_VER);      return;  /* 无可用安全版本区 */
}
if(fw_version < quilla_qboot_secure_min_version_get()) {
    send_nak(4U, QUILLA_QIAP_RC_ROLLBACK); return;  /* 低于下限，拒绝降级 */
}
nv = fw_version;
quilla_qboot_meta_set_manifest(nv, rx_total_len, hash, payload);
quilla_qboot_secure_min_version_set(nv);
```

上位机无法影响该值，它也不取自固件镜像，因此操作员与恶意构建都无法伪造更低的版本。真正的回滚拒绝（`version < floor`）在启动时由 `quilla_qboot_secure_verify_app()` 执行，见 [`boot-flow.md`](boot-flow.md)。

### 11.5 写 manifest

manifest 不是独立页：它是磨损均衡 META 记录里 128 B 的 `QMF1` 块，由 `quilla_qboot_meta_set_manifest()` 写入（追加一条新 META 记录而非原地擦写）。块布局：

| 偏移 | 字段 | 长度 |
| --- | --- | --- |
| 0 | magic `QMF1` (`0x514D4631`) | 4 |
| 4 | version | 4 |
| 8 | image_len | 4 |
| 12 | SHA-256(image) | 32 |
| 44 | Ed25519 signature | 64 |
| 108 | CRC-32 | 4 |
| — | 合计 | **128 B**（其余保留 / `0xFF`） |

manifest 只在安装镜像时写入——SIGN 路径（`version`/`image_len`/`hash`/`sig`）与仅完整性 END 路径（`version`/`image_len`/`hash`，签名留 `0xFF`）。`QBOOT_CHECK` 模式不产生 manifest；此时块仅含 `QMF1` magic + `version` 且 `image_len = 0`，其余 `0xFF`。

### 11.6 提交

```c
if(commit_app() != 0) → NAK 0xC4/0x05;              /* flag = APP1, done = 1 */
send_ack(QUILLA_QIAP_ACK_SIGN, 0U);
```

防回滚下限已在 §11.4 由 QBOOT 抬到**固件版本**（取自已签名镜像，非 QBOOT 自增）。抬升为尽力而为操作：写失败不阻断启动（签名才是主闸门），避免一次 Flash 故障导致设备不可用。

---

## 12. 阶段 7：复位

`quilla_qiap_is_done()` 返回 1 → 主循环打印 `[QIAP] success. Rebooting...` → `quilla_qboot_hal_reboot()`：

```c
__disable_irq();
HAL_NVIC_SystemReset();
while(1) {}
```

先关中断，使刚重写过的交互标志不被任何 ISR 打断读取。

复位之后的流程见 [`boot-flow.md`](boot-flow.md)。

---

## 13. 上位机命令

```bash
# 未启用安全启动（无公钥）的常规升级
python tools/quilla_host_upgrader.py COM3 app.bin

# 启用安全启动：用 keygen 生成的产线私钥发 SIGN 帧。
# 须使用自己 keygen 产出的私钥：仓库默认（公开）钥匙的私钥随仓库一同公开，
# 一旦流出等同公开，因此必须改用自有钥匙并离线保存。
python tools/quilla_host_upgrader.py COM3 app.bin \
       --key keys/prod/private.hex

# 生成产线密钥对（输出 private.hex + public.hex，私钥离线保存）
python tools/quilla_ed25519_sign.py keygen --dir keys/prod
```

`run()` 的实际顺序：`ensure_upgrade_mode()` → `enter()` → `send_data()` → `end()` → `sign()`（仅当指定 `--key`）。

上位机重传策略（`_transact`）：超时或 `reason==0x03`（CRC）最多重试 3 次；其余 reason 立即终止。

若设备已烧公钥而上位机未指定 `--key`，工具会先提示「升级将在 SIGN 处被拒」，随后在 END 之后停止：
设备停在 `flag=QIAP`，可重发正确签名的 SIGN 或直接重跑升级。

完整参数见 `doc/quilla-host-upgrader-guide.md` §10.4；签名工具用法见 `tools/README.md`。

---

## 14. 异常矩阵与恢复

| 故障点 | 设备状态 | 恢复 |
| --- | --- | --- |
| ENTER 前断线 | `flag=QBOOT` 或 APP1 | 重跑工具即可 |
| 数据传输中断电 | **`flag=QIAP`** | 上电自动重进 QBOOT，重发 ENTER 继续烧写 |
| 某片 CRC 错 | 该片未写入 | NAK 后上位机自动重传该片 |
| 回读校验失败 | 该页可能已损坏 | NAK `0x05`，重跑整包升级 |
| END 总长度/CRC 不符 | 未提交 | NAK `0x06`，重发 END 或整包 |
| SIGN 验签失败 | 镜像在 Flash，未提交 | NAK `0x08`；设备停在 `flag=QIAP`，可重发正确签名 |
| SIGN 拒——镜像无安全版本区 | 镜像在 Flash，未提交 | NAK `0x09`；重编 APP 使 `.quilla_secver` 进镜像（Keil：`.ARM.__at_0x08007400`）后重发 |
| SIGN 拒——降级（版本低于下限） | 镜像在 Flash，未提交 | NAK `0x0A`；设备已接受过更新版本——烧录 `QUILLA_APP_SECURITY_VERSION` ≥ 下限的镜像 |
| 提交后、复位前掉电 | `flag=APP1`，镜像完整 | 上电正常启动新 APP |
| manifest 写入失败 | 未提交 | NAK `0x05`，重跑升级 |

任何未提交状态都会使 `flag` 停在 `QIAP`，QBOOT 上电自动回到可交互的升级态，因此升级失败不会导致设备不可用。

---

## 15. 一页速查

```
APP 内 'swqboot'  ──> flag=QBOOT ──> 复位 ──> QBOOT shell
                                                    │ 'qiap'
                                                    v
                                              QIAP 接收循环
ENTER ──> flag=QIAP（掉电恢复锚点）
DATA  ──> 逐页读-改-擦-写 + 回读校验，链累 len/crc
END   ──> 总 len/crc 比对；无公钥则提交，有公钥仅 ACK
SIGN  ──> 实时 SHA-256 → Ed25519 验签 → 防回滚 → 写 manifest
          → 抬升下限 → flag=APP1 → done=1
复位  ──> 见 boot-flow.md
```