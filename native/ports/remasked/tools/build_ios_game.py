#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a private native iOS game, with ReMasked branding and Duo support.

Requires local verified title/game inputs. Never uploads or installs anything.
Set DEVELOPER_DIR explicitly to Xcode 27.1 or newer for the real Duo APIs.
"""
import argparse,json,os,plistlib,shutil,subprocess,hashlib
from pathlib import Path


def stage(app,game,icon,build,models=None):
    expected="io.github.otpr26.remasked.nativeprototype.game.ios"
    info=plistlib.loads((app/"Info.plist").read_bytes())
    if info.get("CFBundleIdentifier")!=expected:raise ValueError("Wrong iOS app identity")
    if not (game/"PrivateGame/process-manifest.json").is_file():raise ValueError("Owned game inputs required")
    # Runtime data is seeded separately; no logs, test timelines or save copies
    # belong in the immutable GameContent directory.
    destination=app/"GameContent";destination.mkdir(exist_ok=True)
    for name in ["PrivateGame","RestorationHud","resources"]:
        source=game/name
        if source.exists():shutil.copytree(source,destination/name,dirs_exist_ok=True)
    for name in ["ReMaskedNativeGame.launch.json","presentation.json","runtime-presentation.json"]:
        if (game/name).is_file():shutil.copyfile(game/name,destination/name)
    if models is not None:
        target=app/"PreparedModels";target.mkdir(exist_ok=True);manifest={}
        for folder in ["adult-link","dark-link","twilight-link"]:
            source=models/folder
            if not source.is_dir():raise ValueError("Missing approved prepared model: "+folder)
            for path in source.rglob("*"):
                if path.is_symlink():raise ValueError("Prepared model symlink")
                if not path.is_file():continue
                relative=path.relative_to(models);model_destination=target/relative;model_destination.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(path,model_destination)
                manifest[str(relative)]=hashlib.sha256(model_destination.read_bytes()).hexdigest()
        (target/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    mappings=Path(__file__).resolve().parents[3]/"runtime/triaevum_resources/gamecontrollerdb.txt"
    if not mappings.is_file():raise ValueError("Bundled controller mappings are required")
    (destination/"resources").mkdir(exist_ok=True)
    shutil.copyfile(mappings,destination/"resources/gamecontrollerdb.txt")
    launch=json.loads((destination/"ReMaskedNativeGame.launch.json").read_text())
    if "--input-timeline" in launch["arguments"]:raise ValueError("Test input timeline cannot ship in the iOS profile")
    catalog=build/"ios-icons.xcassets/AppIcon.appiconset";catalog.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(icon,catalog/"ReMasked.png")
    (catalog/"Contents.json").write_text(json.dumps({"images":[{"filename":"ReMasked.png","idiom":"universal","platform":"ios","size":"1024x1024"}],"info":{"author":"ReMasked","version":1}}))
    shutil.copyfile(icon,app/"ReMaskedLauncher.png")
    port=Path(__file__).resolve().parents[1]
    shutil.copyfile(port/"android-game/app/src/main/res/drawable-nodpi/dekui_launcher_masks.png",app/"ReMaskedMasks.png")
    font_names=["FiraSans-Bold.ttf","FiraSans-Regular.ttf","FiraSansCondensed-Regular.ttf"]
    for name in font_names:shutil.copyfile(port/"resources/fonts"/name,app/name)
    info["UIAppFonts"]=font_names
    alternates=[]
    for i in range(1,5):
        source=icon.parent/f"icon-{i}.png"
        if not source.is_file():raise ValueError("Approved alternate ReMasked icon is missing")
        folder=catalog.parent/f"AppIcon{i}.appiconset";folder.mkdir(exist_ok=True)
        shutil.copyfile(source,folder/"ReMasked.png")
        shutil.copyfile(source,app/f"ReMaskedIcon{i}.png")
        (folder/"Contents.json").write_text(json.dumps({"images":[{"filename":"ReMasked.png","idiom":"universal","platform":"ios","size":"1024x1024"}],"info":{"author":"ReMasked","version":1}}))
        alternates.extend(["--alternate-app-icon",f"AppIcon{i}"])
    sdk="iphonesimulator" if "iPhoneSimulator" in info.get("CFBundleSupportedPlatforms",[]) else "iphoneos"
    partial=build/"ios-icon-info.plist"
    subprocess.run(["xcrun","actool",str(catalog.parent),"--compile",str(app),"--platform",sdk,"--minimum-deployment-target","17.0","--target-device","iphone","--target-device","ipad","--app-icon","AppIcon","--output-partial-info-plist",str(partial),*alternates],check=True)
    info.update(plistlib.loads(partial.read_bytes()))
    info["CFBundleVersion"]="15"
    info.update(UIFileSharingEnabled=True,LSSupportsOpeningDocumentsInPlace=True)
    info.update(CFBundleDisplayName="Majora ReMasked",CFBundleName="Majora ReMasked",NSMotionUsageDescription="Use device motion for in-game aiming.")
    (app/"Info.plist").write_bytes(plistlib.dumps(info))
    (app/"libshaderc.a").unlink(missing_ok=True)
    if sdk=="iphonesimulator":subprocess.run(["codesign","--force","--sign","-","--timestamp=none",str(app)],check=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ["build-root","title-build","molten-root","dependency-cache","game-content","owner-content","icon"]:p.add_argument("--"+name,type=Path,required=True)
    p.add_argument("--prepared-models",type=Path);p.add_argument("--a32-registry",type=Path,required=True);p.add_argument("--device",action="store_true");p.add_argument("--stage-only",action="store_true")
    a=p.parse_args();root=Path(__file__).resolve().parents[1];b=a.build_root.resolve();b.mkdir(parents=True,exist_ok=True)
    if not os.environ.get("DEVELOPER_DIR"):p.error("Set DEVELOPER_DIR to the chosen Xcode")
    sdk="iphoneos" if a.device else "iphonesimulator";sdk_path=subprocess.check_output(["xcrun","--sdk",sdk,"--show-sdk-path"],text=True).strip()
    args=["cmake","-S",str(root/"runtime"),"-B",str(b),"-G","Ninja","-DCMAKE_SYSTEM_NAME=iOS","-DCMAKE_OSX_SYSROOT="+sdk_path,"-DCMAKE_OSX_ARCHITECTURES=arm64","-DCMAKE_OSX_DEPLOYMENT_TARGET=17.0","-DCMAKE_BUILD_TYPE=Release","-DREMASKED_TITLE_BUILD="+str(a.title_build.resolve()),"-DREMASKED_MOLTENVK_ROOT="+str(a.molten_root.resolve()),"-DREMASKED_MOLTENVK_SLICE="+("ios-arm64" if a.device else "ios-arm64_x86_64-simulator"),"-DREMASKED_OWNER_CONTENT="+str(a.owner_content.resolve()),"-DOOT3D_A32_AOT_GENERATED_DIR="+str(a.a32_registry.resolve())]
    for d in a.dependency_cache.resolve().iterdir():
        if d.is_dir() and d.name.endswith("-src"):args.append("-DFETCHCONTENT_SOURCE_DIR_"+d.name[:-4].upper()+"="+str(d))
    nri=a.dependency_cache.resolve().parent/"dependency-sources/nri"
    if nri.exists():args.append("-DFETCHCONTENT_SOURCE_DIR_NRI="+str(nri))
    if not a.stage_only:
        for name,command in [("configure",args),("build",["cmake","--build",str(b),"--target","oot3d_native_game","-j","4"])]:
            with (b/("ios-"+name+".log")).open("w") as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    app=b/"product/ReMaskedNativeGame.app"
    stage(app,a.game_content.resolve(),a.icon.resolve(),b,a.prepared_models.resolve() if a.prepared_models else None)
    print("Private native iOS game staged. No device installed or remote resources written.")
if __name__=="__main__":main()
