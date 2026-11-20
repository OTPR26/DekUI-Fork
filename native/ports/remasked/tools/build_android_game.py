#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the separate private Android game prototype. Never installs or publishes.

The supplied static title contains owner-generated AOT code. This APK is a
private device-test product, not a redistributable release candidate.
"""
import argparse,os,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['build-root','title-build','a32-registry','sdk','gradle','python','java-home','dependency-cache']:
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--adrenotools-source',type=Path)
    args=p.parse_args();build=args.build_root.resolve();build.mkdir(parents=True,exist_ok=True)
    if build.is_relative_to(ROOT):p.error('Keep private build products outside this checkout')
    env=os.environ.copy();env.update(ANDROID_HOME=str(args.sdk.resolve()),JAVA_HOME=str(args.java_home.resolve()))
    def run(name,command,cwd=ROOT):
        print(name,flush=True)
        with (build/(name+'.log')).open('w') as log:
            subprocess.run([str(x) for x in command],cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    native=build/'game-android'
    configure=['cmake','-S',ROOT/'ports/remasked/runtime/android','-B',native,'-G','Ninja',
        '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_TOOLCHAIN_FILE='+str(args.sdk.resolve()/'ndk/29.0.14206865/build/cmake/android.toolchain.cmake'),
        '-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-29',
        '-DREMASKED_TITLE_BUILD='+str(args.title_build.resolve()),
        '-DOOT3D_A32_AOT_GENERATED_DIR='+str(args.a32_registry.resolve()),
        '-DTRIAEVUM_ANDROID_DEPENDENCY_SOURCE_CACHE='+str(args.dependency_cache.resolve()),'-DCMAKE_POLICY_VERSION_MINIMUM=3.5']
    if args.adrenotools_source:
        sha=subprocess.check_output(['git','-C',str(args.adrenotools_source),'rev-parse','HEAD'],text=True).strip()
        if sha!='8fae8ce254dfc1344527e05301e43f37dea2df80':p.error('Wrong cached adrenotools revision')
        configure+=['-DFETCHCONTENT_SOURCE_DIR_REMASKED_ADRENOTOOLS='+str(args.adrenotools_source.resolve())]
    run('game-android-configure',configure)
    run('game-android-build',['cmake','--build',native,'--target','oot3d_native_game','-j','3'])
    stage=build/'game-android-stage/arm64-v8a';stage.mkdir(parents=True,exist_ok=True)
    ndk=args.sdk.resolve()/'ndk/29.0.14206865/toolchains/llvm/prebuilt'
    host=next(x for x in ndk.iterdir() if x.is_dir());strip=host/'bin/llvm-strip'
    sources=[native/'runtime/product/libTriAevum.so',native/'_deps/sdl2-build/libSDL2.so',
        host/'sysroot/usr/lib/aarch64-linux-android/libc++_shared.so']
    for name in ['libtriaevum_title_bootstrap.so','libremasked_vulkan.so','libremasked_driver.so','libmain_hook.so','libhook_impl.so']:
        found=list(native.rglob(name))
        if len(found)!=1:raise RuntimeError('Ambiguous or missing required library '+name)
        sources.append(found[0])
    for source in sources:
        shutil.copyfile(source,stage/source.name)
        subprocess.run([str(strip),'--strip-debug',str(stage/source.name)],check=True)
    run('game-android-apk-build',[args.gradle.resolve(),'-p',ROOT/'ports/remasked/android-game',':app:assembleDebug','--no-daemon',
        '-PremaskedNativeStage='+str(stage.parent),'-PremaskedSdlSource='+str(args.dependency_cache.resolve()/'sdl2-src'),
        '-PremaskedBuildPython='+str(args.python.resolve())])
    shutil.copyfile(ROOT/'ports/remasked/android-game/app/build/outputs/apk/debug/app-debug.apk',build/'Majora-ReMasked-Android-Native-Private.apk')
    print('Private Android game APK built. No app installed or release published.')
if __name__=='__main__':main()
