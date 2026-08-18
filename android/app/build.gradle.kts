plugins {
  alias(libs.plugins.android.application)
  alias(libs.plugins.compose.compiler)
  alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "io.github.abdoeid.phonecam"
    compileSdk = 36
    defaultConfig {
        applicationId = "io.github.abdoeid.phonecam"
        minSdk = 30
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
      compose = true
      aidl = false
      buildConfig = false
      shaders = false
    }

    packaging {
      resources {
        excludes += "/META-INF/{AL2.0,LGPL2.1}"
      }
    }
}

kotlin {
    jvmToolchain(17)
}

dependencies {
  val composeBom = platform(libs.androidx.compose.bom)
  implementation(composeBom)
  androidTestImplementation(composeBom)

  // Core Android dependencies
  implementation(libs.androidx.core.ktx)
  implementation(libs.androidx.lifecycle.runtime.ktx)
  implementation(libs.androidx.activity.compose)

  // Arch Components
  implementation(libs.androidx.lifecycle.runtime.compose)
  implementation(libs.androidx.lifecycle.viewmodel.compose)

  // Compose
  implementation(libs.androidx.compose.ui)
  implementation(libs.androidx.compose.ui.tooling.preview)
  implementation(libs.androidx.compose.material3)
  implementation(libs.androidx.compose.material.icons.extended)
  // Tooling
  debugImplementation(libs.androidx.compose.ui.tooling)
  // Instrumented tests
  androidTestImplementation(libs.androidx.compose.ui.test.junit4)
  debugImplementation(libs.androidx.compose.ui.test.manifest)

  // Local tests: jUnit, coroutines, Android runner
  testImplementation(libs.junit)
  testImplementation(libs.kotlinx.coroutines.test)

  // Instrumented tests: jUnit rules and runners
  androidTestImplementation(libs.androidx.test.core)
  androidTestImplementation(libs.androidx.test.ext.junit)
  androidTestImplementation(libs.androidx.test.runner)
  androidTestImplementation(libs.androidx.test.espresso.core)

  // Navigation
  implementation(libs.androidx.navigation3.ui)
  implementation(libs.androidx.navigation3.runtime)
  implementation(libs.androidx.lifecycle.viewmodel.navigation3)
}

// --- FlatBuffers control-protocol codegen ---
// flatc (winget Google.flatbuffers, must be on PATH) generates Kotlin bindings
// straight from the shared proto/*.fbs schemas -- see docs/build.md and the
// mirrored CMake codegen in windows/host/CMakeLists.txt.
//
// The generated code targets the classic com.google.flatbuffers Java runtime
// (Table/Struct/FlatBufferBuilder/Constants), which isn't published to Maven
// for flatc's exact version (25.12.19, newer than any released
// flatbuffers-java artifact -- Constants.FLATBUFFERS_25_12_19() wouldn't
// resolve against an older one, and the generated code embeds that exact
// symbol name). Compiling it straight from the vendored
// third_party/flatbuffers submodule guarantees an exact version match
// instead, the same reasoning as vendoring the C++ runtime headers in
// windows/host/CMakeLists.txt.
val protoDir = rootProject.projectDir.resolve("../proto")
val flatbuffersGenDir = layout.buildDirectory.dir("generated/source/flatbuffers")
val flatbuffersJavaRuntimeDir = rootProject.projectDir.resolve("../third_party/flatbuffers/java/src/main/java")

val generateFlatBuffers by tasks.registering(Exec::class) {
  val outDir = flatbuffersGenDir.get().asFile
  doFirst { outDir.mkdirs() }
  inputs.files(protoDir.resolve("wire.fbs"), protoDir.resolve("control.fbs"))
  outputs.dir(outDir)
  commandLine(
    "flatc", "--kotlin", "-o", outDir.absolutePath,
    protoDir.resolve("wire.fbs").absolutePath, protoDir.resolve("control.fbs").absolutePath,
  )
}

android {
  sourceSets["main"].kotlin.srcDir(flatbuffersGenDir.get().asFile)
  sourceSets["main"].java.srcDir(flatbuffersJavaRuntimeDir)
}

tasks.matching { it.name.contains("Kotlin") && it.name.startsWith("compile") }.configureEach {
  dependsOn(generateFlatBuffers)
}
