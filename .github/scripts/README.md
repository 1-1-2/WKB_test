# .github/scripts —— CI 用的 shell 脚本

这些脚本被 `.github/workflows/android.yml` 调用。抽出来的原因：**这些逻辑的失败
模式是"挂死"而不是"报错"**，放在 YAML 里既没法单元测试，也很难本地复跑。

| 脚本 | 作用 | 关键约束 |
|---|---|---|
| `setup-android-sdk.sh` | 定位 Android SDK，把 cmdline-tools/platform-tools/emulator 写入 `GITHUB_PATH`；镜像缺 cmdline-tools 时下载官方包 | 不依赖镜像的 PATH 布局 |
| `fetch-opus.sh` | 下载 libopus 源码到 `android/app/src/main/cpp/opus/` | 幂等（已存在则跳过）、`OPUS_URL` 可覆盖、结构与版本校验 |
| `verify-kvm.sh` | 模拟器前置检查：`/dev/kvm`、emulator 二进制、系统镜像、`-accel-check` | 只有"确定跑不了"才 fail fast，其余只 warning |
| `start-emulator.sh` | 创建 AVD 并启动模拟器，等到 `sys.boot_completed=1` | **绝不无超时等待**；每 10s 打一行进度；失败打印 emulator.log |
| `install-app.sh` | 安装 APK（`-g` 授予运行时权限）并以调试音模式启动服务 | 每步套 `timeout`，装完打印 pid/服务记录/端口/logcat |
| `collect-diagnostics.sh` | 收集环境、logcat、dumpsys、emulator.log 到 `/tmp/diag` | 每条 adb 命令 15s 上限（自身绝不能成为新的挂起点） |
| `watch-ci-run.sh` | 外部看门狗：按步骤预算判断是否卡死，超预算主动 `cancel` | 步骤级实时进度（GitHub 无运行中行级日志 API） |
| `tests/run-tests.sh` | 上面脚本的单元测试 | stub 的 adb/emulator/avdmanager，不需要设备 |

## 跑单元测试

```bash
bash .github/scripts/tests/run-tests.sh
```

覆盖的是**挂死与误判**这两类缺陷（而不是"happy path"）：

| 用例 | 验证的行为 |
|---|---|
| T01/T02 | SDK 定位成功；缺 cmdline-tools 且下载失败时**必须非 0**，不能静默通过 |
| T03 | `/dev/kvm` 不存在 → 1 秒级失败并给 `::error::` |
| T04/T05 | accel 可用 → 通过；accel 明确不可用 → **立刻失败**（不空耗 8 分钟） |
| T06 | 模拟器进程立刻崩溃 → 20s 内失败，且打印 emulator 自己的报错 |
| T07 | 进程活着但永不 boot → 到预算就退出（不会挂住） |
| T08 | 正常 boot → 通过 |
| T09 | AVD 不存在时确实调用了 `avdmanager create` |
| T10 | adb 全部挂死时，诊断脚本仍在 60s 内结束并记录"超时" |
| T11–T13 | opus 幂等跳过 / 强制重下 / 下载失败必须报错 |

CI 里由 `script-tests` 作业跑这套用例（约 10 秒）。

## 看门狗用法

```bash
GH_TOKEN=... bash .github/scripts/watch-ci-run.sh              # 分支最新 run
GH_TOKEN=... bash .github/scripts/watch-ci-run.sh 39453084847  # 指定 run
```

预算表写在脚本里的 `BUDGET`（分钟）：例如 `Start emulator` 12 分钟、
`Install Android SDK components` 12 分钟。实际耗时超过 `预算 × GRACE`（默认 1.6）
就判定卡死，打印卡住的步骤与耗时后调用 `cancel`。

## 为什么要在 workflow 之外再做一层超时

已发生的两次事故：

| run | 现象 | 根因 |
|---|---|---|
| 39422464400 | `Start emulator` 卡 25m38s，被人工取消 | 用了无超时的 `adb wait-for-device`，而模拟器起不来 |
| 39453084847 | `Collect diagnostics` 卡 25m+，取消也不退 | 诊断脚本里的 adb 命令没有 `timeout`，子进程阻塞了 runner |

结论：**凡是等待外部状态的地方都必须有上限**（workflow 的 `timeout-minutes` +
脚本内的 `timeout` + 外部看门狗，三层）。单靠 GitHub 的 job timeout 不够——
它只在超时那一刻介入，中间整段时间你都看不出"它已经不可能自己好了"。
