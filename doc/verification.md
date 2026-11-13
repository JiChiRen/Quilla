# Verification evidence / 验证证据

This page collects the on-target observations behind the status table in
`README.md`. It is a record of what was actually observed on hardware, not a
compliance statement.

本页汇总 `README.md` 状态表背后的上板观测记录，是「实际看到过什么」的事实
记录，不构成任何合规或认证声明。

Test setup (2026-11-12 / 13): GD32F502VE board (also referred to as the ZP
board), QBOOT_SECURED in production configuration (`LEVEL=NONE`),
anti-rollback floor = 2, **repository demo keypair** (intentionally public,
development only). Host tools and firmware were the versions tagged `v1.0.0`.

测试条件（2026-11-12 / 13）：GD32F502VE 板（ZP 板），QBOOT_SECURED 产品态
（`LEVEL=NONE`），防回滚下限 = 2，使用**仓库内演示密钥对**（刻意公开，仅供开发）。
上位机与固件均为 `v1.0.0` 对应版本。

================================================================================
English
================================================================================

## Observed on hardware

| # | What was exercised | Observable result |
| --- | --- | --- |
| 1 | Same-version upgrade with the new host on an old QBOOT | `ACK_SIGN 0x84`, `[QAPP] Quilla.ZP V1.0.0001` — the extra 4 bytes of the 16-byte ENTER frame are ignored by old firmware |
| 2 | Same-version upgrade with the new QBOOT (happy path) | ENTER accepted with `image_secver=2`, full transfer, `ACK_SIGN 0x84`, APP boots |
| 3 | Device-side early rejection (probe forces `secver=1`, host pre-check disabled) | `NAK(0xC1) reason=0x0A ROLLBACK`, **no DATA**, QIAP flag not set; after reset the original APP boots |
| 4 | Legacy 12-byte ENTER against the new QBOOT | `NAK(0xC1) reason=0x0B` (unknown to the old host's reason table) — no write occurs |
| 5 | Anti-rollback floor on real OTP | Counter advanced 1→2 on GD32; a lower-version image is refused both at install and at boot |

Build evidence: Keil armclang V6.16 QBOOT_SECURED (production `LEVEL=NONE`)
0 errors / 3 warnings, all pre-existing `#warning` notices (OTP level and the
"provision your own keypair" reminder). Public CI on `develop` runs the Arm
firmware builds and the host-tools self-test as gating jobs; the CH32V305
RISC-V job is experimental and non-blocking.

## Serial log excerpts

Same-version upgrade (host pre-check + ENTER + signed install):

```
[*] Image SEC VER=2 (0x2)
[*] Device FW_SECVER=0x00000002
[*] SEC VER pre-check OK: image=2 >= device=2, continuing
[*] Sending ENTER (app_start=0x08007000, key=QULLOTA1, image_secver=2)...
[+] ENTER succeeded (QIAP flag set: recoverable if power is lost mid-upgrade)
[*] Sending SIGN (64B signature); device-side verify takes ~20s, waiting up to 90s...
[+] UPGRADE COMPLETE: signature verified, manifest written, APP1 set. ... ACK 0x84.
[+] Confirmed: new APP is running, boot version stamp: [QAPP] Quilla.ZP V1.0.0001
```

Downgrade rejected at ENTER, before any flash write:

```
[*] Sending ENTER (app_start=0x08007000, key=QULLOTA1, image_secver=1)...
[!] ENTER rejected: NAK(0xC1) reason=0x0A ROLLBACK  firmware security version is below the device floor (downgrade rejected)
[*] No DATA was written and the QIAP flag was not set: reset the board ... and the original APP boots normally.
```

Legacy host against the new QBOOT:

```
[*] Sending ENTER (app_start=0x08007000, key=QULLOTA1)...
[!] Direct ENTER failed: NAK(0xC1) reason=0x0B unknown(0x0B)
```

`COMx` is a placeholder; the port number is host-specific. Log lines are
condensed (progress output and timings omitted) but not altered in substance.

## Not verified

- **CH32V305VB (RISC-V)**: builds end-to-end, but no on-target results. The
  WPP write-protected NV counter and the WCH fast-interrupt entry path are
  unconfirmed; the stock `riscv-none-elf` toolchain used in CI rewrites the
  WCH-only attribute away, so a green CI build is not evidence that the image
  boots. See `doc/riscv-ch32v305-build.md` section 8.
- **GUI-side SEC VER branches** and **boards without a provisioned public
  key**: covered by offline self-tests, not separately on hardware.
- **No certification claim**: the development process voluntarily adopts key
  practices from IEC 61508 / ISO 26262 (change control, MISRA-C checks,
  independent review, reproducible builds). No SIL level is claimed and no
  third-party assessment has taken place.

================================================================================
中文
================================================================================

## 上板观测

| # | 验证内容 | 可观测结果 |
| --- | --- | --- |
| 1 | 新上位机 + 旧 QBOOT，同版本升级 | `ACK_SIGN 0x84`、`[QAPP] Quilla.ZP V1.0.0001`——16 字节 ENTER 多出的 4 字节被旧固件忽略 |
| 2 | 新 QBOOT 同版本升级（happy path） | ENTER 带 `image_secver=2` 放行、完整传输、`ACK_SIGN 0x84`、APP 正常启动 |
| 3 | 设备侧提前拒降级（探针强制 `secver=1`，禁用宿主预检） | `NAK(0xC1) reason=0x0A ROLLBACK`，**无 DATA**、QIAP flag 未置；复位后原 APP 正常启动 |
| 4 | 旧 12 字节 ENTER 打新 QBOOT | `NAK(0xC1) reason=0x0B`（旧上位机 REASON 表显示为 unknown）——无任何写入 |
| 5 | 真 OTP 上的防回滚下限 | GD32 计数器 1→2；低版本镜像在安装闸与启动闸均被拒 |

编译证据：Keil armclang V6.16 的 QBOOT_SECURED（产品态 `LEVEL=NONE`）
0 Error / 3 Warning，全部为既有 `#warning` 提示（OTP 级别、产品公钥提醒）。
`develop` 上的公开 CI 以 Arm 固件构建与上位机自检为门禁 job；CH32V305 RISC-V
job 为实验性、非阻塞。

## 串口日志片段

同版本升级（宿主预检 + ENTER + 带签名安装）：

```
[*] Image SEC VER=2 (0x2)
[*] Device FW_SECVER=0x00000002
[*] SEC VER pre-check OK: image=2 >= device=2, continuing
[*] Sending ENTER (app_start=0x08007000, key=QULLOTA1, image_secver=2)...
[+] ENTER succeeded (QIAP flag set: recoverable if power is lost mid-upgrade)
[*] Sending SIGN (64B signature); device-side verify takes ~20s, waiting up to 90s...
[+] UPGRADE COMPLETE: signature verified, manifest written, APP1 set. ... ACK 0x84.
[+] Confirmed: new APP is running, boot version stamp: [QAPP] Quilla.ZP V1.0.0001
```

ENTER 阶段拒降级、写入任何 Flash 之前：

```
[*] Sending ENTER (app_start=0x08007000, key=QULLOTA1, image_secver=1)...
[!] ENTER rejected: NAK(0xC1) reason=0x0A ROLLBACK  firmware security version is below the device floor (downgrade rejected)
[*] No DATA was written and the QIAP flag was not set: reset the board ... and the original APP boots normally.
```

旧上位机打新 QBOOT：

```
[*] Sending ENTER (app_start=0x08007000, key=QULLOTA1)...
[!] Direct ENTER failed: NAK(0xC1) reason=0x0B unknown(0x0B)
```

`COMx` 为占位符，串口号随主机而异。日志经过压缩（省略进度输出与耗时），
但实质内容未改动。

## 未验证

- **CH32V305VB（RISC-V）**：全程可编译，但无上板结果。WPP 写保护 NV 计数器
  与 WCH 快中断入口均未确认；CI 用的 stock `riscv-none-elf` 工具链会把
  WCH 专有属性改写掉，因此 CI 编译通过不代表镜像能启动。见
  `doc/riscv-ch32v305-build.md` 第 8 节。
- **GUI 侧 SEC VER 分支**、**未烧公钥的开发板**：离线自测覆盖，未单独上板。
- **不声称认证**：开发过程自愿参照 IEC 61508 / ISO 26262 的关键实践
  （变更受控、MISRA C 机检、独立审查、可复现构建）。未声称任何 SIL 等级，
  也未进行第三方评估。
