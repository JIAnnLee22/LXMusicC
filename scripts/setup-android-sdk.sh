#!/usr/bin/env bash
# CI 安装 Android SDK/NDK 组件（本机已装则自动跳过）。
# 改版本时同步 app/build.gradle.kts（compileSdk / targetSdk / ndkVersion）。
set -euo pipefail
ANDROID_API="${ANDROID_API:-36}"
BUILD_TOOLS="${BUILD_TOOLS:-36.0.0}"
NDK_VERSION="${NDK_VERSION:-29.0.14206865}"
CMAKE_VERSION="${CMAKE_VERSION:-4.1.2}"

SDK_ROOT="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
SDKMANAGER="$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"
if [ ! -x "$SDKMANAGER" ]; then
    SDKMANAGER="$(command -v sdkmanager || true)"
fi
[ -n "$SDKMANAGER" ] || { echo "找不到 sdkmanager（先安装 Android cmdline-tools）" >&2; exit 1; }

yes | "$SDKMANAGER" --licenses >/dev/null 2>&1 || true
"$SDKMANAGER" --install "platform-tools" "platforms;android-$ANDROID_API" \
    "build-tools;$BUILD_TOOLS" "ndk;$NDK_VERSION" "cmake;$CMAKE_VERSION"
