# tools/

**[中文](README.zh-CN.md) · [English](README.md)**

Quilla 工具集。本公开仓库仅提供下列工具，覆盖外部使用全链路——烧录你自己的 APP 镜像，以及为它签名。（内部构建 / 校验 / 审计工具不属于本公开仓库。）

## 对外提供

| 工具 | 用途 |
|------|------|
| `quilla_host_updater.py` | 经串口烧录 QAPP：`python quilla_host_updater.py <COM> app.bin -b 115200` |
| `quilla_ed25519_sign.py` | 安全启动签名 / 密钥管理：`keygen`、`pubkey`、`sign`、`--self-test` |
| `quilla_app_uart_flash.bat` | Windows 一键烧录辅助脚本 |

### 密钥管理（务必）

`tools/_keytest/` 存放仓库的**默认示例密钥对**。其私钥（`private.hex`）**有意公开并随仓库分发**，以保证 fresh clone 开箱即用：默认的 `QBOOT_SEALED` / `QBOOT_SECURED` 构建编译进相匹配的默认公钥，升级流程（`quilla_host_updater.py --key tools/_keytest/private.hex`）须用匹配的私钥签名，否则设备会在 SIGN 帧处拒绝升级。

**量产必须自己离线生成密钥对。** 默认钥匙不得随产品出货——其私钥已公开，任何人都能签出设备会接受的镜像：

```bash
python tools/quilla_ed25519_sign.py keygen --dir <离线安全目录>
```

- 产线 `private.hex` 只留签名工位——**不进固件、不进仓库**；
- 自有 `public.hex`（公钥）编进 QBOOT：`quilla_ed25519_sign.py keygen` 会打印两种流程——GNU/EIDE 在 `C_DEFS` 加 `-DQUILLA_SECURE_BOOT_PUBKEY={...}`；Keil 的 Define 按逗号分隔、放不下该列表，须在 `os/services/quilla-qboot/quilla-qboot-pubkey.h` 自行添加产品分支（模板见 `tools/_keytest/README.md` §3.2）并在 Define 里选它的 token；
- 可加 `-DQUILLA_PRODUCTION_BUILD`；若仍编译进仓库默认（公开）公钥，`quilla-qboot-secure.c` 会发一条 `#warning` 提醒替换（不再以 `#error` 硬拦，见 `os/services/quilla-qboot/quilla-qboot-pubkey.h`）。

## 参考

- Ed25519 —— RFC 8032（本仓库不内联标准原文，请取官方版本）：
  https://www.rfc-editor.org/rfc/rfc8032.txt
