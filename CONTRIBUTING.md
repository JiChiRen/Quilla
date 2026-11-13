# Contributing to Quilla / 贡献指南

================================================================================
English
================================================================================

Bug reports, MCU ports, documentation fixes and test results are all welcome.
This is a firmware project: a change that touches boot, signing, flash writes
or the anti-rollback floor needs evidence from real hardware, not just a
clean build.

## Before opening a PR

- Run the offline host self-test:
  `python tools/quilla_host_updater.py --self-test`
- Build at least one QBOOT variant with the GNU Arm toolchain
  (`make -C user/<mcu>/EIDE -f Makefile.qboot`).
- Public CI runs on `develop`: Arm firmware builds (PY32F040xB, GD32F502VE)
  and the host-tools self-test are gating jobs. The CH32V305 RISC-V job is
  experimental and non-blocking; a failure there does not turn the badge red,
  but it stays visible in the job log.

## Commit messages

Use `<area>: <summary>` with one of these areas:
`arm`, `riscv`, `os`, `shell`, `doc`, `tools`, `eide`, `keil`, `scm`, `safety`.

Internal maintainers additionally attach a `CR: CR-NNNN` line and keep a
record under `safety/change/`. **External contributors do not need to do
this** — `safety/` and the local hook scripts are not part of the published
repository. A plain, descriptive message is enough; maintainers add the
traceability anchor when merging.

## Evidence for behaviour changes

For changes that affect secure boot, the QIAP protocol, flash programming,
partition boundaries or version gates, include the serial log (or an explicit
statement that no hardware run was performed). PRs that change behaviour
without on-target evidence are not merged into `develop`. See
[`doc/verification.md`](doc/verification.md) for the format and the current
verified / not-verified matrix.

Security-relevant changes go through design review before implementation.
Do not add `Reviewed-by:` on behalf of anyone: only the reviewer signs.

## What not to submit

- Machine-local absolute paths, workspace names, internal tool names,
  personal port numbers (use `COMx` in examples), or AI/tooling traces.
- Private keys, production keys or generated build products
  (`*.bin`, `*.hex`, `*.elf`, build directories). The keypair under
  `tools/_keytest/` is intentionally public and for development only; see
  [`SECURITY.md`](SECURITY.md).
- Claims of certification, SIL levels, or "verified on hardware" without an
  accompanying log. The project voluntarily adopts some safety-related
  engineering practices and claims no certification.

## Where to ask

Use GitHub Issues for bugs and GitHub Discussions for design questions.
Security issues: see [`SECURITY.md`](SECURITY.md).

================================================================================
中文
================================================================================

欢迎提交缺陷报告、MCU 移植、文档修订与测试结果。这是固件项目：涉及启动、验签、
Flash 写入或防回滚下限的改动，需要真机证据，不能只靠编译通过。

## 提 PR 之前

- 跑上位机离线自测：`python tools/quilla_host_updater.py --self-test`
- 至少用 GNU Arm 工具链构建一个 QBOOT 变体
  （`make -C user/<mcu>/EIDE -f Makefile.qboot`）。
- `develop` 上的公开 CI：Arm 固件构建（PY32F040xB、GD32F502VE）与上位机自检
  是门禁 job。CH32V305 RISC-V job 为实验性、非阻塞，失败不会让徽章变红，
  但仍如实留在 job 日志中。

## 提交信息

使用 `<area>: <summary>`，area 取以下之一：
`arm`、`riscv`、`os`、`shell`、`doc`、`tools`、`eide`、`keil`、`scm`、`safety`。

内部维护者另附 `CR: CR-NNNN` 行，并在 `safety/change/` 留记录。
**外部贡献者无需照做**——`safety/` 与本地钩子脚本不在公开仓库里。写清楚改了
什么即可，合并时由维护者补追溯锚点。

## 行为变更的证据

涉及安全启动、QIAP 协议、Flash 编程、分区边界或版本闸的改动，请附串口日志
（或明确说明「未上板」）。没有真机证据的行为变更不合并进 `develop`。格式与
当前「已验证 / 未验证」矩阵见 [`doc/verification.md`](doc/verification.md)。

安全相关改动先做设计评审再实施。不要替任何人代签 `Reviewed-by:`——只有实际
审查者才署名。

## 不要提交的内容

- 本机绝对路径、工作区名称、内部工具名、个人串口号（示例统一用 `COMx`），
  以及 AI / 工具痕迹。
- 私钥、产品密钥、构建产物（`*.bin`、`*.hex`、`*.elf`、构建目录）。
  `tools/_keytest/` 下的密钥对刻意公开、仅供开发，见
  [`SECURITY.md`](SECURITY.md)。
- 认证、SIL 等级或「已上板验证」这类无日志支撑的断言。本项目自愿参照
  部分安全相关工程实践，未声称任何认证。

## 在哪里提问

缺陷用 GitHub Issues，设计问题用 GitHub Discussions。
安全问题见 [`SECURITY.md`](SECURITY.md)。
