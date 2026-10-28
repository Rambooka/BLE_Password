import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.example.ble_beacon"
    // 37: required by Nordic's mcumgr-ble (firmware update). targetSdk is
    // unchanged, so runtime behaviour is not affected.
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.example.ble_beacon"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    // Release signing key lives OUTSIDE the project (keep a backup of both files:
    // a lost key means uninstalling the app to install a newer release).
    // Override the properties file with -PreleaseKeystoreProps=...
    val keystoreProps = file(
        providers.gradleProperty("releaseKeystoreProps").getOrElse(
            "${System.getProperty("user.home")}/.android/ble_beacon_keystore.properties"
        )
    )
    signingConfigs {
        if (keystoreProps.isFile) {
            create("release") {
                val p = Properties().apply { keystoreProps.inputStream().use { load(it) } }
                storeFile = file(p.getProperty("storeFile"))
                storePassword = p.getProperty("storePassword")
                keyAlias = p.getProperty("keyAlias")
                keyPassword = p.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            // Release = not debuggable, so `adb run-as` can't read the app's data.
            signingConfig = signingConfigs.findByName("release")
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
}

// ---- Bundled dongle firmware -------------------------------------------------
// Copies each PCB's signed MCUboot update package (NCS dfu_application.zip) out
// of the firmware build tree into the APK's assets/firmware/, so the app ships
// the firmware it was built alongside. Build the firmware first; a missing
// package only warns (that PCB then shows "not bundled" in the app).
// Firmware project location: -PfirmwareDir=... or the repo root (this Android
// project lives in the firmware repo's android/ folder).
val firmwareDir: Provider<String> = providers.gradleProperty("firmwareDir")
    .orElse(rootDir.resolve("..").canonicalPath)

abstract class BundleFirmwareTask : DefaultTask() {
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val officialDongleZip: ConfigurableFileCollection

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val mdbt50qZip: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun bundle() {
        val out = outputDir.get().asFile.resolve("firmware")
        out.deleteRecursively()
        out.mkdirs()
        // Asset names must match Pcb.asset in Firmware.kt.
        listOf(
            "nrf52840dongle.zip" to officialDongleZip,
            "mdbt50q.zip" to mdbt50qZip,
        ).forEach { (name, src) ->
            val zip = src.files.firstOrNull { it.isFile }
            if (zip == null) {
                logger.warn("Dongle firmware NOT bundled: $name (missing ${src.files.joinToString()})")
            } else {
                zip.copyTo(out.resolve(name), overwrite = true)
                logger.lifecycle("Bundled dongle firmware $name from $zip")
            }
        }
    }
}

val bundleFirmware = tasks.register<BundleFirmwareTask>("bundleFirmware") {
    officialDongleZip.from(firmwareDir.map { "$it/build_nRF52840/dfu_application.zip" })
    mdbt50qZip.from(firmwareDir.map { "$it/build_mdbt50q/dfu_application.zip" })
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(
            bundleFirmware, BundleFirmwareTask::outputDir
        )
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation("androidx.compose.material:material-icons-extended")
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.mcumgr.ble)
    testImplementation(libs.junit)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)
}