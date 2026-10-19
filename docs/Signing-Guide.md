# 驱动签名指南：测试签名 vs 生产签名

一句话结论：

> **本项目只能交付“测试签名可安装”的代码。生产签名必须由签名证书持有者（你/你的组织）自行完成——
> 它需要 EV 代码签名证书 + Microsoft Partner Center 账号，且需要组织实名验证。**

| | 测试签名（Test Signing） | 生产签名（Production） |
|---|---|---|
| 目的 | **本机开发自测** | **面向用户分发** |
| 证书 | 自签（`New-SelfSignedCertificate`） | EV 代码签名证书（CA 颁发） |
| 系统要求 | `bcdedit /set testsigning on` + **重启** | 无需改动系统启动配置 |
| 用户可见现象 | 桌面右下角出现 **“测试模式”水印** | 无任何水印、无警告 |
| Secure Boot | **必须关闭**（开了就加载不了） | 正常工作 |
| 用户机器上能否装 | 只能在同样开了测试模式的机器上 | 任意正常 Windows 10/11 |
| 认证流程 | 无 | WHQL 完整认证 或 Attestation 签名 |
| 适用人群 | 开发者本人 | 最终用户 |
| 可否分发给普通用户 | **绝对不可** | 可以 |

---

## 1. 测试签名（仅限本机开发自测）

### 1.1 做什么

1. 生成自签代码签名证书；
2. 把证书导入**本机**的 `Trusted Root CA` 与 `Trusted Publisher`（这一步是关键：让这台机器信任你）；
3. 用 `signtool` 给 `.sys` / `.cat` 签名；
4. `bcdedit /set testsigning on` → **重启**；
5. 安装驱动。

```powershell
# 一步到位（本仓库脚本）
powershell -ExecutionPolicy Bypass -File tools\make-test-cert.ps1
powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode TestSign -Force
shutdown /r /t 0
```

### 1.2 你必须接受的三个副作用

| 副作用 | 说明 |
|---|---|
| **必须重启** | 改了启动配置，不重启不生效 |
| **“测试模式”水印** | 重启后桌面右下角常驻水印，这是 Windows 的既定设计 |
| **Secure Boot 需关闭** | UEFI 里关闭；测试签名在 Secure Boot 开启时被拒绝加载 |

### 1.3 红线（**不能做的事**）

* ❌ **不能**把测试签名的驱动包分发给普通用户。后果：用户机器上加载失败（设备管理器代码 52），
  或者诱导用户去执行 `bcdedit /set testsigning on` 并关闭 Secure Boot——
  **这等于让用户为装一个麦克风而关闭系统安全保护，在合规与安全上都不可接受。**
* ❌ **不能**把 `tools\_out\MicLinkTestSigning.pfx`（及口令）带到其它机器或提交到仓库。
  它让“信任它的机器”信任“任何用它签的东西”。
* ❌ **不能**在文档、README、安装脚本或任何对用户的表述里暗示“测试签名可以正常分发”。
  本仓库的 `tools/install.ps1` 在 TestSign 模式下会**主动打印**这条警示。

### 1.4 清理

```powershell
# 关闭测试签名（重启后水印消失）
bcdedit /set testsigning off
shutdown /r /t 0

# 删除本机上的测试证书（证书管理器 certlm.msc）
#   受信任的根证书颁发机构 / 受信任的发布者 → 找到 "MicLink Open Test Signing" → 删除
```

---

## 2. 生产签名（面向用户分发）

生产签名有两条路，都走 **Microsoft Partner Center**（原 Hardware Dev Center）：

| | **Attestation 签名** | **WHQL 完整认证（HLK 测试）** |
|---|---|---|
| 本质 | 微软对提交包做静态扫描 + 人工/自动合规检查后直接签名 | 跑完 Windows Hardware Lab Kit（HLK）/ HCK 全部测试后签名 |
| 前提条件 | EV 代码签名证书 + Partner Center 账号 | 同左 + 搭建 HLK 测试环境（通常要一台测试机 + 一台控制器） |
| 是否需要跑 HLK 测试 | **不需要** | **需要**（音频类驱动有对应的 HLK 测试项） |
| 分发范围 | Windows 10 起（不受支持更早系统） | 全版本（含 Windows Server、更早的 Windows 10 版本） |
| Windows Update 自动推送 | 不支持 | 支持（可选） |
| 周期（量级，非承诺） | 通常**数个工作日**量级 | 通常**数周**量级（取决于测试与整改轮次） |
| 成本（量级，非承诺） | EV 证书年费 + Partner Center 账号年费 | 同左 + 测试环境与人力成本 |

### 2.1 通用流程

```
1) 购买 EV 代码签名证书（CA：DigiCert / Sectigo / GlobalSign / SSL.com 等）
     └─ 需要组织实名验证（营业执照、邓白氏编号、律师/会计师证明信、电话回访等）
     └─ 私钥通常要求存放在硬件令牌 / HSM 或云 HSM（EV 的硬性要求）
2) 注册 Microsoft Partner Center（Hardware 计划）
     └─ 需要 EV 证书 + 组织验证（也是数年费，具体以当时官方报价为准）
3) 准备驱动提交包（.cab）
     ├─ MicLinkVirtualMic.inf
     ├─ miclinkvirtualmic.sys
     ├─ MicLinkVirtualMic.cat（用 EV 证书先签好）
     └─ 符号文件（可选但建议）
4) 提交到 Partner Center
     ├─ Attestation：勾选 attestation signing，上传，等待微软扫描与签名
     └─ WHQL：先在 HLK 上跑通音频类测试，上传 HLK 结果包（.hlkx）再提交
5) 下载微软签名后的驱动包
     └─ 微软会用 "Microsoft Windows Hardware Compatibility Publisher" 重新签 .cat
6) 用签名后的包安装：
     powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode Production -InfPath <inf>
```

### 2.2 所需材料清单

| 材料 | 说明 |
|---|---|
| 组织主体证明 | 营业执照等（用于 EV 证书与 Partner Center 的组织验证） |
| EV 代码签名证书 | 向 CA 购买；**本仓库无法代你取得** |
| Microsoft Partner Center 账号 | Hardware 计划，年费 |
| 驱动提交包 | inf + sys + （EV 已签的）cat，打包成 cab |
| 驱动版本号管理 | 每次提交需要递增版本号（`DriverVer=` 与 `Version` 段） |
| HLK 环境（仅 WHQL） | 一台控制器 + 至少一台测试机，跑音频类测试 |
| 隐私策略 URL / 公司信息 | Partner Center 提交时要求 |

### 2.3 成本与周期（**量级说明，请以当时官方报价为准**）

| 项目 | 量级 |
|---|---|
| EV 代码签名证书 | 每年数百美元量级（不同 CA、不同渠道差异很大） |
| Partner Center（Hardware 计划）账号 | 每年数百美元量级 |
| Attestation 签名周期 | 数个工作日量级 |
| WHQL 完整认证周期 | 数周量级（含测试与整改轮次） |
| 隐性成本 | HLK 环境搭建、失败整改、每次更新的重新提交 |

> ⚠️ 上表是**数量级估计**，不是报价。
> 实际价格、流程细节、所需材料会随微软与各家 CA 的政策变化，
> **请务必以当时的官方报价与文档为准**（Microsoft Learn: *Windows Hardware Dev Center*、
> *Driver signing policy*、*Attestation signing*）。

### 2.4 两者差异与升级路径

```
        ┌─────────────────┐
        │  测试签名（自签）│  ← 开发自测，本仓库脚本支持
        └────────┬────────┘
                 │ 拿到 EV 证书 + Partner Center 账号
                 ▼
        ┌─────────────────┐
        │ Attestation 签名│  ← 快速对外分发（Win10+），无需 HLK
        └────────┬────────┘
                 │ 需要 Windows Update 推送 / 覆盖更多 Windows 版本 / 企业客户要求
                 ▼
        ┌─────────────────┐
        │ WHQL 完整认证   │  ← 最完整，需跑 HLK
        └─────────────────┘
```

* 同一份驱动代码无需改动，差别只在**签名与认证**；
* 建议路径：开发期用测试签名 → 内测分发用 Attestation → 正式商用/企业客户再补 WHQL。

---

## 3. 本仓库提供什么、不提供什么

| 提供 | 不提供 |
|---|---|
| `tools/make-test-cert.ps1`：生成测试证书、测试签名 | 任何形式的 EV 证书 / 微软签名 |
| `tools/install.ps1 -Mode TestSign`：测试签名安装（含水印与重启提示） | 替你完成 Partner Center 提交 |
| `tools/install.ps1 -Mode Production`：安装**已正式签名**的驱动，并严格校验签名 | 悄悄把生产模式降级成测试模式（**绝不会**） |
| `tools/verify.ps1`：安装后自检（含“无代码 52”检查） | WHQL/HLK 测试执行 |
| `docs/` 的流程与材料清单 | 价格承诺（只给数量级，请以官方报价为准） |

---

## 4. 一句话给决策者

> 如果你想**自己在本机跑通**：用测试签名，接受重启与“测试模式”水印，**只在你自己的机器上**。
> 如果你想**给别人用**：必须先取得 EV 代码签名证书并通过 Attestation 或 WHQL 认证，
> 这一步**本仓库帮不了你**，只能由证书持有者（你/你的组织）完成。
