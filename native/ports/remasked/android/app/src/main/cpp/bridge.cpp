// SPDX-License-Identifier: GPL-3.0-or-later
#include "remasked_core.h"
#include <jni.h>
#include <vector>
namespace {RMCore* core(jlong h){return reinterpret_cast<RMCore*>(h);}}
extern "C" JNIEXPORT jlong JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_titleEntries(JNIEnv*,jclass) {
#ifdef REMASKED_NATIVE_TITLE
    return rm_title_entry_count();
#else
    return 0;
#endif
}
extern "C" JNIEXPORT jint JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_rendererDevice(JNIEnv*,jclass) {
#ifdef REMASKED_NATIVE_NRI
    return rm_renderer_device_check();
#else
    return -1;
#endif
}
extern "C" JNIEXPORT jlong JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_create(JNIEnv* e,jclass,jbyteArray data) {
    if(!data)return 0;const auto n=e->GetArrayLength(data);if(n<=0||n>1024*1024)return 0;
    std::vector<char> text(n);e->GetByteArrayRegion(data,0,n,reinterpret_cast<jbyte*>(text.data()));
    return e->ExceptionCheck()?0:reinterpret_cast<jlong>(rm_create(text.data(),text.size()));
}
extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_destroy(JNIEnv*,jclass,jlong h){rm_destroy(core(h));}
extern "C" JNIEXPORT jbyteArray JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_exportConfig(JNIEnv* e,jclass,jlong h){
    const auto n=rm_copy_config(core(h),nullptr,0);if(!n)return nullptr;std::vector<char> text(n);
    if(rm_copy_config(core(h),text.data(),n)!=n)return nullptr;
    auto result=e->NewByteArray(n-1);if(result)e->SetByteArrayRegion(result,0,n-1,reinterpret_cast<jbyte*>(text.data()));return result;
}
extern "C" JNIEXPORT jfloatArray JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_hud(JNIEnv* e,jclass,jlong h,jint index){
    RMHudElement v{};if(!rm_hud_element(core(h),index,&v))return nullptr;
    const float data[]={v.x,v.y,v.width,v.height,v.opacity,float(v.visible)};auto result=e->NewFloatArray(6);
    if(result)e->SetFloatArrayRegion(result,0,6,data);return result;
}
extern "C" JNIEXPORT jboolean JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_setHud(JNIEnv* e,jclass,jlong h,jstring key,jdouble value){
    if(!key)return false;const char* k=e->GetStringUTFChars(key,nullptr);if(!k)return false;
    const bool result=rm_set_hud_number(core(h),k,value);e->ReleaseStringUTFChars(key,k);return result;
}
extern "C" JNIEXPORT jfloatArray JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_bounds(JNIEnv* e,jclass,jlong h){
    RMHudElement v{};if(!rm_hud_bounds(core(h),&v))return nullptr;
    const float data[]={v.x,v.y,v.width,v.height};auto result=e->NewFloatArray(4);
    if(result)e->SetFloatArrayRegion(result,0,4,data);return result;
}
extern "C" JNIEXPORT jdouble JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_getHud(JNIEnv* e,jclass,jlong h,jstring key){
    if(!key)return 0;const char* k=e->GetStringUTFChars(key,nullptr);if(!k)return 0;
    const double result=rm_hud_number(core(h),k);e->ReleaseStringUTFChars(key,k);return result;
}
extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_button(JNIEnv*,jclass,jlong h,jint source,jint button,jboolean down){rm_input_source_button(core(h),source,button,down);}
extern "C" JNIEXPORT jint JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_mask(JNIEnv*,jclass,jlong h){return rm_input_mask(core(h));}
extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_release(JNIEnv*,jclass,jlong h){rm_release_inputs(core(h));}
extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_releaseSource(JNIEnv*,jclass,jlong h,jint source){rm_release_source(core(h),source);}
extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativeprototype_NativeBridge_sticks(JNIEnv*,jclass,jlong h,jfloat lx,jfloat ly,jfloat rx,jfloat ry){rm_input_sticks(core(h),lx,ly,rx,ry);}
