// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.overlay;

import android.content.SharedPreferences;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** Physical shoulders map to gameplay actions, independently of native 3DS labels. */
public final class ReMaskedShoulderBindings {
    private ReMaskedShoulderBindings() {}
    public static final int ITEM_LEFT=707, ITEM_RIGHT=708, TARGET=773, SHIELD=774;
    private static final String HOST="InputMapping_HostAxis_";
    private static final String REVERSE="InputMapping_ReverseMapping_";
    private static final String VERSION="InputMapping_ShoulderLayoutVersion";
    private static final Set<String> CODES=new HashSet<>(Arrays.asList("707","708","773","774"));
    public static int button(int id){return id==9?ITEM_LEFT:id==10?ITEM_RIGHT:-1;}
    public static int trigger(int id){return id==4?TARGET:id==5?SHIELD:-1;}
    private static Set<String> routes(SharedPreferences prefs,int key){
        Object value=prefs.getAll().get(HOST+key);Set<String> result=new HashSet<>();
        if(value instanceof Set<?>)for(Object item:(Set<?>)value)result.add(String.valueOf(item));
        else if(value instanceof Integer)result.add(value.toString());
        return result;
    }
    private static void removeRoutes(SharedPreferences prefs,SharedPreferences.Editor edit,Set<String> codes){
        for(Map.Entry<String,?> entry:prefs.getAll().entrySet()){
                String key=entry.getKey();Object value=entry.getValue();
                if(!key.startsWith(HOST))continue;
                if(value instanceof Set<?>){
                    Set<String> keep=new HashSet<>();for(Object item:(Set<?>)value)if(!codes.contains(String.valueOf(item)))keep.add(String.valueOf(item));
                    edit.putStringSet(key,keep);
                }else if(value instanceof Integer&&codes.contains(value.toString())){
                    edit.remove(key);
                    if(key.endsWith("_GuestButton")){
                        String axis=key.substring(0,key.length()-"_GuestButton".length());
                        edit.remove(axis+"_GuestOrientation").remove(axis+"_Inverted");
                    }
                }
            }
    }
    public static void clearShoulder(SharedPreferences prefs,int code,String setting){
        if(!CODES.contains(String.valueOf(code)))throw new IllegalArgumentException("Not a shoulder action");
        SharedPreferences.Editor edit=prefs.edit().remove(setting).remove(REVERSE+setting);
        removeRoutes(prefs,edit,new HashSet<>(Arrays.asList(String.valueOf(code))));edit.apply();
    }
    /** Upgrade only the complete legacy shoulder template; preserve custom profiles. */
    public static boolean migrateLegacy(SharedPreferences prefs){
        if(prefs.getInt(VERSION,0)>=2)return false;
        boolean hasBindings=false;
        for(String key:prefs.getAll().keySet())if((key.startsWith("InputMapping_")&&!key.equals(VERSION))||key.startsWith("button_")){hasBindings=true;break;}
        if(!hasBindings)return false; // Do not suppress first-launch default installation.

        Set<String> left=routes(prefs,102),right=routes(prefs,103);
        boolean legacy=left.contains("773")&&!left.contains("707")&&right.contains("774")&&!right.contains("708")
            &&(HOST+"102").equals(prefs.getString(REVERSE+"button_l",""))
            &&(HOST+"103").equals(prefs.getString(REVERSE+"button_r",""))
            &&(HOST+"104").equals(prefs.getString(REVERSE+"button_zl",""))
            &&(HOST+"105").equals(prefs.getString(REVERSE+"button_zr",""));
        SharedPreferences.Editor edit=prefs.edit().putInt(VERSION,2);
        if(legacy){
            removeRoutes(prefs,edit,CODES);
            String[] settings={"button_zl","button_zr","button_l","button_r"};
            String[] labels={"Button L1","Button R1","Button L2","Button R2"};
            int[] keys={102,103,104,105},codes={ITEM_LEFT,ITEM_RIGHT,TARGET,SHIELD};
            for(int i=0;i<keys.length;i++){
                Set<String> keep=routes(prefs,keys[i]);keep.removeAll(CODES);keep.add(String.valueOf(codes[i]));
                edit.putStringSet(HOST+keys[i],keep).putString(settings[i],labels[i])
                    .putString(REVERSE+settings[i],HOST+keys[i]);
            }
            // Android controllers may report each trigger as a key, an axis, or both.
            for(int i=0;i<2;i++){
                String axis=HOST+(17+i);edit.putInt(axis+"_GuestButton",i==0?TARGET:SHIELD)
                    .putInt(axis+"_GuestOrientation",0).putBoolean(axis+"_Inverted",false);
            }
        }
        if(!edit.commit())throw new IllegalStateException("Cannot save shoulder layout");
        return legacy;
    }
}
