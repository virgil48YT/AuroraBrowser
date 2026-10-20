plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Release signing comes only from the environment (CI secrets or a local
// shell), never from files in the repository. When any value is missing the
// release build still succeeds and produces app-release-unsigned.apk, which
// must be signed with apksigner before it can be installed or published.
//   AURORA_ANDROID_KEYSTORE           path to the .jks/.keystore file
//   AURORA_ANDROID_KEYSTORE_PASSWORD  keystore password
//   AURORA_ANDROID_KEY_ALIAS          key alias
//   AURORA_ANDROID_KEY_PASSWORD       key password
val releaseKeystore: String? = System.getenv("AURORA_ANDROID_KEYSTORE")?.takeIf { it.isNotBlank() }
val releaseSigningConfigured: Boolean =
    releaseKeystore != null && file(releaseKeystore).isFile &&
        listOf(
            "AURORA_ANDROID_KEYSTORE_PASSWORD",
            "AURORA_ANDROID_KEY_ALIAS",
            "AURORA_ANDROID_KEY_PASSWORD",
        ).all { !System.getenv(it).isNullOrBlank() }

// Version: AURORA_VERSION_NAME (e.g. "0.2.0" from a v0.2.0 tag) and
// AURORA_VERSION_CODE (a monotonically increasing integer, e.g. the CI run
// number). Local builds fall back to the defaults below.
val auroraVersionName: String =
    System.getenv("AURORA_VERSION_NAME")?.takeIf { it.isNotBlank() } ?: "0.1.0"
val auroraVersionCode: Int =
    System.getenv("AURORA_VERSION_CODE")?.toIntOrNull()?.takeIf { it > 0 } ?: 1

android {
    namespace = "com.aurora.browser"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.aurora.browser"
        minSdk = 26
        targetSdk = 35
        versionCode = auroraVersionCode
        versionName = auroraVersionName
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    signingConfigs {
        if (releaseSigningConfigured) {
            create("release") {
                storeFile = file(releaseKeystore!!)
                storePassword = System.getenv("AURORA_ANDROID_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("AURORA_ANDROID_KEY_ALIAS")
                keyPassword = System.getenv("AURORA_ANDROID_KEY_PASSWORD")
                enableV1Signing = true
                enableV2Signing = true
            }
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            if (releaseSigningConfigured) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        viewBinding = false
        buildConfig = true
    }

    lint {
        // Lint runs as its own CI-independent task (./gradlew lintDebug);
        // it must never block producing the debug APK.
        abortOnError = false
        checkReleaseBuilds = false
    }

    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.swiperefreshlayout:swiperefreshlayout:1.1.0")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("androidx.webkit:webkit:1.12.1")

    // Built-in Tor (like Brave's private windows with Tor): Guardian Project's
    // tor-android ships libtor.so for every ABI plus an in-process TorService,
    // so no Orbot or other external setup is needed. 0.4.8.17.2 is the newest
    // Maven Central build that does not require a newer Kotlin stdlib than the
    // Kotlin 2.0 toolchain used here.
    implementation("info.guardianproject:tor-android:0.4.8.17.2")
    implementation("info.guardianproject:jtorctl:0.4.5.7")
    implementation("androidx.localbroadcastmanager:localbroadcastmanager:1.1.0")

    testImplementation("junit:junit:4.13.2")
}
