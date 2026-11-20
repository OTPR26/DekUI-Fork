/* SPDX-License-Identifier: GPL-2.0-or-later */
package io.github.otpr26.remasked.nativegame;
import android.app.Activity;
import android.app.Presentation;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.*;
import android.widget.*;

/** Independent GPU output and lower-display touch, adapted from ReTuned. */
final class ReMaskedDualScreen implements DisplayManager.DisplayListener {
 private final Activity activity;private final DisplayManager manager;private Presentation lower;
 private final Handler handler=new Handler(Looper.getMainLooper());private boolean closed=false,wanted=false,foreground=true;
 static native void surface(Surface surface);static native boolean displayWanted();
 static native void touch(int x,int y,boolean down);
 static native void size(int width,int height);
 ReMaskedDualScreen(Activity activity){this.activity=activity;manager=(DisplayManager)activity.getSystemService(Activity.DISPLAY_SERVICE);
  manager.registerDisplayListener(this,handler);refresh();
  handler.post(new Runnable(){public void run(){if(closed)return;boolean next=displayWanted();if(next!=wanted){wanted=next;refresh();}handler.postDelayed(this,250);}});
 }
 static int[] mapTouch(float w,float h,float x,float y){
  if(!Float.isFinite(w)||!Float.isFinite(h)||!Float.isFinite(x)||!Float.isFinite(y)||w<=0||h<=0)return null;
  float fitW=Math.min(w,h*4f/3f),fitH=fitW*3f/4f;x-=(w-fitW)/2;y-=(h-fitH)/2;
  if(x<0||y<0||x>=fitW||y>=fitH)return null;
  return new int[]{Math.min(319,(int)(x/fitW*320)),Math.min(239,(int)(y/fitH*240))};
 }
 static boolean hasSecondDisplay(Activity activity){DisplayManager dm=(DisplayManager)activity.getSystemService(Activity.DISPLAY_SERVICE);int main=activity.getWindowManager().getDefaultDisplay().getDisplayId();for(Display d:dm.getDisplays())if(d.getDisplayId()!=main&&d.getState()==Display.STATE_ON)return true;return false;}
 void pause(){foreground=false;refresh();}
 void resume(){foreground=true;refresh();}
 private void refresh(){if(closed)return;Display chosen=null;for(Display d:manager.getDisplays())if(foreground&&wanted&&d.getDisplayId()!=activity.getWindowManager().getDefaultDisplay().getDisplayId()&&d.getState()==Display.STATE_ON){chosen=d;break;}
  if(lower!=null&&(chosen==null||lower.getDisplay().getDisplayId()!=chosen.getDisplayId())){touch(0,0,false);surface(null);size(0,0);lower.dismiss();lower=null;}
  if(lower==null&&chosen!=null){android.util.Log.i("ReMaskedDual","Opening lower display "+chosen.getDisplayId()+" "+chosen.getName());lower=new Lower(activity,chosen);lower.getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE|WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);try{lower.show();}catch(WindowManager.InvalidDisplayException failure){android.util.Log.w("ReMaskedDual","Lower display disappeared",failure);lower=null;touch(0,0,false);surface(null);size(0,0);}}
 }
 void close(){closed=true;manager.unregisterDisplayListener(this);touch(0,0,false);handler.removeCallbacksAndMessages(null);if(lower!=null){touch(0,0,false);surface(null);size(0,0);lower.dismiss();lower=null;}surface(null);}
 public void onDisplayAdded(int id){refresh();}public void onDisplayRemoved(int id){refresh();}public void onDisplayChanged(int id){refresh();}
 private static final class Lower extends Presentation implements SurfaceHolder.Callback {
  private SurfaceView view;private int pointer=-1;
  Lower(Activity a,Display d){super(a,d);}
  protected void onCreate(Bundle b){super.onCreate(b);view=new SurfaceView(getContext());
   FrameLayout frame=new FrameLayout(getContext());frame.addView(view);setContentView(frame);
   getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
   view.getHolder().addCallback(this);view.setOnTouchListener((v,e)->{
    int action=e.getActionMasked();
    if(action==MotionEvent.ACTION_DOWN)pointer=e.getPointerId(0);
    if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL||
       (action==MotionEvent.ACTION_POINTER_UP&&e.getPointerId(e.getActionIndex())==pointer)){
      pointer=-1;touch(0,0,false);return true;
    }
    int index=e.findPointerIndex(pointer);
    int[] point=index<0?null:mapTouch(view.getWidth(),view.getHeight(),e.getX(index),e.getY(index));
    touch(point==null?0:point[0],point==null?0:point[1],point!=null);return true;});
  }
  public void surfaceCreated(SurfaceHolder h){surface(h.getSurface());}
  public void surfaceChanged(SurfaceHolder h,int f,int w,int height){size(view.getWidth(),view.getHeight());surface(h.getSurface());}
  public void surfaceDestroyed(SurfaceHolder h){pointer=-1;touch(0,0,false);surface(null);size(0,0);}
 }
}
