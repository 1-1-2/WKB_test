// Pure-JVM protocol core: wire format, message codec, packetizer, session logic.
// No Android dependencies -> runs as fast JVM unit tests in CI (:core:test).
plugins {
    alias(libs.plugins.kotlin.jvm)
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    testImplementation(libs.junit)
}

tasks.withType<Test> {
    testLogging {
        events("passed", "failed", "skipped")
    }
}
