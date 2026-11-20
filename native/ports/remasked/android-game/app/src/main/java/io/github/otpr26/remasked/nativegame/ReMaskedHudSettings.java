// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;
import android.app.*;
import android.widget.*;
import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.function.*;
import org.json.JSONObject;

/** Edits the real native HUD settings; no schematic HUD is substituted for gameplay. */
final class ReMaskedHudSettings {
    interface LiveUpdate {boolean apply(String json);}
    private final Activity activity;
    private final BooleanSupplier canSave;
    private final LiveUpdate live;
    private JSONObject source;
    private ReMaskedHudSettings(Activity a,BooleanSupplier allowed,LiveUpdate callback)throws Exception {
        activity=a;canSave=allowed;live=callback;reload();
    }
    private void reload()throws Exception {
        source=new JSONObject(new String(Files.readAllBytes(new File(org.triaevum.android.GameDataDirectory.root(activity),"GameContent/presentation.json").toPath()),StandardCharsets.UTF_8));
        if(live!=null){
            JSONObject active=new JSONObject(ReMaskedGameActivity.activePresentation());
            Iterator<String> keys=active.keys();while(keys.hasNext()){String key=keys.next();source.put(key,active.get(key));}
        }
    }
    static void show(Activity activity,BooleanSupplier allowed,LiveUpdate callback){
        try{new ReMaskedHudSettings(activity,allowed,callback).menu();}catch(Exception e){Toast.makeText(activity,"Cannot read HUD settings: "+e.getMessage(),Toast.LENGTH_LONG).show();}
    }
    private void message(String text){Toast.makeText(activity,text,Toast.LENGTH_SHORT).show();}
    private void save(JSONObject patch){
        if(!canSave.getAsBoolean()){message("Quit the game before editing its HUD here.");return;}
        if(patch.length()==0)return;
        try{
            JSONObject runtime=ReMaskedSettingsStore.patch(activity,patch);
            if(live!=null&&!live.apply(patch.toString()))throw new IOException("Native HUD settings were rejected");
            reload();message("HUD settings saved.");
        }catch(Exception error){message("Cannot save HUD: "+error.getMessage());}
    }
    private void menu(){
        String[] groups={"Overall HUD","D-pad","Magic meter","Hearts","Buttons group","Minimap group","Clock","Rupees","Minigame counter","Epona carrots","Timer","Action button layout","Layout presets"};
        new AlertDialog.Builder(activity).setTitle("ReMasked HUD").setItems(groups,(d,index)->{
            if(index==0)overall();else if(index<=10)group(index-1,groups[index]);else if(index==11)buttons();else presets();
        }).show();
    }
    private static final class Field {
        String key,label;double min,max,step,initial;boolean toggle,integer,scaleSteps;
        Field(String k,String l,double lo,double hi,double st,double value){key=k;label=l;min=lo;max=hi;step=st;initial=value;integer=st==1;}
        static Field toggle(String k,String l,boolean value){Field f=new Field(k,l,0,1,1,value?1:0);f.toggle=true;return f;}
        static Field scale(String k,String l){Field f=new Field(k,l,-10,10,1,0);f.scaleSteps=true;return f;}
    }
    private void page(String title,List<Field> fields){
        JSONObject patch=new JSONObject();LinearLayout content=new LinearLayout(activity);content.setOrientation(LinearLayout.VERTICAL);content.setPadding(24,12,24,12);
        for(Field field:fields){
            if(field.toggle){
                CheckBox check=new CheckBox(activity);check.setText(field.label);check.setChecked(source.optBoolean(field.key,field.initial!=0));
                check.setOnCheckedChangeListener((view,on)->{try{patch.put(field.key,on);}catch(Exception e){throw new IllegalStateException(e);}});content.addView(check);continue;
            }
            double value=Math.max(field.min,Math.min(field.max,source.optDouble(field.key,field.initial)));
            TextView label=new TextView(activity);label.setText(text(field,value));content.addView(label);
            SeekBar bar=new SeekBar(activity);bar.setMax((int)Math.round((field.max-field.min)/field.step));bar.setProgress((int)Math.round((value-field.min)/field.step));content.addView(bar);
            bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){public void onStartTrackingTouch(SeekBar b){}public void onStopTrackingTouch(SeekBar b){}
                public void onProgressChanged(SeekBar b,int position,boolean user){
                    if(!user)return;double v=field.min+position*field.step;label.setText(text(field,v));
                    try{if(field.integer)patch.put(field.key,(int)Math.round(v));else patch.put(field.key,v);}catch(Exception e){throw new IllegalStateException(e);}
                }});
        }
        ScrollView scroll=new ScrollView(activity);scroll.addView(content);
        new AlertDialog.Builder(activity).setTitle(title).setView(scroll).setNegativeButton("Cancel",null).setPositiveButton("Save",(d,w)->save(patch)).show();
    }
    private String text(Field f,double value){return f.label+": "+(f.scaleSteps?Math.round(100+5*value)+"%":f.integer?Long.toString(Math.round(value)):String.format(Locale.getDefault(),"%.2f",value));}
    private void overall(){
        page("Overall HUD",Arrays.asList(Field.scale("player_hud_scale","HUD size"),new Field("player_hud_opacity","Opacity",0,1,.01,1),
            new Field("player_hud_left_padding","Left margin",-100,100,1,0),new Field("player_hud_right_padding","Right margin",-100,100,1,0),
            new Field("player_hud_top_padding","Top margin",-40,40,1,0),new Field("player_hud_bottom_padding","Bottom margin",-40,40,1,0),
            Field.toggle("player_hud_hide_minimap","Hide minimap",false),Field.toggle("player_hud_hide_menu_prompts","Hide menu prompts",false),
            Field.toggle("player_hud_mirror_item_buttons","Mirror item backgrounds",false),new Field("player_hud_heart_spacing","Heart spacing",-40,40,1,0),
            new Field("player_hud_magic_width","Magic meter width",.5,3,.05,1),Field.scale("player_hud_magic_height","Magic meter height")));
    }
    private void group(int index,String label){
        String[] keys={"dpad","magic","hearts","buttons","minimap","clock","rupees","minigame","carrots","timer"};String prefix="player_hud_"+keys[index]+"_";
        page(label,Arrays.asList(Field.scale(prefix+"scale","Size"),new Field(prefix+"x","Horizontal offset",-400,400,1,0),new Field(prefix+"y","Vertical offset",-240,240,1,0)));
    }
    private void buttons(){
        List<Field> fields=new ArrayList<>();fields.add(new Field("hud_buttons_scale","Action button size",.1,3,.05,1));
        fields.add(new Field("hud_buttons_flip_peak","Flip animation",1,1.2,.01,1.2));fields.add(Field.toggle("hud_diamond_enabled","Diamond layout",false));
        fields.add(new Field("hud_diamond_spacing","Diamond spacing",0,160,.1,24));fields.add(new Field("hud_diamond_rotation","Diamond rotation",-180,180,.1,0));
        fields.add(new Field("hud_diamond_x","Diamond horizontal position",-512,512,.1,350));fields.add(new Field("hud_diamond_y","Diamond vertical position",-240,240,.1,55));
        String[] names={"Action","Item X","Item Y","Sword","Item I","Item II"};
        for(int i=0;i<6;i++){String prefix="hud_button_"+i+"_";fields.add(new Field(prefix+"x",names[i]+" horizontal offset",-512,512,.1,0));
            fields.add(new Field(prefix+"y",names[i]+" vertical offset",-240,240,.1,i>=4?48:0));fields.add(new Field(prefix+"scale",names[i]+" size",.1,3,.05,1));}
        page("Action button layout",fields);
    }
    private void presets(){
        new AlertDialog.Builder(activity).setTitle("Action button presets").setItems(new String[]{"Preset 1","Preset 2","Preset 3"},(d,index)->
            new AlertDialog.Builder(activity).setTitle("Preset "+(index+1)).setItems(new String[]{"Load","Save current layout","Clear"},(dialog,action)->preset(index+1,action)).show()).show();
    }
    static JSONObject presetPatch(JSONObject source,int slot,int action)throws Exception {
        if(slot<1||slot>3||action<0||action>2)throw new IllegalArgumentException("Unknown HUD preset");
        String prefix="hud_button_layout_"+slot+"_";JSONObject patch=new JSONObject();
        if(action==2){patch.put(prefix+"saved",false);return patch;}
        if(action==0&&!Boolean.TRUE.equals(source.opt(prefix+"saved")))throw new IOException("This preset is empty.");
        Map<String,String> pairs=new LinkedHashMap<>();pairs.put("hud_buttons_scale",prefix+"scale");pairs.put("hud_buttons_flip_peak",prefix+"flip_peak");
        for(String key:new String[]{"enabled","x","y","spacing","rotation"})pairs.put("hud_diamond_"+key,prefix+"diamond_"+key);
        for(int i=0;i<6;i++)for(String key:new String[]{"x","y","scale"})pairs.put("hud_button_"+i+"_"+key,prefix+i+"_"+key);
        for(Map.Entry<String,String> pair:pairs.entrySet()){
            String from=action==0?pair.getValue():pair.getKey(),to=action==0?pair.getKey():pair.getValue();
            if(source.has(from))patch.put(to,source.get(from));
        }
        if(action==1)patch.put(prefix+"saved",true);return patch;
    }
    private void preset(int slot,int action){
        try{reload();save(presetPatch(source,slot,action));}catch(Exception error){message("Cannot update preset: "+error.getMessage());}
    }
}
