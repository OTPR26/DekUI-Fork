// SPDX-License-Identifier: GPL-2.0-or-later
// Touch layout and artwork adapted from Ocarina ReTuned.
package org.citra.citra_emu.overlay;

import android.content.SharedPreferences;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.drawable.Drawable;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import java.util.ArrayList;

/** Native-resolution presentation matching the Ship mobile controller layout. */
public final class ReMaskedTouchControls extends View {
    public interface Bridge {
        void button(int id, boolean down); void axis(int id, int value);
        float opacity(); float size(); int state(); void enabled(boolean value);
        void hud(float[] anchors, boolean enabled);
    }
    private final Bridge bridge;
    private final View owner;
    private final SharedPreferences preferences;
    private boolean editing=false;
    private Control editingControl;
    private float dragX,dragY;
    private String positionKey(Control c) {return "ReMaskedTouch-v1-"+(getWidth()>getHeight()?"landscape":"portrait")+"-"+c.id+"-"+c.shape;}
    private void button(int id,boolean down){bridge.button(id,down);}
    private void axis(int id,int value){bridge.axis(id,value);}
    private float opacity(){return bridge.opacity();}
    private float size(){return bridge.size();}
    private int state(){return bridge.state();}
    private void enabled(boolean value){bridge.enabled(value);}
    private void displayMetrics(float density,float height){}
    public void setEditing(boolean value){cancelInputs();editing=value;editingControl=null;layoutControls();}
    public void resetPlacement(){
        SharedPreferences.Editor edit=preferences.edit();
        String orientation="ReMaskedTouch-v1-"+(getWidth()>getHeight()?"landscape":"portrait")+"-";
        for(String key:preferences.getAll().keySet())if(key.startsWith(orientation))edit.remove(key);
        edit.apply();refresh();
    }
    public void refresh(){cancelInputs();lastState=-1;owner.invalidate();}
    private void publishHud(){
        float[] anchors=new float[10*3];
        for(Control c:controls){
            int slot=c.shape==0&&c.id>=0&&c.id<4?c.id:c.id==9?4:c.id==10?5:c.shape==3&&c.id>=11&&c.id<=14?c.id-5:-1;
            if(slot<0)continue;
            float x=c.bounds.centerX(), y=c.bounds.centerY(), diameter=Math.min(c.bounds.width(),c.bounds.height());
            anchors[slot*3]=x/getWidth();anchors[slot*3+1]=y/getHeight();anchors[slot*3+2]=diameter/getWidth();
        }
        bridge.hud(anchors,itemIcons&&shown&&!editing);
    }
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ArrayList<Control> controls=new ArrayList<>();
    private final SparseArray<Control> fingers=new SparseArray<>();
    private boolean menu=true, shown=true, ready=false, editor=false, floating=true, extras=false, itemIcons=false;
    private int faceLayout=0;
    private float controlSize=1;
    private float unit=1, left=0, right=0, bottom=0, top=0;
    private int lastState=-1;
    private float lastOpacity=-1;
    private ColorMatrixColorFilter opacityFilter;
    private final Drawable cross,crossItems;
    private Drawable image(String name) {return getContext().getDrawable(getResources().getIdentifier("remasked_touch_"+name,"drawable",getContext().getPackageName())).mutate();}
    private final class Control {
        final int id; final String label; final RectF bounds; final int shape;
        final Drawable drawable;
        boolean down; int pointer=-1; float cx,cy,x,y;
        Control(int id,String label,float x,float y,float w,float h,int shape) {
            this.id=id;this.label=label;this.bounds=new RectF(x,y,x+w,y+h);this.shape=shape;
            cx=bounds.centerX();cy=bounds.centerY();
            drawable=shape==2||shape==3?null:image(shape==0?(itemIcons&&id>=0&&id<4?"face_items":"face"):id==104||id==9?(itemIcons&&id==9?"left_items":"left"):id==105||id==10?(itemIcons&&id==10?"right_items":"right"):"system");
        }
        void release() {
            if(shape==2) {axis(id,0);axis(id+1,0);}
            else if(id>=100)axis(id-100,-32768);
            else if(id>=0)button(id,false);
            down=false;pointer=-1;x=y=0;
        }
        void move(float px,float py) {
            float radius=stickRadius(id)*.68f;
            x=(px-cx)/radius;y=(py-cy)/radius;
            float length=(float)Math.hypot(x,y);if(length>1){x/=length;y/=length;}
            axis(id,Math.round(x*32767));axis(id+1,Math.round(y*32767));
        }
    }
    public ReMaskedTouchControls(Context context, View owner, SharedPreferences preferences, Bridge bridge) {
        super(context);this.owner=owner;this.preferences=preferences;this.bridge=bridge;cross=image("cross");crossItems=image("cross_items");setBackgroundColor(android.graphics.Color.TRANSPARENT);
        setFocusable(false);setContentDescription("Touch controller");
        try{displayMetrics(getResources().getDisplayMetrics().density,getResources().getDisplayMetrics().heightPixels);}catch(UnsatisfiedLinkError ignored){}
    }
    public void cancelInputs() {
        for(int i=0;i<fingers.size();i++)fingers.valueAt(i).release();fingers.clear();
    }
    private float individualScale(int nativeId){return (preferences.getInt("controlScale-"+nativeId,50)+50)/100f;}
    private float stickRadius(int id){return 75*unit*individualScale(id==0?713:718);}
    private void add(int id,String label,float x,float y,float w,float h,int shape) {
        int toggle=id>=0&&id<4&&shape==0?id:id==9?4:id==10?5:id==104?6:id==105?7:id==6?8:shape==3?10:shape==2?(id==0?11:12):-1;
        if(toggle>=0&&!preferences.getBoolean("buttonToggle"+toggle,toggle!=12))return;
        if(shape<2&&id>=0){int nativeId=id<4?700+id:id==9?773:id==10?774:id==104?707:id==105?708:id==6?704:705;
            float scale=individualScale(nativeId);x+=w*(1-scale)/2;y+=h*(1-scale)/2;w*=scale;h*=scale;}
        controls.add(new Control(id,label,x,y,w,h,shape));
    }
    private void layoutControls() {
        controls.clear();float w=getWidth(),h=getHeight();
        unit=.85f*controlSize*Math.max(.82f,Math.min(1.35f,h/(390*getResources().getDisplayMetrics().density)))*getResources().getDisplayMetrics().density;
        left=right=bottom=top=16*unit;
        WindowInsets insets=owner.getRootWindowInsets();
        if(insets!=null&&android.os.Build.VERSION.SDK_INT>=30) {
            android.graphics.Insets safe=insets.getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout());
            left+=safe.left;right+=safe.right;bottom+=safe.bottom;top+=safe.top;
        }
        if(editor)return;
        if(shown) {
            {
                if(extras)add(104,"ZL",left,top,84*unit,46*unit,1);
                add(9,"L",left,top+(extras?56:0)*unit,84*unit,46*unit,1);
                if(extras)add(105,"ZR",w-right-84*unit,top,84*unit,46*unit,1);
                add(10,"R",w-right-84*unit,top+(extras?56:0)*unit,84*unit,46*unit,1);
            }
            float fx=w-right-93*unit,fy=h-bottom-93*unit,fs=44*unit;
            if(faceLayout==2) {
                add(0,"A",fx+12*unit-fs*.675f,fy+20*unit-fs*.675f,fs*1.35f,fs*1.35f,0);
                add(1,"B",fx-44*unit-fs/2,fy+28.6f*unit-fs/2,fs,fs,0);
                add(2,"X",fx+44*unit-fs/2,fy-24.2f*unit-fs/2,fs,fs,0);
                add(3,"Y",fx-8*unit-fs/2,fy-44*unit-fs/2,fs,fs,0);
            } else {
                add(0,"A",fx+(faceLayout==0?44:0)*unit-fs/2,fy+(faceLayout==0?0:44)*unit-fs/2,fs,fs,0);
                add(1,"B",fx+(faceLayout==0?0:44)*unit-fs/2,fy+(faceLayout==0?44:0)*unit-fs/2,fs,fs,0);
                add(2,"X",fx+(faceLayout==0?0:-44)*unit-fs/2,fy+(faceLayout==0?-44:0)*unit-fs/2,fs,fs,0);
                add(3,"Y",fx+(faceLayout==0?-44:0)*unit-fs/2,fy+(faceLayout==0?0:-44)*unit-fs/2,fs,fs,0);
            }
            {
                add(6,"Start",w/2+62*unit,h-bottom-30*unit,76*unit,30*unit,1);
            }
            float cross=96*unit*individualScale(780),cx=left,cy=Math.max(top+(extras?114:58)*unit,h-bottom-252*unit);
            add(11,"",cx+cross/3,cy,cross/3,cross/3,3);
            add(12,"",cx+cross/3,cy+2*cross/3,cross/3,cross/3,3);
            add(13,"",cx,cy+cross/3,cross/3,cross/3,3);
            add(14,"",cx+2*cross/3,cy+cross/3,cross/3,cross/3,3);
            {
                // Floating sticks sit behind buttons and do not steal their touches.
                add(0,"",0,0,w*.46f,h,2);
                add(2,"",w*.52f,0,w*.48f,h,2);
            }
        }
        if(shown)add(4,"Settings",w/2-138*unit,h-bottom-30*unit,76*unit,30*unit,1);

        for(Control c:controls) {
            if(c.shape==2){c.cx=c.id==0?left+75*unit:w-right-75*unit;c.cy=h-bottom-75*unit;continue;}
            if(c.shape==3)continue;
            String key=positionKey(c);
            if(preferences.contains(key+"-x"))c.bounds.offsetTo(
                Math.max(left,Math.min(w-right-c.bounds.width(),preferences.getFloat(key+"-x",0)*w)),
                Math.max(top,Math.min(h-bottom-c.bounds.height(),preferences.getFloat(key+"-y",0)*h)));
        }
        // Move the D-pad as one cross, never four independently editable pieces.
        Control up=null;for(Control c:controls)if(c.id==11&&c.shape==3)up=c;
        if(up!=null){String key=positionKey(up);if(preferences.contains(key+"-x")){
            float dx=Math.max(left+up.bounds.width(),Math.min(w-right-up.bounds.width()*2,preferences.getFloat(key+"-x",0)*w))-up.bounds.left;
            float dy=Math.max(top,Math.min(h-bottom-up.bounds.height()*3,preferences.getFloat(key+"-y",0)*h))-up.bounds.top;
            for(Control c:controls)if(c.shape==3)c.bounds.offset(dx,dy);
        }}
        publishHud();
        add(-1,"◉",w-right-30*unit,h-bottom-30*unit,30*unit,30*unit,0);
    }
    @Override protected void onSizeChanged(int w,int h,int ow,int oh) {cancelInputs();try{displayMetrics(getResources().getDisplayMetrics().density,h);}catch(UnsatisfiedLinkError ignored){}layoutControls();}
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        try {
            int current=state();
            float scale=size();
            if(current!=lastState || controlSize!=scale) {
                displayMetrics(getResources().getDisplayMetrics().density,getHeight());
                itemIcons=(current&256)!=0;extras=(current&128)!=0;controlSize=scale;editor=(current&32)!=0;floating=(current&64)!=0;faceLayout=(current>>3)&3;
                cancelInputs();lastState=current;ready=(current&1)!=0;menu=(current&2)!=0;shown=(current&4)!=0;layoutControls();
            }
        } catch(UnsatisfiedLinkError notLoaded) {owner.postInvalidateOnAnimation();return;}
        if(!ready||editor)return;
        // Focus changes and renderer resize can invalidate native anchors without
        // changing the Android controls. Republish against the current viewport.
        publishHud();
        float alpha=opacity();
        if(alpha!=lastOpacity){lastOpacity=alpha;opacityFilter=new ColorMatrixColorFilter(new ColorMatrix(new float[]{1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,alpha/.28f,0}));}
        paint.setTypeface(android.graphics.Typeface.create("sans-serif-medium",android.graphics.Typeface.NORMAL));
        paint.setTextAlign(Paint.Align.CENTER);paint.setTextSize(14*unit);
        for(Control c:controls) {
            if(c.shape==2) {
                if(!c.down){if(!editing&&(floating||c.id!=0))continue;c.cx=left+75*unit;c.cy=getHeight()-bottom-75*unit;}
                paint.setStyle(Paint.Style.FILL);paint.setColor((Math.round(32*alpha/.28f)<<24)|0xffffff);canvas.drawCircle(c.cx,c.cy,stickRadius(c.id),paint);
                paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(2*unit);canvas.drawCircle(c.cx,c.cy,stickRadius(c.id),paint);
                paint.setStyle(Paint.Style.FILL);canvas.drawCircle(c.cx+c.x*stickRadius(c.id)*.68f,c.cy+c.y*stickRadius(c.id)*.68f,stickRadius(c.id)*.32f,paint);continue;
            }
            if(c.shape==3) {
                if(c.id==11) {
                    int cx=Math.round(c.bounds.left-c.bounds.width()),cy=Math.round(c.bounds.top);
                    Drawable dpad=itemIcons?crossItems:cross;
                    dpad.setBounds(cx,cy,Math.round(cx+c.bounds.width()*3),Math.round(cy+c.bounds.height()*3));dpad.setColorFilter(opacityFilter);dpad.draw(canvas);
                }
                continue;
            }
            c.drawable.setState(c.down?new int[]{android.R.attr.state_pressed}:new int[]{});
            c.drawable.setBounds(Math.round(c.bounds.left),Math.round(c.bounds.top),Math.round(c.bounds.right),Math.round(c.bounds.bottom));c.drawable.setColorFilter(opacityFilter);c.drawable.draw(canvas);
            paint.setStyle(Paint.Style.FILL);paint.setColor((Math.min(255,Math.round(37*alpha/.28f))<<24)|0xffffff);
            String label=c.label;
            if(c.shape==3)label=c.id==11?"▲":c.id==12?"▼":c.id==13?"◀":"▶";
            boolean faceIcon=itemIcons&&((c.shape==0&&c.id>=0&&c.id<4)||c.id==9||c.id==10);
            paint.setTextSize((faceIcon?10:14)*unit);
            canvas.drawText(label,faceIcon?c.bounds.left+c.bounds.width()*.8f:c.bounds.centerX(),(faceIcon?c.bounds.top+c.bounds.height()*.8f:c.bounds.centerY())-(paint.ascent()+paint.descent())/2,paint);
        }
        owner.postInvalidateOnAnimation();
    }
    private boolean editTouch(MotionEvent e) {
        if(e.getActionMasked()==MotionEvent.ACTION_DOWN){
            float x=e.getX(),y=e.getY();editingControl=null;
            for(Control c:controls)if(c.id>=0&&c.shape!=2&&c.bounds.contains(x,y)){editingControl=c;break;}
            if(editingControl==null)return true;
            if(editingControl.shape==3)for(Control c:controls)if(c.id==11&&c.shape==3)editingControl=c;
            dragX=x-editingControl.bounds.left;dragY=y-editingControl.bounds.top;
        }else if(e.getActionMasked()==MotionEvent.ACTION_MOVE&&editingControl!=null){
            Control c=editingControl;
            float minX=c.shape==3?left+c.bounds.width():left;
            float maxX=getWidth()-right-(c.shape==3?c.bounds.width()*2:c.bounds.width());
            float maxY=getHeight()-bottom-(c.shape==3?c.bounds.height()*3:c.bounds.height());
            float x=Math.max(minX,Math.min(maxX,e.getX()-dragX)),y=Math.max(top,Math.min(maxY,e.getY()-dragY));
            float dx=x-c.bounds.left,dy=y-c.bounds.top;
            if(c.shape==3){for(Control other:controls)if(other.shape==3)other.bounds.offset(dx,dy);}else c.bounds.offsetTo(x,y);
            owner.invalidate();
        }else if(e.getActionMasked()==MotionEvent.ACTION_UP&&editingControl!=null){
            Control c=editingControl;String key=positionKey(c);
            preferences.edit().putFloat(key+"-x",c.bounds.left/getWidth()).putFloat(key+"-y",c.bounds.top/getHeight()).apply();editingControl=null;
        }else if(e.getActionMasked()==MotionEvent.ACTION_CANCEL)editingControl=null;
        return true;
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        if(!ready||editor)return false;
        if(editing)return editTouch(event);
        int action=event.getActionMasked(),index=event.getActionIndex(),id=event.getPointerId(index);
        if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_POINTER_DOWN) {
            float x=event.getX(index),y=event.getY(index);Control chosen=null;
            // Buttons must win over the full-screen floating-stick touch regions.
            for(Control c:controls)if(c.shape!=2&&c.pointer<0&&c.bounds.contains(x,y)){chosen=c;break;}
            if(chosen==null)for(Control c:controls)if(c.shape==2&&c.pointer<0&&c.bounds.contains(x,y)&&!(menu&&y<getHeight()*.72f)){chosen=c;break;}
            if(chosen==null)return fingers.size()>0;
            if(chosen.id==-1){cancelInputs();enabled(!shown);shown=!shown;layoutControls();return true;}
            chosen.pointer=id;chosen.down=true;fingers.put(id,chosen);
            if(chosen.shape==2){chosen.cx=(floating||chosen.id!=0)?x:(chosen.id==0?left+75*unit:getWidth()-right-75*unit);chosen.cy=(floating||chosen.id!=0)?y:getHeight()-bottom-75*unit;chosen.move(x,y);}
            else if(chosen.id>=100)axis(chosen.id-100,32767);else button(chosen.id,true);
            return true;
        }
        if(action==MotionEvent.ACTION_MOVE) {
            for(int i=0;i<event.getPointerCount();i++) {
                Control c=fingers.get(event.getPointerId(i));if(c!=null&&c.shape==2)c.move(event.getX(i),event.getY(i));
            }
            return fingers.size()>0;
        }
        if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP) {
            Control c=fingers.get(id);if(c!=null){c.release();fingers.remove(id);return true;}
        }
        if(action==MotionEvent.ACTION_CANCEL){cancelInputs();return true;}
        return fingers.size()>0;
    }
}
