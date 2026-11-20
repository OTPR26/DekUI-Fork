// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.otpr26.remasked.nativegame;

import android.app.Instrumentation;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.view.MotionEvent;
import java.util.*;

/** Exercises actual Android vector drawing and gesture ownership in isolated preferences. */
final class TouchControlChecks {
    private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    static void run(Instrumentation test) throws Throwable {
        final Throwable[] failure={null};
        test.runOnMainSync(()->{try{
            SharedPreferences prefs=test.getTargetContext().getSharedPreferences("TouchControlChecks",0);
            prefs.edit().clear().commit();
            try {
                presets();
                settingsStore(test);
                nativeSaves(test);
                appearance(test);
                MotionEvent.PointerProperties pointer=new MotionEvent.PointerProperties();pointer.id=0;
                MotionEvent.PointerCoords coords=new MotionEvent.PointerCoords();
                for(float x:new float[]{0f,.08f,.5f}){
                    coords.setAxisValue(MotionEvent.AXIS_X,x);
                    MotionEvent pad=MotionEvent.obtain(0,1,MotionEvent.ACTION_MOVE,1,new MotionEvent.PointerProperties[]{pointer},new MotionEvent.PointerCoords[]{coords},0,0,1,1,0,0,android.view.InputDevice.SOURCE_JOYSTICK,0);
                    check(ReMaskedTouchHost.controllerMoved(pad)==(x>.2f),"Idle controller must not hide touches; real movement must hide them");pad.recycle();
                }
                Fake bridge=new Fake();
                ReMaskedTouchControls view=new ReMaskedTouchControls(test.getTargetContext(),null,prefs,bridge);
                view.layout(0,0,1920,1080);
                Bitmap bitmap=Bitmap.createBitmap(1920,1080,Bitmap.Config.ARGB_8888);
                Canvas canvas=new Canvas(bitmap);view.draw(canvas);
                check(android.graphics.Color.alpha(bitmap.getPixel(960,540))==0,"Overlay darkens the game outside controls");
                check(bridge.anchors!=null&&bridge.enabled,"HUD anchors not published");
                referenceGeometry(view);
                check(android.graphics.Color.alpha(bitmap.getPixel(Math.round(1920*.62f),Math.round(1080*.62f)))==0,"Camera stick must be hidden at rest");
                check(bridge.anchors[2]>0&&bridge.anchors[14]>0&&bridge.anchors[17]>0,"Action/item bumper anchors missing");
                float ax=bridge.anchors[0]*1920,ay=bridge.anchors[1]*1080;
                touch(view,MotionEvent.ACTION_DOWN,ax,ay);
                check(bridge.buttons.contains("0:true"),"A stolen by floating stick");
                view.cancelInputs();check(bridge.buttons.contains("0:false"),"Cancel left A held");
                float ix=bridge.anchors[12]*1920,iy=bridge.anchors[13]*1080;
                touch(view,MotionEvent.ACTION_DOWN,ix,iy);touch(view,MotionEvent.ACTION_UP,ix,iy);
                check(bridge.buttons.contains("9:true")&&bridge.buttons.contains("9:false"),"Item I bumper press/release missing");
                touch(view,MotionEvent.ACTION_DOWN,800,600);touch(view,MotionEvent.ACTION_MOVE,850,650);
                check(bridge.axes.getOrDefault(0,0)>0&&bridge.axes.getOrDefault(1,0)>0,"Movement stick direction lost");
                view.cancelInputs();check(bridge.axes.get(0)==0&&bridge.axes.get(1)==0,"Stick not neutral after cancel");
                touch(view,MotionEvent.ACTION_DOWN,1200,660);touch(view,MotionEvent.ACTION_MOVE,1250,690);
                check(bridge.axes.getOrDefault(2,0)>0&&bridge.axes.getOrDefault(3,0)>0,"Default right camera stick does not deliver look axes");
                view.cancelInputs();check(bridge.axes.get(2)==0&&bridge.axes.get(3)==0,"Camera stick remains held after cancel");
                prefs.edit().putBoolean("dragLook",true).commit();
                touch(view,MotionEvent.ACTION_DOWN,1200,660);touch(view,MotionEvent.ACTION_MOVE,1250,690);
                check(bridge.axes.getOrDefault(2,0)>0&&bridge.axes.getOrDefault(3,0)>0,"Drag camera must deliver swipe axes");
                bitmap.eraseColor(0);view.draw(canvas);check(android.graphics.Color.alpha(bitmap.getPixel(1200,660))==0,"Drag camera must not draw a stick");
                touch(view,MotionEvent.ACTION_MOVE,1250,690);check(bridge.axes.get(2)==0&&bridge.axes.get(3)==0,"Stationary drag camera must stop");
                view.cancelInputs();prefs.edit().putBoolean("dragLook",false).commit();
                bridge.floating=false;view.refresh();bitmap.eraseColor(0);view.draw(canvas);
                float density=view.getResources().getDisplayMetrics().density;
                float u=Math.max(.82f,Math.min(1.35f,1080/(390*density)))*density;
                check(android.graphics.Color.alpha(bitmap.getPixel(Math.round(91*u),Math.round(1080-91*u)))>0,"Fixed movement stick must remain visible at its anchor");
                check(android.graphics.Color.alpha(bitmap.getPixel(Math.round(1920*.62f),Math.round(1080*.62f)))==0,"Fixed movement mode must not reveal the camera stick");
                touch(view,MotionEvent.ACTION_DOWN,1200,660);
                check(bridge.axes.getOrDefault(2,0)==0&&bridge.axes.getOrDefault(3,0)==0,"Camera stick must start at the finger even with fixed movement mode");
                touch(view,MotionEvent.ACTION_MOVE,1250,690);
                check(bridge.axes.getOrDefault(2,0)>0&&bridge.axes.getOrDefault(3,0)>0,"Fixed movement mode must retain camera input");view.cancelInputs();
                bridge.floating=true;view.refresh();view.draw(canvas);
                view.setEditing(true);view.draw(canvas);touch(view,MotionEvent.ACTION_DOWN,ax,ay);
                touch(view,MotionEvent.ACTION_MOVE,ax-120,ay-80);touch(view,MotionEvent.ACTION_UP,ax-120,ay-80);
                check(prefs.contains("ReMaskedTouch-v1-landscape-0-0-x"),"Edited placement not persisted");
                view.setEditing(false);view.draw(canvas);float moved=bridge.anchors[0];
                check(moved<ax/1920,"Edited control not moved");
                view.resetPlacement();view.draw(canvas);check(Math.abs(bridge.anchors[0]-ax/1920)<.001,"Reset did not restore control");
                prefs.edit().putBoolean("buttonToggle0",false).commit();view.refresh();view.draw(canvas);
                check(bridge.anchors[2]==0,"Hidden action retained HUD anchor");
                check(ReMaskedTouchHost.buttonBit(9)==14&&ReMaskedTouchHost.buttonBit(10)==15,"Bumpers do not route to I/II");
                check(ReMaskedTouchHost.buttonBit(6)==3&&ReMaskedTouchHost.buttonBit(7)==2,"Start/Select route changed");
                bitmap.recycle();
            } finally {prefs.edit().clear().commit();test.getTargetContext().deleteSharedPreferences("TouchControlChecks");}
        } catch(Throwable error){failure[0]=error;}});
        if(failure[0]!=null)throw failure[0];
    }
    private static void referenceGeometry(ReMaskedTouchControls view)throws Exception {
        float density=view.getResources().getDisplayMetrics().density;
        float u=Math.max(.82f,Math.min(1.35f,1080/(390*density)))*density;
        float cy=Math.max(130*u,1080-268*u);
        // ReTuned's landscape default centers and dimensions, with MM3D's
        // additional Select control retained between Settings and Start.
        float[][] expected={
            {0,0,1920-65*u,1080-109*u,44*u,44*u},
            {1,0,1920-109*u,1080-65*u,44*u,44*u},
            {2,0,1920-109*u,1080-153*u,44*u,44*u},
            {3,0,1920-153*u,1080-109*u,44*u,44*u},
            {104,1,47.5f*u,33.25f*u,63*u,34.5f*u},
            {105,1,1920-47.5f*u,33.25f*u,63*u,34.5f*u},
            {9,1,47.5f*u,77.75f*u,63*u,34.5f*u},
            {10,1,1920-47.5f*u,77.75f*u,63*u,34.5f*u},
            {6,1,960+100*u,1080-31*u,76*u,30*u},
            {7,1,960,1080-31*u,76*u,30*u},
            {4,1,960-100*u,1080-31*u,76*u,30*u},
            {-1,0,1920-31*u,1080-31*u,30*u,30*u},
            {11,3,64*u,cy+16*u,32*u,32*u},
            {12,3,64*u,cy+80*u,32*u,32*u},
            {13,3,32*u,cy+48*u,32*u,32*u},
            {14,3,96*u,cy+48*u,32*u,32*u}
        };
        java.lang.reflect.Field field=ReMaskedTouchControls.class.getDeclaredField("controls");field.setAccessible(true);
        java.util.List<?> controls=(java.util.List<?>)field.get(view);
        for(float[] e:expected){boolean found=false;for(Object control:controls){Class<?> type=control.getClass();
            java.lang.reflect.Field id=type.getDeclaredField("id"),shape=type.getDeclaredField("shape"),bounds=type.getDeclaredField("bounds");id.setAccessible(true);shape.setAccessible(true);bounds.setAccessible(true);
            if(id.getInt(control)!=(int)e[0]||shape.getInt(control)!=(int)e[1])continue;
            android.graphics.RectF b=(android.graphics.RectF)bounds.get(control);
            check(Math.abs(b.centerX()-e[2])<.02&&Math.abs(b.centerY()-e[3])<.02&&Math.abs(b.width()-e[4])<.02&&Math.abs(b.height()-e[5])<.02,"ReTuned default geometry differs for control "+(int)e[0]);found=true;break;
        }check(found,"Missing reference control "+(int)e[0]);}
    }
    private static void presets()throws Exception {
        org.json.JSONObject source=new org.json.JSONObject().put("hud_buttons_scale",1.73).put("hud_buttons_flip_peak",1.2)
            .put("hud_diamond_enabled",false).put("hud_diamond_spacing",17.4).put("adult_link_model",true);
        for(int i=0;i<6;i++)source.put("hud_button_"+i+"_x",i-87.9).put("hud_button_"+i+"_y",i+4.5).put("hud_button_"+i+"_scale",1);
        org.json.JSONObject saved=ReMaskedHudSettings.presetPatch(source,1,1);
        java.util.Iterator<String> keys=saved.keys();while(keys.hasNext()){String key=keys.next();source.put(key,saved.get(key));}
        source.put("hud_buttons_scale",2.4).put("hud_button_0_x",99);
        org.json.JSONObject loaded=ReMaskedHudSettings.presetPatch(source,1,0);
        check(loaded.getDouble("hud_buttons_scale")==1.73&&loaded.getDouble("hud_button_0_x")==-87.9,"Preset did not restore saved size/pose");
        check(loaded.getDouble("hud_diamond_spacing")==17.4&&!loaded.getBoolean("hud_diamond_enabled"),"Preset lost diamond settings");
        check(!loaded.has("adult_link_model")&&!loaded.has("hud_button_layout_2_saved"),"Preset changed unrelated model/slot");
        org.json.JSONObject clear=ReMaskedHudSettings.presetPatch(source,1,2);
        check(clear.length()==1&&!clear.getBoolean("hud_button_layout_1_saved"),"Clearing preset changes other fields");
        boolean rejected=false;try{ReMaskedHudSettings.presetPatch(source,2,0);}catch(java.io.IOException expected){rejected=true;}
        check(rejected,"Empty preset loaded");
    }
    private static void settingsStore(Instrumentation test)throws Exception {
        java.io.File directory=new java.io.File(test.getTargetContext().getCacheDir(),"settings-patch-check-"+System.nanoTime());
        try{
            java.io.File game=new java.io.File(directory,"GameContent");game.mkdirs();
            org.json.JSONObject original=new org.json.JSONObject().put("version",1).put("adult_link_model",true)
                .put("future_model_options",new org.json.JSONObject().put("name","Link — 日本語 🎮")).put("hud_buttons_scale",1.73);
            for(String name:new String[]{"presentation.json","runtime-presentation.json"})
                java.nio.file.Files.write(new java.io.File(game,name).toPath(),original.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            android.content.Context context=new android.content.ContextWrapper(test.getTargetContext()){
                @Override public java.io.File getExternalFilesDir(String type){return directory;}
                @Override public java.io.File getFilesDir(){java.io.File isolated=new java.io.File(directory,"private");isolated.mkdirs();return isolated;}
            };
            org.json.JSONObject runtime=ReMaskedSettingsStore.patch(context,new org.json.JSONObject().put("player_hud_hearts_x",17));
            check(runtime.getInt("player_hud_hearts_x")==17&&runtime.getBoolean("adult_link_model"),"HUD patch changed model selection");
            for(String name:new String[]{"presentation.json","runtime-presentation.json"}){
                org.json.JSONObject saved=new org.json.JSONObject(new String(java.nio.file.Files.readAllBytes(new java.io.File(game,name).toPath()),java.nio.charset.StandardCharsets.UTF_8));
                check(saved.getInt("player_hud_hearts_x")==17&&saved.getDouble("hud_buttons_scale")==1.73,"HUD patch must change only the requested field");
                check(saved.getJSONObject("future_model_options").getString("name").equals("Link — 日本語 🎮"),"Unknown/Unicode model fields lost");
            }
            // Only this instrumentation process loads the library; it never
            // starts SDL's game loop or touches a user's live game process.
            System.loadLibrary("SDL2");System.loadLibrary("TriAevum");
            check(ReMaskedGameActivity.applyPresentation(runtime.toString()),"Native presentation JNI rejected valid settings");
            check(ReMaskedGameActivity.applyPresentation("{\"damage_multiplier\":4}"),"Cannot set isolated native difficulty fixture");
            check(ReMaskedGameActivity.applyPresentation("{\"player_hud_hearts_x\":21}"),"HUD-only patch rejected");
            org.json.JSONObject active=new org.json.JSONObject(ReMaskedGameActivity.activePresentation());
            check(active.getInt("damage_multiplier")==4&&active.getInt("player_hud_hearts_x")==21,"HUD edit clobbered an unrelated live setting");
            String retained=ReMaskedGameActivity.activePresentation();
            check(!ReMaskedGameActivity.applyPresentation("[]"),"Native presentation JNI accepted malformed settings");
            check(ReMaskedGameActivity.activePresentation().equals(retained),"Rejected patch changed native settings");
            check(!ReMaskedGameActivity.setGameplayTuning(Float.NaN,1,1,1,false),"Live gameplay accepted NaN");
            check(!ReMaskedGameActivity.setGameplayTuning(1,1,4,1,false),"Live gameplay accepted invalid speed");
            check(ReMaskedGameActivity.setGameplayTuning(1.56f,1.52f,1.94f,2.13f,true),"Live gameplay JNI rejected valid settings");
            String oldPath=android.system.Os.getenv("REMASKED_GAME_PRESENTATION");
            try{
                android.system.Os.setenv("REMASKED_GAME_PRESENTATION",new java.io.File(game,"runtime-presentation.json").getAbsolutePath(),true);
                String failure=ReMaskedGameActivity.checkNativeMenuContract();check(failure==null,"Native menu contract: "+failure);
            }finally{if(oldPath==null)android.system.Os.unsetenv("REMASKED_GAME_PRESENTATION");else android.system.Os.setenv("REMASKED_GAME_PRESENTATION",oldPath,true);}

        }finally{remove(directory);}
    }
    private static void nativeSaves(Instrumentation test)throws Exception {
        java.io.File directory=new java.io.File(test.getTargetContext().getCacheDir(),"native-save-check-"+System.nanoTime());
        org.triaevum.android.saves.SaveArchive archive=org.triaevum.android.saves.SaveArchive.INSTANCE;
        try{
            java.io.File saves=new java.io.File(directory,"GameContent/saves");saves.mkdirs();
            org.triaevum.android.saves.SaveLayout layout=new org.triaevum.android.saves.SaveLayout(directory,saves,new java.io.File(directory,"states"),true);
            byte[] bytes=new byte[0x13618];bytes[0]=0x4d;bytes[1]=0x4a;bytes[2]=0x52;bytes[3]=8;
            int crc=0x1017;for(int i=0;i<bytes.length;i++){crc^=bytes[i]&255;for(int b=0;b<8;b++)crc=(crc>>>1)^((crc&1)!=0?0xa001:0);}
            bytes[0x1a88]=(byte)crc;bytes[0x1a89]=(byte)(crc>>>8);
            java.io.File slot=new java.io.File(saves,"save0.bin");java.nio.file.Files.write(slot.toPath(),bytes);
            java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream();archive.write(archive.collect(layout,false),out,index->kotlin.Unit.INSTANCE);
            org.triaevum.android.saves.ImportPlan plan=archive.stage(new java.io.ByteArrayInputStream(out.toByteArray()),layout);
            check(plan.getSaveFiles()==1&&plan.getFiles().get(0).getRelative().equals("save0.bin"),"Native save export/import uses wrong folder or names");archive.discard(plan);
            bytes[200]^=1;java.nio.file.Files.write(slot.toPath(),bytes);out=new java.io.ByteArrayOutputStream();archive.write(archive.collect(layout,false),out,index->kotlin.Unit.INSTANCE);
            boolean rejected=false;try{archive.stage(new java.io.ByteArrayInputStream(out.toByteArray()),layout);}catch(Exception expected){rejected=expected instanceof java.io.IOException;}
            check(rejected,"Damaged native save accepted even though ZIP manifest is valid");
        }finally{remove(directory);}
    }
    private static void appearance(Instrumentation test)throws Exception {
        java.io.File directory=new java.io.File(test.getTargetContext().getCacheDir(),"appearance-check-"+System.nanoTime());
        try {
            for(String variant:new String[]{"romfs","variants/master/romfs","variants/master-blue/romfs","variants/kokiri/romfs"}){
                java.io.File actor=new java.io.File(directory,variant+"/"+PlayerAppearance.ACTOR);actor.getParentFile().mkdirs();
                if(!actor.createNewFile())throw new AssertionError("Cannot create isolated appearance fixture");
            }
            org.json.JSONObject settings=new org.json.JSONObject().put("adult_link_model",true).put("adult_link_master_sword",true).put("adult_link_tunic_color",1);
            check(PlayerAppearance.selected(settings).equals("adult-link"),"Adult selection missing");
            check(PlayerAppearance.actorLayer(directory,"adult-link",settings).getPath().endsWith("variants/master-blue/romfs"),"Saved sword/blue tunic ignored");
            settings.put("adult_link_tunic_color",2);
            check(PlayerAppearance.actorLayer(directory,"adult-link",settings).getPath().endsWith("variants/master/romfs"),"Missing variant fallback changed");
            settings.put("adult_link_master_sword",false);
            check(PlayerAppearance.actorLayer(directory,"adult-link",settings).getPath().endsWith("variants/kokiri/romfs"),"Kokiri sword ignored");
            settings.put("pak_link_model",true);check(PlayerAppearance.selected(settings).equals("pak-import"),"PAK must take priority over built-in models");
            org.json.JSONObject report=new org.json.JSONObject().put("schema",1).put("kind","pak-import");
            check(!PlayerAppearance.usesTallPlayer("",report),"Original model grew");
            check(PlayerAppearance.usesTallPlayer("adult-link",null),"Built-in adult height lost");
            for(int form:new int[]{0,1,4,104}){
                report.put("source_form",form);
                check(PlayerAppearance.usesTallPlayer("pak-import",report)==(form==0),"Legacy PAK stature changed");
            }
            report.put("source_form",4).put("model_stature","adult");
            check(PlayerAppearance.usesTallPlayer("pak-import",report),"Adult manifest metadata ignored");
            report.put("source_form",0).put("model_stature","child");
            check(!PlayerAppearance.usesTallPlayer("pak-import",report),"Explicit child model grew");
            check(!PlayerAppearance.usesTallPlayer("pak-import",null),"Absent metadata enabled tall scaling");
            report.remove("model_stature");
            for(Object invalid:new Object[]{"0",0.0,false,org.json.JSONObject.NULL}){
                report.put("source_form",invalid);
                check(!PlayerAppearance.usesTallPlayer("pak-import",report),"Malformed metadata enabled tall scaling");
            }
            check(PlayerAppearance.actorLayer(directory,"pak-import",settings).equals(new java.io.File(directory,"romfs")),"PAK actor layer changed");
        } finally {remove(directory);}
    }
    private static void remove(java.io.File f){java.io.File[] children=f.listFiles();if(children!=null)for(java.io.File c:children)remove(c);f.delete();}
    private static void touch(ReMaskedTouchControls view,int action,float x,float y){
        MotionEvent event=MotionEvent.obtain(0,android.os.SystemClock.uptimeMillis(),action,x,y,0);
        view.onTouchEvent(event);event.recycle();
    }
    private static final class Fake implements ReMaskedTouchControls.Bridge {
        final List<String> buttons=new ArrayList<>();final Map<Integer,Integer> axes=new HashMap<>();
        float[] anchors;boolean enabled,floating=true;
        public void button(int id,boolean down){buttons.add(id+":"+down);}
        public void axis(int id,int value){axes.put(id,value);}
        public float opacity(){return .28f;}public float size(){return 1;}
        public int state(){return 1|4|(floating?64:0)|128|256;}public void enabled(boolean value){}
        public void hud(float[] a,boolean on){anchors=a;enabled=on;}
    }
}
