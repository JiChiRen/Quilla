# 硬件单调计数器与防回滚设计裁定

**[English](anti-rollback-counter.md)**

本文回答两个经常被问到的问题：QBOOT 的防回滚为什么需要（或不需要）OTP 硬件
单调计数器；以及 QBOOT 为什么**不做**通用 A/B 双分区切换（swap）。
两个裁定都基于同一台设备的真实威胁模型，请结合 `doc/secure-boot.md` 阅读。

## 1. 现状：防回滚的下限存在哪里

QBOOT 的防回滚下限（floor）持久化在 **META 磨损均衡日志**（`quilla-qboot-meta.c`，
普通可擦写 Flash）里。固件安全版本由镜像侧编译期宏 `QUILLA_APP_SECURITY_VERSION` 决定，定义在
`os/sys/quilla-version.h`（默认 `1U`），经 `quilla-app-secver.h` 的
`QUILLA_APP_SECVER_PLACE()` 宏固化进 `.quilla_secver` 段（地址 `APP_START+0x400`，
PY32F040xB 为 `0x08007400`），随镜像一并被签名覆盖。版本号属已签名镜像的一部分，
由镜像自带——既非 QBOOT 赋值，也非上位机下发。修改版本号只需在编译 APP（QAPP）
时传入 `-DQUILLA_APP_SECURITY_VERSION=<n>`（Keil：在 QAPP 工程的 `<Define>` 追加该项），
**仅重编 APP 并重新签名**即可，无需改动 QBOOT。
启动与升级两条路径都比较

    版本 >= max(META floor, OTP counter)

其中 OTP counter 仅在启用硬件单调计数器时存在（见下）。META 是可擦写的，所以
当前机制无法防御以下两类物理攻击：

1. **重放**：攻击者擦除 META 区，写入任意一份**历史上合法签名**的旧
   manifest+旧镜像——验签仍会通过。
2. **持钥匙者降级**：被授权私钥的终端用户改小版本重签（Layer 2 自检只是混淆，
   见 `doc/secure-boot.md` §1.1）。

## 2. 硬件单调计数器（OTP）

### 2.1 设计

- **存储**：MCU 的 OTP 区（一次编程、永不可擦）。每个计数单元占 1 个 OTP 字节，
  烧一次（0xFF→0x00）计数值 +1；计数值即"未来镜像必须达到的最低安全版本"。
- **接入点**：`quilla_qboot_secure_min_version_get/set()` 同时抬升 META floor 与
  OTP 计数；启动时取两者较大值作为有效下限。烧写失败按尽力而为（best-effort）策略处理——签名校验仍是
  主闸门，OTP 写失败不会导致设备无法启动。
- **水位与回退**：OTP 容量有限。烧写前先判断剩余容量：足够 → 直接烧写；
  不足 → 将剩余计数单元全部烧满，然后烧写**耗尽标记字节**（区内最后一个字节，读回确认），此后
  回退为 META flash 下限单独承载。已烧的计数字节永远有效。
- **编译开关**（构建 QBOOT 时）：
  - `SOC_OTP_AVAILABLE`（`arch/cpu/<arch>/<mcu>/soc.h`，MCU 硬件属性）：
    GD32F502VE = 1（OTP2 数据块 256 B @ `0x1FF20000`）；PY32F040xB = 0（手册无
    OTP 区，但有可配置写保护，见 §2.4）；CH32V305 = 0（128 B 用户选择字区是
    可重写 Flash，全片烧写会被擦除，不算 OTP，见 §2.4）。
  - `QUILLA_QBOOT_HW_COUNTER_LEVEL`（功能等级，定义于 QBOOT 策略头
    `os/services/quilla-qboot/quilla-qboot-secure.h`，既不在 per-MCU 的
    `soc.h`，也不在构建工程里）：取三值之一——
    `QUILLA_QBOOT_HW_COUNTER_NONE`（不使用硬件计数器，下限仅由可重写 META
    flash 日志承载）、`QUILLA_QBOOT_HW_COUNTER_WPP`（受写保护 NV 计数器：主闪存写保护扇区，
    对应 `SOC_WPP_AVAILABLE`）、`QUILLA_QBOOT_HW_COUNTER_OTP`（真 OTP，
    对应 `SOC_OTP_AVAILABLE`）。**默认值按 `soc.h` 声明的硬件能力在
    `quilla-qboot-secure.h` 内派生**，无需工程文件或命令行 `-D`：仅有 WRP 的 MCU
    （PY32F040xB、CH32V305RB）派生为 WPP；具备真 OTP 的 MCU（GD32F502VE）在
    验证开关 `QUILLA_QBOOT_CONF_VALIDATE_TRUE_OTP` 为 1 时派生为 OTP，量产后
    置回 0 即派生 NONE（真 OTP 单元不可逆）。改等级请编辑该策略头，不要从
    Keil/EIDE/Makefile 传 `-D`。`soc.h` 仅声明硬件能力
    （`SOC_OTP_AVAILABLE` / `SOC_WPP_AVAILABLE` 及其 BASE/SIZE/CELL_SIZE）。内部启用宏
    `QUILLA_QBOOT_OTP_COUNTER` 由该等级与硬件派生，端口不得直接定义。
  - **等级与告警**（单点在 `quilla-qboot-otp.c`，每构建恰好一次）：
    - NONE → 告警 `QBOOT: HW counter disabled (LEVEL=NONE)`，并提示本 MCU
      可配置的最高等级（`QBOOT: LEVEL=OTP available on this MCU` 或
      `QBOOT: LEVEL=WPP available on this MCU`，两者皆无则提示无载体）。
    - OTP 等级但硬件无真 OTP → 告警 `QBOOT: LEVEL=OTP selected but this MCU
      has no OTP`；若该 MCU 具备 WRP 扇区，再提示
      `QBOOT: use LEVEL=WPP instead`。
    - WPP 等级但硬件无写保护扇区 → 对称告警（`QBOOT: use LEVEL=OTP instead`）。
    - 等级与硬件匹配 → 无告警。
  - **分区重叠与 APP 自动收缩**：选择 WPP 等级（受写保护 NV 计数器）会占用主闪存顶部一个
    WRP 扇区，因此 `QUILLA_QBOOT_APP_END` 在 `quilla-qboot-hal.h` 中**随 WPP 等级
    自动收缩到计数区基址前一字节**（`APP_END` 为含端点：PY32F040xB →
    `0x0801DFFF`（基址 `0x0801E000`）、CH32V305RB → `0x0001EFFF`
    （基址 `0x0001F000`）），
    仓库自带的 GCC 链接脚本、WCH 链接脚本与 Keil 工程的 IROM1/IRAM1
    （`QAPP.uvprojx` 的 `OCR_RVCT4`）均已同步收缩；`.sct` 是 uVision 生成
    产物、不入库。**集成者使用仓库自带构建配置（Makefile / EIDE / Keil
    `QAPP.uvprojx`）时无需手工改分区**；只有自建构建链路才需
    按同一边界收缩 APP 镜像区。若链路未收缩而 WPP 等级又打开，编译会报
    "QBOOT: APP_END overlaps WPP counter sector; shrink APP_END"。硬件写保护生效后，QIAP 对该扇区的任何
    擦写都会立即失败并置错误标志（WRPERR/WRPRTERR）——失败是显式可检测的，
    不会静默写坏数据。
  - **APP 镜像尺寸上限**：启用受写保护 NV 计数器 后，**APP 镜像末址不得超过
    `QUILLA_QBOOT_APP_END`**，即计数区基址前一字节
    （PY32F040xB = `0x0801DFFF`，CH32V305RB = `0x0001EFFF`）。由于 APP 分区已
    自动收缩到该边界，仓库自带构建产出的镜像天然不越界；QIAP 按写入数据逐块
    擦除，镜像未延伸至计数扇区时升级不受影响，延伸至该扇区则擦除因写保护失败。
    出厂打包工具 `qboot_image.py` 内置检查：目标 QBOOT 启用开关时，`pack`/`verify`
    传入 `--otp-counter-base <计数区基址>`，工具即拒绝末址超限的镜像（未启用的
    QBOOT 不应传此参数，此时计数扇区可正常使用）。GD32 真 OTP 不占 flash 分区，
    无需传。

### 2.1.1 配置方法（等级定义在策略头 quilla-qboot-secure.h）

功能等级不放在 per-MCU 的 `soc.h`，而定义在**策略头
`os/services/quilla-qboot/quilla-qboot-secure.h`**（默认值按 `soc.h` 声明的
硬件能力派生），使 GCC 与 Keil 共用同一处事实来源，同一颗 MCU 在两种编译器下
行为一致；`soc.h` 只保留硬件能力声明。硬件载体解析与 `APP_END` 分区钳位同样
由该策略头派生，`quilla-qboot-hal.h` 只负责布局常量与 HAL 接口（CR-0054）。

- **默认等级由 secure.h 按能力派生**：

  | MCU | 策略头默认（secure.h 派生） | 含义 |
  |---|---|---|
  | PY32F040xB | 派生为 WPP（WRP-only，无真 OTP） | 受写保护 NV 计数器 默认启用 |
  | CH32V305RB | 派生为 WPP（同上） | 受写保护 NV 计数器 默认启用 |
  | GD32F502VE | 派生为 NONE（具备真 OTP，但昂贵/不可逆，默认不开启） | 真 OTP 默认关闭，需构建时显式开启 |

- **如何关闭（opt-out，即选 NONE）**：在 `quilla-qboot-secure.h` 中把
  `QUILLA_QBOOT_HW_COUNTER_LEVEL` 定义为 `QUILLA_QBOOT_HW_COUNTER_NONE`
  （PY32/CH32 默认派生为 WPP）。适合"设备无外部烧录口、基于 Flash 下限已够用"
  的出货形态（见 §2.3）。
- **如何开启真 OTP（GD32）**：把 `quilla-qboot-secure.h` 中的
  `QUILLA_QBOOT_CONF_VALIDATE_TRUE_OTP` 置 1。**仓库当前默认 0**（2026-11-12
  GD32 上板验证结束后已回归），此时 GD32 派生 NONE，编译期会提示
  `QBOOT: LEVEL=OTP available on this MCU`。真 OTP 单元一次可编程、消耗不可逆，
  验证/量产后置回 0（M33 平台 LTO 体积约束下 SEALED/SECURED 变体也须谨慎，见 §2.1.1）。
- **APP 分区无需手工改**：开启受写保护 NV 计数器 后 `quilla-qboot-hal.h` 自动把
  `QUILLA_QBOOT_APP_END` 收到计数区基址，仓库自带的 `.ld` 与 Keil 工程
  IROM1/IRAM1 已收缩（`.sct` 为生成产物、不入库）；
  集成者只要用仓库自带构建配置即可。自建构建链路者须镜像这一收缩（基址同上）。
- **打包侧强制上限**：产线打包调用 `qboot_image.py pack/verify` 时，若目标 QBOOT
  启用了开关，一律传 `--otp-counter-base <计数区基址>`，让工具在打包与复核两端
  拒绝越界的 APP 镜像。

### 2.2 计数器的存储粒度

防回滚计数器的实现差异，主要在"每 +1 消耗多少不可变存储"上。下面把本设计的方案与
其他常见做法并列，便于理解字节粒度 OTP 的取舍：

| 维度 | 本设计（Quilla QBOOT） | 其他常见做法 |
|---|---|---|
| 镜像侧计数 | 安全版本烧在镜像固定地址，随镜像一并被签名覆盖（无独立 TLV） | 安全计数器常作为一个 **4 字节** uint32 放进镜像的 protected TLV |
| 硬件侧递增 | 字节粒度 OTP，**每 +1 烧 1 字节**，容量 = 预留字节数 − 1（GD32F502VE = 255 次） | eFuse 类实现通常**每 +1 烧 1 个 fuse bit**，容量 = 可用 fuse 位数 |
| 无硬件载体时 | 回退为 META 磨损均衡日志（普通可重写 Flash） | 多在 Flash 里模拟 NV counter，思路一致 |
| 计数耗尽 | 烧满剩余字节 + 烧耗尽标记 → 回退为 META flash 下限单独承载 | 基于余量检查的方案在接近上限时即提示容量不足 |

**关键区别**：eFuse 类实现是"按位烧录"（每 +1 消耗 1 个 fuse bit），而本设计所用的
GD32 OTP 是字节粒度一次性编程（每 +1 消耗 1 字节），无法实现按位烧录——这正是本设计
没有套用 eFuse 计数器的原因。容量按"安全版本发布次数"计，255 次对绝大多数产品的
使用寿命而言足够；版本号必须逐次 +1，跳号会按跳过的数字多烧字节。

### 2.3 客户须知：启用与不启用的利弊

**启用硬件单调计数器，将获得更高的安全性**：META 区被擦写后的重放攻击、以及持
钥匙者的版本回滚，都会被 OTP 中不可擦除的计数值拒绝。

但要注意两点边界：

1. **OTP 有容量上限**。多次升级之后，计数值可能超出 OTP 容量上限——届时 QBOOT
   会烧耗尽标记并自动回退，**依然只能使用 Flash 作为单调计数器**，安全水平回到
   与不启用时相同。版本号跳号会加速耗尽。
2. **如果设备没有暴露 QBOOT 升级功能之外的芯片烧录接口**（生产时锁定 SWD/RDP、
   不留 ISP/调试口），攻击者不具备擦写 META 区的任何途径，基于 Flash 的
   安全下限持久化已能满足防回滚需求，**可以考虑不启用 OTP 作为单调计数器**。
   反之，只要 SWD 或调试接口存在被物理接触的可能，即应启用。

概括而言：**OTP 是将"防回滚"从"依赖流程纪律"提升到"依赖硅片物理"的唯一手段**；
是否启用取决于设备的物理暴露面与产品寿命内的预期升级次数。

### 2.4 三颗 MCU 的硬件特性（OTP / 写保护）

| MCU | 真 OTP | 可配置写保护 | 本项目裁定 |
|---|---|---|---|
| GD32F502VE | **有**：OTP2 数据块 256 B @ `0x1FF20000`，字节一次编程、永不可擦 | 不需要 | 真 OTP 载体；`QUILLA_QBOOT_CONF_VALIDATE_TRUE_OTP=1` 时为 OTP 等级（量产置回 0；M33 平台 LTO 体积约束见 §2.1.1） |
| PY32F040xB | 无（14 KB Information 区 = FT info0 / 选项字节 / UID / 系统 BootROM，均非用户 OTP） | **有**：主闪存 WRP，粒度 8 KB | **WRP 受写保护 NV 计数器 已实现**（`quilla-qboot-secure.h` 派生为 WPP 等级，可在其中选 NONE 关闭） |
| CH32V305RB | 无（128 B 系统配置区出厂固化不可改；128 B 用户选择字区可重写） | **有**：主闪存 32 个写保护扇区（4 KB/扇区） | 同 PY32，**WRP 受写保护 NV 计数器 已实现**（`quilla-qboot-secure.h` 派生为 WPP 等级，可在其中选 NONE 关闭） |

**PY32F040xB（依据 PY32F0 系列 FLASH IP 参考手册）**：

- **Information 区不可写配置**：选项字节（Information 区内）默认**可读、写保护**；
  写入需向 `FLASH_OPTKEYR` 写 OPTKEY1/2 专用序列解锁，`OPTLOCK` 位可由软件置位
  阻止后续擦写；错误时序触发 HardFault 并锁定至复位。数据手册将其列为
  "Option byte 写保护，专门的解锁设计"。
- **主闪存写保护 WRP**：**只作用于主闪存**（Information 区不受 WRP 保护）；
  粒度 8 KB（128 KB 片 = `WRP[15:0]` 16 位）；受保护区禁止擦写，任何一位置位时
  整片擦除失效，违规操作置 `WRPERR`。
- **生效时机（关键约束）**：修改选项字节是"整页擦除 + 按寄存器值回写（自动补
  码）"，且新值只在 **POR/BOR 或 `OBL_LAUNCH`（本身触发复位）** 后加载——
  **WRP 开关必然伴随一次复位**，无法在运行中静默切换。
- **受写保护 NV 计数器 方案（已实现，`quilla-qboot-secure.h` 派生为 WPP 等级、可在其中选 NONE
  关闭）**：把主闪存顶部一个专用 WRP 扇区作为计数区，生命周期遵循：**仅在
  QBOOT 烧录计数的窗口内打开写保护，写入完成立即恢复；烧录后以及 APP 使用时均
  保持写保护**。实现要点（`quilla-qboot-otp.c` + 各 MCU `quilla-qboot-hal.c`）：
  - **选择 NONE 等级 = 零成本**：整个功能编译为空操作——无额外复位、无选项字节
    页磨损、无 Flash 预留（仅 hal.h 里几个派生常量）。
  - **cell 粒度按编程原语定**：GD32 OTP = 1 字节（255 版）；PY32 = 1 个
    256 B 页（32 页 − 1 耗尽标记页 = 31 版）；CH32 = 1 个 32 位字
    （1024 字 − 1 标记 = 1023 版）。
  - **流程与代价**：QIAP SIGN 时只抬 META floor；`quilla_qboot_otp_sync()`
    在下次启动时烧计数——`counter < floor` 且扇区受保护 → 解锁（选项字节
    重写 + OBL_LAUNCH 复位）；窗口打开 → 逐 cell 烧 + 读回确认 → 重新上锁
    （再复位）。**每次升级多两次复位**，且只在 WPP 等级选择时发生。
  - **幂等与掉电安全**：每次启动从扫描恢复，烧写中断最多浪费一个 cell；
    WRP 状态判定以存储的选项字节配置为准（CH32 读原始选择字而非镜像寄存
    器），避免"重载未发生→重复解锁"的复位死循环。
- **安全注解（必须向客户如实说明）**：WRP/OPTLOCK 属于**可配置硬件**，不是 OTP
  物理保证——它可阻止 APP 与失控代码对该区的擦写，但无法阻止具备选项字节解锁能力的代码
  或外部烧录器（能重配 WRP 的攻击者同样能重置计数）。安全水位：高于裸 flash
  计数、低于真 OTP，与常见的 flash 模拟 NV counter（如 STM32 上各类方案）同级。另注意
  选项字节每次修改整页擦写，其擦写寿命低于主闪存，频繁升级需评估。
- **启用受写保护 NV 计数器 后 APP 镜像的上限**（分区起止本身不变，见 §2.1）：
  PY32F040xB 的 APP 镜像末址 ≤ `0x0801DFFF`（计数区基址 `0x0801E000` 前一字节；
  有效容量 92 KiB，原 100 KiB 减去顶部 8 KiB 计数扇区）；CH32V305RB 的 APP
  镜像末址 ≤ `0x0001EFFF`（基址 `0x0001F000` 前一字节；有效容量 124 KiB）。`qboot_image.py pack/verify` 传 `--otp-counter-base` 即可在打包
  与复核两端强制该上限。
- **为什么不用"直接写保护 META 分区"替代计数扇区**：地址上可行（META
  `0x00006000–0x00006FFF` 正好一个 4 KB WRP 扇区），但方案不成立——
  ① QIAP 升级标志（flag）与 manifest、floor 同在 META 这 4 KB 内，常保护会
  把 QBOOT 自己的写入一并挡掉（`swqboot` 失效、QIAP 提交失效、floor 抬升
  失效），要保功能就得每次升级走解锁/上锁复位循环，代价不降反升；
  ② WRP 位本身存于可重写的用户选择字，攻击者重配选项字清除保护后即可
  擦除 META 并重放旧镜像——保护强度与计数扇区同级，却没有任何增量收益；
  ③ 计数扇区只在 SIGN 之后、启动门之前被 QBOOT 独占写入，写流量极小，
  适合常保护；META 是每次升级都要频繁写的活动区，本质上不适合常保护。

**CH32V305RB（依据 CH32V303/305/307 数据手册与 WCH 官方论坛）**：

- **无真 OTP 的结论维持**：128 B 系统非易失配置信息存储区出厂固化、用户不可
  修改；128 B 用户自定义信息存储区（用户选择字 @ `0x1FFFF800`）= 结构 16 B +
    用户数据约 112 B，半字编程、数据与其反码成对存储、整区读改写——**可重写
    flash，不算 OTP**。WCH 社区博客《CH32V20x_30x系列用户选择字区域应用与操作》亦演示了该区域经
    FLASH 解锁后可整区读改写（写入前需先整区擦除再回写 16 B 结构字节），印证"可重写、
    非 OTP"；该博客操作的是选择字剩余 112 B 用户数据区，与本设计的 WRP 主闪存扇区
    属不同载体，但同样说明选择字可被重配——支撑上面"可配置硬件、非物理 OTP"的安全注解。
- **可写保护区**：主闪存划分 32 个写保护扇区（`FLASH_WRProt_Sectors0..30`，
  第 31~127 扇区共用一位），保护位存于用户选择字，上电/选项字节重载后生效
  （`FLASH_EnableWriteProtection()`）。机制与 PY32 同级：**可配置硬件**，
  上面的安全注解同样适用。

## 3. 为什么 QBOOT 不做 A/B swap

业界常见的 A/B 双分区 + 自动回退（swap & revert）常被视为更先进的默认方案，
QBOOT **有意不做**，理由是安全而非成本：

A/B swap 的语义是"新镜像先在 B 区试运行，失败自动回退 A 区，设备**始终在跑**"。
这为攻击者提供了一个日常运维中不可见的反复试验环境：攻击者可以**反复尝试**
改写、反编译并修改的固件——失败即自动回退，设备照常运行，**设备管理人员在日常
运维中无法察觉设备正在被持续攻击**。单次试错成本被 A/B 机制大幅降低。

QBOOT 的单区模型使同样的攻击表现为**显式失败**：任何未通过验签的镜像都无法
进入启动路径，设备停在 QBOOT shell（串口仍可 QIAP 恢复），一次被篡改的升级尝试
就是一个显性事件。对安全关键的设备，"失败可见"优于"静默自愈"；升级中断的可靠性
问题由 QIAP 的掉电可恢复标志（`flag=QIAP`）与 META 双槽记录解决，不需要双分区。

## 4. 验证状态

- 三 MCU 的 QBOOT 构建全部通过：GD32F502VE（CHECK/SEALED/SECURED 三变体）、
  PY32F040xB、CH32V305RB（GNU 构建）。
- 告警矩阵实测符合第 2.1 节矩阵（PY32/CH32 带 define 的构建亦通过：受写保护 NV 计数器
  代码全量链接，QBOOT 体积 PY32 22 108 B / CH32 21 951 B，均在 24 K 分区内；
  GD32 带 define 20 272 B）。
- GD32F502VE 真 OTP 载体已上板验证（2026-11-11/12，Quilla.ZP 板 SECURED）：
  `secver` 读回 `OTP_COUNTER` 由 1 推进到 2、`exhausted=0`；`pinfo` 确认
  `APP_END=0x0807FFFF` 不被钳位；签名升级 `ACK_SIGN 0x84` 通过；低版本
  （真私钥签名）镜像被 QIAP SIGN 拒绝 `NAK(0xC4) reason=0x0A ROLLBACK`；
  同版本再升级合法且计数器不重复烧写。CH32 WPP 路径仍待上板确认。
