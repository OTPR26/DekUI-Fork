// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativeprototype;
import android.app.Instrumentation;
import android.app.Activity;
import android.os.Bundle;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Iterator;

public final class CoreInstrumentation extends Instrumentation {
    private int checks;
    private boolean requireTitle, requireRenderer;
    private void check(boolean value,String why){checks++;if(!value)throw new AssertionError(why);}
    private void same(Object a,Object b)throws Exception {
        if(a instanceof JSONObject x && b instanceof JSONObject y) {
            check(x.length()==y.length(),"object key count");
            for(Iterator<String> it=x.keys();it.hasNext();) {String k=it.next();check(y.has(k),"key retained");same(x.get(k),y.get(k));}
        } else if(a instanceof JSONArray x && b instanceof JSONArray y) {
            check(x.length()==y.length(),"array length");for(int i=0;i<x.length();i++)same(x.get(i),y.get(i));
        } else if(a instanceof Number x && b instanceof Number y) {
            check(Math.abs(x.doubleValue()-y.doubleValue())<=Math.max(1,Math.abs(x.doubleValue()))*1e-12,"number retained");
        } else check(a.equals(b),"value retained");
    }
    @Override public void onCreate(Bundle arguments){
        super.onCreate(arguments);
        if(arguments!=null){requireTitle="true".equals(arguments.getString("requireNativeTitle"));requireRenderer="true".equals(arguments.getString("requireNativeRenderer"));}
        start();
    }
    @Override public void onStart() {
        Bundle result=new Bundle();long core=0;
        try {
            long titleEntries=NativeBridge.titleEntries();
            int rendererDevice=NativeBridge.rendererDevice();
            if(requireTitle)check(titleEntries==241681,"actual MM3D static title dispatch inventory");
            if(requireRenderer)check(rendererDevice==1,"real NRI Vulkan device and graphics queue");
            result.putLong("native_title_entries",titleEntries);result.putInt("nri_device",rendererDevice);
            byte[] bytes;
            try(InputStream input=getTargetContext().getAssets().open("presentation.json");ByteArrayOutputStream data=new ByteArrayOutputStream()) {
                byte[] block=new byte[8192];int n;while((n=input.read(block))!=-1)data.write(block,0,n);bytes=data.toByteArray();
            }
            JSONObject source=new JSONObject(new String(bytes,StandardCharsets.UTF_8));check(source.length()==586,"baseline");
            source.put("future_model",new JSONObject().put("voice","Adult — 日本語 🎮").put("twilight",true));
            core=NativeBridge.create(source.toString().getBytes(StandardCharsets.UTF_8));check(core!=0,"JNI create");
            same(source,new JSONObject(new String(NativeBridge.exportConfig(core),StandardCharsets.UTF_8)));
            check(NativeBridge.setHud(core,"player_hud_scale",2),"HUD edit");source.put("player_hud_scale",2);
            same(source,new JSONObject(new String(NativeBridge.exportConfig(core),StandardCharsets.UTF_8)));
            check(!NativeBridge.setHud(core,"player_hud_scale",11),"range guard");
            NativeBridge.button(core,0,2,true);check(NativeBridge.mask(core)==4,"SELECT");
            NativeBridge.button(core,1,3,true);check(NativeBridge.mask(core)==12,"START plus SELECT");
            NativeBridge.releaseSource(core,1);check(NativeBridge.mask(core)==4,"controller disconnect preserves touch");
            NativeBridge.button(core,1,3,true);
            NativeBridge.button(core,0,2,false);check(NativeBridge.mask(core)==8,"independent release");
            NativeBridge.release(core);check(NativeBridge.mask(core)==0,"lifecycle release");
            float[] bounds=NativeBridge.bounds(core);check(bounds!=null&&bounds[2]>0&&bounds[3]>0,"HUD bounds");
            for(int i=0;i<16;i++){float[] h=NativeBridge.hud(core,i);check(h!=null&&h[0]>=bounds[0]&&h[1]>=bounds[1]&&h[0]+h[2]<=bounds[0]+bounds[2]&&h[1]+h[3]<=bounds[1]+bounds[3],"HUD fits");}
            result.putString("result","PASS "+checks+" Android JNI/settings/input checks; no gameplay claim");
            finish(Activity.RESULT_OK,result);
        } catch(Throwable error) {
            result.putString("result","FAIL after "+checks+" checks: "+error.toString());finish(Activity.RESULT_CANCELED,result);
        } finally {if(core!=0)NativeBridge.destroy(core);}
    }
}
