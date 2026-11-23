plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "jiannlee22.lxmusicc"
    compileSdk {
        version = release(36)
    }

    defaultConfig {
        applicationId = "jiannlee22.lxmusicc"
        minSdk = 26
        targetSdk = 36
        versionCode = 3
        versionName = "1.1.0"

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
            version = "4.1.2"
        }
    }

    // 未配置专用证书时仍输出可安装、不可调试的包；签名限制见 docs/RELEASE.md。
    val signingEnv = listOf("LX_RELEASE_STORE_FILE", "LX_RELEASE_STORE_PASSWORD",
        "LX_RELEASE_KEY_ALIAS", "LX_RELEASE_KEY_PASSWORD").map { providers.environmentVariable(it).orNull }
    require(signingEnv.all { it.isNullOrBlank() } || signingEnv.all { !it.isNullOrBlank() }) {
        "LX_RELEASE_* 签名参数必须全部提供或全部省略"
    }
    val releaseSigning = if (signingEnv.all { !it.isNullOrBlank() }) {
        signingConfigs.create("distribution") {
            storeFile = file(signingEnv[0]!!)
            storePassword = signingEnv[1]
            keyAlias = signingEnv[2]
            keyPassword = signingEnv[3]
        }
    } else signingConfigs.getByName("debug")
    buildTypes {
        getByName("release") {
            signingConfig = releaseSigning
        }
    }

    ndkVersion = "29.0.14206865"
    buildToolsVersion = "36.0.0"
}

// 05 明确要求 Media3；仅用于系统播放/会话，不引入 Kotlin UI 或原生 SDK。
dependencies {
    implementation("androidx.media3:media3-exoplayer:1.8.0")
    implementation("androidx.media3:media3-session:1.8.0")
}
