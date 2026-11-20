plugins { id("com.android.application"); id("org.jetbrains.kotlin.android"); id("com.chaquo.python") }
val nativeStage = providers.gradleProperty("remaskedNativeStage")
val sdlSource = providers.gradleProperty("remaskedSdlSource")
val existingIdentity = providers.gradleProperty("remaskedExistingIdentity").getOrElse("false").toBoolean()
android {
 namespace = "org.triaevum.android"
 compileSdk = 35
 ndkVersion = "29.0.14206865"
 defaultConfig {
  applicationId = if (existingIdentity) "io.github.otpr26.dekuifork" else "io.github.otpr26.remasked.nativegame"
  minSdk = 29; targetSdk = if (existingIdentity) 37 else 35
  versionCode = providers.gradleProperty("remaskedVersionCode").orNull?.toInt() ?: if (existingIdentity) 34311421 else 1
  versionName = if (existingIdentity) "0.5.0-rc.2" else "0.1-native"
  testInstrumentationRunner = "io.github.otpr26.remasked.nativegame.ModelRuntimeInstrumentation"
  ndk { abiFilters += "arm64-v8a" }
  buildConfigField("boolean", "VERIFY_FORGE_PACKAGE", "false")
 }
 buildFeatures { buildConfig = true }
 sourceSets.getByName("main") {
  if (existingIdentity) manifest.srcFile("src/existingIdentity/AndroidManifest.xml")
  java.srcDir("../../../android/app/src/main/java")
  res.srcDir("../../../android/app/src/main/res")
  if (sdlSource.isPresent) java.srcDir("${sdlSource.get()}/android-project/app/src/main/java")
  if (nativeStage.isPresent) jniLibs.srcDir(nativeStage.get())
 }
 compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
 kotlinOptions { jvmTarget = "17" }
 packaging { jniLibs.useLegacyPackaging = true; jniLibs.keepDebugSymbols += "**/*.so" }
}
chaquopy {
 defaultConfig { version = "3.13"; buildPython(providers.gradleProperty("remaskedBuildPython").getOrElse("python3.13")); pip { install("numpy==1.26.2"); install("pillow==11.0.0") } }
 sourceSets.getByName("main") { srcDir("../../models"); exclude("**/__pycache__/**", "**/*.pyc") }
}
dependencies { implementation(project(":controls")) }
tasks.register("checkReMaskedNativeStage") {
 doLast {
  check(nativeStage.isPresent && sdlSource.isPresent) { "Supply the isolated native runtime and SDL source" }
  for (name in listOf("SDL2", "TriAevum", "c++_shared", "triaevum_title_bootstrap", "remasked_vulkan", "remasked_driver", "main_hook", "hook_impl"))
   check(file("${nativeStage.get()}/arm64-v8a/lib$name.so").isFile) { "Missing native runtime library $name" }
 }
}
tasks.named("preBuild") { dependsOn("checkReMaskedNativeStage") }
