// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;

import android.os.Bundle;
import android.system.Os;
import java.io.File;
import java.nio.file.Files;
import org.json.JSONObject;
import org.triaevum.android.TriAevumActivity;

/** The same native MM3D game session, isolated from the production app. */
public final class ReMaskedGameActivity extends TriAevumActivity {
    private ReMaskedTouchHost touch;
    @Override protected int defaultMaximumSurfaceShortEdge(){
        int choice=ReMaskedTouchHost.preferences(this).getInt("outputResolution",0);
        return choice==1?720:choice==2?1080:0;
    }
    @Override protected boolean useDonorTouchControls(){return false;}
    private static native void trimTextureMemory(int mib);
    private static native boolean applyPresentationBytes(byte[] json);
    private static native byte[] activePresentationBytes();
    static String activePresentation(){return new String(activePresentationBytes(),java.nio.charset.StandardCharsets.UTF_8);}
    static boolean applyPresentation(String json){
        return json!=null&&applyPresentationBytes(json.getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }
    static native boolean setGameplayTuning(float distance,float walk,float climb,float swim,boolean resetCamera);
    static native void reportAppIconPixels(int index,int width,int height,byte[] rgba);
    static native void reportMenuDisplayMetrics(float density,float height);
    static native void reportTouchPreferences(byte[] values);
    static native byte[] takeTouchPreferences();
    static native boolean nativeMenuVisible();
    static native String checkNativeMenuContract();
    private ReMaskedDualScreen dualScreen;
    @Override protected void onCreate(Bundle state) {
        try {
            ReMaskedMotionSettings.prepare(this);
            ReMaskedGraphicsSupport.configure(this);
            File root=org.triaevum.android.GameDataDirectory.root(this), game=new File(root,"GameContent");
            Os.setenv("TRIAEVUM_DATA_DIRECTORY",root.getAbsolutePath(),true);
            Os.setenv("REMASKED_START_MENU",getIntent().getBooleanExtra("open_settings",false)?"1":"0",true);
            Os.setenv("REMASKED_GAME_PRESENTATION",new File(game,"runtime-presentation.json").getAbsolutePath(),true);
            Os.setenv("REMASKED_BUNDLED_CONTENT",new File(root,"OwnerContent").getAbsolutePath(),true);
            Os.setenv("REMASKED_GAME_HUD_LAYER",new File(game,"RestorationHud").getAbsolutePath(),true);
            Os.setenv("REMASKED_PRESERVE_BUTTON_ART","1",true);
            Os.setenv("REMASKED_ANDROID_TOUCH_SKIN","1",true);
            Os.setenv("REMASKED_ANDROID_DENSITY",Float.toString(getResources().getDisplayMetrics().density),true);
            Os.setenv("TRIAEVUM_NATIVE_TEXTURE_CACHE_MIB","512",true);
            Os.setenv("TRIAEVUM_TEXTURE_IDLE_FRAMES","60",true);
            Os.setenv("TRIAEVUM_NRI_FAILED_PIPELINE_DIR",new File(game,"logs/pipeline-failure").getAbsolutePath(),true);
            Os.setenv("TRIAEVUM_PIPELINE_PREPARE_THREADS","1",true);
            Os.setenv("TRIAEVUM_STARTUP_PART_THREADS","1",true);
            File profile=new File(game,"presentation.json");
            JSONObject settings=new JSONObject(new String(Files.readAllBytes(profile.toPath()),java.nio.charset.StandardCharsets.UTF_8));
            // A previous native serializer could omit these ReMasked keys.
            // Restore only missing keys from the preserved canonical profile.
            File runtimeFile=new File(game,"runtime-presentation.json");
            JSONObject runtime=new JSONObject(new String(Files.readAllBytes(runtimeFile.toPath()),java.nio.charset.StandardCharsets.UTF_8));
            boolean restored=false;
            for(String key:new String[]{"camera_distance","walk_speed","climb_speed","swim_speed"})
                if(!runtime.has(key)&&settings.has(key)){runtime.put(key,settings.get(key));restored=true;}
            if(restored){
                File temporary=new File(game,"runtime-presentation.json.partial");
                Files.write(temporary.toPath(),runtime.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
                Files.move(temporary.toPath(),runtimeFile.toPath(),java.nio.file.StandardCopyOption.ATOMIC_MOVE,java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
            String selected=PlayerAppearance.selected(settings);
            Os.setenv("REMASKED_TALL_PLAYER","0",true);
            Os.unsetenv("REMASKED_GAME_VOICE_LAYER");
            Os.unsetenv("REMASKED_SWORD_ICON");
            Os.unsetenv("REMASKED_GAME_MODEL_TEXTURES");
            Os.setenv("REMASKED_ADULT_TUNIC_COLOR",selected.equals("adult-link")?Integer.toString(settings.optInt("adult_link_tunic_color",0)):"0",true);
            if(!selected.isEmpty()) {
                File installed=new File(root,"load/mm3d-models/"+selected);
                JSONObject report=null;
                if(selected.equals("pak-import")) {
                    report=new JSONObject(new String(Files.readAllBytes(new File(installed,"import.json").toPath()),java.nio.charset.StandardCharsets.UTF_8));
                    if(report.optInt("schema")!=1 || !"pak-import".equals(report.optString("kind")))
                        throw new IllegalStateException("The selected PAK model is incomplete");
                }
                Os.setenv("REMASKED_TALL_PLAYER",PlayerAppearance.usesTallPlayer(selected,report)?"1":"0",true);
                File layer=PlayerAppearance.actorLayer(installed,selected,settings);
                if(!selected.equals("pak-import")){
                    File swordIcon=new File(installed,"master-sword-icon.png");
                    if((selected.equals("dark-link")||layer.getAbsolutePath().contains("/variants/master"))&&swordIcon.isFile()&&swordIcon.length()<=16L*1024*1024)
                        Os.setenv("REMASKED_SWORD_ICON",swordIcon.getAbsolutePath(),true);
                    File voice=new File(installed,"voices/romfs");
                    if(new File(voice,"sound/sequence/JokerSound.bcsar").isFile())Os.setenv("REMASKED_GAME_VOICE_LAYER",voice.getAbsolutePath(),true);
                    File textures=new File(installed,"texture-overrides");
                    if(textures.isDirectory())Os.setenv("REMASKED_GAME_MODEL_TEXTURES",textures.getAbsolutePath(),true);
                }
                if(new File(layer,"actors/zelda2_link_child_new.gar.lzs").isFile())
                    Os.setenv("REMASKED_GAME_MODEL_LAYER",layer.getAbsolutePath(),true);
                else throw new IllegalStateException("The selected player model is incomplete");
            } else Os.unsetenv("REMASKED_GAME_MODEL_LAYER");
        } catch(Exception error) { throw new IllegalStateException("Cannot prepare ReMasked game settings",error); }
        super.onCreate(state);
        if(!mBrokenLibraries)dualScreen=new ReMaskedDualScreen(this);
        if(!mBrokenLibraries){touch=new ReMaskedTouchHost(this,mLayout);mSurface.requestFocus();}
    }
    @Override protected void onNewIntent(android.content.Intent intent){super.onNewIntent(intent);setIntent(intent);if(intent.getBooleanExtra("open_settings",false))org.triaevum.android.NativeHostBridge.INSTANCE.requestPause();}
    @Override protected void onResume(){super.onResume();if(dualScreen!=null)dualScreen.resume();if(touch!=null)touch.resume();}
    @Override protected void onPause(){if(dualScreen!=null)dualScreen.pause();if(touch!=null)touch.pause();super.onPause();}
    @Override protected void onDestroy(){if(dualScreen!=null){dualScreen.close();dualScreen=null;}if(touch!=null){touch.destroy();touch=null;}super.onDestroy();}
    @Override public boolean dispatchKeyEvent(android.view.KeyEvent e){if(touch!=null&&e.getAction()==android.view.KeyEvent.ACTION_DOWN&&ReMaskedTouchHost.isController(e.getSource()))touch.controllerUsed();return super.dispatchKeyEvent(e);}
    @Override public boolean dispatchGenericMotionEvent(android.view.MotionEvent e){if(touch!=null&&ReMaskedTouchHost.controllerMoved(e))touch.controllerUsed();return super.dispatchGenericMotionEvent(e);}
    @Override public boolean dispatchTouchEvent(android.view.MotionEvent e){if(touch!=null&&e.getActionMasked()==android.view.MotionEvent.ACTION_DOWN)touch.touched();return super.dispatchTouchEvent(e);}
    @Override protected void onActivityResult(int request,int result,android.content.Intent data){super.onActivityResult(request,result,data);if(request==org.triaevum.android.GraphicsDriver.PICK_REQUEST){org.triaevum.android.GraphicsDriver.result(data);if(touch!=null)touch.refreshPlatform();}}
    @Override protected void openInstallerPage(String page) {
        if(page.startsWith("remasked-data-")){startActivity(new android.content.Intent(this,ReMaskedDataFolderActivity.class).putExtra("use-app-folder",page.endsWith("default")));return;}
        if(page.startsWith("remasked-save-")){startActivity(new android.content.Intent(this,org.triaevum.android.saves.SavesActivity.class).putExtra("remasked-action",page.endsWith("import")?"import":"export"));return;}
        if(page.equals("saves")){startActivity(new android.content.Intent(this,org.triaevum.android.saves.SavesActivity.class).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));return;}
        if(page.startsWith("remasked-"))startActivity(new android.content.Intent(this,ReMaskedLauncherActivity.class)
            .putExtra("page",page).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK|android.content.Intent.FLAG_ACTIVITY_CLEAR_TOP|android.content.Intent.FLAG_ACTIVITY_SINGLE_TOP));
        else super.openInstallerPage(page);
    }
    @Override public void loadLibraries() {
        // Mesa's default HOME on Android is /data, which this app cannot write.
        // Configure an app-owned, bounded cache before loading the chosen driver.
        try {
            File cache=new File(getCacheDir(),"mesa-shaders");
            if((cache.isDirectory()||cache.mkdirs())&&cache.canWrite()){
                Os.setenv("MESA_SHADER_CACHE_DIR",cache.getAbsolutePath(),true);
                Os.setenv("MESA_SHADER_CACHE_MAX_SIZE","256M",true);
                Os.setenv("MESA_SHADER_CACHE_DISABLE","false",true);
            }else android.util.Log.w("ReMasked","Driver shader cache is unavailable");
        }catch(Exception error){android.util.Log.w("ReMasked","Driver shader cache is unavailable");}
        try {
            org.triaevum.android.GraphicsDriver.load(this,org.triaevum.android.GraphicsDriver.read(this));
            Os.setenv("SDL_VULKAN_LIBRARY",getApplicationInfo().nativeLibraryDir+"/libremasked_vulkan.so",true);
        } catch(Exception|LinkageError error){throw new IllegalStateException("Graphics driver initialization failed",error);}
        super.loadLibraries();
    }
    @Override protected String[] getLibraries() { return new String[]{"SDL2","TriAevum"}; }
    @Override protected String[] getArguments() {
        return new String[]{"--launch-profile",new File(org.triaevum.android.GameDataDirectory.root(this),"GameContent/ReMaskedNativeGame.launch.json").getAbsolutePath()};
    }
    @Override public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        if (!mBrokenLibraries) try {trimTextureMemory(level>=15?16:64);} catch(UnsatisfiedLinkError ignored) {}
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        if(!focused&&touch!=null)touch.cancel();
        super.onWindowFocusChanged(focused);
        if(focused && !mBrokenLibraries) try {trimTextureMemory(192);} catch(UnsatisfiedLinkError ignored) {}
    }
}
