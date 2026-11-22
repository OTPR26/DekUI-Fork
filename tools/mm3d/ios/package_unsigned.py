#!/usr/bin/env python3
"""Compile the ReMasked iOS host and package it without a signing identity."""
import argparse
import hashlib
import json
import plistlib
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HOST = REPO / "src/ios/remasked"
ANDROID_RES = REPO / "src/android/app/src/main/res"
LIBRETRO = REPO / "externals/libretro-common/libretro-common/include"
bundle_id = "io.github.otpr26.remasked.azaharprototype.ios"
cloud_container = f"iCloud.{bundle_id}"

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def output(*args):
    return subprocess.check_output([str(a) for a in args])

def plist(path, value):
    path.write_bytes(plistlib.dumps(value))

def stage_branding(app, staging):
    assets = REPO / "resources/mm3d/launcher-icons"
    catalog = staging / "ReMasked.xcassets"
    alternates = []
    for index in range(5):
        name = "AppIcon" if index == 0 else f"AppIcon{index}"
        folder = catalog / f"{name}.appiconset"
        folder.mkdir(parents=True, exist_ok=True)
        source = assets / f"ios-icon-{index}.png"
        shutil.copyfile(source, folder / "ReMasked.png")
        shutil.copyfile(source, app / f"ReMaskedIcon{index}.png")
        (folder / "Contents.json").write_text(json.dumps({
            "images": [{"filename": "ReMasked.png", "idiom": "universal",
                        "platform": "ios", "size": "1024x1024"}],
            "info": {"author": "ReMasked", "version": 1},
        }))
        if index:
            alternates.extend(["--alternate-app-icon", name])

    masks = ANDROID_RES / "drawable-nodpi/dekui_launcher_masks.png"
    shutil.copyfile(masks, app / "ReMaskedMasks.png")
    partial = staging / "icon-info.plist"
    run("xcrun", "actool", catalog, "--compile", app, "--platform", "iphoneos",
        "--minimum-deployment-target", "17.0", "--target-device", "iphone", "--target-device", "ipad",
        "--app-icon", "AppIcon", "--output-partial-info-plist", partial, *alternates)
    icons = plistlib.loads(partial.read_bytes())
    for key in ("CFBundleIcons", "CFBundleIcons~ipad"):
        group = icons[key]
        assert group["CFBundlePrimaryIcon"]["CFBundleIconName"] == "AppIcon"
        assert set(group["CFBundleAlternateIcons"]) == {f"AppIcon{i}" for i in range(1, 5)}
    return icons

def european_setup_files():
    resources = REPO / "resources/mm3d"
    recipe = resources / "android-layout-recipe.json"
    files = {"supported-rom.json": resources / "supported-rom.json", recipe.name: recipe}
    hud = ANDROID_RES.parent / "assets/dekui-hud8x"
    upgrade = json.loads((hud / "upgrade.json").read_text())
    texture = hud / upgrade["texture"]
    if texture.parent != hud or hashlib.sha256(texture.read_bytes()).hexdigest() != upgrade["texture_sha256"]:
        raise ValueError("HUD texture does not match the pinned upgrade recipe")
    for name in ("layout.json", "play.json", "upgrade.json", texture.name):
        files[f"hud/{name}"] = hud / name
    return {f"EuropeanSetup/{name}": path for name, path in files.items()}


def build_app(app, staging, core_library, molten, version, experimental_eur=False):
    target_id = bundle_id + ".eurpreview" if experimental_eur else bundle_id
    staging.mkdir(exist_ok=True)
    app.mkdir(exist_ok=True)
    frameworks = app / "Frameworks"
    frameworks.mkdir(exist_ok=True)
    core = frameworks / "AzaharCore.framework"
    core.mkdir(exist_ok=True)
    shutil.copy2(core_library,core/"AzaharCore")
    run("install_name_tool","-id","@rpath/AzaharCore.framework/AzaharCore",core/"AzaharCore")
    plist(core/"Info.plist",{"CFBundleIdentifier":target_id+".core","CFBundleName":"AzaharCore",
           "CFBundleExecutable":"AzaharCore","CFBundlePackageType":"FMWK","CFBundleVersion":"1",
           "CFBundleShortVersionString":"1.0","MinimumOSVersion":"17.0"})
    fonts = {"FiraSans-Regular.ttf": "fira_sans_regular.ttf", "FiraSans-Bold.ttf": "fira_sans_bold.ttf"}
    for target, name in fonts.items():
        shutil.copy2(ANDROID_RES / "font" / name, app / target)
    shutil.copy2(ANDROID_RES.parent / "assets/remasked-fonts/FiraSans-OFL.txt", app / "FiraSans-OFL.txt")
    shutil.copy2(REPO / "license.txt", app / "LICENSE.txt")
    shutil.copy2(molten.parent / "LICENSE", app / "MoltenVK-LICENSE.txt")
    vk_source = molten/"dynamic/MoltenVK.xcframework/ios-arm64/MoltenVK.framework"
    vk = frameworks/"MoltenVK.framework"
    vk.mkdir(exist_ok=True)
    for name in ("Info.plist", "MoltenVK"):
        shutil.copy2(vk_source/name, vk/name)
    icons = stage_branding(app, staging)
    cloud = {} if experimental_eur else {"NSUbiquitousContainers": {cloud_container: {
              "NSUbiquitousContainerIsDocumentScopePublic":True,
              "NSUbiquitousContainerName": app.stem,
              "NSUbiquitousContainerSupportedFolderLevels":"Any"}}}
    preview_flags = []
    preview_sources = []
    if experimental_eur:
        preview_flags = ["-DMM3D_EUR_PREVIEW=1"]
        preview_sources = [HOST / "EuropeanSetup.mm"]
        for name, source in european_setup_files().items():
            target = app / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    plist(app/"Info.plist",{**icons, **cloud, "CFBundleIdentifier":target_id,"CFBundleExecutable":"ReMaskedAzahar",
          "UIAppFonts":list(fonts),"CFBundlePackageType":"APPL","CFBundleDisplayName":app.stem,"CFBundleVersion":version,
          "CFBundleShortVersionString":version,"UILaunchScreen":{},"MinimumOSVersion":"17.0",
          "UIRequiresFullScreen":True,"UIDeviceFamily":[1,2],"UIFileSharingEnabled":True,
          "LSSupportsOpeningDocumentsInPlace":True,"CFBundleSupportedPlatforms":["iPhoneOS"],
          "UIApplicationSceneManifest":{"UIApplicationSupportsMultipleScenes":False,
              "UISceneConfigurations":{
                  "UIWindowSceneSessionRoleApplication":[{
                      "UISceneConfigurationName":"ReMasked","UISceneDelegateClassName":"ReMaskedScene"}],
                  "UIWindowSceneSessionRoleExternalDisplayNonInteractive":[{
                      "UISceneConfigurationName":"ReMasked AirPlay",
                      "UISceneDelegateClassName":"ReMaskedAirPlayScene"}]}},
          "UISupportedInterfaceOrientations":["UIInterfaceOrientationPortrait",
          "UIInterfaceOrientationLandscapeLeft","UIInterfaceOrientationLandscapeRight"]})
    sdk = output("xcrun","--sdk","iphoneos","--show-sdk-path").decode().strip()
    run("xcrun","--sdk","iphoneos","clang++","-target","arm64-apple-ios17.0","-isysroot",sdk,
        "-std=c++20","-O2","-fobjc-arc",f"-ffile-prefix-map={Path.home()}=build-home",
        "-I",LIBRETRO,"-I",molten/"include","-F",frameworks,
        "-DREMASKED_AZAHAR_TOUCH_HOST=1","-I",REPO/"src", *preview_flags, *preview_sources,
        *(HOST/name for name in ("touch_controls.mm", "drive_data.mm", "oot3d_menu_model.cpp",
                                  "Main.mm", "SettingsPanel.mm", "RetroVideo.mm")),
        "-framework","AzaharCore","-framework","MoltenVK",
        "-framework","UIKit","-framework","CoreGraphics","-framework","Metal","-framework","MetalKit","-framework","QuartzCore",
        "-framework","AVFoundation","-framework","GameController","-framework","CoreMotion",
        "-framework","UniformTypeIdentifiers","-Wl,-rpath,@executable_path/Frameworks","-o",app/"ReMaskedAzahar")


def export_unsigned(app, destination, experimental_eur=False):
    executables = {"ReMaskedAzahar", "Frameworks/AzaharCore.framework/AzaharCore",
                   "Frameworks/MoltenVK.framework/MoltenVK"}
    resources = {"Info.plist", "Assets.car", "AppIcon60x60@2x.png", "AppIcon76x76@2x~ipad.png",
                 "ReMaskedMasks.png", "LICENSE.txt", "MoltenVK-LICENSE.txt", "FiraSans-Regular.ttf", "FiraSans-Bold.ttf", "FiraSans-OFL.txt",
                 "Frameworks/AzaharCore.framework/Info.plist", "Frameworks/MoltenVK.framework/Info.plist",
                 *(f"ReMaskedIcon{i}.png" for i in range(5))}
    if experimental_eur:
        resources.update(european_setup_files())
    members = {path.relative_to(app).as_posix(): path for path in app.rglob("*")
               if not path.is_dir() or path.is_symlink()}
    if set(members) != resources | executables or any(path.is_symlink() for path in members.values()):
        raise SystemExit("The unsigned app contents do not match the release allowlist")
    for name in sorted(executables):
        binary = members[name]
        if output("lipo", "-archs", binary).strip() != b"arm64":
            raise SystemExit("The unsigned IPA requires ARM64 executables")
        if b"LC_CODE_SIGNATURE" in output("otool", "-l", binary):
            run("codesign", "--remove-signature", binary)
        if b"LC_CODE_SIGNATURE" in output("otool", "-l", binary):
            raise SystemExit("A signature remains in an unsigned executable")
    with tempfile.TemporaryDirectory(dir=destination.parent) as tmp:
        archive = Path(tmp) / destination.name
        with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as ipa:
            for name, member in sorted(members.items()):
                ipa.write(member, f"Payload/{app.name}/{name}")
        with zipfile.ZipFile(archive) as ipa:
            if ipa.testzip() is not None:
                raise SystemExit("The unsigned IPA failed its archive integrity check")
        checker = REPO / "tools/mm3d/check_release_privacy.py"
        run(sys.executable, checker, archive, "--private-prefix", "/Users/",
            "--private-prefix", "/private/var/folders/", "--private-prefix", "/var/folders/")
        destination.hardlink_to(archive)
    print(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-library", required=True, type=Path)
    parser.add_argument("--moltenvk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--experimental-eur", action="store_true")
    args = parser.parse_args()
    version = output(sys.executable, REPO / "tools/mm3d/release_version.py").decode().strip()
    destination = args.output.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        parser.error("refusing to overwrite an existing IPA")
    with tempfile.TemporaryDirectory() as tmp:
        staging = Path(tmp)
        app = staging / ("ReMasked EUR Preview.app" if args.experimental_eur else "Majora ReMasked.app")
        build_app(app, staging, args.core_library.resolve(), args.moltenvk.resolve(), version, args.experimental_eur)
        export_unsigned(app, destination, args.experimental_eur)


if __name__ == "__main__":
    main()
