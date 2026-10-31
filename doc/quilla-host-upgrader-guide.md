**[English](quilla-host-upgrader-guide.md) · [中文](quilla-host-upgrader-guide.zh-CN.md)**

# Quilla QBOOT Serial Upgrade: Host Integration Guide

Audience: engineers developing a serial-upgrade host for Quilla devices.

- Authoritative protocol definition: `doc/qboot-app-architecture.md` §8
- Reference implementation described here: `tools/quilla_host_upgrader.py` (Python 3.8+, depends only on `pyserial`)

---

## 1. Prerequisites

While the device is parked at the **QBOOT interactive shell**, the serial port is in line-parsing mode and binary frames are discarded as commands.
The host must first send the text command `qiap` in the shell to switch to the raw byte stream before it can send upgrade frames.
The typical symptom of sending frames without first sending `qiap` is "send ENTER, no ACK".

---

## 2. Communication Parameters

| Item | Value |
|---|---|
| Serial port | PY32F040xB: USART2 / PA2-TX / PA3-RX; GD32F502VE: USART0 / PB6-TX / PB7-RX |
| Baud rate | 115200 |
| Frame format | 8 / N / 1 (8-N-1) |
| Shell line ending | `\r` or `\n`; send `qiap\n` for the `qiap` command |

---

## 3. State Determination

`ensure_upgrade_mode()` reads the power-on banner and decides whether `qiap` must be sent first:

| banner feature | Device state | Action |
|---|---|---|
| Contains `enter upgrade mode` | Already in upgrade mode (leftover `flag==QIAP`) | Skip `qiap`, send ENTER directly |
| Contains `[BOOT]` | QBOOT shell | Send `qiap`, wait for `enter upgrade mode` |
| Neither | QBOOT not entered | Report an error and exit |

Supplementary confirmation: send the `part` command to the shell; if the echo contains `qboot`, the device is confirmed to be parked at the QBOOT partition.
After the raw sink takes over, this command is swallowed by frame parsing with no echo, so it is only useful in the shell state.

The QBOOT shell also exposes `pinfo`, which prints the partition layout (`[PART] APP_START=0x… APP_END=0x… APP_SIZE=0x…`). The host uses it to learn the device's real `APP_END` (and thus the flashable cap) instead of assuming a fixed MCU; see §9. In the APP shell `pinfo` is not registered, and after the raw sink takes over it is swallowed like `part`.

**If the device is still in the APP shell** (`part` echo contains `app1`): send `swqboot` to make the device set `flag=QBOOT` and reset,
then handle it according to the table above once the `[BOOT]` banner appears.

---

## 4. Handshake Sequence

```
1. Enter QBOOT
   a. Send the text swqboot inside the APP  -> set flag=QBOOT and reset -> print the [BOOT] banner
   b. Cold boot with leftover flag==QIAP   -> print [QIAP] enter upgrade mode. Send ENTER frame. directly

2. If in the shell (case a): send qiap
   -> echo qiap: switching to upgrade mode
   -> print [QIAP] enter upgrade mode. Send ENTER frame.
   Only after this step does the serial port switch to the raw byte stream.

3. Send the ENTER frame -> receive ACK 0x81
4. Send DATA frames in chunks -> receive ACK 0x82 for each chunk
5. Send the END frame -> receive ACK 0x83 -> the device writes back APP1 and resets, entering the new APP
```

---

## 5. Frame Format

```
SOF      1B    fixed 0xA5
CMD      1B    command / response code
LEN      2B    PAYLOAD length, uint16 little-endian
PAYLOAD  LEN   parameters
CRC16    2B    uint16 little-endian, covers [CMD][LEN][PAYLOAD]
```

The CRC matches the firmware's `crc16_data` in `os/lib/crc16.c` byte for byte:
CRC-CCITT, polynomial 0x1021, reflected input/output, initial value 0, truncated to `unsigned short` at each step (`& 0xFFFF`).

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

## 6. Commands and Responses

| Direction | CMD | payload | Response |
|---|---|---|---|
| H→M | `0x01` ENTER | `app_start(4B LE)` + `unlock_key(8B)` | `0x81` + status(1) / `0xC1` NAK |
| H→M | `0x02` DATA | `start(4B)` + `end(4B)` + `data(≤256B)` | `0x82` + echoed start(4) / `0xC2` NAK |
| H→M | `0x03` END | `total_len(4B)` + `total_crc16(2B)` | `0x83` + status(1) / `0xC3` NAK |

Conventions:

- `unlock_key` defaults to `0x51 0x55 0x4C 0x4C 0x4F 0x54 0x41 0x31`, i.e. ASCII `QULLOTA1`,
  defined by `QUILLA_QBOOT_UNLOCK_KEY` in `quilla-qboot-hal.h`; new products should change this value.
- Addresses are **absolute APP-region addresses**, base `0x08007000`; `start`/`end` must fall within `[APP_START, APP_END]` (`[0x08007000, 0x0801FFFF]` on PY32F040xB; the end follows the target MCU's `SOC_FLASH_END`) and satisfy `end - start + 1 == len(data)`, otherwise NAK(ADDR).
- The `data` of a single DATA frame must not exceed 256 B; `start` need not be page-aligned — the device handles it with read-modify-erase-write.
- After ENTER validation passes, the `QIAP` flag is set: if power is lost mid-upgrade, the next power-on automatically re-enters the upgrade state, allowing a resume.
- END's `total_len` is the sum of all data bytes; `total_crc16` is the chained accumulation over all data **in send order** (`total_crc = crc16(chunk, total_crc)`, initial value 0). The device accumulates and compares step by step, and a mismatch gives NAK(LEN).

### NAK reason codes (payload byte 1)

| reason | Meaning | Host action |
|---|---|---|
| `0x01` | ADDR address out of range | Abort |
| `0x02` | KEY unlock key mismatch | Abort |
| `0x03` | CRC frame/field error | Retransmit this frame, up to 3 times |
| `0x04` | ERASE Flash erase failure | Abort |
| `0x05` | PROG Flash program/read-back failure | Abort |
| `0x06` | LEN total length or total CRC mismatch | Abort |
| `0x07` | SZ length field does not match data length | Abort |

---

## 7. Retransmission and Error Handling

- After each frame is sent, wait for the ACK; the default timeout is 2.0 s, with up to 3 retransmissions.
- `0x03` retransmits this frame; ADDR / KEY / ERASE / PROG / LEN / SZ are configuration or Flash errors — abort immediately.
- The serial port carries a byte stream mixing text banners with binary ACKs: the host parses only frames starting with `0xA5`,
  and discards the rest as log output.

---

## 8. Host Layering

| Layer | Responsibility |
|---|---|
| Transport layer | Serial TX/RX, frame build/parse, CRC check, timeout control |
| Session layer | `enter()` / `send_data()` / `end()` state machine, accumulating `total_len` and `total_crc`, handling retransmission |
| UI layer | Progress, logging, serial port and firmware file selection |

The `Upgrader` class in the reference implementation is the session layer and can be used independently of the UI.

---

## 9. Reference Implementation Constants and Interfaces

Constants are taken from `quilla-qboot-hal.h` and `quilla-qiap.h`:

| Name | Value | Source |
|---|---|---|
| `APP_START` | `0x08007000` (default) | `quilla-qboot-hal.h`; identical across current MCUs |
| `APP_END` | **fetched from the device at runtime** | QBOOT shell `pinfo` → `SOC_FLASH_END` in the target `soc.h`. PY32F040xB = `0x0801FFFF`; GD32F502VE = `0x0807FFFF` |
| `APP_SIZE` | **fetched from the device at runtime** | APP region capacity upper limit = `APP_END` − `APP_START`; MCU-specific, never hardcoded in the tool |
| `UNLOCK_KEY` | `51 55 4C 4C 4F 54 41 31` | `quilla-qboot-hal.h` |
| `PAGE_SIZE` / `CHUNK` | `256` | `QUILLA_QBOOT_PAGE_SIZE` |
| `MAX_PAYLOAD` | `264` | `QUILLA_QIAP_MAX_PAYLOAD` |
| `ACK_TIMEOUT` | `2.0` s | Implementation convention |
| `MAX_RETRIES` | `3` | Implementation convention |

> **MCU-adaptive APP boundary (no longer hardcoded).** Earlier versions pinned `APP_END = 0x0801FFFF` (PY32F040xB's 128 KB), which wrongly rejected valid GD32F502VE images larger than 102400 B. The reference tool now queries the QBOOT shell command `pinfo` and derives `APP_START` / `APP_END` / `APP_SIZE` from the running firmware — so the host-side pre-flight cap always matches the device's real `SOC_FLASH_END`. If `pinfo` is unavailable (old firmware) or the device is already in the upgrade state (raw sink), the tool skips the host-side cap and lets the device's ADDR NAK (`0x01`) enforce the real boundary.

| Method | Description |
|---|---|
| `Upgrader(port, baud=115200, verbose=True)` | Open the serial port and flush the buffer |
| `ensure_upgrade_mode()` | Decide whether to send `qiap` based on the §3 banner determination; also queries `pinfo` to learn the device's real partition layout |
| `enter()` | Send ENTER (app_start + key), set the `QIAP` flag; `app_start` comes from the device `pinfo` |
| `send_data(data, base=None)` | Send DATA in chunks, returning `(total_len, total_crc)`; `base` defaults to the device `APP_START` |
| `end(total_len, total_crc)` | Send END, write back APP1 and reset |
| `run(bin_path)` | Sequence: read bin → `ensure_upgrade_mode` → (host cap check) → `enter` → `send_data` → `end` |
| `self_test()` | Offline self-test, see §10 |

---

## 10. Command Line

### 10.1 Installing Dependencies

```bash
python -m pip install pyserial
```

### 10.2 Running an Upgrade

```bash
# Windows
python tools/quilla_host_upgrader.py COM3 app.bin

# Linux
python tools/quilla_host_upgrader.py /dev/ttyUSB0 app.bin

# Specify the baud rate (default 115200)
python tools/quilla_host_upgrader.py COM3 app.bin -b 115200

# Reduce output
python tools/quilla_host_upgrader.py COM3 app.bin -q
```

### 10.3 Selecting the Key by QBOOT Variant

The upgrade flow is identical for the three QBOOT variants; the only differences are whether `--key` is provided and which private key is used (see `doc/secure-boot.md` §2):

| Target variant | `--key` | Notes |
|----------|---------|------|
| **QBOOT_CHECK** | Do not pass | Keyless; the END frame writes the integrity manifest and commits directly. **Never pass `--key`** — after the device receives END it has already committed and reset, so a subsequent SIGN frame will time out and falsely report failure. |
| **QBOOT_SEALED** | `tools/_keytest/private.hex` (default private key) | For R&D / integration; the default public key is already compiled into QBOOT. |
| **QBOOT_SEC** | Same one as SEALED — the default private key `tools/_keytest/private.hex`, or your own private key after replacement | Must match the public key compiled into QBOOT (default public key ⇔ default private key; own public key ⇔ own private key), otherwise SIGN verification fails and the upgrade is rejected. |

Examples:

```bash
# QBOOT_CHECK (integrity only, no key required)
python tools/quilla_host_upgrader.py COM3 app.bin

# QBOOT_SEALED (R&D state, default (public) key)
python tools/quilla_host_upgrader.py COM3 app.bin --key tools/_keytest/private.hex

# QBOOT_SEC (mass-production state, SECURE mode verifies the signature on every power cycle; the key is the same one as SEALED's)
python tools/quilla_host_upgrader.py COM3 app.bin --key keys/product_priv.hex
```

> The host does **not** supply a version number (and never did): the anti-rollback version is **baked into the signed APP image** as `QUILLA_APP_SECURITY_VERSION` (at the fixed Flash address `0x08007400`, covered by the signature), so neither the host nor QBOOT invents it. The anti-rollback floor is still enforced at boot and at the SIGN stage; a lower-version image is refused (see `doc/secure-boot.md` §1.1). Do not confuse "no `--version` argument" with "no anti-rollback".

On Windows there is also the batch file `tools/quilla_app_uart_flash.bat`; see the comment header of that file for usage.

### 10.4 Offline Self-Test

```bash
python tools/quilla_host_upgrader.py --self-test
```

The self-test covers the frame codec round trip (ENTER / DATA / END commands) and chained CRC accumulation consistency
(asserting `crc16(d0 + d1) == crc16(d1, crc16(d0))`), with no serial port dependency.
The self-test verifies only the codec and CRC; it does not replace hardware integration testing.

### 10.5 Parameters

| Parameter | Description |
|---|---|
| `port` | Serial port, e.g. `COM3`, `/dev/ttyUSB0`; may be omitted with `--self-test` |
| `binfile` | APP firmware `.bin` (raw binary); may be omitted with `--self-test` |
| `-b`, `--baud` | Baud rate, default 115200 |
| `-q`, `--quiet` | Reduce output |
| `--key PATH` | Signing private key file (`private.hex`). SIGN frames are sent only when this is specified |
| `--probe` | Probe the device state only, without performing an upgrade |
| `--banner-timeout SEC` | How long to wait for the banner (seconds) |
| `--self-test` | Offline self-test; no serial port or firmware file required |

---

## 11. Pre-Flight Checks

- The firmware is a raw `.bin`, not `.hex` / `.elf`; its size must not exceed the **APP region capacity upper limit** (`APP_SIZE` = `APP_END` − `APP_START`). The reference tool no longer hardcodes this: it queries the device's `pinfo` shell command and derives the cap from the target MCU's `SOC_FLASH_END`, so GD32F502VE (512 KB) is accepted just as readily as PY32F040xB (128 KB) — no manual editing of the tool is required. If `pinfo` is unavailable or the device is already in the upgrade state, the host skips the pre-flight cap and leaves the boundary enforcement to the device's ADDR NAK (`0x01`).
- QBOOT and APP are already flashed; the flashing configuration must be **Erase Sectors** — Erase Full Chip is forbidden, as it would erase the FLAG and manifest pages.

---

## 12. Corresponding Firmware Files

| Host concept | Firmware file |
|---|---|
| Frame format, CRC, command codes | `os/services/quilla-qboot/quilla-qiap.{h,c}` |
| Partition layout, unlock key | `os/services/quilla-qboot/quilla-qboot-hal.h` |
| CRC algorithm | `os/lib/crc16.c` |
| Startup branching (shell / upgrade / jump to APP) | `arch/cpu/arm/<mcu>/qboot-main.c` |
| QBOOT flag state | `os/services/quilla-qboot/quilla-qboot-flag.{h,c}` |
| `swqboot` command | `os/services/quilla-qboot/quilla-app-qboot.c` |
