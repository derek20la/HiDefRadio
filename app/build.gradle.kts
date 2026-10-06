import java.util.Properties   // for keystore.properties below (in a Gradle script the bare word "java" means Gradle's Java plugin)

plugins {
    alias(libs.plugins.android.application)
}

// 14b: the UPLOAD KEY (the key every build sent to Google Play is signed with).
// The key file and its passwords are NOT in this repository. They are described by a small
// file "keystore.properties" in the project's root folder, which git ignores:
//     storeFile=C:/Users/you/keys/hidefradio-upload.jks      (forward slashes, also on Windows)
//     storePassword=...
//     keyAlias=upload
//     keyPassword=...
// Without that file everything still builds; the release build is then just not signed
// (debug builds never need it). Anyone building from source makes their own key.
val keystoreProperties = Properties()
val keystorePropertiesFile = rootProject.file("keystore.properties")
if (keystorePropertiesFile.exists()) {
    keystorePropertiesFile.inputStream().use { keystoreProperties.load(it) }
}
val haveUploadKey = listOf("storeFile", "storePassword", "keyAlias", "keyPassword")
    .all { !keystoreProperties.getProperty(it).isNullOrBlank() }

android {
    namespace = "io.github.derek20la.hidefradio"
    ndkVersion = "30.0.16248370"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "io.github.derek20la.hidefradio"
        minSdk = 26
        targetSdk = 37
        // versionCode = the BUILD NUMBER: goes up by one with every step that changes the app,
        // and must go up with every upload to Google Play. versionName is what people see.
        // Settings > About shows both: "Version 1.0-beta1 (build 1)".
        versionCode = 7
        versionName = "1.0-beta2"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // Build native (C/C++) code only for 64-bit ARM phones for now.
        // To add 32-bit ARM later: listOf("arm64-v8a", "armeabi-v7a")
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    signingConfigs {
        create("release") {
            if (haveUploadKey) {
                storeFile = file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        // The build Android Studio puts on the phone while developing. It is a SEPARATE app
        // ("HiDef Radio dev", id ...hidefradio.debug), so it can sit next to the version from
        // Google Play, which is signed with another key and could not be installed over it.
        getByName("debug") {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-dev"
        }
        release {
            optimization {
                enable = false
            }
            if (haveUploadKey) signingConfig = signingConfigs.getByName("release")
            // Function names of the C/C++ code go into the bundle, so a crash report in the
            // Play Console shows where in nrsc5 / native-lib it happened.
            ndk {
                debugSymbolLevel = "SYMBOL_TABLE"
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildFeatures {
        viewBinding = true
    }
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.androidx.core.ktx)
    implementation(libs.material)
    implementation(libs.androidx.preference.ktx)   // 9e step 2: the settings screen
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
}