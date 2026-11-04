# Quilla QBOOT serial upgrade: quick reference

Full version in `doc/quilla-host-updater-guide.md`; reference implementation in `tools/quilla_host_updater.py`.

## 1. Prerequisite

While the device sits in the QBOOT interactive shell, the serial port is in line-parsing mode. You must first send the text command `qiap` to switch to a raw byte stream; only then may binary upgrade frames be sent. The typical symptom of sending frames without `qiap` is "ENTER sent, no ACK".

## 2. Parameters

PY32F040xB: USART2 / PA2-TX / PA3-RX; GD32F502VE: USART0 / PB6-TX / PB7-RX. Both 115200-8-N-1; shell line terminator `\r` or `\n`.

## 3. Banner state detection

| Banner contains | State | Action |
|---|---|---|
| `enter upgrade mode` | already in upgrade mode | skip `qiap`, send ENTER directly |
| `[BOOT]` | QBOOT shell | send `qiap`, wait for `enter upgrade mode` |
| neither | not in QBOOT | error out |

The `part` command output containing `qboot` serves as a second confirmation (valid in shell state only).
If the output contains `app1`, the device is still in the APP shell — send `swqboot` first to reset into QBOOT.

## 4. Handshake sequence

```
1. Enter QBOOT: send swqboot from the APP and reset -> [BOOT] banner
                or a leftover flag==QIAP -> "enter upgrade mode" directly
2. In shell state send qiap -> "enter upgrade mode" (raw byte stream from here)
3. ENTER -> ACK 0x81
4. Chunked DATA -> ACK 0x82 per chunk (echoes the start address)
5. END -> ACK 0x83 -> write back to APP1 and reset into the new APP
```

## 5. Frame format

```
SOF 1B 0xA5 | CMD 1B | LEN 2B LE | PAYLOAD LEN | CRC16 2B LE (covers CMD+LEN+PAYLOAD)
```

CRC-CCITT, poly 0x1021, reflected, init value 0, masked with `& 0xFFFF` per byte (matches `os/lib/crc16.c`).

## 6. Commands

| CMD | payload | response |
|---|---|---|
| `0x01` ENTER | `app_start(4B LE=0x08007000)` + `unlock_key(8B)` | `0x81` + status / `0xC1` NAK |
| `0x02` DATA | `start(4B)` + `end(4B)` + `data(<=256B)` | `0x82` echoes start / `0xC2` NAK |
| `0x03` END | `total_len(4B)` + `total_crc16(2B)` | `0x83` + status / `0xC3` NAK |

- `unlock_key` = `51 55 4C 4C 4F 54 41 31` (ASCII `QULLOTA1`); new products should change it.
- Addresses are absolute APP-region addresses (base `0x08007000`); `end - start + 1 == len(data)`.
- `total_crc16` is the chained `crc16` over all data in transmission order (init value 0).

### NAK reason (first payload byte)

`0x01` ADDR · `0x02` KEY · `0x03` CRC (retryable) · `0x04` ERASE · `0x05` PROG · `0x06` LEN · `0x07` SZ

Timeout / CRC retry limit is 3; anything else aborts immediately.

## 7. Command line

```bash
python -m pip install pyserial
python tools/quilla_host_updater.py COM3 app.bin          # Windows
python tools/quilla_host_updater.py /dev/ttyUSB0 app.bin  # Linux
python tools/quilla_host_updater.py COM3 app.bin -b 115200 -q
python tools/quilla_host_updater.py --self-test           # offline self-test
```

- The firmware must be a raw `.bin`, no larger than the target MCU's APP-region capacity (`APP_END - APP_START`). At runtime the tool queries the device's `pinfo` command for that limit, so it adapts per MCU: PY32F040xB = 102400 B (`0x19000`), GD32F502VE = 495616 B (`0x79000`).
- Flash with Erase Sectors; never Erase Full Chip (it wipes the FLAG and manifest pages).
