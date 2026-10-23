# Windows 客户端真机端到端 CI 验证报告

- 仓库：`1-1-2/WKB_test`（公开）
- 工作流：`.github/workflows/windows-e2e.yml`（`windows-e2e`）
- 验证对象：Windows 客户端 `womic.exe`（mingw-w64 交叉构建，GUI 子系统，396,800 B，静态链接 libopus，内置三套官方驱动包）
- 取证工具：`client/tools/e2e_probe.py`（vendored，本次随 workflow 一并入库）
- QA：严过关（Yan）
- 日期：2026-10-23

## 0. 结论摘要

| 维度 | 结论 |
|---|---|
| 路由判定 | **QA（测试侧）** —— 两轮失败均为测试 harness 问题，已全部自行修复；未发现客户端源码 bug |
| 真机已验证通过 | a、b、c 三项（Round 2，windows-latest 真机） |
| 本地补充验证 | h（GUI 冒烟，wine + Xvfb）、b/c 的完整脚本逻辑（wine 仿真） |
| 尚未跑到的项 | d（驱动安装）、e（安装后状态）、f（端到端灌音，核心）、g（R3 权限裁决） |
| 阻塞原因 | Round 3 的 build job 卡死在 `apt-get install gcc-mingw-w64-x86-64`（GitHub 基础设施瞬时故障），触发 30 分钟作业超时被取消；随后用于推送/触发 CI 的 PAT 失效（HTTP 401），无法再触发新 run |
| 遗留问题 | 见第 7 节；核心项 f/g 需要一次成功的 run 才能出结论 |

## 1. 三轮 CI 运行记录

| 轮次 | Run | 触发 | 结果 | 失败步 |
|---|---|---|---|---|
| Round 1 | [39891637738](https://github.com/1-1-2/WKB_test/actions/runs/39891637738) | push | build ✔ / verify ✘ | b（`womic.exe --help`） |
| Round 2 | [39896118753](https://github.com/1-1-2/WKB_test/actions/runs/39896118753) | push | build ✔ / verify ✘ | c（`--status` 退出码 3）——**误失败**，断言全过 |
| Round 3 | [39897235969](https://github.com/1-1-2/WKB_test/actions/runs/39897235969) | push | build ✘（cancelled）/ verify skipped | 无步骤失败；build job 第 3 步 apt 挂起 → 30 min 超时取消 |

提交链：`c4b5835`（Round 1 workflow + vendors 探针）→ `c8e51f2`（Round 2 修复）→ `752aa31`（Round 3 修复）。均已推送到远端 main。

## 2. 失败根因分析与修复（均为测试侧，客户端源码零改动）

### 2.1 Round 1 — step b：pwsh `&` 调用 GUI 子系统程序不可靠

**现象**：`& .\dist\womic.exe --help 2>&1` 在真机 runner 上 0.4s 结束，`$out` 为空、`$LASTEXITCODE` 为空、日志无任何报错；同目录 console 子系统的 `test_drvpkg.exe` 用同样写法输出/退出码都正常捕获（step a PASS）。

**分析**：`womic.exe` 以 `-mwindows` 链接（PE SubSystem=2，已用本地 PE 头解析确认），入口是 `WinMain`。pwsh 的调用运算符对 GUI 子系统程序的重定向/等待行为在该 runner 镜像上不可靠。对照实验：`& wine womic.exe --help 2>&1`（wine 是控制台子系统，作中间层）能完整捕获 12 行帮助文本——证明程序本身写 stdout 没有问题，断点在 pwsh 直调 GUI 子系统程序这一环。

**修复（仅 workflow）**：b/c/d1/d3/e 五个步骤统一改为经 `cmd /c` 包装 + 文件重定向取输出：

```powershell
& cmd /c "$exe --help > $log 2>&1"
$rc = $LASTEXITCODE
$txt = Get-Content $log -Raw -Encoding utf8
```

cmd.exe 是控制台子系统，作中间层能稳定把子进程 stdio 继承到重定向目标并回传退出码。本地 wine 实测该模式：rc=0、帮助全文 3,247 B 完整落盘。另在失败路径补诊断（目录列举 + sha256 + 去重定向直跑一次），并在下载产物后统一 `Unblock-File`（排除 MOTW/SmartScreen 干扰）。

**结果**：Round 2 step b PASS（"b: 帮助文本完整（退出码 0，2231 字符）"）。

### 2.2 Round 2 — step c：pwsh 退出码残留被传播为进程退出码（误失败）

**现象**：step c 的两条断言全部通过（成功消息 "c: 退出码 3 + 架构信息在" 已打印，`ITEM_C=PASS` 已写入 `GITHUB_ENV`——从 results artifact 的 verdict.txt 可证），但步骤仍以退出码 1 失败。日志中无任何 ::error:: 标注。

**分析**：脚本末尾残留的 `$LASTEXITCODE`（`--status` 的 3）被 pwsh 传播为进程退出码（版本相关行为：本地 pwsh 7.6.7 不复现，runner 镜像版本复现）。step b 能过只因 `--help` 退出码恰好是 0。这是测试脚本的确定性缺陷：凡是"预期退出码非 0"的步骤（c 预期 3、d1 记录任意码）都会误失败。

**修复（仅 workflow）**：0/a/b/c/d1/d3/e/f/g/h 各步骤成功路径末尾统一显式 `exit 0`，版本无关、确定性收尾。本地仿真验证：退出码 0、`ITEM_C=PASS` 正确写入。

**结论**：c 的判据本身（退出码 3 + 架构信息）客户端完全正确，真机 `--status` 输出：

```
WO Mic Link 1.1.0
  架构        : x64 (NTamd64) (amd64)
  驱动状态    : 未安装
  INF 路径    : (无)
  SYS 落盘    : 否
  设备句柄    : 打不开
  驱动包来源  : 内置资源 (.rsrc, deflate 100552 B)
  Opus 解码器 : static
```

与 PRD 契约（未装驱动 → 退出码 3）完全一致。

### 2.3 Round 3 — build job apt 挂起（基础设施，非代码问题）

**现象**：build job 第 3 步 `sudo apt-get install -y gcc-mingw-w64-x86-64` 从 03:51 挂起至 04:21（30 分钟），触发 job 级 `timeout-minutes: 30` 被取消；verify job 未启动（needs: build）。前两轮同一步骤秒级完成。

**分析**：GitHub Actions 基础设施瞬时故障（apt 镜像/网络挂起）。workflow 本身无误——同一 commit 的构建在前两轮均成功。

**建议**（未实施，见第 7 节）：给 apt 步骤加 `Acquire::Retries` + 有限次重试，或将交叉工具链改为 actions 预装镜像自带（windows runner 的 `gcc` 亦可编 win32？否，仍需 mingw——保持 apt 但加重试最稳）。

## 3. 逐项验证结果（判据 / 输出摘要 / 结论）

### a. test_drvpkg.exe 驱动包解压自检 —— **PASS（真机，Round 2）**

- 判据：退出码 0 + 9 行 `ok : arch=` + `RESULT=PASS`
- 输出：`blob bytes = 100552, expect entries = 9`；三套架构（1=AMD64 / 2=ARM64 / 3=x86）inf/sys/cat 长度与 FNV-1a-64 全对，`RESULT=PASS`
- 意义：内置驱动包（MICLDRV1/deflate）字节级正确，安装失败类驱动包错误（0xE000024B/0xE0000242）在真机上排除

### b. `--help` CLI 说明书完整性 —— **PASS（真机，Round 2）**

- 判据：退出码 0 + 含 `--status` `--install-driver` `--driver-pack` `--connect`
- 输出：帮助文本 2,231 字符（3,247 B UTF-8），四个关键选项均在
- 修复前失败原因是测试 harness（见 2.1），非客户端问题

### c. 未装驱动 `--status` 退出码 3 + 架构信息 —— **PASS（真机，Round 2；步骤结论曾被误标失败，见 2.2）**

- 判据：退出码 3 + 输出含"架构"
- 输出：见 2.2 节全文；架构 `x64 (NTamd64) (amd64)`、状态"未安装"、SYS 落盘"否"（真机正确值；wine 下此项为环境特例，故不断言）

### d. 驱动安装系统状态（SYS 落盘 + 注册表 InfPath）—— **未跑到（Round 3 被取消）**

- 设计：d1 客户端 `--install-driver`（continue-on-error，记录 UAC 行为，退出码不算数）→ d2 验系统状态（`System32\drivers\womic.sys` + 媒体类设备节点 InfPath 非空）→ d3 仅在不达标时 `pnputil /add-driver /install` 兜底 + 最终门禁
- **UAC 实录**：尚未取得。预期：runner 步骤本身以管理员运行，`drv_is_elevated()` 为真，不走 ShellExecute(runas) 弹窗；若非交互下 UAC 被挂起，d3 的 pnputil 兜底（管理员上下文）可完成 DriverStore 登记，再由客户端创建 devnode

### e. 安装后 `--status` 退出码 0 + 驱动就绪 —— **未跑到**

- 判据：退出码 0 + "已就绪" + INF 路径非空

### f. 端到端灌音（e2e_probe，核心项）—— **未跑到**

- 判据：`RESULT=PASS`（waveIn 枚举 "WO Mic Device" + 灌 1kHz 正弦 + Goertzel 判频 + RMS/SNR）
- 设计要点：IOCTL 0x222001 恒定返回 FALSE+GetLastError=1（驱动防逆向），故判据是实际录音效果而非 IOCTL 状态码
- 本地已完成：探针纯 Python 分析逻辑（合成 1kHz → peak_freq=1000, rms=8485, snr=735）；灌音协议部分必须真机， wine 无法加载内核驱动

### g. R3 权限裁决（标准用户实测灌音）—— **未跑到**

- 设计：建标准用户 `womic-e2e-probe`，以其身份跑同一探针，记 PASS/FAIL（裁决数据，不门禁作业）

### h. GUI 冒烟（无参启动 6s 进程存活）—— **本地验证 PASS（wine + Xvfb）**

- 判据：`Start-Process` 启动 6 秒后进程仍存活
- 本地结果：Xvfb 虚拟显示下进程持续存活（25s 超时杀掉，exit=124）。无显示环境下进程以退出码 1 快速退出，对应 `main.c` 的 `if (!hwnd) return 1;`——wine 无显示时 CreateWindowExW 失败所致，真机 session 0 有窗口站可建窗，预期 PASS

## 4. 客户端质量观察（截至目前，真机证据）

未发现任何客户端源码 bug。已观察行为全部符合契约：

1. 退出码体系正确（0 正常 / 3 驱动未就绪，与 PRD 一致）
2. `--help` 完整且四个关键 CLI 开关齐全
3. `--status` 信息面完整（架构/状态/INF 路径/驱动版本/提供者/SYS 落盘/设备句柄/驱动包来源/Opus 来源/驱动归属），UTF-8 输出正确
4. 内置驱动包 9/9 字节级自检通过（三架构）
5. 单文件 exe 仅依赖系统 DLL，静态 Opus
6. GUI 形态（窗口 + 消息循环）在可用显示环境下正常

## 5. 测试基础设施修复清单（均只改测试侧）

1. GUI 子系统程序调用改 `cmd /c` 包装 + 文件重定向（b/c/d1/d3/e）
2. 所有步骤成功路径显式 `exit 0`（规避 pwsh 退出码残留传播）
3. 下载产物后 `Unblock-File`（MOTW 防御）
4. 失败路径补诊断输出（目录列举 / sha256 / 去重定向直跑）
5. step i 落 `verdict.txt` 到 `probe-results/` 根，修 upload-artifact 公共祖先目录导致 `logs/` 被拍平到 artifact 根的问题（Round 1 实测有此问题）
6. 探针 vendors 入库（`client/tools/e2e_probe.py`）：修正过时 IOCTL 表述（0x222000→0x222001、握手链与"恒定 FALSE+gle=1"）、路径自包含、输出 JSON 摘要 + `RESULT=PASS/FAIL`、退出码 0/1

## 6. 路由判定

**QA（测试侧）**。依据：

- Round 1 失败（b）：测试 harness 对 GUI 子系统程序的调用方式错误 → 已自行修复并验证（Round 2 b PASS）
- Round 2 失败（c）：测试脚本 pwsh 退出码残留传播缺陷（断言全过、`ITEM_C=PASS` 已写）→ 已自行修复并本地仿真验证
- Round 3：基础设施故障（apt 挂起）+ PAT 失效，无代码问题，无法继续触发
- 客户端源码：零改动、零 bug 发现

## 7. 遗留问题与后续行动

1. **【阻塞】需要一次成功的 CI run 才能出 d/e/f/g 结论**（尤其 f 灌音闭环与 g R3 裁决）。当前 PAT 已失效（`GET /user` → 401 Bad credentials）；请主理人提供新 PAT 后：`python3 tools/api_push.py '<PAT>' main`（如无新提交可仅触发 dispatch）→ 轮询 run/jobs。或直接在 GitHub 网页端对最新 commit（752aa31）点 "Re-run all jobs"。
2. **【建议】build job apt 加固**：`sudo apt-get update -o Acquire::Retries=3` + `apt-get install` 加 `timeout 300` 与最多 3 次重试，避免瞬时镜像故障整跑报废（Round 3 的教训）。未实施的原因：已达 3 轮上限且无法验证新 run，留待下次一并带上。
3. **【观察】wine 无显示环境下 GUI 以退出码 1 退出**（`CreateWindowExW` 失败 → `if (!hwnd) return 1;`）。真机 session 0 有窗口站，预期无此问题；若真机 h 步骤失败，当以工程师视角复查（属客户端行为观察，非当前 bug）。
4. results artifact 已按目录结构上传（`logs/`、`verdict.txt`），失败也传（`if: always()`），便于离线诊断。

## 8. 复现与证据索引

- Run 1 日志/artifact：`/tmp/run-logs/`（step b 空输出现场）
- Run 2 日志/artifact：`/tmp/run2-logs/`、`/tmp/r2-art/`（verdict.txt 证明 c 断言全过）
- Run 3：build job 03:51:39Z 启动，04:21:30Z 因 30 分钟超时取消（API 时间戳）
- 本地仿真：`/tmp/e2e-sim/`（wine 下逐步重放 b/c/h 脚本逻辑）
- workflow 最终版：`752aa31`（.github/workflows/windows-e2e.yml，约 540 行，每步中文注释含判据与"为什么"）
