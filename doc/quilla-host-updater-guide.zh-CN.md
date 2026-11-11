**[中文](quilla-host-updater-guide.zh-CN.md) · [English](quilla-host-updater-guide.md)**

# Quilla QBOOT 串口升级：上位机对接说明

适用对象：为 Quilla 设备开发串口升级上位机（host）的工程人员。

- 协议权威定义：`doc/qboot-app-architecture.md` §8
- 本文所述参考实现：`tools/quilla_host_updater.py`（Python 3.8+，仅依赖 `pyserial`）
- 图形化 / 打包版替代：[`QuillaUpdater`](https://github.com/JiChiRen/QuillaUpdater)——同一协议的独立 Tk 图形界面 + 命令行（`quflash`），带预检、可协作取消、中英双语。

---

## 1. 前提

设备停留在 **QBOOT 交互 shell** 时，串口处于行解析模式，二进制帧会被当作命令丢弃。
上位机必须先在 shell 中发送文本命令 `qiap` 切到原始字节流，之后才能发送升级帧。
未发送 `qiap` 而直接发帧的典型现象是"发 ENTER 无 ACK"。

---

## 2. 通信参数

| 项 | 值 |
|---|---|
| 串口 | PY32F040xB：USART2 / PA2-TX / PA3-RX；GD32F502VE：USART0 / PB6-TX / PB7-RX |
| 波特率 | 115200 |
| 帧格式 | 8 / N / 1（8-N-1） |
| shell 行结束符 | `\r` 或 `\n`；`qiap` 命令发送 `qiap\n` |

---

## 3. 状态判定

`ensure_upgrade_mode()` 读取上电 banner，据此决定是否需要先行发送 `qiap`：

| banner 特征 | 设备状态 | 动作 |
|---|---|---|
| 含 `enter upgrade mode` | 已在升级态（`flag==QIAP` 遗留） | 跳过 `qiap`，直接发 ENTER |
| 含 `[BOOT]` | QBOOT shell | 发 `qiap`，等待 `enter upgrade mode` |
| 均无 | 未进入 QBOOT | 报错退出 |

补充确认手段：向 shell 发送 `part` 命令，回显含 `qboot` 即确认停在 QBOOT 分区。
该命令在 raw sink 接管后会被帧解析吞掉、无回显，因此只用于 shell 态。

QBOOT shell 还提供 `pinfo` 命令，回显分区布局（`[PART] APP_START=0x… APP_END=0x… APP_SIZE=0x…`）。
主机借此取得设备真实的 `APP_END`（进而得到可烧录上限），而不再假定某个固定 MCU，见 §9。
APP shell 下 `pinfo` 未注册，raw sink 接管后同样被吞掉，与 `part` 一致。

**若设备仍在 APP shell**（`part` 回显含 `app1`）：发送 `swqboot` 使设备置 `flag=QBOOT` 并复位，
待出现 `[BOOT]` banner 后再按上表处理。

---

## 4. 握手时序

```
1. 进入 QBOOT
   a. APP 内发文本 swqboot    -> 置 flag=QBOOT 并复位 -> 输出 [BOOT] banner
   b. 冷启动 flag==QIAP 遗留  -> 直接输出 [QIAP] enter upgrade mode. Send ENTER frame.

2. 若在 shell（情况 a）：发 qiap
   -> 回显 qiap: switching to upgrade mode
   -> 输出 [QIAP] enter upgrade mode. Send ENTER frame.
   此步之后串口才切换为原始字节流。

3. 发 ENTER 帧 -> 收 ACK 0x81
4. 分片发 DATA 帧 -> 每片收 ACK 0x82
5. 发 END 帧 -> 收 ACK 0x83 -> 设备写回 APP1 并复位，进入新 APP
```

---

## 5. 帧格式

```
SOF      1B    固定 0xA5
CMD      1B    命令 / 响应码
LEN      2B    PAYLOAD 长度，uint16 小端
PAYLOAD  LEN   参数
CRC16    2B    uint16 小端，覆盖 [CMD][LEN][PAYLOAD]
```

CRC 与固件 `os/lib/crc16.c` 的 `crc16_data` 逐字节一致：
CRC-CCITT，多项式 0x1021，反射输入/输出，初始值 0，每步按 `unsigned short` 截断（`& 0xFFFF`）。

```python
def crc16_add(b, acc):
    acc ^= b
    acc &= 0xFFFF
    acc = (acc >> 8) | (acc << 8)
    acc &= 0xFFFF
    acc ^= (acc & 0xFF00) << 4
    acc &= 0xFFFF
    acc ^= (acc >> 8) >> 4
    acc &= 0xFFFF
    acc ^= (acc & 0xFF00) >> 5
    acc &= 0xFFFF
    return acc

def crc16(data, acc=0):
    for b in data:
        acc = crc16_add(b, acc)
    return acc & 0xFFFF

def build_frame(cmd, payload):
    body = bytes([cmd]) + struct.pack("<H", len(payload)) + payload
    return bytes([SOF]) + body + struct.pack("<H", crc16(body))
```

---

## 6. 命令与响应

| 方向 | CMD | payload | 响应 |
|---|---|---|---|
| H→M | `0x01` ENTER | `app_start(4B LE)` + `unlock_key(8B)` | `0x81` + status(1) / `0xC1` NAK |
| H→M | `0x02` DATA | `start(4B)` + `end(4B)` + `data(≤256B)` | `0x82` + 回显 start(4) / `0xC2` NAK |
| H→M | `0x03` END | `total_len(4B)` + `total_crc16(2B)` | `0x83` + status(1) / `0xC3` NAK |

约定：

- `unlock_key` 默认 `0x51 0x55 0x4C 0x4C 0x4F 0x54 0x41 0x31`，即 ASCII `QULLOTA1`，
  由 `quilla-qboot-hal.h` 的 `QUILLA_QBOOT_UNLOCK_KEY` 定义；新产品应修改该值。
- 地址为**绝对 APP 区地址**，基址 `0x08007000`；`start`/`end` 须落在 `[APP_START, APP_END]`（PY32F040xB 为 `[0x08007000, 0x0801FFFF]`；末址随目标 MCU 的 `SOC_FLASH_END`），且满足 `end - start + 1 == len(data)`，否则 NAK(ADDR)。
- DATA 单片 `data` 不超过 256 B；`start` 无需页对齐，设备侧按读-改-擦-写处理。
- ENTER 校验通过后落 `QIAP` 标志：升级中途掉电，下次上电自动重进升级态，可续传。
- END 的 `total_len` 为全部 data 字节数之和；`total_crc16` 为对全部 data **按发送顺序**链式累加的结果（`total_crc = crc16(chunk, total_crc)`，初值 0）。设备侧逐步累加比对，不符则 NAK(LEN)。

### NAK reason 码（payload 第 1 字节）

| reason | 含义 | 上位机动作 |
|---|---|---|
| `0x01` | ADDR 地址越界 | 终止 |
| `0x02` | KEY 解锁密钥不匹配 | 终止 |
| `0x03` | CRC 帧/字段错误 | 重传本帧，上限 3 次 |
| `0x04` | ERASE Flash 擦除失败 | 终止 |
| `0x05` | PROG Flash 编程/回读失败 | 终止 |
| `0x06` | LEN 总长或总 CRC 不符 | 终止 |
| `0x07` | SZ 长度字段与数据长度不符 | 终止 |

---

## 7. 重传与错误处理

- 每帧发出后等待 ACK，超时默认 2.0 s，重传上限 3 次。
- `0x03` 重传本帧；ADDR / KEY / ERASE / PROG / LEN / SZ 属配置或 Flash 错误，立即终止。
- 串口上是文本 banner 与二进制 ACK 混合的字节流：上位机只解析以 `0xA5` 起始的帧，
  其余字节按日志丢弃。

---

## 8. 上位机分层结构

| 层 | 职责 |
|---|---|
| 传输层 | 串口收发、组帧/解帧、CRC 校验、超时控制 |
| 会话层 | `enter()` / `send_data()` / `end()` 状态机，累计 `total_len`、`total_crc`，处理重传 |
| UI 层 | 进度、日志、串口与固件文件选择 |

参考实现中的 `Updater` 类即会话层，可独立于 UI 使用。

---

## 9. 参考实现常量与接口

常量取自 `quilla-qboot-hal.h` 与 `quilla-qiap.h`：

| 名称 | 值 | 来源 |
|---|---|---|
| `APP_START` | `0x08007000`（默认） | `quilla-qboot-hal.h`；当前各 MCU 一致 |
| `APP_END` | **运行期从设备取得** | QBOOT shell `pinfo` → 目标 `soc.h` 的 `SOC_FLASH_END`。PY32F040xB = `0x0801FFFF`；GD32F502VE = `0x0807FFFF` |
| `APP_SIZE` | **运行期从设备取得** | APP 区容量上限 = `APP_END` − `APP_START`；随 MCU 而定，工具内不再写死 |
| `UNLOCK_KEY` | `51 55 4C 4C 4F 54 41 31` | `quilla-qboot-hal.h` |
| `PAGE_SIZE` / `CHUNK` | `256` | `QUILLA_QBOOT_PAGE_SIZE` |
| `MAX_PAYLOAD` | `264` | `QUILLA_QIAP_MAX_PAYLOAD` |
| `ACK_TIMEOUT` | `2.0` s | 实现约定 |
| `MAX_RETRIES` | `3` | 实现约定 |

> **APP 边界 MCU 自适应（不再写死）。** 旧版把 `APP_END` 钉死为 `0x0801FFFF`（PY32F040xB 的 128KB），会误拒 GD32F502VE 超过 102400 B 的合法镜像。参考工具现改为升级前查询 QBOOT shell 的 `pinfo` 命令，从运行中的固件推导 `APP_START`/`APP_END`/`APP_SIZE`——主机侧预检上限始终与设备的真实 `SOC_FLASH_END` 一致。若 `pinfo` 不可用（旧固件）或设备已处于升级态（raw sink），工具跳过主机侧上限、交由设备的 ADDR NAK（`0x01`）按真实边界兜底。

| 方法 | 说明 |
|---|---|
| `Updater(port, baud=115200, verbose=True)` | 打开串口并清空缓冲 |
| `ensure_upgrade_mode()` | 按 §3 banner 判定决定是否发送 `qiap`；同时查询 `pinfo` 取得设备真实分区布局 |
| `enter()` | 发送 ENTER（app_start + key），落 `QIAP` 标志；`app_start` 取自设备 `pinfo` |
| `send_data(data, base=None)` | 分片发送 DATA，返回 `(total_len, total_crc)`；`base` 默认取设备 `APP_START` |
| `end(total_len, total_crc)` | 发送 END，写回 APP1 并复位 |
| `run(bin_path)` | 串联：读 bin → `ensure_upgrade_mode` →（主机上限检查）→ `enter` → `send_data` → `end` |
| `self_test()` | 离线自检，见 §10 |

---

## 10. 命令行

### 10.1 依赖安装

```bash
python -m pip install pyserial
```

### 10.2 执行升级

```bash
# Windows
python tools/quilla_host_updater.py COM3 app.bin

# Linux
python tools/quilla_host_updater.py /dev/ttyUSB0 app.bin

# 指定波特率（默认 115200）
python tools/quilla_host_updater.py COM3 app.bin -b 115200

# 减少输出
python tools/quilla_host_updater.py COM3 app.bin -q
```

### 10.3 按 QBOOT 变体选择密钥

升级流程对三种 QBOOT 变体一致，差别仅在是否提供 `--key` 与用哪把私钥（见 `doc/secure-boot.md` §2）：

| 目标变体 | `--key` | 说明 |
|----------|---------|------|
| **QBOOT_CHECK** | 不传 | 无钥匙；END 帧直接写完整性 manifest 并提交。**切勿传 `--key`**——设备收到 END 后已提交并复位，随后的 SIGN 帧会超时并误报失败。 |
| **QBOOT_SEALED** | `tools/_keytest/private.hex`（默认私钥） | 研发 / 联调用，默认公钥已编译进 QBOOT。 |
| **QBOOT_SECURED** | 与 SEALED 同一把——默认私钥 `tools/_keytest/private.hex`，或替换后的自有私钥 | 必须与编译进 QBOOT 的公钥匹配（默认公钥⇔默认私钥；自有公钥⇔自有私钥），否则 SIGN 验签失败、升级被拒。 |

示例：

```bash
# QBOOT_CHECK（仅完整性，不需要密钥）
python tools/quilla_host_updater.py COM3 app.bin

# QBOOT_SEALED（研发态，默认(公开)钥匙）
python tools/quilla_host_updater.py COM3 app.bin --key tools/_keytest/private.hex

# QBOOT_SECURED（量产态，SECURE 模式每电验签；钥匙与 SEALED 同一把）
python tools/quilla_host_updater.py COM3 app.bin --key keys/product_priv.hex
```

> 上位机**不**传版本号（从来就不传）：防回滚版本**烧录在已签名的 APP 镜像**内，即 `QUILLA_APP_SECURITY_VERSION`（固定 Flash 地址 `0x08007400`，受签名覆盖），因此上位机与 QBOOT 都不发明它。启动期与 SIGN 阶段仍强制校验防回滚下限，低于的版本被拒（见 `doc/secure-boot.md` §1.1）。"没有 `--version` 参数"不等于"没有防回滚"。

Windows 下另有批处理 `tools/quilla_app_uart_flash.bat`，用法见该文件的注释头部。

### 10.4 离线自检

```bash
python tools/quilla_host_updater.py --self-test
```

自检覆盖帧编解码往返（ENTER / DATA / END 三种命令）与链式 CRC 累加一致性
（断言 `crc16(d0 + d1) == crc16(d1, crc16(d0))`），不依赖串口。
自检只验证编解码与 CRC，不替代硬件联调。

### 10.5 参数

| 参数 | 说明 |
|---|---|
| `port` | 串口号，如 `COM3`、`/dev/ttyUSB0`；`--self-test` 时可省略 |
| `binfile` | APP 固件 `.bin`（raw binary）；`--self-test` 时可省略 |
| `-b`, `--baud` | 波特率，默认 115200 |
| `-q`, `--quiet` | 减少输出 |
| `--key PATH` | 签名私钥文件（`private.hex`）。指定后才发送 SIGN 帧 |
| `--probe` | 仅探测设备状态，不执行升级 |
| `--banner-timeout SEC` | 等待 banner 的时长（秒） |
| `--self-test` | 离线自检，不需要串口与固件文件 |

---

## 11. 执行前检查

- 固件为 raw `.bin`，不使用 `.hex` / `.elf`；大小不得超过 **APP 区容量上限**（`APP_SIZE` = `APP_END` − `APP_START`）。参考工具不再写死该值：它查询设备的 `pinfo` shell 命令，从目标 MCU 的 `SOC_FLASH_END` 推导上限，因此 GD32F502VE（512KB）与 PY32F040xB（128KB）一视同仁，无需手工改工具。若 `pinfo` 不可用或设备已处于升级态，主机跳过预检上限，改由设备的 ADDR NAK（`0x01`）兜底边界。
- QBOOT 与 APP 已烧录；烧录配置为 **Erase Sectors**，禁止 Erase Full Chip，后者会擦除 FLAG 与 manifest 页。

---

## 12. 固件侧对应文件

| 上位机概念 | 固件文件 |
|---|---|
| 帧格式、CRC、命令码 | `os/services/quilla-qboot/quilla-qiap.{h,c}` |
| 分区布局、unlock key | `os/services/quilla-qboot/quilla-qboot-hal.h` |
| CRC 算法 | `os/lib/crc16.c` |
| 启动分支（shell / 升级 / 跳转 APP） | `os/services/quilla-qboot/qboot-main.c`（共享） |
| QBOOT 标志状态 | `os/services/quilla-qboot/quilla-qboot-flag.{h,c}` |
| `swqboot` 命令 | `os/services/quilla-qboot/quilla-app-qboot.c` |
