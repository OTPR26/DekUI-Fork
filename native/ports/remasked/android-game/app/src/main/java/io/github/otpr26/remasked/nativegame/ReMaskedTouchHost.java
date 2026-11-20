// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.otpr26.remasked.nativegame;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.view.ViewGroup;
import android.widget.*;
import org.triaevum.android.NativeInputBridge;

/** ReMasked's original touch surface, connected to the native game's virtual pad only. */
final class ReMaskedTouchHost implements ReMaskedTouchControls.Bridge {
    private final Activity activity;
    private final ViewGroup parent;
    private final ViewGroup anchor;
    private final PopupWindow overlay;
    private final SharedPreferences prefs;
    private final NativeInputBridge nativeInput=NativeInputBridge.INSTANCE;
    private final Handler handler=new Handler(Looper.getMainLooper());
    final ReMaskedTouchControls view;
    private final float[] axes=new float[4];
    private boolean hidden, editing, titleDraws, active, menuVisible;
    private int revision, editRequest, resetRequest;
    private LinearLayout editBar;
    private final Runnable poll=new Runnable(){public void run(){
        if(!active)return;
        boolean draws=nativeInput.titleDrawsControls();
        if(draws!=titleDraws){titleDraws=draws;view.refresh();}
        applyRequests();
        boolean menu=ReMaskedGameActivity.nativeMenuVisible();
        if(menu!=menuVisible){menuVisible=menu;cancel();syncVisibility();if(menu)clearAnchors();}
        try{
            org.json.JSONObject patch=new org.json.JSONObject(new String(ReMaskedGameActivity.takeTouchPreferences(),java.nio.charset.StandardCharsets.UTF_8));
            if(patch.length()>0){SharedPreferences.Editor editor=prefs.edit();java.util.Iterator<String> keys=patch.keys();while(keys.hasNext()){String key=keys.next();Object v=patch.get(key);if(key.equals("platformAction")){if(v.equals("remasked-driver-system"))org.triaevum.android.GraphicsDriver.useSystem(activity);else if(v.equals("remasked-driver-pick"))org.triaevum.android.GraphicsDriver.chooseInline(activity);continue;}if(key.equals("appIconSelection")){try{ReMaskedAppIcons.select(activity,((Number)v).intValue());}catch(java.io.IOException error){android.widget.Toast.makeText(activity,"Could not save app icon",android.widget.Toast.LENGTH_LONG).show();}continue;}if(v instanceof Boolean)editor.putBoolean(key,(Boolean)v);else if(v instanceof Number)editor.putInt(key,((Number)v).intValue());}editor.apply();view.refresh();report();}
        }catch(org.json.JSONException error){throw new IllegalStateException(error);}

        // Anchors can be invalidated when the native viewport changes.
        view.invalidate();
        handler.postDelayed(this,150);
    }};
    static SharedPreferences preferences(Context context){
        return context.getSharedPreferences("ReMaskedTouch_preferences",Context.MODE_PRIVATE);
    }
    ReMaskedTouchHost(Activity activity,ViewGroup parent){
        this.activity=activity;this.anchor=parent;this.parent=new FrameLayout(activity);prefs=preferences(activity);
        ReMaskedAppIcons.reportPreviews(activity);
        view=new ReMaskedTouchControls(activity,null,prefs,this);
        this.parent.addView(view,new ViewGroup.LayoutParams(-1,-1));
        // Keep the touch artwork in its own transparent application panel. On
        // the Odin, SDL's resized Vulkan surface can leave the activity window
        // buffer with a zero transform; ordinary sibling Views then receive
        // input but disappear. This panel keeps controller focus in SDL.
        overlay=new PopupWindow(this.parent,-1,-1,false);
        overlay.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(android.graphics.Color.TRANSPARENT));
        overlay.setClippingEnabled(false);overlay.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);
        for(int id:InputDevice.getDeviceIds()){
            InputDevice d=InputDevice.getDevice(id);
            if(d!=null&&!d.isVirtual()&&isController(d.getSources())){controllerUsed();break;}
        }
    }
    static boolean isController(int source){return (source&InputDevice.SOURCE_GAMEPAD)==InputDevice.SOURCE_GAMEPAD
        ||(source&InputDevice.SOURCE_JOYSTICK)==InputDevice.SOURCE_JOYSTICK;}
    static boolean controllerMoved(android.view.MotionEvent event){
        if(!isController(event.getSource()))return false;
        for(int axis:new int[]{android.view.MotionEvent.AXIS_X,android.view.MotionEvent.AXIS_Y,
            android.view.MotionEvent.AXIS_Z,android.view.MotionEvent.AXIS_RZ,
            android.view.MotionEvent.AXIS_HAT_X,android.view.MotionEvent.AXIS_HAT_Y})
            if(Math.abs(event.getAxisValue(axis))>.2f)return true;
        return event.getAxisValue(android.view.MotionEvent.AXIS_LTRIGGER)>.2f||event.getAxisValue(android.view.MotionEvent.AXIS_RTRIGGER)>.2f
            ||event.getAxisValue(android.view.MotionEvent.AXIS_BRAKE)>.2f||event.getAxisValue(android.view.MotionEvent.AXIS_GAS)>.2f;
    }
    public void button(int id,boolean down){
        if(id==4){if(down)settings();return;}
        int bit=buttonBit(id);if(bit<0)return;
        nativeInput.button(1<<bit,down);
        if(down&&prefs.getBoolean("vibration",true))view.performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
    }
    static int buttonBit(int id){
        switch(id){case 0:return 0;case 1:return 1;case 2:return 10;case 3:return 11;
            case 6:return 3;case 7:return 2;case 9:return 14;case 10:return 15;
            case 11:return 6;case 12:return 7;case 13:return 5;case 14:return 4;default:return -1;}
    }
    public void axis(int id,int value){
        if(id==4||id==5){nativeInput.button(1<<(id==4?9:8),value>0);return;}
        if(id<0||id>=4)return;
        axes[id]=Math.max(-1f,Math.min(1f,value/32767f));
        if(id<2)nativeInput.circlePad(axes[0],-axes[1]);else nativeInput.cStick(axes[2],-axes[3]);
    }
    public float opacity(){return prefs.getInt("controlOpacity",28)/100f;}
    public float size(){return (prefs.getInt("controlScale",50)+50)/100f;}
    public int state(){return 1|((!hidden||editing)&&prefs.getBoolean("visible",true)?4:0)
        |(prefs.getInt("faceLayout",0)<<3)|(prefs.getBoolean("floating",true)?64:0)|128
        |(prefs.getBoolean("itemIcons",true)?256:0);}
    public void enabled(boolean value){prefs.edit().putBoolean("visible",value).apply();report();}
    public void hud(float[] anchors,boolean enabled){
        if(!enabled||hidden||menuVisible||editing||view.getWidth()==0||view.getHeight()==0){clearAnchors();return;}
        for(int i=0;i<6;i++){
            float x=anchors[i*3],y=anchors[i*3+1],r=anchors[i*3+2]*.5f;
            float ry=r*view.getWidth()/view.getHeight();
            nativeInput.controlBounds(i,r>0,x-r,y-ry,x+r,y+ry);
        }
        float l=1,t=1,r=0,b=0;boolean present=false;
        for(int i=6;i<10;i++)if(anchors[i*3+2]>0){
            float rx=anchors[i*3+2]*.5f,ry=rx*view.getWidth()/view.getHeight();
            l=Math.min(l,anchors[i*3]-rx);r=Math.max(r,anchors[i*3]+rx);
            t=Math.min(t,anchors[i*3+1]-ry);b=Math.max(b,anchors[i*3+1]+ry);present=true;
        }
        nativeInput.controlBounds(6,present,l,t,r,b);
    }
    private void clearAnchors(){for(int i=0;i<13;i++)nativeInput.controlBounds(i,false,0,0,0,0);}
    private void syncVisibility(){
        view.setVisibility(hidden||menuVisible?android.view.View.INVISIBLE:android.view.View.VISIBLE);
        overlay.setTouchable(!hidden&&!menuVisible);if(overlay.isShowing())overlay.update();
    }
    void controllerUsed(){if(editing||!prefs.getBoolean("hideWithController",true)||hidden)return;
        cancel();hidden=true;syncVisibility();view.refresh();clearAnchors();}
    void touched(){if(menuVisible)return;if(hidden){hidden=false;syncVisibility();view.refresh();}}
    void cancel(){view.cancelInputs();java.util.Arrays.fill(axes,0);nativeInput.releaseAll();}
    void resume(){active=true;anchor.post(()->{if(active&&!overlay.isShowing()){overlay.setTouchable(!hidden&&!menuVisible);overlay.showAtLocation(anchor,android.view.Gravity.TOP|android.view.Gravity.LEFT,0,0);}});report();handler.removeCallbacks(poll);handler.post(poll);}
    void pause(){overlay.dismiss();active=false;handler.removeCallbacks(poll);cancel();clearAnchors();}
    void destroy(){pause();parent.removeView(view);if(editBar!=null)parent.removeView(editBar);}
    void refreshPlatform(){report();}
    private void report(){
        org.json.JSONObject values=new org.json.JSONObject(prefs.getAll());
        try{values.put("lowerDisplayAvailable",ReMaskedDualScreen.hasSecondDisplay(activity));values.put("appIconSelection",ReMaskedAppIcons.selected(activity));values.put("graphicsDriver",org.triaevum.android.GraphicsDriver.status(activity));values.put("dataDirectory",org.triaevum.android.GameDataDirectory.root(activity).getAbsolutePath());}catch(org.json.JSONException error){throw new IllegalStateException(error);}
        ReMaskedGameActivity.reportTouchPreferences(values.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        nativeInput.reportOverlaySettings(revision,prefs.getBoolean("visible",true),
        prefs.getInt("controlOpacity",28),Math.round(size()*100),prefs.getBoolean("vibration",true),prefs.getBoolean("hideWithController",true));}
    private void applyRequests(){
        int[] r=nativeInput.overlayRequests();if(r.length<8)return;
        if(r[0]!=revision){revision=r[0];prefs.edit().putBoolean("visible",r[1]!=0).putInt("controlOpacity",r[2])
            .putInt("controlScale",r[3]-50).putBoolean("vibration",r[4]!=0).putBoolean("hideWithController",r[5]!=0).apply();
            if(r[5]==0)hidden=false;syncVisibility();view.refresh();report();}
        if(r[7]!=resetRequest){resetRequest=r[7];view.resetPlacement();}
        if(r[6]!=editRequest){editRequest=r[6];edit(true);}
    }
    private void edit(boolean value){
        if(value==editing)return;cancel();editing=value;hidden=false;syncVisibility();view.setEditing(value);clearAnchors();
        if(!value){parent.removeView(editBar);editBar=null;return;}
        editBar=new LinearLayout(activity);editBar.setOrientation(LinearLayout.HORIZONTAL);
        Button reset=new Button(activity);reset.setText("Reset placement");reset.setOnClickListener(v->view.resetPlacement());editBar.addView(reset);
        Button done=new Button(activity);done.setText("Done");done.setOnClickListener(v->edit(false));editBar.addView(done);
        parent.addView(editBar,new ViewGroup.LayoutParams(-2,-2));
        editBar.post(()->{if(editBar!=null){editBar.setX((parent.getWidth()-editBar.getWidth())/2f);editBar.setY(parent.getHeight()*.35f);}});
    }
    private void settings(){cancel();nativeInput.requestMenu();}
    static void showPreferences(Activity activity,Runnable changed){
        SharedPreferences p=preferences(activity);
        LinearLayout content=new LinearLayout(activity);content.setOrientation(LinearLayout.VERTICAL);content.setPadding(24,8,24,8);
        for(String[] option:new String[][]{{"visible","Show touch controls"},{"hideWithController","Hide when using a controller"},
                {"floating","Floating movement stick"},{"itemIcons","Show equipped items on controls"},{"vibration","Touch vibration"}}){
            CheckBox box=new CheckBox(activity);box.setText(option[1]);box.setChecked(p.getBoolean(option[0],true));
            box.setOnCheckedChangeListener((v,on)->{p.edit().putBoolean(option[0],on).apply();changed.run();});content.addView(box);
        }
        slider(activity,content,p,"Opacity","controlOpacity",0,100,28,changed);
        slider(activity,content,p,"Control size","controlScale",0,100,50,changed);
        TextView label=new TextView(activity);label.setText("Face button layout");content.addView(label);
        Spinner face=new Spinner(activity);face.setAdapter(new ArrayAdapter<String>(activity,android.R.layout.simple_spinner_dropdown_item,
            new String[]{"Nintendo diamond","Alternate diamond","Compact"}));face.setSelection(p.getInt("faceLayout",0));
        face.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener(){
            public void onNothingSelected(android.widget.AdapterView<?> v){}
            public void onItemSelected(android.widget.AdapterView<?> v,android.view.View row,int i,long id){p.edit().putInt("faceLayout",i).apply();changed.run();}});content.addView(face);
        String[] labels={"A action","B action","X item","Y item","Item I bumper","Item II bumper","Target trigger","Shield trigger","Start / Items menu","Select / App settings","D-pad","Movement stick","Camera stick"};
        TextView individual=new TextView(activity);individual.setText("Individual controls");content.addView(individual);
        int[] scaleIds={700,701,702,703,773,774,707,708,704,705,780,713,718};
        for(int i=0;i<labels.length;i++){
            final int index=i;CheckBox box=new CheckBox(activity);box.setText(labels[i]);box.setChecked(p.getBoolean("buttonToggle"+i,i!=12));
            box.setOnCheckedChangeListener((v,on)->{p.edit().putBoolean("buttonToggle"+index,on).apply();changed.run();});content.addView(box);
            slider(activity,content,p,labels[i]+" size","controlScale-"+scaleIds[i],0,150,50,changed);
        }
        ScrollView scroll=new ScrollView(activity);scroll.addView(content);
        new AlertDialog.Builder(activity).setTitle("ReMasked touch controls").setView(scroll).setPositiveButton("Done",(d,w)->changed.run()).show();
    }
    private static void slider(Activity a,LinearLayout layout,SharedPreferences p,String label,String key,int min,int max,int fallback,Runnable changed){
        TextView text=new TextView(a);SeekBar bar=new SeekBar(a);bar.setMax(max-min);bar.setProgress(p.getInt(key,fallback)-min);
        text.setText(label+": "+(bar.getProgress()+min));layout.addView(text);layout.addView(bar);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){public void onStartTrackingTouch(SeekBar b){}public void onStopTrackingTouch(SeekBar b){}
            public void onProgressChanged(SeekBar b,int value,boolean user){if(!user)return;p.edit().putInt(key,value+min).apply();text.setText(label+": "+(value+min));changed.run();}});
    }
}
