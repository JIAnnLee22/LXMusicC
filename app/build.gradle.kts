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
            version = "4.1.2"
        }
    }

    ndkVersion = "29.0.14206865"
    buildToolsVersion = "36.0.0"
}
