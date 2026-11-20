package io.github.otpr26.remasked.nativegame;

import android.content.Context;
import android.os.Build;
import android.system.Os;
import java.io.File;

// Private, driver-specific renderer report. Does not initialize Vulkan or the game.
final class ReMaskedGraphicsSupport {
    static void configure(Context context) throws Exception {
        String driver=org.triaevum.android.GraphicsDriver.read(context).optString("id","system");
        String key="android-graphics-v1|"+Build.FINGERPRINT+"|"+Build.VERSION.SDK_INT+"|"+driver;
        Os.setenv("REMASKED_GRAPHICS_DEVICE_KEY",key,true);
        Os.setenv("REMASKED_GRAPHICS_CAPABILITIES",new File(context.getFilesDir(),"graphics-capabilities.json").getAbsolutePath(),true);
    }
}
