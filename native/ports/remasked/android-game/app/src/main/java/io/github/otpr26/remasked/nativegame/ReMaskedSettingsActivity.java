// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;

import android.app.Activity;
import android.os.Bundle;
import android.content.Intent;
import android.opengl.GLSurfaceView;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import org.json.JSONObject;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** Startup settings use the shared native menu without creating a game session. */
public final class ReMaskedSettingsActivity extends Activity implements GLSurfaceView.Renderer {
    private GLSurfaceView surface;
    private volatile boolean closing;
    private int width,height;
    private int stickDirections;
    private long openedAt;
    private boolean firstFrame=true;
    private static native String create(String root);
    private static native String frame(int width,int height,float density);
    private static native void category(String title);
    private static native void pointer(float x,float y,boolean down);
    private static native void key(int code,boolean down);
    @Override public void onCreate(Bundle state){
        super.onCreate(state);openedAt=android.os.SystemClock.elapsedRealtime();
        try{ReMaskedMotionSettings.prepare(this);}catch(Exception error){fail("Motion settings could not be opened");return;}
        try{System.loadLibrary("SDL2");System.loadLibrary("TriAevum");}
        catch(LinkageError error){fail("Settings could not be opened");return;}
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_STABLE|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        surface=new GLSurfaceView(this);surface.setEGLContextClientVersion(3);surface.setPreserveEGLContextOnPause(true);surface.setRenderer(this);
        surface.setOnTouchListener((v,event)->{int action=event.getActionMasked();if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_MOVE||action==MotionEvent.ACTION_CANCEL){float x=event.getX(),y=event.getY();boolean down=action!=MotionEvent.ACTION_UP&&action!=MotionEvent.ACTION_CANCEL;surface.queueEvent(()->pointer(x,y,down));}return true;});
        setContentView(surface);ReMaskedAppIcons.applyTaskIcon(this);
    }
    private void report(){
        JSONObject values=new JSONObject(ReMaskedTouchHost.preferences(this).getAll());
        try{ReMaskedGraphicsSupport.configure(this);values.put("lowerDisplayAvailable",ReMaskedDualScreen.hasSecondDisplay(this));values.put("appIconSelection",ReMaskedAppIcons.selected(this));values.put("graphicsDriver",org.triaevum.android.GraphicsDriver.status(this));values.put("dataDirectory",org.triaevum.android.GameDataDirectory.root(this).getAbsolutePath());}catch(Exception error){throw new IllegalStateException(error);}
        ReMaskedGameActivity.reportTouchPreferences(values.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }
    @Override public void onSurfaceCreated(GL10 gl,EGLConfig config){
        try{report();ReMaskedAppIcons.reportPreviews(this);String error=create(org.triaevum.android.GameDataDirectory.root(this).getAbsolutePath());if(error!=null)runOnUiThread(()->fail(error));else category(getIntent().getStringExtra("category")==null?"General":getIntent().getStringExtra("category"));}
        catch(Exception|LinkageError error){runOnUiThread(()->fail("Settings could not be opened"));}
    }
    @Override public void onSurfaceChanged(GL10 gl,int w,int h){width=w;height=h;}
    @Override public void onDrawFrame(GL10 gl){
        if(closing)return;
        String event=frame(width,height,getResources().getDisplayMetrics().density);
        if(firstFrame){firstFrame=false;android.util.Log.i("ReMaskedSettings","Ready without game renderer in "+(android.os.SystemClock.elapsedRealtime()-openedAt)+" ms; output "+width+"x"+height);}
        if(event!=null){try{JSONObject result=new JSONObject(event);if(result.optBoolean("close"))closing=true;runOnUiThread(()->receive(result));}catch(Exception error){runOnUiThread(()->fail("Settings could not be saved"));}}
    }
    private void receive(JSONObject result){
        try{
            JSONObject patch=result.getJSONObject("preferences");
            if(patch.length()>0){android.content.SharedPreferences.Editor editor=ReMaskedTouchHost.preferences(this).edit();java.util.Iterator<String> keys=patch.keys();while(keys.hasNext()){String k=keys.next();Object v=patch.get(k);if(k.equals("platformAction")){if(v.equals("remasked-driver-system"))org.triaevum.android.GraphicsDriver.useSystem(this);else if(v.equals("remasked-driver-pick"))org.triaevum.android.GraphicsDriver.chooseInline(this);continue;}if(k.equals("appIconSelection")){ReMaskedAppIcons.select(this,((Number)v).intValue());continue;}if(v instanceof Boolean)editor.putBoolean(k,(Boolean)v);else if(v instanceof Number)editor.putInt(k,((Number)v).intValue());}if(!editor.commit())throw new java.io.IOException("Preferences not saved");if(!closing)surface.queueEvent(this::report);}
            String action=result.optString("route");
            if(!result.optBoolean("close")&&action.startsWith("remasked-save-")){startActivity(new Intent(this,org.triaevum.android.saves.SavesActivity.class).putExtra("remasked-action",action.endsWith("import")?"import":"export"));return;}
            if(result.optBoolean("close")){
                String route=result.optString("route");
                if(route.equals("remasked-data-folder")||route.equals("remasked-data-default"))startActivity(new Intent(this,ReMaskedDataFolderActivity.class).putExtra("use-app-folder",route.endsWith("default")));
                else if(route.startsWith("remasked-save-"))startActivity(new Intent(this,org.triaevum.android.saves.SavesActivity.class).putExtra("remasked-action",route.endsWith("import")?"import":"export"));
                else if(route.equals("saves"))startActivity(new Intent(this,org.triaevum.android.saves.SavesActivity.class));
                else if(!route.isEmpty()&&!route.equals("back"))startActivity(new Intent(this,ReMaskedLauncherActivity.class).putExtra("page",route).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP|Intent.FLAG_ACTIVITY_SINGLE_TOP));
                finish();
            }
        }catch(Exception error){fail("Settings could not be saved");}
    }
    @Override protected void onActivityResult(int request,int result,Intent data){super.onActivityResult(request,result,data);if(request==org.triaevum.android.GraphicsDriver.PICK_REQUEST){org.triaevum.android.GraphicsDriver.result(data);if(surface!=null)surface.queueEvent(this::report);}}
    private void fail(String text){closing=true;new android.app.AlertDialog.Builder(this).setMessage(text).setPositiveButton("Close",(d,w)->finish()).setCancelable(false).show();}
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        if(surface!=null&&(event.getAction()==KeyEvent.ACTION_DOWN||event.getAction()==KeyEvent.ACTION_UP)){int code=event.getKeyCode();if(code==KeyEvent.KEYCODE_VOLUME_UP||code==KeyEvent.KEYCODE_VOLUME_DOWN)return super.dispatchKeyEvent(event);boolean down=event.getAction()==KeyEvent.ACTION_DOWN;surface.queueEvent(()->key(code,down));return true;}return super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event){
        if(surface!=null&&event.getActionMasked()==MotionEvent.ACTION_MOVE){
            float x=event.getAxisValue(MotionEvent.AXIS_X),y=event.getAxisValue(MotionEvent.AXIS_Y);
            float hx=event.getAxisValue(MotionEvent.AXIS_HAT_X),hy=event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            if(Math.abs(hx)>.5f)x=hx;if(Math.abs(hy)>.5f)y=hy;
            int directions=(y<-.5f?1:0)|(y>.5f?2:0)|(x<-.5f?4:0)|(x>.5f?8:0);
            int changed=directions^stickDirections;stickDirections=directions;int[] codes={19,20,21,22};
            for(int i=0;i<4;i++)if((changed&(1<<i))!=0){int code=codes[i];boolean down=(directions&(1<<i))!=0;surface.queueEvent(()->key(code,down));}return true;
        }return super.dispatchGenericMotionEvent(event);
    }
    @Override protected void onPause(){if(surface!=null)surface.onPause();super.onPause();}
    @Override protected void onResume(){super.onResume();
        android.app.ActivityManager manager=(android.app.ActivityManager)getSystemService(ACTIVITY_SERVICE);
        java.util.List<android.app.ActivityManager.RunningAppProcessInfo> processes=manager.getRunningAppProcesses();
        if(processes!=null)for(android.app.ActivityManager.RunningAppProcessInfo process:processes)if(process.uid==android.os.Process.myUid()&&process.processName.equals(getPackageName()+":game")){
            startActivity(new Intent(this,ReMaskedGameActivity.class).putExtra("open_settings",true).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT|Intent.FLAG_ACTIVITY_SINGLE_TOP));closing=true;finish();return;
        }
        if(surface!=null)surface.onResume();
    }
    @Override protected void onDestroy(){super.onDestroy();if(isFinishing())android.os.Process.killProcess(android.os.Process.myPid());}
}
