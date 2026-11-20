// 顶层构建文件：插件版本统一在 gradle/libs.versions.toml 里。
plugins {
    alias(libs.plugins.android.application) apply false
}
