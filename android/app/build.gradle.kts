plugins {
    id("com.android.application")
    // AGP 9.0 起 Kotlin 支持内置于 AGP（运行时依赖 KGP），无需再显式应用
    // org.jetbrains.kotlin.android（应用它会直接报错）。Compose 编译器插件
    // 仍是独立的 KGP 编译器插件，需显式应用（版本与 Kotlin 对齐）。
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.miclink.open"
    // Android 17 (API 37)：Compose 1.12 / core-ktx 1.19 / lifecycle 2.11 要求
    compileSdk = 37

    defaultConfig {
        applicationId = "com.miclink.open"
        // minSdk 26：AudioRecord.Builder / 前台服务通知渠道 / Opus 均需要较新的 API
        minSdk = 26
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"

        // ---- WO Mic 协议默认值（唯一事实来源：docs/protocol/womic-wifi-protocol-v1.md）----
        // 手机侧 TCP 控制端口，官方 app SharedPreferences "controlPort"（证据 V1/f.java）
        buildConfigField("int", "DEFAULT_CONTROL_PORT", "8125")
        // UDP 头音量字段，单位增益（证据 V1/f.java + 规格 v1 第 3 节格式 C）
        buildConfigField("int", "DEFAULT_VOLUME", "5")
        // CONFIG 采样率表索引：0/1/2 = 8000/16000/48000，官方默认 48000
        buildConfigField("int", "DEFAULT_SAMPLE_RATE_INDEX", "2")
        // 心跳看门狗周期（证据 V1/k.java）
        buildConfigField("long", "HEARTBEAT_TIMEOUT_MS", "5000")
        // Opus 帧长（证据 V1/m.java：app 硬编码 20ms）
        buildConfigField("int", "DEFAULT_FRAME_MS", "20")
        // 语音在 Wi-Fi 上的稳妥档位
        buildConfigField("int", "DEFAULT_BITRATE", "64000")
    }

    signingConfigs {
        // 发布签名留给使用者自行配置（gradle.properties 中提供 storeFile / storePassword 等）
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    defaultConfig {
        externalNativeBuild {
            cmake {
                // 默认只打主流 ABI；需要全 ABI 时改成空数组或删除 abiFilters
                abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
                cFlags += listOf("-fvisibility=hidden", "-O2")
                cppFlags += listOf("-fvisibility=hidden", "-O2", "-std=c++17")
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }

    buildFeatures {
        buildConfig = true
        compose = true
        viewBinding = false
    }

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    // 协议核心（纯 JVM）：帧编解码 / 会话状态机 / UDP 包化 + 字节级单元测试
    implementation(project(":core"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)

    val composeBom = platform(libs.androidx.compose.bom)
    implementation(composeBom)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons.extended)
    debugImplementation(libs.androidx.compose.ui.tooling)
}
