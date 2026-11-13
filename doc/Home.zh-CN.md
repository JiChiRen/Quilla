# Quilla 文档索引

**中文** · [English](Home.md)

Quilla 是在 Contiki-NG 基础上扩展的安全关键嵌入式运行时，采用 QBOOT / QAPP 双区引导架构。
本文是文档入口，按读者角色列出**本公开仓库内实际存在的文档**。

Quilla 由**机器人组件及光通信设计公司——深圳市积器仁信息有限公司（JiChiRen Info）**的开源办公室（OSPO）维护。它脱胎于积器仁量产项目中的信息安全固件系统——剥离业务内容、为开源重新适配 3 个 Demo MCU——作为对开源社区的回馈对外发布。该子系统的设计已在面向工业与消费电子产品的量产固件中实际使用；此处发布的代码是开源再适配版本，并非交付产品的原始固件。公司网站：[https://www.jichiren.com](https://www.jichiren.com)

上游 Contiki-NG 的通用说明（构建系统、网络、教程、其它平台）已隔离至 [`doc/upstream/`](upstream/README.md)——它们讲的是 Contiki-NG，不是 Quilla；本仓库自有文档（直接位于 `doc/` 下的部分）仅覆盖 Quilla 自身的架构、构建与交付流程。

---

## 1. 应用开发者（基于 Quilla 开发自己的 QAPP）

| 主题 | 文档 |
|---|---|
| 双区架构、分区与 QIAP 协议 | `qboot-app-architecture.zh-CN.md` |
| 构建 QAPP（Keil 工程） | `keil-qboot-app-build.zh-CN.md` |
| 串口升级上位机对接 | `quilla-host-updater-guide.zh-CN.md`（速查：`quilla-host-updater-brief.zh-CN.md`） |
| 升级流程与状态机 | `upgrade-flow.zh-CN.md` |
| 安全启动与密钥管理 | `secure-boot.zh-CN.md` |
| 防回滚：OTP 硬件单调计数器与不做 A/B 的裁定 | `anti-rollback-counter.zh-CN.md` |
| 启动流程 | `boot-flow.zh-CN.md` |
| APP 侧 OS API | `app-api-reference.zh-CN.md` |

工具交付说明见 `tools/README.zh-CN.md`：对外提供的是烧录工具与签名工具，其余工具仅用于 Quilla 自身开发。

### 安全上岗前必读

- 量产前**必须**把仓库默认（公开）钥匙更换为自有离线密钥对——默认钥匙的私钥随仓库公开，用它出货等同把私钥交出去；是否替换由客户决定，编译期会发 `#warning` 提醒。见 `secure-boot.zh-CN.md`。
- 烧录配置为 **Erase Sectors**，禁止 Erase Full Chip（会擦除 FLAG 与 manifest 页）。

---

## 2. 移植者（新增 MCU / 板级支持）

| 主题 | 文档 |
|---|---|
| CMSIS 与工程树布局 | `cmsis-layout.zh-CN.md` |
| 平台目录与移植约定 | `port-layout.zh-CN.md` |

---

## 3. 仓库约定摘要

- 工程树按 MCU 划分：`user/<mcu>/{Src,Inc,EIDE,MDK-ARM}`。
- 分区：QBOOT `0x08000000`–`0x08005FFF`；META `0x08006000`–`0x08006FFF`；APP `0x08007000` 起至 Flash 末（`APP_END` = Flash 末址，定义在 `arch/cpu/arm/<mcu>/soc.h` 的 `SOC_FLASH_END`）。分区布局由 `quilla-qboot-hal.h` 与每 MCU 的 `soc.h` 共同定义。
- 本项目禁止动态内存分配。
- 编码规范及其范围：`coding-guidelines.zh-CN.md`。
- 许可证见根目录 `LICENSE.md`。

---

## 4. 社区与维护

| 主题 | 文档 |
|---|---|
| 本地构建、CI 门禁、提交约定 | [`CONTRIBUTING.md`](../CONTRIBUTING.md) |
| 安全问题上报与演示密钥边界 | [`SECURITY.md`](../SECURITY.md) |
| 上板验证日志与状态矩阵 | [`verification.md`](verification.md) |
| 发布说明（v1.0.0） | [`release-notes.md`](release-notes.md) |
