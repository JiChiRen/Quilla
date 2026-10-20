# Quilla

**中文** · [English](README.md)

Quilla 是一套面向资源受限 MCU 的**轻量级、可安全启动、原生支持固件升级**的嵌入式操作系统，派生自 [Contiki-NG](https://github.com/contiki-ng/contiki-ng)。它在保留 Contiki 协程（protothread）进程模型简洁性的基础上，构建了双分区引导、安全启动与可观测性设施，目标是让产品级固件既能快速开发，又具备可控的安全属性。

面向主流 RTOS 很少开箱支持的低成本 **Arm Cortex-M** 微控制器。目前已移植两类定位档位——**超低功耗入门档**（PY32F040xB，Cortex-M0+）与**带安全扩展的主流档**（GD32F502VE，Cortex-M33），覆盖了 Cortex-M 家族从低端到带 TrustZone 的主流区间；更多 Arm Cortex-M 型号与 RISC-V 内核仍需进一步适配。

<p align="center">
  <img src="doc/images/quilla-demo.gif" alt="Quilla 演示：经串口烧录签名 APP——QBOOT 以 Ed25519 验签后引导新镜像" width="640">
</p>

## 系统特点

- **安全启动与防回滚**
  - 基于 RFC 8032 Ed25519 的镜像验签、SHA-256 完整性、版本地板。三种构建变体：
    - `QBOOT_CHECK`——仅 SHA-256 完整性（无钥匙）；
    - `QBOOT_SEALED`——seal 快路径（已验证镜像跳过完整验签，PY32F040xB 上 ~0.6 s）；
    - `QBOOT_SEC`——每次启动都完整 Ed25519 验签。
  - 公钥编译进 QBOOT，私钥离线留存于签名工位，绝不进入固件或仓库。

- **原生可升级（QIAP）**
  - 出厂即内置双分区引导：QBOOT（bootloader）+ APP。APP 可通过串口由上位机工具一键升级，无需专用烧录器。
  - 升级采用"标志 + manifest"握手，掉电可恢复；APP 侧仅需暴露 `swqboot` 命令并标记分区标识。

- **轻量且确定性的内核**
  - 基于协程（protothread）的进程模型，无抢占式多线程；仅依赖单个 rtimer 硬件中断驱动高精度定时，调度路径确定。
  - 内核零动态内存分配（禁堆），从机制上排除堆相关一类失效。

- **可观测与安全基础设施**
  - 栈水印监控（kern-stack）、统一状态码（kern_status_t）、低功耗（lpm，PM0/PM1）。
  - 程序流序列监控（kern-flow）：在 idle 循环布检查点，探测控制流故障，异常即转入安全态。
  - 配套独立看门狗（qwdt，带超时边界）与时钟交叉校验（qclk，检测 LSI 失锁）。

- **规范化的工程布局与多 MCU 支持**
  - 遵循 CMSIS-Pack 规范的目录布局；APP 代码通过统一公开 API 与 MCU 解耦，换芯无需改应用层。

- **自愿对标安全相关标准（未送第三方认证）**
  - 开发过程自愿参照 IEC 61508 / ISO 26262 等安全相关标准的关键实践——变更受控、工具链锁定、MISRA C 机检、独立审查、可复现构建。
  - **当前未提交任何第三方认证机构**：本仓库中的安全分析与等级结论仅代表内部评估，不构成认证声明。

## 支持硬件

| MCU | 内核 / 主频 | Flash / SRAM | 状态 |
| --- | --- | --- | --- |
| PY32F040xB | Cortex-M0+ @ 8 MHz | 128 KB / 16 KB | 已上板实测 |
| GD32F502VE | Cortex-M33 @ 160 MHz | 512 KB / 64 KB | 已上板实测（QBOOT_CHECK / QBOOT_SEC） |

## 快速开始

用 GNU `arm-none-eabi` 工具链构建 bootloader（每 MCU 的 makefile 在 `user/<mcu>/EIDE/`，Keil 工程在 `user/<mcu>/MDK-ARM/`），再经串口烧录 APP：

```bash
# 通过串口烧录 APP 镜像（目标须已烧 QBOOT）
python tools/quilla_host_upgrader.py <COM> app.bin -b 115200

# 生成自有密钥对并签名镜像（量产前须替换仓库内的示例公钥）
python tools/quilla_ed25519_sign.py keygen
python tools/quilla_ed25519_sign.py sign --key private.hex app.bin
```

完整构建步骤见 [`doc/keil-qboot-app-build.md`](doc/keil-qboot-app-build.md)，升级工具见 [`doc/quilla-host-upgrader-guide.md`](doc/quilla-host-upgrader-guide.md)。

## 文档

入口与读者导航：[`doc/Home.md`](doc/Home.md)（English）· [`doc/Home.zh-CN.md`](doc/Home.zh-CN.md)（中文）。

- **应用开发者**（整体功能 / 架构 / 协议 / 安全启动 / 流程 / 接口契约）：
  `doc/qboot-app-architecture.md`、`doc/secure-boot.md`、`doc/boot-flow.md`、`doc/upgrade-flow.md`、
  `doc/quilla-host-upgrader-guide.md`、`doc/app-api-reference.md`
- **移植者 / 应用开发者**（MCU 适配 / APP 调用 OS API）：
  `doc/port-layout.md`、`doc/cmsis-layout.md`、`doc/keil-qboot-app-build.md`

## 许可与归属

Quilla 派生自 Contiki-NG。除非显式声明，源代码在原 **3-clause BSD license** 下分发，并保留上游版权声明；派生源另含 `Copyright (c) 2026, JiChiRen Info. Tech.`。完整许可见 [`LICENSE.md`](LICENSE.md)。

原始上游 README 已备份为 [`CONTIKI-README.md`](CONTIKI-README.md)，供查阅 Contiki-NG 原始说明与归属信息。
