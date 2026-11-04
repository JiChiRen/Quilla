# Quilla Documentation

**English** · [中文](Home.zh-CN.md)

Quilla is a safety-oriented embedded runtime built on top of Contiki-NG, with a **QBOOT / QAPP dual-partition boot architecture**. This page is the documentation entry point, organized by reader role, and lists only the documents that live in this public repository.

Quilla is maintained by the Open Source Program Office (OSPO) of **Shenzhen JiChiRen Information Technology Co., Ltd. (JiChiRen Info. Tech.)**, a robotics-components and optical-communication design company. It originated from the secure-firmware subsystem used in JiChiRen's production projects — business content stripped, three demo MCUs re-adapted for open source — and is released as a contribution back to the community. The same codebase already powers industrial projects (e.g. remote information-security modules) and consumer electronics (e.g. cryptographic smart locks). Company site: [https://www.jichiren.com](https://www.jichiren.com)

General Contiki-NG material (build system, networking, tutorials, other platforms) remains the upstream project's and is isolated under [`doc/upstream/`](upstream/README.md) — it describes Contiki-NG, not Quilla. This repository's own docs (everything directly under `doc/`) cover Quilla's architecture, build, and delivery flow.

---

## 1. Application developers (build your own QAPP on Quilla)

| Topic | Document |
|---|---|
| Dual-partition architecture, partitions & QIAP protocol | `qboot-app-architecture.md` |
| Building a QAPP (Keil project) | `keil-qboot-app-build.md` |
| Host updater integration | `quilla-host-updater-guide.md` (quick ref: `quilla-host-updater-brief.md`) |
| Upgrade flow & state machine | `upgrade-flow.md` |
| Secure boot & key management | `secure-boot.md` |
| Boot flow | `boot-flow.md` |
| APP-facing OS API | `app-api-reference.md` |

Tooling deliverables are described in `tools/README.md`: the flashing tool and the signing tool are provided for external use; the remaining tools are for Quilla's own development.

### Read before shipping

- Replace the in-repo **default (public) key** with your own offline keypair before production. The default key's private half is public, so shipping with it is equivalent to handing out the private key. The choice is yours; the build emits a `#warning` reminder. See `secure-boot.md`.
- Flashing configuration must be **Erase Sectors** — never Erase Full Chip (it would wipe the FLAG and manifest pages).

---

## 2. Porters (add a new MCU / board)

| Topic | Document |
|---|---|
| CMSIS & project-tree layout | `cmsis-layout.md` |
| Platform directory & porting conventions | `port-layout.md` |

---

## 3. Repository conventions (summary)

- Project trees are split per MCU: `user/<mcu>/{Src,Inc,EIDE,MDK-ARM}`.
- Partitions: QBOOT `0x08000000`–`0x08005FFF`; META `0x08006000`–`0x08006FFF`; APP from `0x08007000` to the end of flash (`APP_END` = last flash address, defined as `SOC_FLASH_END` in `arch/cpu/arm/<mcu>/soc.h`). The layout is defined jointly by `quilla-qboot-hal.h` and each MCU's `soc.h`.
- Dynamic memory allocation is forbidden in this project.
- Coding guidelines and their scope: `coding-guidelines.md`.
- License: see `LICENSE.md` at the repository root.
