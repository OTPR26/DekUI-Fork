package io.github.otpr26.dekuifork.settingscheck;

import android.app.Instrumentation;
import android.content.Context;
import android.content.ContextWrapper;
import android.os.Bundle;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Collections;
import java.util.Arrays;
import org.json.JSONObject;

/** Runs the actual production persistence class against an isolated fixture. */
public class StoreCheck extends Instrumentation {
    @Override public void onCreate(Bundle args) { super.onCreate(args);start(); }
    @Override public void onStart() {
        Bundle result=new Bundle();File stage=null;
        try {
            Context target=getTargetContext();
            File real=new File(target.getFilesDir(),"dekui/config/mm3d-local/presentation.json");
            byte[] untouched=Files.readAllBytes(real.toPath());
            stage=new File(target.getCacheDir(),"settings-store-check-"+System.nanoTime());
            final File isolated=stage;
            Context fixture=new ContextWrapper(target) { @Override public File getFilesDir() { return isolated; } };
            File config=new File(stage,"dekui/config/mm3d-local/presentation.json");config.getParentFile().mkdirs();
            String baseline="{\"version\":1,\"show_performance\":false,\"player_hud_hearts_x\":-37,\"player_hud_clock_y\":14,\"unknown\":{\"profile\":\"keep\",\"values\":[1,2,3]}}";
            Files.write(config.toPath(),baseline.getBytes(StandardCharsets.UTF_8));
            Class<?> type=target.getClassLoader().loadClass("org.citra.citra_emu.utils.DekuiPresentationStore");
            Object store=type.getConstructor(Context.class).newInstance(fixture);
            type.getMethod("update",java.util.Map.class).invoke(store,Collections.singletonMap("show_performance",true));
            JSONObject changed=new JSONObject(new String(Files.readAllBytes(config.toPath()),StandardCharsets.UTF_8));
            if(!changed.getBoolean("show_performance") || changed.getInt("player_hud_hearts_x")!=-37 || changed.getInt("player_hud_clock_y")!=14 || !changed.getJSONObject("unknown").getString("profile").equals("keep") || changed.getJSONObject("unknown").getJSONArray("values").length()!=3)throw new AssertionError("Unrelated settings changed");
            // A later update must read the latest file rather than rewrite a stale copy.
            changed.put("external_change",123);Files.write(config.toPath(),changed.toString().getBytes(StandardCharsets.UTF_8));
            type.getMethod("update",java.util.Map.class).invoke(store,Collections.singletonMap("android_menu_text_size",20));
            JSONObject fresh=new JSONObject(new String(Files.readAllBytes(config.toPath()),StandardCharsets.UTF_8));
            if(fresh.getInt("external_change")!=123)throw new AssertionError("Stale write");
            for(String bad:new String[]{"not-json","{\"version\":2}"}) {
                Files.write(config.toPath(),bad.getBytes(StandardCharsets.UTF_8));
                boolean rejected=false;
                try { type.getMethod("update",java.util.Map.class).invoke(store,Collections.singletonMap("show_performance",false)); } catch(java.lang.reflect.InvocationTargetException expected) { rejected=true; }
                if(!rejected || !new String(Files.readAllBytes(config.toPath()),StandardCharsets.UTF_8).equals(bad))throw new AssertionError("Invalid settings overwritten");
            }
            if(!Arrays.equals(untouched,Files.readAllBytes(real.toPath())))throw new AssertionError("User settings modified");
            result.putString("stream","PASS: production store preserves HUD/unknown fields, merges latest values, rejects invalid/versioned files; actual user settings byte-for-byte unchanged.\n");
            finish(-1,result);
        } catch(Throwable error) { result.putString("stream",error.toString());finish(0,result); }
        finally { if(stage!=null)remove(stage); }
    }
    private static void remove(File file) { if(file.isDirectory())for(File child:file.listFiles())remove(child);file.delete(); }
}
