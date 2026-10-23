package io.github.otpr26.remasked.touchcheck;
import android.app.Instrumentation;
import android.os.Bundle;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.view.MotionEvent;
import android.view.View;
import org.citra.citra_emu.overlay.ReMaskedTouchControls;
import java.io.FileOutputStream;
import java.util.*;
import org.citra.citra_emu.overlay.ReMaskedShoulderBindings;

public class TouchCheck extends Instrumentation {
    @Override public void onCreate(Bundle arguments){super.onCreate(arguments);start();}
    static void check(boolean ok,String message){if(!ok)throw new AssertionError(message);}
    static class Bridge implements ReMaskedTouchControls.Bridge {
        int bits=1|4|64|128|256; final boolean[] buttons=new boolean[15];final int[] axes=new int[6];
        float[] anchors;boolean hud;
        public void button(int id,boolean down){buttons[id]=down;}
        public void axis(int id,int value){axes[id]=value;}
        public float opacity(){return .5f;}public float size(){return 1;}
        public int state(){return bits;}
        public void enabled(boolean value){bits=value?bits|4:bits&~4;}
        public void hud(float[] values,boolean enabled){anchors=values;hud=enabled;}
    }
    void event(ReMaskedTouchControls v,int action,int[] ids,float[] xs,float[] ys){
        MotionEvent.PointerProperties[] pp=new MotionEvent.PointerProperties[ids.length];
        MotionEvent.PointerCoords[] pc=new MotionEvent.PointerCoords[ids.length];
        for(int i=0;i<ids.length;i++){pp[i]=new MotionEvent.PointerProperties();pp[i].id=ids[i];pp[i].toolType=MotionEvent.TOOL_TYPE_FINGER;
            pc[i]=new MotionEvent.PointerCoords();pc[i].x=xs[i];pc[i].y=ys[i];pc[i].pressure=1;pc[i].size=1;}
        MotionEvent e=MotionEvent.obtain(1,2,action,ids.length,pp,pc,0,0,1,1,0,0,0,0);
        v.onTouchEvent(e);e.recycle();
    }
    void draw(ReMaskedTouchControls v,Bitmap image){image.eraseColor(android.graphics.Color.TRANSPARENT);v.draw(new Canvas(image));}
    void shoulderChecks(){
        SharedPreferences p=getContext().getSharedPreferences("shoulder-check",0);p.edit().clear().commit();
        check(!ReMaskedShoulderBindings.migrateLegacy(p)&&p.getAll().isEmpty(),"Fresh install still receives defaults");
        String h="InputMapping_HostAxis_",r="InputMapping_ReverseMapping_";
        p.edit().putStringSet(h+102,new HashSet<>(Arrays.asList("773")))
            .putStringSet(h+103,new HashSet<>(Arrays.asList("774")))
            .putStringSet(h+104,new HashSet<>(Arrays.asList("773","707","1000")))
            .putStringSet(h+105,new HashSet<>(Arrays.asList("774","708")))
            .putString(r+"button_l",h+102).putString(r+"button_r",h+103)
            .putString(r+"button_zl",h+104).putString(r+"button_zr",h+105)
            .putInt(h+"17_GuestButton",707).putInt(h+"18_GuestButton",708)
            .putString("button_a","custom face").putInt(h+"0_GuestButton",713).commit();
        check(ReMaskedShoulderBindings.migrateLegacy(p),"Legacy shoulder template migrated");
        check(p.getStringSet(h+102,null).equals(new HashSet<>(Arrays.asList("707"))),"L1 item I only");
        check(p.getStringSet(h+103,null).equals(new HashSet<>(Arrays.asList("708"))),"R1 item II only");
        check(p.getStringSet(h+104,null).equals(new HashSet<>(Arrays.asList("773","1000"))),"L2 target and existing hotkey preserved");
        check(p.getStringSet(h+105,null).equals(new HashSet<>(Arrays.asList("774"))),"R2 shield only");
        check(p.getInt(h+"17_GuestButton",-1)==773&&p.getInt(h+"18_GuestButton",-1)==774,"Trigger axes match trigger buttons");
        check(p.getString("button_a","").equals("custom face")&&p.getInt(h+"0_GuestButton",-1)==713,"Face and stick unchanged");
        Map<String,?> once=p.getAll();check(!ReMaskedShoulderBindings.migrateLegacy(p)&&once.equals(p.getAll()),"Restart does not flip bindings again");
        check(ReMaskedShoulderBindings.button(9)==707&&ReMaskedShoulderBindings.button(10)==708&&ReMaskedShoulderBindings.trigger(4)==773&&ReMaskedShoulderBindings.trigger(5)==774,"Touch and hardware shoulder actions agree");
        ReMaskedShoulderBindings.clearShoulder(p,773,"button_l");
        check(!p.contains(h+"17_GuestButton")&&!p.getStringSet(h+104,null).contains("773"),"Rebind removes old axis and key aliases");
        check(p.getStringSet(h+104,null).contains("1000"),"Rebinding preserves unrelated hotkeys");
        p.edit().clear().putStringSet(h+102,new HashSet<>(Arrays.asList("708"))).commit();
        check(!ReMaskedShoulderBindings.migrateLegacy(p)&&p.getStringSet(h+102,null).contains("708"),"Unrecognized custom profile retained");
    }
    void checks()throws Exception {
        shoulderChecks();
        SharedPreferences prefs=getContext().getSharedPreferences("touch-check",0);prefs.edit().clear().apply();
        Bridge b=new Bridge();View owner=new View(getContext());
        ReMaskedTouchControls v=new ReMaskedTouchControls(getContext(),owner,prefs,b);
        v.layout(0,0,1920,1080);Bitmap image=Bitmap.createBitmap(1920,1080,Bitmap.Config.ARGB_8888);draw(v,image);
        check(b.hud,"HUD overlay enabled");
        check(b.anchors.length==30,"Independent anchors for all four D-pad directions");
        for(int i=0;i<10;i++){check(b.anchors[i*3]>0&&b.anchors[i*3]<1,"X anchor fits screen");check(b.anchors[i*3+1]>0&&b.anchors[i*3+1]<1,"Y anchor fits screen");}
        check(b.anchors[6*3+1]<b.anchors[7*3+1],"Up anchor above down");
        check(b.anchors[8*3]<b.anchors[9*3],"Left anchor left of right");
        check(b.anchors[4*3+2]<b.anchors[0*3+2]*1.1f,"Shoulder glyph uses its short edge");
        b.hud=false; b.anchors=null;draw(v,image);
        check(b.hud&&b.anchors!=null,"Unchanged layout republishes anchors after focus or viewport reset");
        float ax=b.anchors[0]*1920,ay=b.anchors[1]*1080;
        event(v,MotionEvent.ACTION_DOWN,new int[]{0},new float[]{ax},new float[]{ay});
        check(b.buttons[0],"A touch goes to A instead of floating stick");check(b.axes[2]==0&&b.axes[3]==0,"Button never moves camera");
        event(v,MotionEvent.ACTION_CANCEL,new int[]{0},new float[]{ax},new float[]{ay});check(!b.buttons[0],"Cancel releases A");
        event(v,MotionEvent.ACTION_DOWN,new int[]{4},new float[]{380},new float[]{920});
        event(v,MotionEvent.ACTION_POINTER_DOWN|(1<<8),new int[]{4,8},new float[]{380,ax},new float[]{920,ay});
        event(v,MotionEvent.ACTION_MOVE,new int[]{4,8},new float[]{450,ax},new float[]{920,ay});
        check(b.axes[0]>0&&b.buttons[0],"Move and A work simultaneously");
        event(v,MotionEvent.ACTION_POINTER_UP|(1<<8),new int[]{4,8},new float[]{450,ax},new float[]{920,ay});
        check(!b.buttons[0]&&b.axes[0]>0,"Releasing A keeps movement finger");v.cancelInputs();check(b.axes[0]==0&&b.axes[1]==0,"Cancel centers movement");
        float before=b.anchors[0];v.setEditing(true);draw(v,image);check(!b.hud,"Editing disables HUD relocation");
        event(v,MotionEvent.ACTION_DOWN,new int[]{0},new float[]{ax},new float[]{ay});
        event(v,MotionEvent.ACTION_MOVE,new int[]{0},new float[]{ax-80},new float[]{ay-40});
        event(v,MotionEvent.ACTION_UP,new int[]{0},new float[]{ax-80},new float[]{ay-40});
        v.setEditing(false);draw(v,image);check(b.anchors[0]<before,"Edited button and HUD anchor move together");
        v.refresh();draw(v,image);check(b.anchors[0]<before,"Placement survives refresh");
        v.resetPlacement();draw(v,image);check(Math.abs(b.anchors[0]-before)<.001,"Reset restores designed layout");
        for(int style=0;style<3;style++){b.bits=(b.bits&~24)|(style<<3);v.refresh();draw(v,image);check(b.hud,"All layouts publish HUD anchors");}
        try(FileOutputStream output=new FileOutputStream(getContext().getFilesDir()+"/touch-preview.png")){image.compress(Bitmap.CompressFormat.PNG,100,output);}
        b.bits&=~4;v.refresh();draw(v,image);check(!b.hud,"Hidden controls restore ordinary HUD");
    }
    @Override public void onStart(){Bundle result=new Bundle();try{
        final Throwable[] error={null};runOnMainSync(()->{try{checks();}catch(Throwable t){error[0]=t;}});
        if(error[0]!=null)throw new RuntimeException(error[0]);result.putString("result","PASS: shoulder migration/persistence/alias cleanup, multitouch, cancellation, bounds, editing, layouts, and HUD anchors");finish(-1,result);
    }catch(Throwable e){result.putString("result",e.toString());finish(0,result);}}
}
