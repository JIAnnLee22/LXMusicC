#!/usr/bin/env bash
# pi-loop.sh — 反复启动 Pi 编码代理，按 PLAN.md 的顺序完成一个 Android 音乐播放器项目。
#
# 每一轮（= 一个功能集）：用 `pi -p` 非交互执行 → 脚本自己再构建验证 →
# 校验 PLAN.md 勾选与 git 提交 → push。push 分支本身不会触发任何 GitHub 构建。
#
# 发布节奏：每 MILESTONE_HOURS（默认 6）小时打一个 `snapshot-*` 标签并推送，
# 只有 tag 推送才会触发 GitHub Actions 构建并发布 Release（附 APK）。
# 项目全部完成时打 `v1.0.0` 正式标签，同样由 CI 构建发布。平时提交不产出安装包。
set -Eeuo pipefail

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
PROJECT_DIR="${PROJECT_DIR:-$(cd "$(dirname "${SCRIPT_PATH}")/.." && pwd)}"
REFERENCE_DIR="${REFERENCE_DIR:-$(cd "$PROJECT_DIR/.." 2>/dev/null && pwd)/CUIDemo}"
APP_NAME="LXMusicC"
APP_PACKAGE="jiannlee22.lxmusicc"
APP_LIB="lxmusicc"
SDK_API="36"
BUILD_TOOLS="36.0.0"
NDK_VERSION="29.0.14206865"
CMAKE_VERSION="4.1.2"

RUNS_MAX=80
TIMEOUT_SECONDS=4200           # 单轮上限 70 分钟
ATTEMPTS_PER_ITEM=2            # 同一功能集最多连续尝试次数
MILESTONE_HOURS=6              # 每 N 小时打一个快照版本
MILESTONE_NOW=0                # 启动后立刻打一次快照（调试用）
MILESTONE_ONLY=0               # 只打一次快照然后退出
MODEL=""                       # 空 = pi 默认模型
THINKING="high"
PUSH=1
VERIFY_CMD=""
DRY_RUN=0
ONCE=0
BOOTSTRAP_ONLY=0
SKIP_BOOTSTRAP=0
REMOTE_URL=""
GIT_NAME=""
GIT_EMAIL=""
LOG_DIR=""
PUSH_WARNED=0

usage() {
    cat <<'EOF'
pi-loop.sh — 用 Pi 编码代理循环开发本项目（进度见 PLAN.md）

用法：
  tools/pi-loop.sh --bootstrap-only        只生成工程骨架/CI/文档并首次提交
  tools/pi-loop.sh --dry-run               打印计划与提示词，不调用模型
  tools/pi-loop.sh                         一直跑到 PLAN.md 全部勾选
  tools/pi-loop.sh --once                  只做一个功能集（调试用）
  tools/pi-loop.sh --remote git@github.com:YOU/LXMusicC.git

选项：
  --project DIR        项目目录（默认：脚本所在仓库根目录）
  --reference DIR      参考项目（默认 ../CUIDemo，用来复制 Gradle wrapper）
  --remote URL         设置 git origin（首次运行会配置并推送）
  --git-name NAME      提交者名字（默认沿用参考仓库的 git 配置）
  --git-email EMAIL    提交者邮箱（默认沿用参考仓库的 git 配置）
  --model MODEL        pi 使用的模型（默认使用 pi 配置）
  --thinking LEVEL     思考等级 off/minimal/low/medium/high/xhigh（默认 high）
  --timeout SECONDS    单轮超时（默认 4200）
  --max-runs N         总轮数上限（默认 80）
  --attempts N         同一功能集最多尝试次数（默认 2）
  --milestone-hours H  每 H 小时打一个 snapshot 快照（默认 6，0 = 关闭）
  --milestone-now      本轮开始前立刻打一次快照（调试用）
  --milestone-only     只生成一个快照版本并推送标签，然后退出
  --verify CMD         每轮结束后的验证命令（默认 ./scripts/build.sh）
  --no-push            只提交本地，不 push
  --once               只跑一个功能集后退出
  --dry-run            只打印计划与提示词
  --bootstrap-only     只生成骨架与文档，然后退出
  --skip-bootstrap     已有工程，跳过骨架生成
  -h, --help           显示帮助
EOF
}

log()  { printf '\033[1;36m[%s]\033[0m %s\n' "$(date +%H:%M:%S)" "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --project) PROJECT_DIR="$2"; shift 2 ;;
        --reference) REFERENCE_DIR="$2"; shift 2 ;;
        --remote) REMOTE_URL="$2"; shift 2 ;;
        --git-name) GIT_NAME="$2"; shift 2 ;;
        --git-email) GIT_EMAIL="$2"; shift 2 ;;
        --model) MODEL="$2"; shift 2 ;;
        --thinking) THINKING="$2"; shift 2 ;;
        --timeout) TIMEOUT_SECONDS="$2"; shift 2 ;;
        --max-runs) RUNS_MAX="$2"; shift 2 ;;
        --attempts) ATTEMPTS_PER_ITEM="$2"; shift 2 ;;
        --milestone-hours) MILESTONE_HOURS="$2"; shift 2 ;;
        --milestone-now) MILESTONE_NOW=1; shift ;;
        --milestone-only) MILESTONE_ONLY=1; shift ;;
        --verify) VERIFY_CMD="$2"; shift 2 ;;
        --no-push) PUSH=0; shift ;;
        --once) ONCE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --bootstrap-only) BOOTSTRAP_ONLY=1; shift ;;
        --skip-bootstrap) SKIP_BOOTSTRAP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "未知参数：$1（--help 查看用法）" ;;
    esac
done

PROJECT_DIR="$(cd "$PROJECT_DIR" 2>/dev/null && pwd || true)"
[ -n "$PROJECT_DIR" ] || die "项目目录不存在"
LOG_DIR="$PROJECT_DIR/logs"
STATE_DIR="$PROJECT_DIR/.pi-loop"
command -v pi >/dev/null || die "找不到 pi 命令"
command -v flock >/dev/null || die "找不到 flock（util-linux）"

# ---------------------------------------------------------------- 骨架生成

# write_file <相对路径> [--exec]  内容从 stdin；已存在则跳过（可重复运行）
write_file() {
    local path="$PROJECT_DIR/$1" exec="${2:-}"
    if [ -e "$path" ]; then
        warn "已存在，跳过：$1"
        return 0
    fi
    mkdir -p "$(dirname "$path")"
    cat > "$path"
    if [ "$exec" = "--exec" ]; then
        chmod +x "$path"
    fi
    log "写入 $1"
}

bootstrap_wrapper() {
    if [ -f "$PROJECT_DIR/gradlew" ]; then
        return 0
    fi
    if [ -f "$REFERENCE_DIR/gradle/wrapper/gradle-wrapper.jar" ]; then
        cp "$REFERENCE_DIR/gradlew" "$PROJECT_DIR/gradlew"
        cp "$REFERENCE_DIR/gradlew.bat" "$PROJECT_DIR/gradlew.bat"
        chmod +x "$PROJECT_DIR/gradlew"
        mkdir -p "$PROJECT_DIR/gradle/wrapper"
        cp "$REFERENCE_DIR/gradle/wrapper/gradle-wrapper.jar" "$PROJECT_DIR/gradle/wrapper/"
        log "从 $REFERENCE_DIR 复制 Gradle wrapper"
    else
        warn "参考项目里没有 Gradle wrapper：$REFERENCE_DIR"
        warn "可以手动执行 gradle wrapper --gradle-version 9.6.0 再重跑本脚本"
    fi
    write_file gradle/wrapper/gradle-wrapper.properties <<'EOF'
distributionBase=GRADLE_USER_HOME
distributionPath=wrapper/dists
distributionUrl=https\://services.gradle.org/distributions/gradle-9.6.0-bin.zip
networkTimeout=10000
validateDistributionUrl=true
zipStoreBase=GRADLE_USER_HOME
zipStorePath=wrapper/dists
EOF
}

bootstrap_project() {
    log "生成工程骨架到 $PROJECT_DIR"
    mkdir -p "$LOG_DIR" "$STATE_DIR"

    write_file .gitignore <<'EOF'
*.iml
.gradle
/local.properties
/.idea
.DS_Store
build/
/captures
.externalNativeBuild
.cxx
local.properties
logs/
.pi-loop/
*.keystore
*.jks
EOF

    write_file README.md <<'EOF'
# LXMusicC

一个轻量的 Android 音乐播放器：搜索、在线播放、离线缓存、后台播放，兼容 LX Music 自定义音源。

## 功能

- **多音源搜索**：一次搜索并发查询所有启用的音源，结果合并去重，显示歌名、歌手、时长与音质
- **在线播放**：点开即播，可拖动进度、切上下首、查看播放队列、随机播放与单曲循环
- **离线缓存**：歌曲缓存到系统公共音乐目录（`Music/LXMusicC/<音源>/<歌手> - <歌名>.<扩展名>`），
  文件管理器和其它播放器都能直接看到；缓存可查看、清理、限制总大小
- **断网可用**：已缓存的歌曲没有网络也能播放
- **后台播放**：前台服务 + 通知栏控制（播放/暂停/上下首/进度），锁屏和耳机按键可控；
  来电或其它应用抢占音频时自动暂停，回到 App 继续
- **音源管理**：导入 LX Music 自定义音源（粘贴链接、JSON 或文件），启用、排序、删除、校验
- **我的音乐**：收藏、最近播放、自建歌单，全部本地保存
- **设置**：音源管理、缓存上限、仅 Wi-Fi 下载、主题色
- **界面**：单手可用的深色界面，竖屏/横屏自适应，Android 8.0 及以上

## 支持的格式

mp3 / flac / m4a(aac) / ogg / opus / wav —— 由音源提供的格式与系统解码能力决定。

## 安装

1. 打开 [Releases](../../releases)，下载最新的 `lxmusicc-*.apk`
2. 允许“安装未知应用”，完成安装

版本节奏：每个 `snapshot-*` 是约每 6 小时的进度快照（预发布），`v1.x` 才是正式版。
平时的提交不会产生安装包。

## 使用

1. **设置 → 音源**：导入 LX 音源（链接或音源文件内容），启用需要的音源
2. **搜索**：输入歌名/歌手，点搜索结果即可播放
3. **缓存**：播放页可把当前歌曲缓存到本地；缓存管理里能看到占用并清理
4. 缓存目录：`内部存储/Music/LXMusicC/<音源>/<歌手> - <歌名>.<扩展名>`

## 常见问题

- **搜不到歌**：音源失效或被限制，换一个音源，或更新音源脚本
- **播放失败**：部分音源需要特定的 UA/Referer，或者音频链接已过期
- **缓存失败**：检查存储空间与“仅 Wi-Fi 下载”设置
- **没有声音**：确认没有其它应用占用音频焦点，或在设置里切换输出设备

## 开发与发布

- 构建：`./scripts/build.sh`
- 自动化：`tools/pi-loop.sh` 按 `PLAN.md` 逐条实现；每 6 小时打 `snapshot-*` 标签，
  GitHub Actions 只在收到标签时构建并发 Release（平时 push 不产出安装包）
- 结构与桥接说明见 `docs/`，功能计划见 `PLAN.md`

## 致谢

LX 音源格式来自 LX Music 社区。本项目界面与播放内核以 C 实现，仅在系统强制要求处使用极少量 Kotlin 桥接。
EOF

    write_file AGENTS.md <<'EOF'
# 自动化开发规则（Pi 循环）

本仓库由 `tools/pi-loop.sh` 反复驱动 Pi 编码代理推进，进度以 `PLAN.md` 为准。

## 目标

做成能日常使用的 Android 音乐播放器：LX 音源 → 搜索 → 播放 → 缓存 → 后台播放 → 库/歌单。

## 技术边界

- 界面、渲染、交互、音源解析、缓存索引、播放状态机：**C**（NDK，NativeActivity + EGL/GLES）。
  纯 C UI 的写法可以参考同级仓库 `../CUIDemo`（EGL 封装、SDF 文字、批次渲染、damage 局部重绘）。
- **Kotlin 只允许**出现在 Android 框架绕不开的地方：前台服务/MediaSession/通知、MediaStore 公共目录写入、
  系统 HTTP(S) 客户端、必须的 JS 引擎封装。每个 Kotlin 文件 ≤ 400 行、不写 UI、业务逻辑留在 C。
- 所有 JNI 接口集中登记到 `docs/BRIDGE.md`（函数名、签名、线程、返回值）。C 侧调用要检查异常与返回值。
- 不允许新增 Java 源码。
- 第三方库优先 vendor 单文件/小体量 C 库（stb、miniz、sqlite3、QuickJS/duktape 等），
  附 LICENSE 与来源说明；不要引入需要 Gradle 原生依赖的大型 SDK。

## 每轮交付（硬性）

1. `./scripts/build.sh` 必须成功（CI 用同一份脚本）。
2. 完成 `PLAN.md` 当前条目的验收项，并把该条的 `- [ ]` 改成 `- [x]`（只勾这一条）。
3. `README.md` 只更新功能/使用层面的内容；架构与实现细节写进 `docs/ARCHITECTURE.md`。
4. 一个功能集一个提交：`git add -A && git commit -m "<条目ID>: <一句话>"`，然后 `git push origin HEAD`。
5. 不要给 workflow 增加 `push: branches` 触发，也不要自己打 tag 或建 Release：
   只有 `tools/pi-loop.sh` 会在每 6 小时的里程碑和项目完成时打 `snapshot-*` / `v1.0.0` 标签，
   由标签触发 CI 构建发布。平时提交要保持“没有安装包产出”。

## 工程质量

- 单文件 ≤ 800 行；公共接口写在头文件；复杂逻辑写注释（中文即可）。
- 不在渲染/主线程做网络或磁盘 IO；用后台线程 + 队列，回调时加锁。
- 列表用分页/游标，避免一次性加载全部数据；大缓冲区及时释放。
- 网络、解析、播放失败都要可重试且不崩溃；日志统一 `adb logcat -s LXMusicC`。
- 无网络、无音源、无缓存、空搜索结果都要有空状态。

## 交互

- 不向用户提问、不等待确认；不确定就选最简单能用的方案，并记进 `docs/DECISIONS.md`。
- 不做与本轮条目无关的重构；不要删除之前轮次已实现的功能。

## 提交与发布

- 默认分支 `main`；禁止 force-push、禁止改写历史。
- 提交信息：`<条目ID>: <一句话>`，例如 `04-search: 多源并发搜索与结果列表`。
- `versionName` 在 `10-release` 之前保持 `0.x`；正式发布由 `pi-loop.sh` 打 `v1.0.0` 标签完成。
EOF

    write_file PLAN.md <<'EOF'
# 功能计划

`tools/pi-loop.sh` 每次取**第一个未勾选**的条目交给 Pi；完成后把 `- [ ]` 改成 `- [x]` 再提交。
标签与 Release 由脚本统一管理（每 6 小时一个 `snapshot-*`，完成时 `v1.0.0`）。

## 功能集

- [ ] `01-ui-shell` 纯 C 界面框架与页面骨架
  - 用 C 重写骨架里的占位渲染：EGL/GLES 初始化、帧循环、文字渲染（SDF 图集）、圆角卡片、列表滚动
  - 底部四个页签：歌曲 / 搜索 / 正在播放 / 设置，可切换并保持状态
  - 深色主题 + 强调色；竖屏/横屏都能正常布局；返回键处理
  - 验收：`scripts/build.sh` 通过；启动后能看到完整界面，页签切换不闪、不崩

- [ ] `02-lx-source-import` LX 音源导入与元数据解析
  - 支持粘贴音源链接/JSON/文件内容导入；纯 C 解析 LX 音源脚本的注释元数据（name/description/version/author/homepage）
  - 音源列表：启用/禁用、删除、排序；导入失败给出可读错误
  - 音源持久化到应用私有目录；重启后仍在
  - 验收：能导入至少一个真实 LX 音源并看到元数据；非法输入不崩

- [ ] `03-source-runtime` 音源请求执行（JS + HTTP）
  - 引入纯 C JS 引擎（QuickJS 或 duktape，vendor 源码）执行 LX 音源脚本的 `musicUrl`/`lyric`/`search` 等 handler
  - 实现最小 LX 兼容层（`lx.request`、实际用到的 utils）；网络请求由桥接完成
  - 若 1 轮内跑不通 JS，则先支持“声明式 URL 模板”类音源，并把 JS 执行拆成后续条目写入 `docs/DECISIONS.md`
  - 验收：对示例音源能发起真实搜索并解析出歌曲数组（含歌名/歌手/专辑/时长/音质/下载地址）

- [ ] `04-search` 多源搜索界面
  - 搜索页：输入框、并发查询多个启用音源、聚合去重排序、加载/空/错误状态、可取消
  - 结果项显示歌名/歌手/时长/音质；点击进入播放
  - 验收：真实网络下能搜出结果；弱网/断网有错误提示且可重试

- [ ] `05-playback-service` 前台播放服务与通知
  - 最小 Kotlin 桥：前台服务 + MediaSession（Media3）+ 通知栏控制 + 音频焦点 + 耳机按键
  - C 侧控制：播放/暂停/上下首/seek/播放列表替换，接收进度与状态回调
  - 验收：切后台/锁屏不断音，通知按钮有效，来电暂停后可恢复

- [ ] `06-cache` 公共目录缓存与离线播放
  - 下载音频到公共目录 `Music/LXMusicC/<音源>/<歌手> - <歌名>.<ext>`（MediaStore 写入走桥接）
  - 缓存索引与查找：命中缓存直接离线播放；同一首歌不重复下载
  - 缓存管理页：列表、占用统计、单条删除、清空、上限设置
  - 验收：文件管理器的 Music 下能看到下载的文件；断网能播放已缓存歌曲

- [ ] `07-player-ui` 播放页与进度
  - 封面下载与缓存、进度条拖动 seek、时间显示、播放队列、随机/循环、音量/输出跟随系统
  - 播放页动画与手势（上滑展开队列、左右切歌可选）
  - 验收：seek 准确，切歌不留旧状态，封面缺失有占位

- [ ] `08-library` 收藏 / 历史 / 歌单
  - 收藏、最近播放、自建歌单（创建/重命名/增删歌曲）；库页浏览与本地搜索
  - 数据持久化（C 侧文件或 sqlite3），重启后保留
  - 验收：重启后数据不丢；删除/去重正确

- [ ] `09-settings-polish` 设置与打磨
  - 设置页：音源管理入口、缓存上限、仅 Wi-Fi 下载、主题色、关于
  - 空状态/骨架屏/错误重试；长列表滚动性能；横竖屏与安全区适配
  - 验收：所有页面无裁切、无卡死；连续操作 5 分钟不崩溃、内存平稳

- [ ] `10-release` 发布 1.0.0
  - 版本号 `1.0.0`、README 与 CHANGELOG 定稿（只写功能与使用）
  - 确认 `docs/MILESTONE-LATEST.md` 与 `scripts/setup-android-sdk.sh` 的一致性
  - 不要自己打 tag：`pi-loop.sh` 在全部条目完成后会打 `v1.0.0` 并触发 CI 发布
  - 验收：正式 Release 能下载安装包并正常启动
EOF

    write_file docs/ARCHITECTURE.md <<'EOF'
# 架构说明

（每轮实现时补充：模块划分、线程模型、数据流、缓存目录约定、错误处理策略。）
EOF
    write_file docs/BRIDGE.md <<'EOF'
# JNI 桥接契约

C（NativeActivity）与 Kotlin（系统接口）之间只通过这里登记的函数通信。新增/修改接口时同步更新本表。

| C 侧函数 | Kotlin 实现 | 签名 | 线程 | 说明 |
| --- | --- | --- | --- | --- |
| （待填） | | | | |
EOF
    write_file docs/DECISIONS.md <<'EOF'
# 决策记录

每轮把“不确定但会影响后续”的选择记在这里：做了什么、为什么、后续要补什么。
EOF
    write_file docs/MILESTONE-LATEST.md <<'EOF'
# 最新里程碑

工程骨架刚刚建立，等待第一个 6 小时快照。
EOF
    write_file docs/MILESTONES.md <<'EOF'
# 里程碑历史

EOF

    write_file scripts/build.sh "--exec" <<'EOF'
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
EOF

    write_file scripts/setup-android-sdk.sh "--exec" <<EOF
#!/usr/bin/env bash
# CI 安装 Android SDK/NDK 组件（本机已装则自动跳过）。
# 改版本时同步 app/build.gradle.kts（compileSdk / targetSdk / ndkVersion）。
set -euo pipefail
ANDROID_API="\${ANDROID_API:-$SDK_API}"
BUILD_TOOLS="\${BUILD_TOOLS:-$BUILD_TOOLS}"
NDK_VERSION="\${NDK_VERSION:-$NDK_VERSION}"
CMAKE_VERSION="\${CMAKE_VERSION:-$CMAKE_VERSION}"

SDK_ROOT="\${ANDROID_HOME:-\${ANDROID_SDK_ROOT:-\$HOME/Android/Sdk}}"
SDKMANAGER="\$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"
if [ ! -x "\$SDKMANAGER" ]; then
    SDKMANAGER="\$(command -v sdkmanager || true)"
fi
[ -n "\$SDKMANAGER" ] || { echo "找不到 sdkmanager（先安装 Android cmdline-tools）" >&2; exit 1; }

yes | "\$SDKMANAGER" --licenses >/dev/null 2>&1 || true
"\$SDKMANAGER" --install "platform-tools" "platforms;android-\$ANDROID_API" \\
    "build-tools;\$BUILD_TOOLS" "ndk;\$NDK_VERSION" "cmake;\$CMAKE_VERSION"
EOF

    # 平时提交只跑这个（无产物上传），且不监听 push 分支。
    write_file .github/workflows/ci.yml <<'EOF'
# 只做编译检查：PR 或手动触发。push 分支不触发，也没有任何产物上传。
name: CI (build only)

on:
  pull_request:
  workflow_dispatch:

permissions:
  contents: read

jobs:
  build:
    runs-on: ubuntu-latest
    timeout-minutes: 45
    steps:
      - uses: actions/checkout@v4

      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: '21'

      - uses: android-actions/setup-android@v3

      - name: Install SDK / NDK packages
        run: bash scripts/setup-android-sdk.sh

      - uses: gradle/actions/setup-gradle@v4

      - name: Build debug APK
        run: ./gradlew --console=plain --no-daemon :app:assembleDebug
EOF

    # 只有里程碑标签会构建并发布 Release（附件 = APK）。
    write_file .github/workflows/release.yml <<'EOF'
# 只有标签推送才构建并发布：snapshot-* 为每 6 小时的进度快照，v* 为正式版。
name: Release (milestone)

on:
  push:
    tags:
      - 'snapshot-*'
      - 'v*'
  workflow_dispatch:

permissions:
  contents: write

concurrency:
  group: release-${{ github.ref }}
  cancel-in-progress: false

jobs:
  release:
    runs-on: ubuntu-latest
    timeout-minutes: 60
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0

      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: '21'

      - uses: android-actions/setup-android@v3

      - name: Install SDK / NDK packages
        run: bash scripts/setup-android-sdk.sh

      - uses: gradle/actions/setup-gradle@v4

      - name: Build debug APK
        run: ./gradlew --console=plain --no-daemon :app:assembleDebug

      - name: Build release APK (unsigned until signing is configured)
        continue-on-error: true
        run: ./gradlew --console=plain --no-daemon :app:assembleRelease

      - name: Collect artifacts
        run: |
          mkdir -p dist
          cp app/build/outputs/apk/debug/app-debug.apk dist/lxmusicc-debug.apk
          shopt -s nullglob
          for f in app/build/outputs/apk/release/*.apk; do
            cp "$f" "dist/lxmusicc-$(basename "$f" | sed 's/^app-//')"
          done
          ls -l dist

      - name: Publish release
        if: startsWith(github.ref, 'refs/tags/')
        uses: softprops/action-gh-release@v2
        with:
          tag_name: ${{ github.ref_name }}
          name: ${{ github.ref_name }}
          prerelease: ${{ startsWith(github.ref_name, 'snapshot-') }}
          body_path: docs/MILESTONE-LATEST.md
          files: dist/*.apk
EOF

    write_file settings.gradle.kts <<'EOF'
pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "LXMusicC"
include(":app")
EOF

    write_file build.gradle.kts <<'EOF'
// 顶层构建文件：插件版本统一在 gradle/libs.versions.toml 里。
plugins {
    alias(libs.plugins.android.application) apply false
}
EOF

    write_file gradle.properties <<'EOF'
org.gradle.jvmargs=-Xmx2048m -Dfile.encoding=UTF-8
org.gradle.configuration-cache=true
android.useAndroidX=true
EOF

    write_file gradle/libs.versions.toml <<'EOF'
[versions]
agp = "9.4.1"

[plugins]
android-application = { id = "com.android.application", version.ref = "agp" }
EOF

    write_file app/build.gradle.kts <<EOF
plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "$APP_PACKAGE"
    compileSdk {
        version = release($SDK_API)
    }

    defaultConfig {
        applicationId = "$APP_PACKAGE"
        minSdk = 26
        targetSdk = $SDK_API
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // 真机（arm64-v8a）与模拟器（x86_64）各一个 ABI。
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=none")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "$CMAKE_VERSION"
        }
    }

    ndkVersion = "$NDK_VERSION"
    buildToolsVersion = "$BUILD_TOOLS"
}
EOF

    write_file app/src/main/AndroidManifest.xml <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android">

    <uses-feature
        android:glEsVersion="0x00030000"
        android:required="true" />

    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />

    <application
        android:allowBackup="false"
        android:hasCode="false"
        android:label="@string/app_name"
        android:supportsRtl="false"
        android:theme="@android:style/Theme.NoTitleBar.Fullscreen">

        <!-- 纯 C 界面：只有 NativeActivity。加 Kotlin 桥接时把 hasCode 改成 true。 -->
        <activity
            android:name="android.app.NativeActivity"
            android:configChanges="orientation|keyboardHidden|screenSize|smallestScreenSize|density|uiMode|navigation"
            android:exported="true"
            android:screenOrientation="fullSensor">

            <meta-data
                android:name="android.app.lib_name"
                android:value="$APP_LIB" />

            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
EOF

    write_file app/src/main/res/values/strings.xml <<EOF
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <string name="app_name">$APP_NAME</string>
</resources>
EOF

    write_file app/src/main/cpp/CMakeLists.txt <<EOF
cmake_minimum_required(VERSION 3.22)

project($APP_LIB C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# NativeActivity 胶水代码随 NDK 分发。
add_library(native_app_glue STATIC
        \${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
        \${ANDROID_NDK}/sources/android/native_app_glue)
target_link_libraries(native_app_glue android log)

add_library($APP_LIB SHARED main.c)

target_include_directories($APP_LIB PRIVATE \${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options($APP_LIB PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Java 侧通过 dlsym 查找入口，防止 --gc-sections 把入口优化掉。
target_link_options($APP_LIB PRIVATE "-Wl,--undefined=ANativeActivity_onCreate")

target_link_libraries($APP_LIB
        EGL
        GLESv3
        android
        log
        m
        dl
        native_app_glue)
EOF

    write_file app/src/main/cpp/main.c <<'EOF'
// LXMusicC 骨架：NativeActivity + EGL + GLES 3.0 的最小可运行循环。
// 界面、渲染、交互会在后续功能集中逐步替换（见 PLAN.md 的 01-ui-shell）。

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "LXMusicC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef struct Engine {
    struct android_app *app;
    ANativeWindow *window;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    bool ready;
    double start_time;
} Engine;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

static bool engine_create(Engine *e) {
    if (e->ready || !e->window) {
        return false;
    }
    e->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (e->display == EGL_NO_DISPLAY || !eglInitialize(e->display, NULL, NULL)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint config_attrs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0,
            EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint config_count = 0;
    if (!eglChooseConfig(e->display, config_attrs, &config, 1, &config_count) || config_count == 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    EGLint native_visual = 0;
    eglGetConfigAttrib(e->display, config, EGL_NATIVE_VISUAL_ID, &native_visual);
    ANativeWindow_setBuffersGeometry(e->window, 0, 0, native_visual);

    e->surface = eglCreateWindowSurface(e->display, config, e->window, NULL);
    if (e->surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed");
        return false;
    }

    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    e->context = eglCreateContext(e->display, config, EGL_NO_CONTEXT, context_attrs);
    if (e->context == EGL_NO_CONTEXT || !eglMakeCurrent(e->display, e->surface, e->surface, e->context)) {
        LOGE("eglCreateContext/eglMakeCurrent failed");
        return false;
    }
    eglSwapInterval(e->display, 1);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    e->ready = true;
    LOGI("GLES ready: %s", (const char *) glGetString(GL_VERSION));
    return true;
}

static void engine_destroy(Engine *e) {
    if (e->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(e->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (e->context != EGL_NO_CONTEXT) eglDestroyContext(e->display, e->context);
        if (e->surface != EGL_NO_SURFACE) eglDestroySurface(e->display, e->surface);
        eglTerminate(e->display);
    }
    e->context = EGL_NO_CONTEXT;
    e->surface = EGL_NO_SURFACE;
    e->display = EGL_NO_DISPLAY;
    e->ready = false;
}

static void engine_draw(Engine *e) {
    if (!e->ready) {
        return;
    }
    EGLint width = 0;
    EGLint height = 0;
    eglQuerySurface(e->display, e->surface, EGL_WIDTH, &width);
    eglQuerySurface(e->display, e->surface, EGL_HEIGHT, &height);
    if (width <= 0 || height <= 0) {
        return;
    }

    // 骨架画面：呼吸的深色背景 + 一条随时间移动的高亮带，用来确认渲染循环在跑。
    const float t = (float) (now_seconds() - e->start_time);
    const float pulse = 0.5f + 0.5f * sinf(t * 1.2f);
    glViewport(0, 0, width, height);
    glClearColor(0.02f + 0.03f * pulse, 0.03f, 0.07f + 0.05f * pulse, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, (int) ((0.5f + 0.4f * sinf(t * 0.7f)) * (float) height), width, height / 24);
    glClearColor(0.25f, 0.45f, 0.95f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    eglSwapBuffers(e->display, e->surface);
}

static void on_app_cmd(struct android_app *app, int32_t cmd) {
    Engine *e = app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            e->window = app->window;
            engine_create(e);
            break;
        case APP_CMD_TERM_WINDOW:
            engine_destroy(e);
            e->window = NULL;
            break;
        case APP_CMD_GAINED_FOCUS:
            e->start_time = now_seconds();
            break;
        default:
            break;
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *event) {
    (void) app;
    (void) event;
    return 0;   // 01-ui-shell 里接管触摸
}

void android_main(struct android_app *app) {
    Engine engine;
    memset(&engine, 0, sizeof(engine));
    engine.app = app;
    engine.display = EGL_NO_DISPLAY;
    engine.surface = EGL_NO_SURFACE;
    engine.context = EGL_NO_CONTEXT;
    engine.start_time = now_seconds();

    app->userData = &engine;
    app->onAppCmd = on_app_cmd;
    app->onInputEvent = on_input;

    while (!app->destroyRequested) {
        int events = 0;
        struct android_poll_source *source = NULL;
        const int timeout = engine.ready ? 16 : -1;
        if (ALooper_pollOnce(timeout, NULL, &events, (void **) &source) >= 0 && source) {
            source->process(app, source);
        }
        engine_draw(&engine);
    }

    engine_destroy(&engine);
    LOGI("native activity finished");
}
EOF

    bootstrap_wrapper
}

# ---------------------------------------------------------------- git / 计划

ensure_git() {
    if [ ! -d "$PROJECT_DIR/.git" ]; then
        if [ "$DRY_RUN" = 1 ]; then
            log "[dry-run] git init -b main"
        else
            git -C "$PROJECT_DIR" init -q -b main
            log "git init（分支 main）"
        fi
    fi

    if [ -n "$REMOTE_URL" ]; then
        if [ "$DRY_RUN" = 1 ]; then
            log "[dry-run] git remote origin = $REMOTE_URL"
        elif git -C "$PROJECT_DIR" remote get-url origin >/dev/null 2>&1; then
            git -C "$PROJECT_DIR" remote set-url origin "$REMOTE_URL"
        else
            git -C "$PROJECT_DIR" remote add origin "$REMOTE_URL"
        fi
        log "origin = $REMOTE_URL"
    fi

    [ "$DRY_RUN" = 1 ] && return 0

    # 提交身份：优先命令行 → 参考仓库的本地配置 → 仓库自身 → 兜底默认值。
    if [ -z "$GIT_NAME" ]; then
        GIT_NAME="$(git -C "$PROJECT_DIR" config user.name || true)"
    fi
    if [ -z "$GIT_NAME" ] && [ -d "$REFERENCE_DIR/.git" ]; then
        GIT_NAME="$(git -C "$REFERENCE_DIR" config user.name || true)"
    fi
    if [ -z "$GIT_EMAIL" ]; then
        GIT_EMAIL="$(git -C "$PROJECT_DIR" config user.email || true)"
    fi
    if [ -z "$GIT_EMAIL" ] && [ -d "$REFERENCE_DIR/.git" ]; then
        GIT_EMAIL="$(git -C "$REFERENCE_DIR" config user.email || true)"
    fi
    [ -n "$GIT_NAME" ] || GIT_NAME="pi-loop"
    [ -n "$GIT_EMAIL" ] || GIT_EMAIL="pi-loop@localhost"
    git -C "$PROJECT_DIR" config user.name "$GIT_NAME"
    git -C "$PROJECT_DIR" config user.email "$GIT_EMAIL"

    if ! git -C "$PROJECT_DIR" rev-parse --verify HEAD >/dev/null 2>&1; then
        git -C "$PROJECT_DIR" add -A
        if git -C "$PROJECT_DIR" commit -q -m "chore: 工程骨架（纯 C + 里程碑式 CI 发布）"; then
            log "首次提交完成（$GIT_NAME <$GIT_EMAIL>）"
        else
            die "首次提交失败：检查文件权限或 git 配置"
        fi
    fi
}

push_branch() {
    [ "$PUSH" = 1 ] || { warn "已跳过 push（--no-push）"; return 0; }
    if ! git -C "$PROJECT_DIR" remote get-url origin >/dev/null 2>&1; then
        if [ "$PUSH_WARNED" = 0 ]; then
            warn "还没有配置远程仓库：提交只在本机。创建 GitHub 仓库后执行"
            warn "  git -C \"$PROJECT_DIR\" remote add origin git@github.com:<你>/LXMusicC.git"
            warn "  然后重跑本脚本（或加 --remote 参数）"
            PUSH_WARNED=1
        fi
        return 0
    fi
    if [ "$DRY_RUN" = 1 ]; then
        log "[dry-run] git push -u origin HEAD"
        return 0
    fi
    if git -C "$PROJECT_DIR" push -q -u origin HEAD 2>/dev/null; then
        log "push 分支完成（不会触发构建）"
    else
        warn "push 失败（网络/权限？），下次轮次会重试"
    fi
}

first_open_item() { awk '/^- \[ \]/ { print; exit }' "$PROJECT_DIR/PLAN.md" 2>/dev/null; }
item_id()   { sed -n 's/^- \[ \] `\([^`]*\)`.*/\1/p' <<<"$1"; }
item_title(){ sed -n 's/^- \[ \] `[^`]*`[[:space:]]*//p' <<<"$1"; }
item_ticked() { grep -qE "^- \[[xX]\] \`$1\`" "$PROJECT_DIR/PLAN.md"; }
plan_done() { [ -z "$(first_open_item)" ]; }
last_commit() { git -C "$PROJECT_DIR" rev-parse HEAD 2>/dev/null || echo none; }
dirty() { [ -n "$(git -C "$PROJECT_DIR" status --porcelain 2>/dev/null)" ]; }

# ---------------------------------------------------------------- 里程碑

milestone_seconds() { echo $((MILESTONE_HOURS * 3600)); }

milestone_due() {
    [ "$MILESTONE_HOURS" -le 0 ] && return 1
    local last now
    last="$(cat "$STATE_DIR/last-milestone" 2>/dev/null || echo 0)"
    now="$(date +%s)"
    [ $((now - last)) -ge "$(milestone_seconds)" ]
}

last_tag() { cat "$STATE_DIR/last-tag" 2>/dev/null || true; }

write_summary() {
    local tag="$1" kind="$2"
    local prev range
    prev="$(last_tag)"
    if [ -n "$prev" ] && git -C "$PROJECT_DIR" rev-parse "$prev" >/dev/null 2>&1; then
        range="$prev..HEAD"
    else
        range="HEAD"
    fi

    local open_item; open_item="$(first_open_item)"
    {
        echo "# $tag"
        echo
        echo "- 生成时间：$(date '+%F %T')"
        echo "- 类型：$([ "$kind" = release ] && echo '正式版' || echo '每 6 小时进度快照')"
        echo
        echo "## 已完成的功能集"
        echo
        grep -E '^- \[[xX]\] ' "$PROJECT_DIR/PLAN.md" | sed 's/^- \[[xX]\] /- /' || true
        echo
        if [ -n "$open_item" ]; then
            echo "## 进行中"
            echo
            sed 's/^- \[ \] /- /' <<<"$open_item"
            echo
        fi
        echo "## 本次里程碑包含的提交"
        echo
        echo '```'
        git -C "$PROJECT_DIR" log --oneline --no-decorate -40 "$range" 2>/dev/null || true
        echo '```'
        echo
        echo "## 安装"
        echo
        echo "下载本 Release 的 \`lxmusicc-debug.apk\` 安装即可。"
    } > "$PROJECT_DIR/docs/MILESTONE-LATEST.md"

    {
        echo
        echo "## $tag（$([ "$kind" = release ] && echo '正式版' || echo '快照')）"
        echo
        echo "已完成：$(grep -cE '^- \[[xX]\] ' "$PROJECT_DIR/PLAN.md" || true) / $(grep -cE '^- \[[ xX]\] `' "$PROJECT_DIR/PLAN.md" || true) 个功能集"
        echo
        echo "<details><summary>提交</summary>"
        echo
        echo '```'
        git -C "$PROJECT_DIR" log --oneline --no-decorate -20 "$range" 2>/dev/null || true
        echo '```'
        echo
        echo "</details>"
    } >> "$PROJECT_DIR/docs/MILESTONES.md"
}

make_milestone() {
    local kind="$1"
    [ "$PUSH" = 1 ] || { warn "未配置 push，跳过里程碑"; return 0; }
    if ! git -C "$PROJECT_DIR" remote get-url origin >/dev/null 2>&1; then
        warn "没有远程仓库，无法触发 GitHub 构建；先配置 origin 再打里程碑"
        return 0
    fi

    run_verify "$LOG_DIR/milestone-build.log" || { warn "构建失败，本次里程碑推迟"; return 1; }

    local tag
    if [ "$kind" = release ]; then
        tag="v1.0.0"
    else
        tag="snapshot-$(date +%Y%m%d-%H%M)"
    fi
    if git -C "$PROJECT_DIR" rev-parse "$tag" >/dev/null 2>&1; then
        warn "标签 $tag 已存在，跳过"
        return 0
    fi

    if [ "$DRY_RUN" = 1 ]; then
        log "[dry-run] 生成 docs/MILESTONE-LATEST.md 并打标签 $tag"
        return 0
    fi
    write_summary "$tag" "$kind"
    git -C "$PROJECT_DIR" add -A
    git -C "$PROJECT_DIR" commit -q -m "chore(milestone): $tag 总结快照" || true
    git -C "$PROJECT_DIR" tag -a "$tag" -m "$tag 里程碑"
    push_branch
    if git -C "$PROJECT_DIR" push -q origin "$tag" 2>/dev/null; then
        echo "$tag" > "$STATE_DIR/last-tag"
        date +%s > "$STATE_DIR/last-milestone"
        log "里程碑 $tag 已推送：GitHub Actions 会构建并发布 Release"
    else
        warn "标签推送失败，删除本地标签稍后重试：$tag"
        git -C "$PROJECT_DIR" tag -d "$tag" >/dev/null 2>&1 || true
        return 1
    fi
}

maybe_milestone() {
    if [ "$MILESTONE_NOW" = 1 ]; then
        MILESTONE_NOW=0
        make_milestone snapshot || true
        return 0
    fi
    if milestone_due; then
        log "已到 $(milestone_seconds) 秒的快照时间"
        make_milestone snapshot || true
    fi
}

# ---------------------------------------------------------------- 每轮执行

build_prompt() {
    local id="$1" title="$2" failure="$3" logfile="$4"
    cat <<EOF
你是这个仓库的自动化开发者。仓库是一个 Android 音乐播放器（兼容 LX Music 自定义音源），
界面与核心逻辑用 C 实现，只有系统强制要求的部分才写最小 Kotlin 桥接。

本轮只做 PLAN.md 中第一个未勾选的条目：
  ${id} — ${title}
验收标准见 PLAN.md 该条目下的子项。不要开始后续条目，不要顺手大改无关代码。

开始前先读 AGENTS.md、PLAN.md、README.md；需要时参考 docs/DECISIONS.md 与 docs/BRIDGE.md。
同级仓库 ../CUIDemo 是一份可参考的纯 C Android UI 实现（EGL、SDF 文字、批次渲染、局部重绘）。

本轮硬性要求：
1. 构建：改完后运行 ./scripts/build.sh，必须成功（失败就继续修，直到通过）。
2. 计划：完成后把 PLAN.md 里本条的 "- [ ]" 改成 "- [x]"，只改这一条。
3. 文档：README.md 只写功能/使用/安装；架构与 JNI 细节写进 docs/。
4. 提交：git add -A && git commit -m "${id}: <一句话>" ，然后 git push origin HEAD。
5. 不要给 workflow 增加 push 分支触发，不要自己打 tag 或建 Release：
   发布由 pi-loop.sh 每 6 小时打 snapshot 标签触发，平时提交不能产出安装包。
6. 不要提问、不要等待确认：拿不准就选最简单能用的方案，并写进 docs/DECISIONS.md。

结束时用 3-6 行汇报：改了哪些模块、构建结果、commit hash、push 结果。
EOF
    if [ -n "$failure" ]; then
        cat <<EOF

【上一次尝试失败】原因：${failure}
日志尾部：
$(tail -n 40 "$logfile" 2>/dev/null | sed 's/^/    /')
本轮先修复上述问题，再完成本条；仍然只做这一条。
EOF
    fi
}

run_pi() {
    local prompt="$1" logfile="$2"
    local -a args=(-p --approve)
    [ -n "$MODEL" ] && args+=(--model "$MODEL")
    [ -n "$THINKING" ] && args+=(--thinking "$THINKING")
    args+=(--session-dir "$STATE_DIR/sessions")

    if [ "$DRY_RUN" = 1 ]; then
        log "[dry-run] pi ${args[*]} <<提示词>>"
        printf '%s\n' "$prompt" | sed 's/^/    │ /'
        return 0
    fi
    mkdir -p "$STATE_DIR/sessions"
    {
        printf '\n===== %s =====\n' "$(date '+%F %T')"
        printf '%s\n' "$prompt"
        printf '%s\n' '----- pi 输出 -----'
    } >>"$logfile"

    set +e
    ( cd "$PROJECT_DIR" && timeout --foreground -k 30 "${TIMEOUT_SECONDS}" \
        pi "${args[@]}" "$prompt" ) 2>&1 | tee -a "$logfile"
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        return 124
    fi
    return "$rc"
}

run_verify() {
    local logfile="$1"
    local cmd="$VERIFY_CMD"
    if [ -z "$cmd" ]; then
        if [ -x "$PROJECT_DIR/scripts/build.sh" ]; then
            cmd="./scripts/build.sh"
        elif [ -x "$PROJECT_DIR/gradlew" ]; then
            cmd="./gradlew --console=plain :app:assembleDebug"
        else
            return 0
        fi
    fi
    if [ "$DRY_RUN" = 1 ]; then
        log "[dry-run] 验证：$cmd"
        return 0
    fi
    log "验证构建：$cmd"
    set +e
    ( cd "$PROJECT_DIR" && eval "$cmd" ) >>"$logfile" 2>&1
    local rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        warn "构建验证失败（日志：$logfile）"
    fi
    return "$rc"
}

auto_commit_if_dirty() {
    local id="$1" title="$2"
    dirty || return 0
    if [ "$DRY_RUN" = 1 ]; then
        log "[dry-run] 自动提交遗留改动"
        return 0
    fi
    warn "工作区仍有未提交改动，脚本代为提交（$id）"
    git -C "$PROJECT_DIR" add -A
    git -C "$PROJECT_DIR" commit -q -m "${id}: ${title}（自动提交）" || true
}

# ---------------------------------------------------------------- 主流程

main() {
    mkdir -p "$LOG_DIR" "$STATE_DIR"
    exec 9>"$STATE_DIR/lock"
    flock -n 9 || die "已有另一个 pi-loop 在跑（$STATE_DIR/lock）"

    if [ "$SKIP_BOOTSTRAP" = 0 ] && [ ! -f "$PROJECT_DIR/PLAN.md" ]; then
        bootstrap_project
    else
        log "已有工程，跳过骨架生成"
    fi

    if [ "$BOOTSTRAP_ONLY" = 1 ]; then
        ensure_git
        push_branch
        log "骨架就绪：$PROJECT_DIR"
        exit 0
    fi

    ensure_git
    [ -f "$PROJECT_DIR/PLAN.md" ] || die "缺少 PLAN.md"
    [ -f "$STATE_DIR/last-milestone" ] || date +%s > "$STATE_DIR/last-milestone"

    log "项目：$PROJECT_DIR"
    log "模型：${MODEL:-默认}  思考：$THINKING  单轮超时：${TIMEOUT_SECONDS}s  最多轮数：$RUNS_MAX"
    log "快照节奏：每 ${MILESTONE_HOURS} 小时一个 snapshot（0 = 关闭）；push 分支本身不触发构建"

    if [ "$MILESTONE_ONLY" = 1 ]; then
        log "只生成一个快照版本（--milestone-only）"
        make_milestone snapshot
        exit $?
    fi

    local run=0
    while :; do
        if plan_done; then
            log "PLAN.md 全部完成 🎉"
            run_verify "$LOG_DIR/final-build.log" || warn "最后一次构建验证失败，请检查"
            make_milestone release || warn "正式版里程碑没有完成，稍后重试"
            log "下一步：在 GitHub 的 Actions/Releases 里确认 v1.0.0 的安装包"
            exit 0
        fi

        local line id title
        line="$(first_open_item)"
        id="$(item_id <<<"$line")"
        title="$(item_title <<<"$line")"
        [ -n "$id" ] || die "PLAN.md 解析失败：$line"

        log "下一个功能集：${id} — ${title}"
        if [ "$DRY_RUN" = 1 ]; then
            run_pi "$(build_prompt "$id" "$title" "" "")" "/dev/null"
            log "[dry-run] 结束：没有调用模型，也没有改动文件"
            exit 0
        fi
        [ "$ONCE" = 1 ] && log "（--once：本轮结束后退出）"
        maybe_milestone

        local attempt=1 done_item=0 failure="" last_log=""
        while [ "$attempt" -le "$ATTEMPTS_PER_ITEM" ]; do
            run=$((run + 1))
            if [ "$run" -gt "$RUNS_MAX" ]; then
                warn "达到最大轮数 $RUNS_MAX，停止"
                push_branch
                exit 3
            fi
            local logfile="$LOG_DIR/run-$(printf '%02d' "$run")-${id}-a${attempt}.log"
            local before; before="$(last_commit)"

            log "第 $run 轮：${id}（第 $attempt 次尝试），日志：$logfile"
            local prompt; prompt="$(build_prompt "$id" "$title" "$failure" "$last_log")"

            local rc=0
            run_pi "$prompt" "$logfile" || rc=$?

            if [ "$rc" -eq 124 ]; then
                failure="超时（> ${TIMEOUT_SECONDS}s）"
            elif [ "$rc" -ne 0 ]; then
                failure="pi 退出码 $rc"
            elif ! item_ticked "$id"; then
                failure="PLAN.md 里没有勾选 ${id}"
            elif [ "$(last_commit)" = "$before" ] && ! dirty; then
                failure="没有产生新的提交"
            else
                failure=""
            fi

            if [ -z "$failure" ]; then
                auto_commit_if_dirty "$id" "$title"
                if ! run_verify "$logfile"; then
                    failure="构建失败（scripts/build.sh）"
                fi
            fi

            if [ -z "$failure" ]; then
                push_branch
                log "完成 ${id}（第 $run 轮）"
                maybe_milestone
                done_item=1
                break
            fi

            warn "第 $run 轮未完成：$failure"
            last_log="$logfile"
            attempt=$((attempt + 1))
        done

        if [ "$done_item" != 1 ]; then
            warn "功能集 ${id} 连续 $ATTEMPTS_PER_ITEM 次未完成"
            push_branch
            die "停止：${id} 卡住了。看日志：$LOG_DIR（修好后重跑本脚本会从这一条继续）"
        fi

        if [ "$ONCE" = 1 ]; then
            log "--once：已处理 ${id}，退出"
            exit 0
        fi
    done
}

main "$@"
