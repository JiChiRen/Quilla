**[中文](secure-boot.zh-CN.md) · [English](secure-boot.md)**

# Quilla 安全启动（Secure Boot）设计说明

本文件说明 QBOOT 的启动期信任建立策略，帮助构建 / 发布 / 认证负责人在三种**构建变体（build variant）**之间做选择，并给出 IEC 62443 合规级别的对应建议。

> 阅读对象：负责固件构建、发布与认证的开发者 / 安全负责人。
> 密钥管理（默认公钥、自有公钥、私钥存放）见 `tools/README.md`。
> 硬约束：默认公钥（随仓库公开）禁止随产品交付；私钥须离线保存，仅留在签名工位，绝不进固件或仓库；
> 量产构建可定义 `QUILLA_PRODUCTION_BUILD`——若仍编译进仓库默认公钥，头文件会发编译警告提醒替换（不再以 `#error` 硬拦）。

---

## 1. 启动期做了什么

QBOOT 把控制权交给 APP 之前，按以下顺序建立信任（步骤 1 永远执行；步骤 2、3 取决于构建变体）：

1. **完整性（Integrity，始终执行）**：对 APP 镜像（`[APP_START, APP_START + image_len)`，`image_len` 为 manifest 记录的实际写入固件长度）做 SHA-256，与摘要比对，检测"镜像被改坏 / 半残烧录"。
   成本与**实际写入的 APP 镜像大小成正比**：在 **PY32F040xB（Cortex-M0+ @ 8 MHz）** 上，对一个典型的 ~18 KB QAPP 镜像约需 **0.6 s**（≈ 30 KB/s），即写入 N KB 的 APP 镜像约需 N × 0.033 s。
   无 manifest 或摘要不符 → 拒绝启动、落入 QBOOT shell。
2. **真实性（Authenticity）**：对 `image_len(4) || SHA-256(32)` 这个 36 字节描述符做 **Ed25519 验签**，公钥编译进 QBOOT 镜像（不可改），证明镜像"确实由持有私钥的一方签发"。
   验签对象固定为 36 字节描述符，与镜像大小**无关**：在 **PY32F040xB（Cortex-M0+ @ 8 MHz，无 32×32→64 乘法器、无密码加速，纯 C 大数运算）** 上约需 **20 s**。GD32F502VE（Cortex-M33 @ 160 MHz，含 32×32→64 乘法）实测 **< 500 ms**（~18 KB 镜像）。
   **仅"带钥匙"的构建变体执行此步。**
3. **防回滚（Anti-rollback）**：APP 镜像的**固件安全版本**低于 META 中持久化的单调下限时拒绝启动。
   安全版本**不由 QBOOT 持有、也绝不来自上位机**：它烧录在签名镜像区域内的固定地址 `QUILLA_QBOOT_APP_START + 0x400`（`0x08007400`），属于被签名的 `image_len` 的一部分，因此受 SHA-256 摘要与 Ed25519 签名覆盖——没有私钥就无法伪造。认证安装成功后，下限被抬到该固件版本。
   完整设计（含非安全性的 APP 侧混淆层）见 **§1.1 双重防回滚机制**。
   **仅"带钥匙"的构建变体执行**（QBOOT_CHECK 不执行，因其无钥匙、无法信任下限）。

### 1.1 双重防回滚机制

防回滚闸门由**两层协作**实施。只有第一层是安全控制；第二层是诚实的纵深防御，**明确不是**密码学边界。

**第一层——固件自有的安全版本（真正的闸门）。**
APP 镜像携带一个 32 位安全版本（`QUILLA_APP_SECURITY_VERSION`），烧录在固定 Flash 地址 `QUILLA_QBOOT_APP_START + 0x400`（`0x08007400`），位于被签名的 `image_len` 之内。因此它受 SHA-256 摘要与 Ed25519 签名覆盖：攻击者在无私钥时无法篡改。QBOOT 在启动（`quilla_qboot_secure_fw_version()`）与 QIAP SIGN 阶段读取它，与持久化下限比较，低于则拒绝（`QUILLA_SB_ERR_ROLLBACK`）。用真实私钥签出的降级镜像只是带着更低的版本，同样被拒。该版本**从不由 QBOOT 持有、也从不由上位机下发**——只有真正的（已签名）发布才能确定其值。每次"不可被回滚"的固件发布都应抬高 `QUILLA_APP_SECURITY_VERSION`。

**第二层——APP 侧混淆（`QUILLA_APP_SECVER_SELFCHECK`，默认开）。**
APP 在启动期额外对 Flash 中的该区域做自检（`quilla_app_secver_selfcheck()`），若 magic / version 内部不一致则停机。**诚实边界——依赖前请先读：** 这是**混淆（obfuscation），不是安全控制**。它唯一的作用是**抬高被授权私钥的终端用户的门槛**——这类用户本可定位二进制里显性的安全版本字节、改大、重新签名，用一行修改就绕过 QBOOT 闸门。它**挡不住**持有私钥的坚定攻击者：区域与编译期常量都在被签名镜像内、受签名覆盖，持钥匙者总能造出一致的（`magic`, `version`）对并重新签名。真正的闸门仍是第一层（签名 + 下限）；量产地仍须**健全的私钥授权**，而能挡住持钥匙者回滚的唯一控制是**硬件单调计数器（eFuse/OTP）**。若干扰调试流程，可用 `-DQUILLA_APP_SECVER_SELFCHECK=0` 关闭第二层。

> **密钥管理推论：** 因版本已是签名镜像的一部分，上位机升级工具（`quilla_host_upgrader.py`）不再传输版本——它只发镜像，版本归属签它的人。无钥匙（QBOOT_CHECK）安装仍写完整性 manifest、把版本当作下限占位，但那里不强制防回滚（无钥匙 ⇒ 无可信下限）。

**未编译任何公钥**（pubkey 全 0xFF）→ 步骤 2、3 自动跳过，仅保留步骤 1 的完整性。
这是历史"无钥匙"状态，现对应构建变体 **QBOOT_CHECK**。

---

## 2. 三种构建变体

变体由构建期宏决定，输出不同的 QBOOT 镜像（banner 中以 `[CHECK]` / `[SEALED]` / `[SEC]` 区分）。
三者启动策略与适用场景如下：

| 变体 | 钥匙（SEALED / SEC 同一把） | 启动策略（启动模式） | banner | 防回滚 | 是否声称 secure boot | 适用 |
|------|----------------------------|----------------------|--------|--------|----------------------|------|
| **QBOOT_CHECK** | 无 | 仅完整性（CHECK 模式，步骤 1） | `[CHECK]` | 否 | 否（仅完整性） | 开发 / 内部 / 无需真实性的产线 |
| **QBOOT_SEALED** | 默认钥匙（可替换为自有，见 §2.5） | SEALED 模式：SHA-256(~0.6 s，PY32F040xB) 比对 seal，命中即跳过验签；首启 / 换镜像回退 Ed25519 一次后重新 seal | `[SEALED]` | 是（固件版本，§1.1） | 是（依赖 RDP 锁 SWD） | 研发联调 + 量产（开 RDP，SL1/SL2） |
| **QBOOT_SEC** | 默认钥匙（与 SEALED 同一把，可替换为自有，见 §2.5） | SECURE 模式：每次启动都 Ed25519 验签（~20 s，PY32F040xB） | `[SEC]` | 是（固件版本，§1.1） | 是（每电重新验证） | 量产（认证轮廓要求每电再认证，SL3/SL4） |

### 2.1 QBOOT_CHECK（仅传输完整性）

- 启动只做 SHA-256 比对，不验签、不认来源、不防回滚。
- **不声称 secure boot**：它只能保证"镜像没在传输 / 烧录中损坏"，不能保证"镜像来自可信方"。
- 无需任何钥匙管理，开箱即用，适合开发调试与对真实性无要求的内部 / 封闭场景。
- 升级时由 QIAP 的 END 帧写入完整性 manifest（见 `doc/quilla-host-upgrader-guide.md` §10）；该 manifest 不含可信签名，签名域写 0。

### 2.2 QBOOT_SEALED（SEALED 启动模式，默认(公开)钥匙）

- 启动采用 **SEALED 模式**：稳态下每次启动只重算 SHA-256（PY32F040xB 上 ~0.6 s，随镜像大小线性变化）并与 META 记录中的 seal 比对，**跳过 Ed25519 验签**；仅在首次启动 / 换镜像（SWD 重烧、seal 丢失或过期）时才跑完整 Ed25519（PY32F040xB 上 ~20 s），通过后重新 seal。
- 编译进的**钥匙与 QBOOT_SEC 完全相同**：默认是仓库公开的"默认公钥"（`QUILLA_QBOOT_DEFAULT_KEY`，配套私钥 `tools/_keytest/private.hex` 随仓库公开——任何人都能签出它被接受的镜像）。
- **"能否出货"取决于钥匙是否仍是默认公开钥匙，而非 SEALED 这个变体名**：换成自有钥匙后，SEALED 模式本身（配合 RDP 锁 SWD）是合法的量产配置（SL1/SL2）。若仍用默认钥匙，则等同把私钥交出去，**禁止出货**。钥匙替换见 §2.5。

### 2.3 QBOOT_SEC（SECURE 启动模式，默认(公开)钥匙）

- 启动采用 **SECURE 模式**：每次上电都做完整 Ed25519 验签（PY32F040xB 上 ~20 s，与镜像大小无关），**不使用 seal 快路径**。适用于认证轮廓明确要求"每次启动重新验证真实性"的产品（SL3/SL4，或威胁模型把运行时妥协 / RDP 不足信纳入范围）。
- 编译进的**钥匙与 QBOOT_SEALED 完全相同**：默认是仓库公开"默认公钥"，出货前须替换为自有公钥。**两者的区别只在启动校验策略（SEALED 快路径 vs SECURE 每电验签），不在钥匙**——替换方式对两者一模一样，见 §2.5。
- 可定义 `QUILLA_PRODUCTION_BUILD`——仅让"仍编译进默认公钥"的编译警告更醒目，不再触发编译失败；是否替换由客户决定。

### 2.4 启动模式（SECURE / SEALED / CHECK）与变体的关系

"启动模式"是另一个正交的编译期旋钮（`QUILLA_SB_BOOT_MODE`）：

| 模式 | 含义 | 开机是否跑 Ed25519 | 开机耗时（PY32F040xB，M0+/8 MHz） |
|------|------|-------------------|----------|
| `SECURE` (=0) | 每次上电都验签 | **是**，每电一次 | ~20 s（与镜像大小无关） |
| `SEALED` (=1，默认) | 验签最多每镜像一次（seal 快路径） | **否**（seal 命中时） | ~0.6 s（SHA-256，随镜像大小线性变化） |
| `CHECK` (=2) | 仅完整性（无验签） | 否 | ~0.6 s（SHA-256，随镜像大小线性变化） |

> 上表耗时为 **PY32F040xB（Cortex-M0+ @ 8 MHz）** 实测量级，基准为 ~18 KB 的 QAPP 镜像；SHA-256 段耗时与实际写入的 APP 镜像大小成正比（≈ 30 KB/s）。APP 可用分区：PY32F040xB ≈ 100 KB（`0x08007000`–`0x0801FFFF`）、GD32F502VE ≈ 484 KB（`0x08007000`–`0x0807FFFF`）；SHA-256 只算实际写入的 `image_len` 字节，不扫整片。
> GD32F502VE（Cortex-M33 @ 160 MHz）已上板：Ed25519 实测 **< 500 ms**（~18 KB 镜像）；SHA-256 仍为估计 ~0.03 s/18 KB（未单独实测）。
>
> **注意**：本文所有耗时数据均为**开发过程实测值**，会随软件版本与工具链变化而不同，**不构成交付规格**；请在自己的构建上复测。

三个变体的出厂启动模式分别为：**QBOOT_CHECK = CHECK 模式**；**QBOOT_SEALED = SEALED 模式**；**QBOOT_SEC = SECURE 模式**。如需进一步调整，把对应变体的 `QUILLA_SB_BOOT_MODE` 改为 `SECURE` 即可。

> SEALED 的安全性依赖芯片读保护（RDP / 锁 SWD）：SWD 不锁，攻击者可改写 APP、manifest 与
> META 记录中的 seal，seal 比对即被绕过。**QBOOT_SEALED / QBOOT_SEC 要声称安全，量产必须开 RDP。**

### 2.5 默认钥匙替换（SEALED / SEC 通用）

SEALED 与 SEC 两个变体编译进的是**同一把钥匙**：默认是随仓库公开的"默认钥匙"。无论选哪个变体量产，都必须把它换成客户自有密钥对（两者的替换方式完全一致）：

1. **离线生成**：`tools/quilla_ed25519_sign.py keygen` → 产出 `private.hex`（私钥）+ `public.hex`（公钥）。
2. **公钥编入 QBOOT**：把自有公钥以 `-DQUILLA_SECURE_BOOT_PUBKEY={...}` 或产品宏编入，替换默认的 `QUILLA_QBOOT_DEFAULT_KEY` 分支；分支模板见 `os/services/quilla-qboot/quilla-qboot-pubkey.h` 头注释与 `tools/README.md`。
3. **私钥只留签名工位**，离线保存，**绝不进固件或仓库**；烧录时由 `quilla_host_upgrader.py --key` 指向它（见 `doc/quilla-host-upgrader-guide.md` §10.3）。

> 若仍编译进默认公钥，头文件会发 `#warning`（`QUILLA_PRODUCTION_BUILD` 时更醒目）提醒替换——
> **是否替换由客户决定**，但流出默认钥匙等于把私钥交给任何人（任何人都能签出设备接受的镜像），
> 构成虚假安全声明。

---

## 3. 编译期选择（GNU / EIDE / Keil）

### 3.1 GNU / EIDE（`user/<mcu>/EIDE/Makefile.qboot*` 的 `C_DEFS`）

| 变体 | Makefile | 关键 `C_DEFS`（节选） |
|------|----------|------------------------|
| QBOOT_CHECK | `Makefile.qboot` | `-DQUILLA_SB_BOOT_MODE=QUILLA_SB_BOOT_MODE_CHECK -DQUILLA_QBOOT_VARIANT_STR=\"CHECK\"` |
| QBOOT_SEALED | `Makefile.qboot.sealed` | `-DQUILLA_QBOOT_DEFAULT_KEY -DQUILLA_SB_BOOT_MODE=QUILLA_SB_BOOT_MODE_SEALED -DQUILLA_QBOOT_VARIANT_STR=\"SEALED\"` |
| QBOOT_SEC | `Makefile.qboot.sec` | `-DQUILLA_PRODUCTION_BUILD -DQUILLA_QBOOT_DEFAULT_KEY -DQUILLA_SB_BOOT_MODE=QUILLA_SB_BOOT_MODE_SECURE -DQUILLA_QBOOT_VARIANT_STR=\"SEC\"`（钥匙注入见该 makefile 头部注释） |

`quilla-qboot-secure.h` 中 `#ifndef QUILLA_SB_BOOT_MODE` 默认取 `SEALED`。不定义即 SEALED（带钥匙时即完整验签，无钥匙时退化为仅完整性）。

### 3.2 Keil（`user/<mcu>/MDK-ARM/QBOOT.uvprojx` 的 Define 框）

每个 `<mcu>` 工程含三个 Target：`QBOOT_CHECK` / `QBOOT_SEALED` / `QBOOT_SEC`，Define 与 3.1 一一对应。含逗号的 `{...}` 公钥列表在 Keil Define 里会被切坏，故公钥走"产品宏"那一层（见 `tools/README.md`），不在 Define 里直接写。

---

## 4. 串口升级（manifest 写入）

无论哪种变体，APP 的安装 / 恢复都必须走 **QIAP 串口升级**（不要用纯 SWD 直烧 APP），因为 manifest（完整性摘要；SEALED/SEC 还含签名）由升级路径写入：

- **带钥匙**（QBOOT_SEALED / QBOOT_SEC）：END 仅 ACK，由 SIGN 帧验签通过后才写 manifest 并提交；
- **无钥匙**（QBOOT_CHECK）：END 帧直接写完整性 manifest 并提交。

参考实现与命令见 `doc/quilla-host-upgrader-guide.md` §10。

---

## 5. 与 IEC 62443 合规级别的对应（开发者自判）

> **免责声明**：下面是基于 IEC 62443-4-2 通用实践的**工程建议**，不是认证结论。
> 具体产品落在哪个 SL、是否要求"每次启动再认证"，取决于**目标组件的 SL 能力轮廓（component security capability profile）与威胁模型**，最终以认证评估机构裁定为准。不要自行夸大声称。

- **IEC 61508 SIL（功能安全）与此正交**：SIL2 关心的是"随机硬件故障 / 系统错误导致的损坏镜像在运行前被检出"，这由**完整性（SHA-256）**保证，三个变体都提供。**真实性 / 防篡改是 IEC 62443（信息安全）的事，不是 SIL。**

- **IEC 62443-4-2（组件安全要求）**：secure boot 通常是 SL2 及以上的要求项。

| 变体 / 模式 | 适用倾向 | 说明 |
|------|----------|------|
| **QBOOT_CHECK** | 不要求任何 62443 secure-boot 要求的产品、纯内部 / 开发 | 仅完整性，**不声称 secure boot** |
| **QBOOT_SEALED（SEALED 模式）** | **SL1 / SL2** 多数产品轮廓：安装时认证 + 每次启动完整性 + 锁 SWD(RDP) | 满足"启动前验证真实性/完整性"；前提是 RDP 开启 |
| **SECURE 模式** | **SL3 / SL4** 或更严轮廓：要求**每次启动**重新验证，或威胁模型把"运行时妥协 / RDP 不足信"纳入范围 | 提供每电再认证，代价是每次上电 ~20 s（PY32F040xB，M0+/8 MHz；GD32F502VE 实测 < 500 ms） |

**务实建议**：绝大多数终端产品用 **QBOOT_SEALED（SEALED 模式 + 开 RDP）** 即可满足 SL1/SL2，且开机快；仅当目标认证轮廓**明确要求每次启动重新验签**时才选 **QBOOT_SEC（SECURE 模式）**，并接受每次上电 ~20 s 的代价（PY32F040xB，M0+/8 MHz；GD32F502VE 实测 < 500 ms）。无论哪种模式，**公钥必须编译进 QBOOT**（这是安全启动的标准做法），**私钥只留签名工位、绝不进固件 / 仓库**；产品构建可加 `QUILLA_PRODUCTION_BUILD`，若仍编译进仓库默认公钥，头文件会发编译警告提醒替换（不再以 `#error` 硬拦）。

---

## 6. 作为从属 MCU 集成时的真实性责任分配

**场景**：Quilla MCU 作为从属元件挂在一个已通过功能安全认证的 SoC 之下，由 SoC 经串口（或板内总线）负责升级 MCU 固件。此时 MCU 是否仍需自行做 Ed25519 验签？

> **标准边界（关键）**：IEC 61508 / ISO 26262 是**功能安全**标准，关注随机 / 系统故障导致的意外损坏，并**明确把信息安全（security）排除在范围之外**（指向 IEC 62443）。Ed25519 验签解决的是**固件真实性（防故意篡改）**，属于**信息安全（intentional fault）**范畴。因此：**SoC 的 61508/26262 合规，与"MCU 要不要验 Ed25519"几乎无关**——SoC 的合规不提供任何针对恶意固件的保护。真正把"固件更新必须真实"列为硬性要求的是 **ISO/SAE 21434 + UN R155/R156（汽车）** 与 **IEC 62443（工控）**，而非 61508/26262。

### 6.1 MCU 是否需要独立验签：取决于三件事

| 判定项 | 若满足 → 真实性可委托 SoC，MCU 不必独立验 | 若不满足 → MCU 须独立验 |
|--------|------------------------------------------|--------------------------|
| ① SoC→MCU 升级链路是否敌手不可达 | 密封单板内、无外接线 / 调试口、SoC 本身不被攻破则链路安全 | 链路可物理触碰 / 有调试头 / SoC 可能被攻破 |
| ② MCU 是否存在其它固件入口 | 仅此一条经 SoC 的升级路径，无其他 | 还有 JTAG / SWD / USB / OTA 任一入口 |
| ③ 系统是否需满足 21434 / 62443 / R155 | 纯封闭安全系统，无网络安全合规要求 | 汽车 / 工控，必须端到端真实性 |

### 6.2 把真实性"分配"给 SoC 是否成立

可以成立，但有条件。在系统安全 / 网络安全论证中，可将"MCU 固件真实性"**分配（allocate）给 SoC**：SoC 在发送前完成验签（或签名由 SoC 侧保证），MCU 只收只烧。IEC 61508 / ISO 26262 只关注**结果**（跑起来的软件是授权且完整的），不规定**机制必须落在哪一级**；标准亦强调安全功能独立性——若 SoC 是经认证的信任锚且链路受控，这种委托是被接受的。

成立前提：6.1 的三项均站得住。任一不满足，则"委托 SoC"退化为**单点信任**——SoC 被绕过即整体真实性论证失效。
### 6.3 实务建议（防御纵深）

即便架构上 SoC 为锚点，也**不建议**因此从 MCU 删除 Ed25519：

- MCU 侧验签**成本极低**（仅存公钥、私钥始终离线，不引入任何密钥暴露面）；
- 它令 MCU **独立自保**：即便 SoC↔MCU 链路日后暴露、SoC 被攻破、或产线 / 返修有人直连 MCU 烧录，真实性仍成立；
- 同时简化安全 / 网络安全论证（每个元件独立验真，独立性正是 IEC 61508 强调的）。

**底线**：MCU 端至少保留 **hash / CRC 完整性校验**（捕获传输损坏，属合法诊断手段）；Ed25519 是否强制，取决于 6.1 的三项与系统是否需过网络安全标准——而**不是** SoC 是否持有 61508/26262 证书。

> 区分：若产品本就不要求安全启动，应使用 **QBOOT_CHECK**（旁路验签、仅保留完整性），这是"不需要安全启动"的明确声明；它与"MCU 作为从属元件、真实性由 SoC 负责"是两件不同性质的事，不要混用。

---

## 7. 串口可观测性（调试）

QBOOT 开机串口（`[qb]` 标记，DEBUG ONLY，禁止发货）可区分两条路径：

```
[qb] m-sig-in / m-sig-out    完整 Ed25519 验签窗口（~20 s）
[qb] m-sealed                走了 seal 快路径（Ed25519 被跳过）
[qb] sb-sealed               seal 快路径命中，跳过完整验签
[qb] sb-out=00000000         QUILLA_SB_OK（完整 Ed25519 验签通过）
[qb] sb-out=00000002         QUILLA_SB_OK_INTEGRITY_ONLY（仅完整性，CHECK 模式）
[qb] sb-out=00000003         QUILLA_SB_OK_SEALED（由 seal 通过）
[qb] sb-out=FFFFFFFC         QUILLA_SB_ERR_SIG（验签失败 / 钥匙不匹配）
[qb] sb-out=FFFFFFFD         QUILLA_SB_ERR_HASH（完整性不符，镜像被改 / 传输出错）
[qb] sb-out=FFFFFFFE         QUILLA_SB_ERR_NO_MANIFEST（无 manifest）
```

> 想验证"完整验签链路本身"而非只看 seal 快路径：清空 META 记录 seal 记录，或直接用
> `SECURE` 模式构建一次上板即可。
