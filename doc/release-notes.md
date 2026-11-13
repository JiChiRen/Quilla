# Release notes / 发布说明

Source text for GitHub Releases. Each section is ready to paste into the
Release editor for the corresponding repository and tag.

各小节可直接粘贴到对应仓库、对应 tag 的 GitHub Release 编辑框。
`QuillaUpdater` 已有 `v1.0.0` / `v1.0.1` Release，无需新建。

================================================================================
Quilla v1.0.0 — English
================================================================================

First public release of the Quilla secure-boot / QIAP reference platform.

**What is in it**
- Dual-partition QBOOT with RFC 8032 Ed25519 image verification, a version
  floor (META + OTP / write-protected NV counters) and QIAP upgrade over UART.
- CR-0053: host SEC VER pre-check. Same-version upgrades pass; a downgrade is
  skipped before any transfer and the board returns to the APP.
- CR-0055: QIAP ENTER carries the image SEC VER. Keyed devices reject a
  downgrade **before writing a single flash byte**; the SIGN gate stays
  authoritative. The QIAP flag is not set on rejection, so the original APP
  still boots after a reset.
- CR-0056: the CH32V305 CI job is marked experimental and non-blocking.
- GD32 HAL: OTP range check uses physical SOC macros (LEVEL=NONE build fix).
- Ports: PY32F040xB (Cortex-M0+), GD32F502VE (Cortex-M33), CH32V305VB
  (RISC-V, bring-up only).

**Verification**
- Board-verified on PY32F040xB and GD32F502VE: signed QIAP upgrade
  (`ACK_SIGN 0x84`), OTP counter 1→2, downgrade rejected with `NAK 0x0A`,
  ENTER-stage rejection with no DATA, and new-host / old-QBOOT compatibility.
  Serial log excerpts and the full matrix: `doc/verification.md`.
- CI: Arm firmware builds + host-tools self-test are gating jobs. The RISC-V
  job is non-blocking and may fail.

**Boundaries — read before shipping**
- CH32V305VB is bring-up only: the WPP write-protected NV counter and the WCH
  fast-interrupt path are not on-target verified.
- The demo Ed25519 keypair in `tools/_keytest/` is intentionally public.
  Generate and provision your own keypair before shipping a product.
- `QBOOT_CHECK` verifies transport integrity only; it is not secure boot.
- No IEC 61508 / ISO 26262 certification or SIL level is claimed.

Baseline derived from Contiki-NG (3-clause BSD); upstream notices are kept and
summarised in `NOTICE`. Reviewed-by: Ali.

================================================================================
Quilla v1.0.0 — 中文
================================================================================

Quilla 安全启动 / QIAP 参考平台的首个公开版本。

**本版内容**
- 双分区 QBOOT：RFC 8032 Ed25519 镜像验签、版本下限（META + OTP / 写保护 NV
  计数器）、经串口的 QIAP 升级。
- CR-0053：上位机 SEC VER 预检。同版本放行；降级在传输前跳过并 `swapp` 回 APP。
- CR-0055：QIAP ENTER 帧携带镜像 SEC VER。已烧公钥的设备在**写入任何 Flash
  字节之前**拒绝降级；SIGN 闸仍为权威。拒绝时不置 QIAP flag，复位后原 APP
  照常启动。
- CR-0056：CH32V305 CI job 标记为实验性、非阻塞。
- GD32 HAL：OTP 范围检查改用物理 SOC 宏（LEVEL=NONE 构建修复）。
- 移植：PY32F040xB（Cortex-M0+）、GD32F502VE（Cortex-M33）、CH32V305VB
  （RISC-V，仅 bring-up）。

**验证**
- PY32F040xB 与 GD32F502VE 上板验证：带签名 QIAP 升级（`ACK_SIGN 0x84`）、
  OTP 计数器 1→2、降级被 `NAK 0x0A` 拒绝、ENTER 阶段拒降级且无 DATA、
  新上位机 / 旧 QBOOT 兼容。串口日志片段与完整矩阵见
  `doc/verification.md`。
- CI：Arm 固件构建 + 上位机自检为门禁 job；RISC-V job 非阻塞，可能失败。

**边界——出货前必读**
- CH32V305VB 仅 bring-up：WPP 写保护 NV 计数器与 WCH 快中断入口未上板验证。
- `tools/_keytest/` 下的演示 Ed25519 密钥对刻意公开；产品出货前须生成并
  provision 自有密钥对。
- `QBOOT_CHECK` 只校验传输完整性，不构成安全启动。
- 未声称任何 IEC 61508 / ISO 26262 认证或 SIL 等级。

代码基线派生自 Contiki-NG（3-clause BSD）；上游声明保留并汇总于 `NOTICE`。
Reviewed-by: Ali.

================================================================================
Quilla.FR v1.0.0 / Quilla.ZP v1.0.0
================================================================================

EN — Paste into each repository's Release editor:

> First public release of the Quilla.FR / Quilla.ZP reference design:
> Quilla QBOOT booting a **FreeRTOS appliance on PY32F040xB (Cortex-M0+)** /
> a **Zephyr RTOS appliance on GD32F502VE (Cortex-M33)**. Boot and APP share
> no code — only a binary flash contract (partition layout, META record, SEC
> VER at image offset 0x400, shared 115200 console). QIAP upgrade verified on
> hardware. `quilla/` and (for ZP) `appliance/`, `modules/` are independent,
> untracked upstream clones and are not redistributed by this repository.
> Reviewed-by: Ali.

中文 — 粘贴到对应仓库的 Release 编辑框：

> Quilla.FR / Quilla.ZP 参考设计首个公开版本：Quilla QBOOT 引导
> **PY32F040xB（Cortex-M0+）上的 FreeRTOS 应用** /
> **GD32F502VE（Cortex-M33）上的 Zephyr RTOS 应用**。引导与 APP 零代码耦合，
> 仅靠二进制 Flash 契约对接（分区布局、META 记录、镜像偏移 0x400 的 SEC VER、
> 共用的 115200 串口）。QIAP 升级已上板验证。`quilla/`（ZP 另含
> `appliance/`、`modules/`）为独立、不跟踪的上游 clone，本仓库不重新分发。
> Reviewed-by: Ali.
