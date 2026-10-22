#!/usr/bin/env python3
"""在 Windows runner 上部署"脱离控制台"的计划任务。

为什么必须这么做
----------------
驱动安装（`devcon install` 会触发 PnP 重配置）会让 Windows 重建控制台会话，
我们所在的交互式 SSH PTY 随之被切断——日志停在 `Updating drivers ...` 一行，
重连后脚本从头重放，于是在同一处反复失败。

把执行体换成计划任务后：
  * 任务以 SYSTEM 身份在 Session 0 运行，不继承交互式控制台；
  * 装驱动即便重建 conhost，也杀不到它；
  * 它把每个阶段的完成情况写进 state.jsonl，断了也能续跑；
  * 另配一个"设备安装完成"事件触发器任务，装完自动接着做验证与探针。

用法（在 runner 上，由一次性 SSH 会话调用）：
    python deploy_task.py            # 创建并立即启动主任务
    python deploy_task.py --status   # 只查询状态
    python deploy_task.py --delete   # 删除任务
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

BASE = Path(os.environ.get("WOMIC_WORK", r"D:\a\_temp"))
MAIN_TASK = "WomicStage"
POST_TASK = "WomicStagePost"

XML_HEAD = '''<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>{desc}</Description>
    <URI>\\{name}</URI>
  </RegistrationInfo>
  <Triggers>
{triggers}
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>S-1-5-18</UserId>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings>
      <StopOnIdleEnd>false</StopOnIdleEnd>
      <RestartOnIdle>false</RestartOnIdle>
    </IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT45M</ExecutionTimeLimit>
    <Priority>4</Priority>
    <RestartOnFailure>
      <Interval>PT1M</Interval>
      <Count>3</Count>
    </RestartOnFailure>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>cmd.exe</Command>
      <Arguments>/c "{bat}"</Arguments>
      <WorkingDirectory>{workdir}</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
'''

BOOT_TRIGGER = '    <BootTrigger><Enabled>false</Enabled></BootTrigger>'

# 设备安装完成事件：UserPnp 的 20001/20003，Kernel-PnP 的 400/410 一并监听。
# 这样"驱动装好"这个事实本身会触发后续验证，不再依赖任何在线会话。
PNP_EVENT_TRIGGER = '''    <EventTrigger>
      <Enabled>true</Enabled>
      <Subscription>&lt;QueryList&gt;&lt;Query Id="0" Path="Microsoft-Windows-UserPnp/DeviceInstall"&gt;&lt;Select Path="Microsoft-Windows-UserPnp/DeviceInstall"&gt;*[System[(EventID=20001 or EventID=20003)]]&lt;/Select&gt;&lt;/Query&gt;&lt;Query Id="1" Path="System"&gt;&lt;Select Path="System"&gt;*[System[Provider[@Name='Microsoft-Windows-Kernel-PnP'] and (EventID=400 or EventID=410)]]&lt;/Select&gt;&lt;/Query&gt;&lt;/QueryList&gt;</Subscription>
      <Delay>PT3S</Delay>
    </EventTrigger>'''


def write_bat(path: Path, py: str, args: str, log: str) -> None:
    """写批处理。用 CRLF，避免 cmd 解析异常；路径加引号防空格。"""
    lines = [
        "@echo off",
        "chcp 65001 >nul 2>&1",
        # stdout 重定向到文件时 Windows 用 ANSI 代码页（cp1252），中文日志会崩；
        # 强制 Python 按 UTF-8 输出（agent_stage.py 里也有兜底 reconfigure）。
        "set PYTHONIOENCODING=utf-8",
        f'cd /d "{BASE}"',
        f'echo [%DATE% %TIME%] === stage run {args} === >> "{log}"',
        f'"{py}" "{BASE / "agent_stage.py"}" {args} >> "{log}" 2>&1',
        f'echo [%DATE% %TIME%] === exit %ERRORLEVEL% === >> "{log}"',
    ]
    path.write_text("\r\n".join(lines) + "\r\n", encoding="utf-8", newline="")


def install_task(name: str, desc: str, bat: Path, triggers: str) -> tuple[bool, str]:
    xml = XML_HEAD.format(desc=desc, name=name, triggers=triggers,
                          bat=str(bat), workdir=str(BASE))
    xp = BASE / f"{name}.xml"
    xp.write_text(xml, encoding="utf-16")      # schtasks 要求 Unicode
    r = subprocess.run(["schtasks", "/create", "/tn", name, "/xml", str(xp), "/f"],
                       capture_output=True, text=True, errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    return r.returncode == 0, out.strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--delete", action="store_true")
    ap.add_argument("--no-event", action="store_true", help="不装事件触发任务")
    a = ap.parse_args()

    if a.status:
        for t in (MAIN_TASK, POST_TASK):
            r = subprocess.run(["schtasks", "/query", "/tn", t, "/fo", "LIST", "/v"],
                               capture_output=True, text=True, errors="replace")
            print(f"--- {t} rc={r.returncode}")
            print((r.stdout or r.stderr).strip()[:800])
        st = BASE / "state.jsonl"
        print("--- state.jsonl:")
        print(st.read_text(encoding="utf-8", errors="replace") if st.exists() else "(空)")
        return 0

    if a.delete:
        for t in (MAIN_TASK, POST_TASK):
            subprocess.run(["schtasks", "/delete", "/tn", t, "/f"], capture_output=True)
        print("任务已删除")
        return 0

    py = sys.executable
    main_bat = BASE / "stage_main.bat"
    write_bat(main_bat, py, "--auto", "stage.log")
    ok, out = install_task(MAIN_TASK, "WOMic 驱动安装与探针（主任务，脱离控制台）",
                           main_bat, BOOT_TRIGGER)
    print(f"[main] created={ok}\n{out}")
    if not ok:
        return 1

    if not a.no_event:
        post_bat = BASE / "stage_post.bat"
        write_bat(post_bat, py, "--stage verify_device", "stage.log")
        ok2, out2 = install_task(POST_TASK, "WO Mic 设备安装完成后自动验证（事件触发）",
                                 post_bat, PNP_EVENT_TRIGGER)
        print(f"[post] created={ok2}\n{out2}")

    r = subprocess.run(["schtasks", "/run", "/tn", MAIN_TASK],
                       capture_output=True, text=True, errors="replace")
    print(f"[run] rc={r.returncode} {(r.stdout or r.stderr).strip()}")
    return 0 if r.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
