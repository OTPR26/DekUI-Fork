// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativeprototype;
import android.app.Activity;
import android.os.Bundle;
import android.graphics.*;
import android.view.*;
import android.widget.*;
import android.util.AtomicFile;
import android.hardware.input.InputManager;
import java.io.*;
import java.util.*;

public final class MainActivity extends Activity implements InputManager.InputDeviceListener {
    private long core;
    private LayoutPreview preview;
    private TextView status;
    private AtomicFile profile;
    private InputManager inputManager;
    private int activeController=-1;
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        profile=new AtomicFile(new File(getFilesDir(),"native-prototype-presentation.json"));
        try(InputStream input=profile.getBaseFile().exists()?profile.openRead():getAssets().open("presentation.json")) {
            ByteArrayOutputStream data=new ByteArrayOutputStream();byte[] block=new byte[8192];int n;
            while((n=input.read(block))!=-1){if(data.size()+n>1024*1024)throw new IOException("Profile too large");data.write(block,0,n);}
            core=NativeBridge.create(data.toByteArray());
            if(core==0)throw new IOException("Invalid prototype profile");
        } catch(Exception error) {
            TextView message=new TextView(this);message.setText("The prototype profile could not be loaded. It has been left unchanged.");setContentView(message);return;
        }
        inputManager=getSystemService(InputManager.class);
        LinearLayout page=new LinearLayout(this);page.setOrientation(LinearLayout.VERTICAL);page.setPadding(20,8,20,8);
        TextView title=new TextView(this);title.setText("Majora ReMasked · Native Prototype");title.setTextSize(22);page.addView(title);
        TextView note=new TextView(this);note.setText("HUD layout preview · Game rendering and model mods are not connected yet.");page.addView(note);
        preview=new LayoutPreview();page.addView(preview,new LinearLayout.LayoutParams(-1,0,1));
        LinearLayout settings=new LinearLayout(this);
        for(int delta:new int[]{-1,1}) {Button b=new Button(this);b.setText(delta<0?"HUD smaller":"HUD larger");
            b.setOnClickListener(v->{double value=NativeBridge.getHud(core,"player_hud_scale")+delta;if(NativeBridge.setHud(core,"player_hud_scale",value)){save();preview.invalidate();}});settings.addView(b);}
        Button save=new Button(this);save.setText("Save profile");save.setOnClickListener(v->save());settings.addView(save);page.addView(settings);
        status=new TextView(this);status.setText("0.4.4 data is separate. Controller and touch input are available for testing.");page.addView(status);
        setContentView(page);preview.setFocusableInTouchMode(true);preview.requestFocus();
        new Thread(()->{
            long entries=NativeBridge.titleEntries();
            int renderer=NativeBridge.rendererDevice();
            android.util.Log.i("ReMaskedNative", "Static title entries="+entries+", NRI device="+renderer+"; diagnostics only, not game boot");
        },"NativeBackendProbe").start();
    }
    private void save() {
        if(core==0)return;FileOutputStream stream=null;
        try {byte[] bytes=NativeBridge.exportConfig(core);if(bytes==null)throw new IOException();stream=profile.startWrite();stream.write(bytes);profile.finishWrite(stream);status.setText("Prototype profile saved.");}
        catch(IOException error){if(stream!=null)profile.failWrite(stream);status.setText("Could not save the prototype profile; previous file retained.");}
    }
    private void release() {activeController=-1;if(core!=0)NativeBridge.release(core);if(preview!=null){preview.pointers.clear();preview.invalidate();}}
    @Override protected void onResume(){super.onResume();if(inputManager!=null)inputManager.registerInputDeviceListener(this,null);}
    @Override protected void onPause(){if(inputManager!=null)inputManager.unregisterInputDeviceListener(this);release();super.onPause();}
    @Override public void onInputDeviceAdded(int id){}
    @Override public void onInputDeviceChanged(int id){}
    @Override public void onInputDeviceRemoved(int id){if(core!=0&&id==activeController){NativeBridge.releaseSource(core,1);NativeBridge.sticks(core,0,0,0,0);activeController=-1;preview.invalidate();}}
    private boolean activePad(int id){if(activeController<0)activeController=id;return activeController==id;}
    @Override public void onWindowFocusChanged(boolean focus){super.onWindowFocusChanged(focus);if(!focus)release();}
    @Override protected void onDestroy(){release();if(core!=0){NativeBridge.destroy(core);core=0;}super.onDestroy();}
    private int button(int code) {
        return switch(code){case KeyEvent.KEYCODE_BUTTON_A->0;case KeyEvent.KEYCODE_BUTTON_B->1;case KeyEvent.KEYCODE_BUTTON_SELECT->2;case KeyEvent.KEYCODE_BUTTON_START->3;
            case KeyEvent.KEYCODE_DPAD_RIGHT->4;case KeyEvent.KEYCODE_DPAD_LEFT->5;case KeyEvent.KEYCODE_DPAD_UP->6;case KeyEvent.KEYCODE_DPAD_DOWN->7;
            case KeyEvent.KEYCODE_BUTTON_R1->8;case KeyEvent.KEYCODE_BUTTON_L1->9;case KeyEvent.KEYCODE_BUTTON_X->10;case KeyEvent.KEYCODE_BUTTON_Y->11;
            case KeyEvent.KEYCODE_BUTTON_L2->14;case KeyEvent.KEYCODE_BUTTON_R2->15;default->-1;};
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int b=button(event.getKeyCode());boolean pad=event.isFromSource(InputDevice.SOURCE_GAMEPAD)||event.isFromSource(InputDevice.SOURCE_JOYSTICK);
        if(core!=0&&pad&&b>=0&&activePad(event.getDeviceId())&&(event.getAction()==KeyEvent.ACTION_DOWN||event.getAction()==KeyEvent.ACTION_UP)) {
            NativeBridge.button(core,1,b,event.getAction()==KeyEvent.ACTION_DOWN);preview.invalidate();return true;
        }return super.dispatchKeyEvent(event);
    }
    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if(core!=0&&event.isFromSource(InputDevice.SOURCE_JOYSTICK)&&activePad(event.getDeviceId())) {
            NativeBridge.sticks(core,event.getAxisValue(MotionEvent.AXIS_X),-event.getAxisValue(MotionEvent.AXIS_Y),event.getAxisValue(MotionEvent.AXIS_Z),-event.getAxisValue(MotionEvent.AXIS_RZ));return true;
        }return super.onGenericMotionEvent(event);
    }
    private final class LayoutPreview extends View {
        final Paint paint=new Paint(3);final Map<Integer,Integer> pointers=new HashMap<>();
        final String[] labels={"D-pad","Magic","Hearts","Items","Map","Clock","Rupees","Counter","Carrots","Timer","X","Y","I","II","A","B"};
        final int[] buttons={2,3,0,1};final String[] names={"Select","Start","A","B"};
        LayoutPreview(){super(MainActivity.this);setContentDescription("HUD layout preview and Select, Start, A, B controls");}
        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);canvas.drawColor(Color.rgb(15,23,29));float[] bounds=NativeBridge.bounds(core);if(bounds==null)return;
            float scale=Math.min(getWidth()/bounds[2],Math.max(1,getHeight()-70)/bounds[3]);canvas.save();canvas.scale(scale,scale);canvas.translate(-bounds[0],-bounds[1]);
            paint.setTextSize(10);
            for(int i=0;i<16;i++){float[] h=NativeBridge.hud(core,i);if(h==null||h[5]==0)continue;
                paint.setColor(Color.argb((int)(h[4]*210),81,178,150));canvas.drawRoundRect(h[0],h[1],h[0]+h[2],h[1]+h[3],3,3,paint);
                paint.setColor(Color.WHITE);canvas.drawText(labels[i],h[0]+2,h[1]+Math.min(11,h[3]),paint);}
            canvas.restore();paint.setTextSize(18);paint.setColor(Color.WHITE);canvas.drawText("Input: "+Integer.toHexString(NativeBridge.mask(core)),10,getHeight()-52,paint);
            for(int i=0;i<4;i++){RectF box=box(i);paint.setColor(Color.rgb(51,65,80));canvas.drawRoundRect(box,10,10,paint);paint.setColor(Color.WHITE);canvas.drawText(names[i],box.left+12,box.top+27,paint);}
        }
        RectF box(int n){float w=getWidth()/4f;return new RectF(n*w+4,getHeight()-44,(n+1)*w-4,getHeight()-4);}
        void publishTouches(){for(int b:buttons)NativeBridge.button(core,0,b,pointers.containsValue(b));invalidate();}
        @Override public boolean onTouchEvent(MotionEvent event) {
            int action=event.getActionMasked(),index=event.getActionIndex(),id=event.getPointerId(index);
            if(action==MotionEvent.ACTION_CANCEL){pointers.clear();publishTouches();return true;}
            if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP){pointers.remove(id);publishTouches();performClick();return true;}
            if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_POINTER_DOWN){for(int i=0;i<4;i++)if(box(i).contains(event.getX(index),event.getY(index)))pointers.put(id,buttons[i]);publishTouches();return true;}
            return true;
        }
        @Override public boolean performClick(){super.performClick();return true;}
    }
}
