#!/usr/bin/env bash
# 构建脚本：本机与 CI 共用。用法：./scripts/build.sh [gradle 任务...]
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -f local.properties ] && [ -n "${ANDROID_HOME:-}" ]; then
    printf 'sdk.dir=%s\n' "$ANDROID_HOME" > local.properties
fi
TASKS=("$@")
[ ${#TASKS[@]} -eq 0 ] && TASKS=(":app:assembleDebug")
exec ./gradlew --console=plain "${TASKS[@]}"
