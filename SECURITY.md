# Security Policy / 安全策略

================================================================================
English
================================================================================

## Supported versions

`develop` and the `v1.0.0` tag. Older snapshots are not maintained.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting on this repository if it is
enabled (Security → Report a vulnerability). Otherwise open an issue prefixed
with `[SECURITY]`, or ask in Discussions — do **not** post exploit details in
a public issue before maintainers have had a chance to respond.

## The demo keypair is not a security boundary

`tools/_keytest/` contains a **public, deliberately shared** Ed25519 keypair.
It exists so the demo works out of the box. Consequences that are easy to miss:

- Anyone holding that private key can produce an image that a board provisioned
  with the matching public key will accept. Images signed with it provide
  **no authenticity guarantee**.
- A QBOOT built with `QUILLA_QBOOT_DEFAULT_KEY` emits a `#warning` at compile
  time reminding you to provision your own keypair.
- Before shipping: generate your own keypair, provision the public key into
  QBOOT, rebuild and re-verify the secure-boot chain on hardware.
  See `doc/keil-qboot-app-build.md` and `tools/quilla_ed25519_sign.py`.

## What the security mechanisms actually cover

- `QBOOT_CHECK` verifies **transport integrity only** (SHA-256 / CRC). It is
  not secure boot. `QBOOT_SEALED` and `QBOOT_SECURED` are the variants that
  enforce authenticity; `SECURED` runs Ed25519 verification on every boot.
- The anti-rollback floor is `max(META floor, OTP counter)`. A signed image
  whose security version is below the floor is refused at install (QIAP ENTER,
  `NAK 0x0A`) and at boot (`APP REFUSED - version rollback attempted`).
  OTP counters are irreversible: a board that has advanced its floor cannot be
  returned to a lower version by reflashing.
- The host-side SEC VER pre-check (CR-0053) only avoids a useless transfer.
  It is **not a security control**; the device gates are authoritative.
- Secure boot and anti-rollback are **orthogonal to functional safety**. No
  IEC 61508 / ISO 26262 certification or SIL level is claimed, and no
  third-party assessment has taken place.

## Known gaps

- CH32V305VB (RISC-V) is a bring-up port with no on-target results. The WPP
  write-protected NV counter and the WCH fast-interrupt path are unverified;
  CI rewrites the WCH-only attribute away, so a green build is not evidence
  that the image boots. See `doc/riscv-ch32v305-build.md` section 8.
- Boards without a provisioned public key (blank key region) do not enforce
  the signature gate. This is intended for development only.
- The full verified / not-verified matrix is in
  [`doc/verification.md`](doc/verification.md).

================================================================================
中文
================================================================================

## 支持版本

`develop` 与 `v1.0.0` tag。更早的快照不维护。

## 漏洞上报

若本仓已启用 GitHub 私有漏洞上报，优先使用（Security → Report a
vulnerability）。否则开一个以 `[SECURITY]` 开头的 issue，或先在 Discussions
询问——维护者回应之前，**不要**在公开 issue 里贴利用细节。

## 演示密钥对不构成安全边界

`tools/_keytest/` 下的 Ed25519 密钥对是**刻意公开、随仓库分发**的，目的是
让 demo 开箱可用。容易踩的坑：

- 任何持有该私钥的人都能造出「被烧了对应公钥的板子」会接受的镜像。用这套
  密钥签的镜像**不提供任何真实性保证**。
- 用 `QUILLA_QBOOT_DEFAULT_KEY` 构建的 QBOOT 会在编译期打出 `#warning`，
  提醒自行 provision 密钥。
- 出货前：生成自有密钥对、把公钥 provision 进 QBOOT、重新构建，并在真机上
  重新验证安全启动链。见 `doc/keil-qboot-app-build.md` 与
  `tools/quilla_ed25519_sign.py`。

## 安全机制实际覆盖什么

- `QBOOT_CHECK` **只校验传输完整性**（SHA-256 / CRC），不是安全启动。
  强制执行真实性的是 `QBOOT_SEALED` 与 `QBOOT_SECURED`；后者每次启动都跑
  Ed25519 验签。
- 防回滚下限为 `max(META floor, OTP counter)`。安全版本低于下限的签名镜像
  在安装闸（QIAP ENTER，`NAK 0x0A`）与启动闸
  （`APP REFUSED - version rollback attempted`）都会被拒。OTP 计数器不可逆：
  下限已推进的板子，重新烧录也回不到低版本。
- 上位机 SEC VER 预检（CR-0053）只是省掉一次无意义的传输，**不是安全控制**；
  以设备侧闸门为准。
- 安全启动与防回滚**与功能安全正交**。未声称任何 IEC 61508 / ISO 26262
  认证或 SIL 等级，也未进行第三方评估。

## 已知边界

- CH32V305VB（RISC-V）是 bring-up 移植，无上板结果。WPP 写保护 NV 计数器与
  WCH 快中断路径未验证；CI 会改写 WCH 专有属性，编译通过不代表镜像能启动。
  见 `doc/riscv-ch32v305-build.md` 第 8 节。
- 未烧公钥（公钥区空白）的开发板不执行验签闸，这仅供开发使用。
- 完整「已验证 / 未验证」矩阵见 [`doc/verification.md`](doc/verification.md)。
