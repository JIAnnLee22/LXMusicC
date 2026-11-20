#!/usr/bin/env bash
# CI 安装 Android SDK/NDK 组件（本机已装则自动跳过）。
# 改版本时同步 app/build.gradle.kts、tools/pi-loop.sh、docs/RELEASE.md 与 release.yml。
set -euo pipefail
ANDROID_API="${ANDROID_API:-36}"
BUILD_TOOLS="${BUILD_TOOLS:-36.0.0}"
NDK_VERSION="${NDK_VERSION:-29.0.14206865}"
CMAKE_VERSION="${CMAKE_VERSION:-4.1.2}"

# SDK 位置：环境变量 → GitHub runner 预装路径 → 本机默认路径
SDK_ROOT="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [ -z "$SDK_ROOT" ]; then
    for candidate in /usr/local/lib/android/sdk "$HOME/Android/Sdk"; do
        [ -d "$candidate" ] && SDK_ROOT="$candidate" && break
    done
fi
[ -n "$SDK_ROOT" ] || { echo "找不到 Android SDK：请设置 ANDROID_HOME" >&2; exit 1; }

SDKMANAGER="$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"
if [ ! -x "$SDKMANAGER" ]; then
    SDKMANAGER="$(ls "$SDK_ROOT"/cmdline-tools/*/bin/sdkmanager 2>/dev/null | head -1 || true)"
fi
if [ ! -x "$SDKMANAGER" ]; then
    SDKMANAGER="$(command -v sdkmanager || true)"
fi
[ -n "$SDKMANAGER" ] && [ -x "$SDKMANAGER" ] || {
    echo "找不到 sdkmanager（SDK_ROOT=$SDK_ROOT）" >&2
    ls -la "$SDK_ROOT" 2>/dev/null | head -20 >&2 || true
    exit 1
}

echo "SDK_ROOT=$SDK_ROOT"
echo "sdkmanager=$SDKMANAGER"

# 只安装缺失的组件：本机已装时是空操作，CI 上只补缺口。
NEED=()
[ -d "$SDK_ROOT/platforms/android-$ANDROID_API" ] || NEED+=("platforms;android-$ANDROID_API")
[ -d "$SDK_ROOT/build-tools/$BUILD_TOOLS" ] || NEED+=("build-tools;$BUILD_TOOLS")
[ -d "$SDK_ROOT/ndk/$NDK_VERSION" ] || NEED+=("ndk;$NDK_VERSION")
[ -d "$SDK_ROOT/cmake/$CMAKE_VERSION" ] || NEED+=("cmake;$CMAKE_VERSION")
[ -d "$SDK_ROOT/platform-tools" ] || NEED+=("platform-tools")

if [ ${#NEED[@]} -gt 0 ]; then
    echo "安装缺失组件：${NEED[*]}"
    yes | "$SDKMANAGER" --sdk_root="$SDK_ROOT" --licenses >/dev/null 2>&1 || true
    "$SDKMANAGER" --sdk_root="$SDK_ROOT" --install "${NEED[@]}"
else
    echo "SDK 组件已齐全，无需安装"
fi

MISSING=0
for path in "platform-tools" "platforms/android-$ANDROID_API" "build-tools/$BUILD_TOOLS" "ndk/$NDK_VERSION" "cmake/$CMAKE_VERSION"; do
    if [ ! -d "$SDK_ROOT/$path" ]; then
        echo "缺少 SDK 组件：$path" >&2
        MISSING=1
    fi
done
[ "$MISSING" = 0 ] || exit 1
echo "Android SDK/NDK 就绪"
