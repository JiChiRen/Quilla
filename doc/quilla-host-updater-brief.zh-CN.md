# Quilla QBOOT 串口升级：速查

完整版见 `doc/quilla-host-updater-guide.md`；参考实现 `tools/quilla_host_updater.py`。

## 1. 前提

设备停在 QBOOT 交互 shell 时，串口为行解析模式；必须先发文本命令 `qiap` 切换到原始字节流，之后才能发二进制升级帧。未发 `qiap` 直接发帧的典型现象是"发 ENTER 无 ACK"。

## 2. 参数

PY32F040xB：USART2 / PA2-TX / PA3-RX；GD32F502VE：USART0 / PB6-TX / PB7-RX。均为 115200-8-N-1；shell 行结束符 `\r` 或 `\n`。

## 3. banner 状态判定

| banner 特征 | 状态 | 动作 |
|---|---|---|
| 含 `enter upgrade mode` | 已在升级态 | 跳过 `qiap`，直接 ENTER |
| 含 `[BOOT]` | QBOOT shell | 发 `qiap`，等 `enter upgrade mode` |
| 均无 | 未进入 QBOOT | 报错退出 |

`part` 命令回显含 `qboot` 可作二次确认（仅 shell 态有效）。
若回显含 `app1`，说明仍在 APP shell，需先发 `swqboot` 复位进 QBOOT。

## 4. 握手时序

```
① 进入 QBOOT：APP 内发 swqboot 并复位 -> 输出 [BOOT] banner
             或 flag==QIAP 遗留 -> 直接输出 enter upgrade mode
② shell 态下发 qiap -> 输出 enter upgrade mode（此后为原始字节流）
③ ENTER -> ACK 0x81
④ 分片 DATA -> 每片 ACK 0x82（回显 start 地址）
⑤ END -> ACK 0x83 -> 写回 APP1 并复位进新 APP
```

## 5. 帧格式

```
SOF 1B 0xA5 | CMD 1B | LEN 2B LE | PAYLOAD LEN | CRC16 2B LE（覆盖 CMD+LEN+PAYLOAD）
```

CRC-CCITT，poly 0x1021，反射型，初值 0，逐字节 `& 0xFFFF`（对齐 `os/lib/crc16.c`）。

## 6. 命令

| CMD | payload | 响应 |
|---|---|---|
| `0x01` ENTER | `app_start(4B LE=0x08007000)` + `unlock_key(8B)` + `image_secver(4B LE)` | `0x81` + status / `0xC1` NAK |
| `0x02` DATA | `start(4B)` + `end(4B)` + `data(≤256B)` | `0x82` 回显 start / `0xC2` NAK |
| `0x03` END | `total_len(4B)` + `total_crc16(2B)` | `0x83` + status / `0xC3` NAK |

- `unlock_key` = `51 55 4C 4C 4F 54 41 31`（ASCII `QULLOTA1`），新产品应改。
- 地址为绝对 APP 区地址（基址 `0x08007000`），`end - start + 1 == len(data)`。
- `total_crc16` 为全部 data 按发送顺序链式 `crc16` 的结果（初值 0）。

### NAK reason（payload 首字节）

`0x01` ADDR · `0x02` KEY · `0x03` CRC（可重传） · `0x04` ERASE · `0x05` PROG · `0x06` LEN · `0x07` SZ

超时 / CRC 重传上限 3 次；其余立即终止。

## 7. 命令行

```bash
python -m pip install pyserial
python tools/quilla_host_updater.py COM3 app.bin          # Windows
python tools/quilla_host_updater.py /dev/ttyUSB0 app.bin  # Linux
python tools/quilla_host_updater.py COM3 app.bin -b 115200 -q
python tools/quilla_host_updater.py --self-test           # 离线自检
```

- 固件用 raw `.bin`，大小 ≤ 目标 MCU 的 APP 区容量上限（`APP_END − APP_START`）。工具运行期向设备的 `pinfo` 命令索取该上限，随 MCU 自适应：PY32F040xB = 102400 B（`0x19000`），GD32F502VE = 495616 B（`0x79000`）。
- 烧录用 Erase Sectors，禁止 Erase Full Chip（会擦除 FLAG 与 manifest 页）。
