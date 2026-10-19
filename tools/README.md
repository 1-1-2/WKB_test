# tools/ —— Windows 安装 / 卸载 / 签名 / 验证脚本

| 脚本 | 作用 | 是否需要管理员 | 典型场景 |
|---|---|---|---|
| `make-test-cert.ps1` | 生成**测试**自签证书并给 `.sys` / `.cat` 测试签名 | ✅ | 首次在本机跑通驱动 |
| `install.ps1` | 安装驱动（`TestSign` / `Production` 两种模式，绝不互降） | ✅ | 装驱动 |
| `verify.ps1` | 安装后自动检查，输出 PASS / FAIL 清单 | 建议 ✅ | 装完立刻自检 |
| `uninstall.ps1` | 移除设备与驱动包，并检查残留 | ✅ | 卸载 / 重装前清理 |

> **全部脚本都只面向 Windows。** 本仓库的开发沙箱是 Linux，这些脚本**从未被真正执行过**，
> 只做过人工语法与逻辑自审（详见下文“自审清单”与 `docs/VERIFICATION.md`）。

---

## 0. 前置条件

| 项 | 要求 |
|---|---|
| 系统 | **Windows 11 21H2（build 22000）或更高**（INF 声明的目标 OS；源码下限为 Win10 2004/19041，差异说明见 `README.md`） |
| 权限 | 管理员 PowerShell |
| 执行策略 | `powershell -ExecutionPolicy Bypass -File <脚本>`（或用管理员 PowerShell 里 `Set-ExecutionPolicy RemoteSigned`） |
| 驱动产物 | 先用 VS + WDK 编译出 `MicLinkVirtualMic.inf` / `miclinkvirtualmic.sys` / `MicLinkVirtualMic.cat`（见 `docs/BUILD.md`） |
| 签名工具 | `make-test-cert.ps1` 需要 `signtool.exe`（Windows SDK / WDK 自带） |

---

## 1. 测试签名路线（**仅限本机开发自测**）

```powershell
# 1) 生成测试证书并签名（会让"这台机器"信任"你自己"）
powershell -ExecutionPolicy Bypass -File tools\make-test-cert.ps1

# 2) 安装（会检查 bcdedit testsigning，未开启则提示；-Force 直接执行）
powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode TestSign -Force

# 3) 重启电脑（必须）—— 重启后桌面右下角会出现"测试模式"水印

# 4) 自动检查
powershell -ExecutionPolicy Bypass -File tools\verify.ps1

# 5) 人工验证：docs\Windows-Manual-Verification.md
```

**必须知道的三件事**：

1. `bcdedit /set testsigning on` 之后**必须重启**才生效；
2. 重启后桌面右下角会出现 **“测试模式”水印**，这是 Windows 的既定行为，不是异常；
3. 测试签名**不能**分发给普通用户：在没开测试模式的机器上会加载失败（设备管理器代码 52），
   而“让用户关掉系统安全保护”这一要求在合规与安全上都是不可接受的。

---

## 2. 生产签名路线

```powershell
powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Mode Production -InfPath C:\build\MicLinkVirtualMic.inf
```

`Production` 模式的行为：

* **不碰** `bcdedit`，**不会**降级成测试签名；
* 要求 `.sys` 与 `.cat` 都在，且 `Get-AuthenticodeSignature` 状态为 `Valid`、签名链可验证到受信任根；
* 任一项不满足 → 打印失败横幅并 `exit 5`，**不做任何安装动作**。

签名本身（EV 证书 + WHQL / Attestation）**不在本脚本范围内**，见 `docs/Signing-Guide.md`。

#### `install.ps1` 的退出码（缺陷 D-08 之后）

| 退出码 | 含义 |
|---|---|
| `0` | 成功：设备已枚举且无问题码；或显式 `-AllowNoDevice` / `-NoWait` 放宽 |
| `7` | 超时（`-TimeoutSeconds`，默认 90 s）内**未枚举到设备** —— 需 `-AllowNoDevice` 才会按成功处理 |
| `8` | 设备已枚举但**有问题码**（例如 52 = 测试签名未通过） |

其它失败分支（签名缺失 5、`pnputil` 失败 6 等）保持不变。CI / 批处理脚本可直接用退出码分支。

---

## 3. 卸载与残留检查

```powershell
powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1           # 移除设备 + 删除驱动包
powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1 -Force     # 仍有引用时加 /force
powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1 -KeepDriverPackage  # 只移除设备
```

检查项（PASS / FAIL 清单）：PnP 设备、Driver Store 驱动包、驱动服务、注册表服务项、
DriverStore 文件目录、`Win32_SoundDevice`、控制设备 `\\.\MicLinkControl` 是否还能打开。

> 注册表服务项与 DriverStore 目录常常要**重启后**才被系统清理，脚本会明确区分
> “确实没清干净”与“重启后会自动消失”。

---

## 4. verify.ps1 查什么

| # | 检查项 | 说明 |
|---|---|---|
| 1 | 管理员权限 | 影响 pnputil / 服务查询 |
| 2 | `bcdedit testsigning` | 测试签名模式下必须为开（`-ExpectTestSigning:$false` 切到生产语义） |
| 3 | 驱动服务 `MicLinkVirtualMic` | `Get-Service` |
| 4 | INF 注册 | `pnputil /enum-drivers` 匹配 `MicLinkVirtualMic` |
| 5 | PnP 设备存在且 `Status=OK` | `Get-PnpDevice` |
| 6 | 无问题码（重点 **代码 52**） | 52 = 签名未通过 |
| 7 | `Win32_SoundDevice` 出现设备 | WMI/CIM |
| 8 | Core Audio **捕获端点**出现设备 | 注册表 `MMDevices\Audio\Capture\...\Properties` 的 `DEVPKEY_Device_FriendlyName` |
| 9 | `\\.\MicLinkControl` 能 `CreateFile` 打开 | P/Invoke（`kernel32!CreateFileW`） |
| 10 | `IOCTL GET_VERSION` 返回版本号 | `CTL_CODE(0x22, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)` = `0x00222400` |
| M | 录音机 / OBS / Zoom / 24h 稳定性 | 标记 `MANUAL`，**必须人工执行** |

退出码：`0` = 无 FAIL（MANUAL 不计入）；`1` = 存在 FAIL，并打印排查指引。

---

## 5. 脚本自审清单（**本机无法执行，以下是人工审查过的点**）

这些脚本在本仓库的 Linux 沙箱里**一行都没有运行过**。已人工核对：

| 审查项 | 具体核对内容 |
|---|---|
| 参数绑定 | `param()` 块位于脚本最前（`#requires` 之后）；`[ValidateSet('TestSign','Production')]` 约束 `-Mode`；开关参数用 `[switch]`，布尔参数 `-ExpectTestSigning` 显式声明 `[bool]`，避免 PowerShell 把 `$false` 当成位置参数 |
| 管理员检测 | 用 `WindowsPrincipal.IsInRole(Administrator)`，不依赖 `$env:USERNAME` 之类的脆弱判断 |
| `$ErrorActionPreference` | 顶部统一设为 `Stop`；所有可能失败的外部调用都额外用 `try/catch` 或检查 `$LASTEXITCODE` |
| 外部命令退出码 | `pnputil` / `bcdedit` / `signtool` 之后立即读 `$LASTEXITCODE`（先赋值给局部变量，避免被下一条命令覆盖） |
| 变量作用域 | 跨函数使用的量（驱动名、服务名、结果集合）显式放在 `$script:` 作用域；函数内部不用 `$using:`；`Add-Result` 只追加到 `$script:Results` |
| `Set-StrictMode -Version Latest` | 已启用；对可能缺失的属性（如 `Get-PnpDevice` 的 `Problem`、MMDevices 的注册表值）一律用 `PSObject.Properties[...]` 或 `-ErrorAction SilentlyContinue` 取值，避免严格模式下抛异常 |
| 字符串与转义 | 中文提示统一放在 `@'...'@`（单引号 here-string，不做变量展开），避免 `$` 被误插值；`\\.\MicLinkControl` 在 PowerShell 里写成 `'\\.\MicLinkControl'`（字面两个反斜杠 + 点） |
| P/Invoke | `Add-Type` 前先判断类型是否已存在（重复运行不报错）；`CreateFileW` 用 `IntPtr` + `IsInvalid()` 判定 `-1`，不用 `0` 判定；句柄用完 `CloseHandle` |
| 幂等性 | `install/uninstall` 可重复运行：找不到设备/驱动包时走 WARN 分支而不是报错退出 |
| 错误分支覆盖 | 生产模式签名缺失、cat 缺失、pnputil 失败、设备枚举超时、非交互且无 `-Force` 等分支都有明确退出码（2/3/4/5/6/…） |
| 安全表述 | 测试签名相关输出统一附带“仅限本机开发自测、不得分发”的提示；**没有任何位置**暗示测试签名可以面向普通用户无警告分发 |

**未验证的部分**（只能靠真机）：PowerShell 版本兼容（`Win32_PnpDevice` / `pnputil /add-driver` 语法在不同 build 上的差异）、
`pnputil /enum-drivers` 输出字段的本地化文本（脚本已尽量只匹配 `MicLinkVirtualMic` 这类不受语言影响的串）、
`Add-Type` 编译 C# 探针在本机 .NET 版本下的可用性、以及全部真实安装效果。
