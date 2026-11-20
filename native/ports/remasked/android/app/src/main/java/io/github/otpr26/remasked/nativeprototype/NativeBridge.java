// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativeprototype;
final class NativeBridge {
    static { System.loadLibrary("remasked_mobile"); }
    static native long titleEntries();
    static native int rendererDevice();
    static native long create(byte[] json);
    static native void destroy(long handle);
    static native byte[] exportConfig(long handle);
    static native float[] hud(long handle,int index);
    static native float[] bounds(long handle);
    static native boolean setHud(long handle,String key,double value);
    static native double getHud(long handle,String key);
    static native void button(long handle,int source,int button,boolean down);
    static native void sticks(long handle,float lx,float ly,float rx,float ry);
    static native int mask(long handle);
    static native void release(long handle);
    static native void releaseSource(long handle,int source);
}
