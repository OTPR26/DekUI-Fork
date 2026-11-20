// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.otpr26.remasked.nativegame;
import java.io.File;
import org.json.JSONObject;
/** Launch-time model selection matching ReMasked 0.4.4's RomFS ordering. */
final class PlayerAppearance {
    static final String ACTOR="actors/zelda2_link_child_new.gar.lzs";
    static String selected(JSONObject settings){
        for(String[] model:new String[][]{{"pak_link_model","pak-import"},{"twilight_link_model","twilight-link"},{"dark_link_model","dark-link"},{"adult_link_model","adult-link"}})
            if(settings.optBoolean(model[0],false))return model[1];
        return "";
    }
    static boolean usesTallPlayer(String selected,JSONObject report){
        if(selected.isEmpty())return false;
        if(!selected.equals("pak-import"))return true;
        if(report==null||report.optInt("schema")!=1||!"pak-import".equals(report.optString("kind")))return false;
        String stature=report.optString("model_stature","");
        if(stature.equals("adult"))return true;
        if(stature.equals("child"))return false;
        // Existing imports already record the zzplayas rig tag; no reimport needed.
        Object form=report.opt("source_form");
        return (form instanceof Integer||form instanceof Long)&&((Number)form).longValue()==0;
    }
    static File actorLayer(File installed,String selected,JSONObject settings){
        if(selected.equals("pak-import")||selected.equals("dark-link"))return new File(installed,"romfs");
        boolean twilight=selected.equals("twilight-link");
        String variant=settings.optBoolean(twilight?"twilight_link_master_sword":"adult_link_master_sword",twilight)?"master":"kokiri";
        int color=twilight?0:settings.optInt("adult_link_tunic_color",0);
        String suffix=color==1?"-blue":color==2?"-red":"";
        for(String path:new String[]{"variants/"+variant+suffix+"/romfs","variants/"+variant+"/romfs","romfs"}){
            File layer=new File(installed,path);if(new File(layer,ACTOR).isFile())return layer;
        }
        return new File(installed,"romfs");
    }
}
