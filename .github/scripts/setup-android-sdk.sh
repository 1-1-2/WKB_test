#!/usr/bin/env bash
# 定位 Android SDK 并把工具链加入 PATH。
#
# 背景：ubuntu-latest 镜像不再把 cmdline-tools 放进 PATH（实测 `sdkmanager:
# command not found`，见 run 39408485677），而 GitHub 托管 runner 的 SDK 布局
# 随镜像版本变化，所以这里做三件事：
#   1) 依次探测 ANDROID_HOME / ANDROID_SDK_ROOT / 常见安装路径
#   2) 取 cmdline-tools/*/bin 中版本最高的一个（sort -V）
#   3) 确实没有 cmdline-tools 时，下载官方 commandlinetools zip 补齐
#
# 本地复跑：ANDROID_HOME=/opt/android-sdk bash setup-android-sdk.sh

set -euo pipefail

SDK=""
for cand in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" \
            /usr/local/lib/android/sdk /opt/android-sdk "$HOME/Android/Sdk"; do
    if [ -n "$cand" ] && [ -d "$cand" ]; then SDK="$cand"; break; fi
done
SDK="${SDK:-/usr/local/lib/android/sdk}"
echo "[sdk] 使用 SDK: $SDK"

TOOLS="$(ls -d "$SDK"/cmdline-tools/*/bin 2>/dev/null | sort -V | tail -1 || true)"
if [ -z "$TOOLS" ]; then
    echo "[sdk] 未找到 cmdline-tools，下载官方 commandlinetools"
    mkdir -p "$SDK/cmdline-tools"
    curl -fsSL --retry 3 -o /tmp/cmdline-tools.zip \
        https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
    unzip -q /tmp/cmdline-tools.zip -d "$SDK/cmdline-tools"
    mv "$SDK/cmdline-tools/cmdline-tools" "$SDK/cmdline-tools/latest"
    TOOLS="$SDK/cmdline-tools/latest/bin"
fi
echo "[sdk] cmdline-tools: $TOOLS"

if [ -n "${GITHUB_ENV:-}" ]; then
    {
        echo "ANDROID_HOME=$SDK"
        echo "ANDROID_SDK_ROOT=$SDK"
    } >> "$GITHUB_ENV"
fi
if [ -n "${GITHUB_PATH:-}" ]; then
    {
        echo "$TOOLS"
        echo "$SDK/platform-tools"
        echo "$SDK/emulator"
    } >> "$GITHUB_PATH"
fi

echo "[sdk] sdkmanager --version: $("$TOOLS/sdkmanager" --version 2>&1 | head -1)"
echo "[sdk] 已装组件（前 20 项）:"
ls "$SDK" | head -20
