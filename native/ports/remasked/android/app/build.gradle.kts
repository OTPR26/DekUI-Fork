plugins { id("com.android.application") }
android {
    namespace = "io.github.otpr26.remasked.nativeprototype"
    compileSdk = 35
    ndkVersion = "29.0.14206865"
    defaultConfig {
        applicationId = "io.github.otpr26.remasked.nativeprototype"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.1-prototype"
        testInstrumentationRunner = "io.github.otpr26.remasked.nativeprototype.CoreInstrumentation"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild.cmake {
            arguments += listOf("-DANDROID_STL=c++_shared",
                "-DREMASKED_JSON_INCLUDE=${providers.gradleProperty("remaskedJsonInclude").getOrElse("")}")
            arguments += listOf(
                "-DREMASKED_TITLE_BUILD=${providers.gradleProperty("remaskedTitleBuild").getOrElse("")}",
                "-DREMASKED_NRI_BUILD=${providers.gradleProperty("remaskedNriBuild").getOrElse("")}")
        }
    }
    externalNativeBuild.cmake { path = file("../../CMakeLists.txt"); version = "3.31.6" }
    sourceSets.getByName("main").assets.srcDir("../../resources")
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
}
