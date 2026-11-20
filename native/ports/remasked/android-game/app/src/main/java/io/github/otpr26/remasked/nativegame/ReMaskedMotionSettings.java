// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.otpr26.remasked.nativegame;
import android.content.Context;
import android.util.AtomicFile;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
final class ReMaskedMotionSettings {
    static boolean normalize(JSONObject controls) throws Exception {
        JSONObject aim=controls.optJSONObject("aim");if(aim==null)return false;
        String source=aim.optString("source","");
        if(!source.equals("controller_gyroscope")&&!source.equals("controller_accelerometer"))return false;
        aim.put("source","controller_motion");return true;
    }
    static void prepare(Context context) throws Exception {
        File path=new File(org.triaevum.android.GameDataDirectory.root(context),"GameContent/oot3d_controls.json");
        if(!path.isFile())return;
        AtomicFile store=new AtomicFile(path);
        JSONObject controls=new JSONObject(new String(store.readFully(),StandardCharsets.UTF_8));
        if(!normalize(controls))return;
        FileOutputStream out=store.startWrite();
        try{out.write((controls.toString(2)+"\n").getBytes(StandardCharsets.UTF_8));store.finishWrite(out);}
        catch(Exception error){store.failWrite(out);throw error;}
    }
}
