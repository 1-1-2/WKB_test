plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.womic.open"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.womic.open"
        // minSdk 26：AudioRecord.Builder / 前台服务通知渠道 / Opus 均需要较新的 API
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        // UDP 目标端口默认值与 Windows 服务端一致（server/src/config.h: listen_port = 5004）
        buildConfigField("int", "DEFAULT_UDP_PORT", "5004")
        // 发现服务端口（server/src/config.h: discovery_port = 5005）
        buildConfigField("int", "DEFAULT_DISCOVERY_PORT", "5005")
        buildConfigField("int", "DEFAULT_SAMPLE_RATE", "48000")
        buildConfigField("int", "DEFAULT_FRAME_MS", "20")
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

    kotlinOptions {
        jvmTarget = "17"
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
        viewBinding = false
    }

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("org.jetbrains.kotlin:kotlin-stdlib:1.9.22")
    // 只依赖 androidx.core：NotificationCompat / ServiceCompat 均在其中，
    // 不引入 AppCompat 以减小体积。
}
