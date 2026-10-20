pluginManagement {
    repositories {
        // Tencent Cloud mirror first: aggregates Maven Central + Google Maven
        // (kotlin plugin, AGP, plugin markers, Compose BOM all verified 200),
        // ~150-230KB/s from this network vs 2.8KB/s from plugins.gradle.org.
        maven { url = uri("https://mirrors.tencent.com/nexus/repository/maven-public/") }
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
        maven { url = uri("https://mirrors.tencent.com/nexus/repository/maven-public/") }
        google()
        mavenCentral()
    }
}
rootProject.name = "womic-wifi-client"
include(":app")
include(":core")
