plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "fr.yvespetit.androidnb"
    compileSdk = 37

    defaultConfig {
        applicationId = "fr.yvespetit.androidnb"
        minSdk = 26
        targetSdk = 37
        // The release workflow passes the tag (v0.2 -> 0.2) and its run number.
        versionCode = System.getenv("NB_VERSION_CODE")?.toIntOrNull() ?: 1
        versionName = System.getenv("NB_VERSION_NAME") ?: "0.1"
    }

    // Fixed debug key, committed on purpose (it protects nothing): every CI
    // build then updates the app on the phone instead of needing an uninstall.
    signingConfigs {
        getByName("debug") {
            storeFile = file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
        // Release key from the CI secrets (see README, « Distribuer »). Without
        // them a release build falls back to the debug key above.
        val releaseStore = System.getenv("NB_KEYSTORE")
        if (releaseStore != null && file(releaseStore).exists()) {
            create("release") {
                storeFile = file(releaseStore)
                storePassword = System.getenv("NB_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("NB_KEY_ALIAS")
                keyPassword = System.getenv("NB_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    buildFeatures {
        compose = true
    }

    // Board files for the in-app installer (assets/board/): the scripts from
    // board/, and nbd from build-arm/ when it was built (CI and releases);
    // without it the installer button explains that this APK has no nbd.
    // A plain File: AGP 9 refuses a Provider here (the Copy task below runs before preBuild).
    sourceSets.getByName("main").assets.srcDir(layout.buildDirectory.get().dir("generated/boardAssets").asFile)

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

val boardAssets by tasks.registering(Copy::class) {
    from(rootProject.file("board")) { include("nbctl.sh", "install.sh", "nb.conf") }
    from(rootProject.file("build-arm")) { include("nbd") }
    into(layout.buildDirectory.dir("generated/boardAssets/board"))
}
tasks.named("preBuild") { dependsOn(boardAssets) }

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2026.09.00")

    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    // ssh for the in-app board installer (maintained JSch fork).
    implementation("com.github.mwiede:jsch:0.2.18")

    testImplementation("junit:junit:4.13.2")
}
