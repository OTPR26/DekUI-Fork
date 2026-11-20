// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.otpr26.remasked.nativegame;
import android.app.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.graphics.BitmapFactory;
import android.util.AtomicFile;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
/** Launcher aliases keep the current task alive until every app activity is backgrounded. */
final class ReMaskedAppIcons {
    private static AtomicFile file(Context c){return new AtomicFile(new File(c.getFilesDir(),"app-icon-selection"));}
    static int selected(Context c){try{int value=Integer.parseInt(new String(file(c).readFully(),StandardCharsets.UTF_8).trim());return value>=0&&value<5?value:0;}catch(Exception e){return 0;}}
    private static ComponentName component(Context c,int index){return new ComponentName(c,c.getPackageName()+".LauncherIcon"+index);}
    static void select(Activity a,int index) throws IOException {
        if(index<0||index>4)throw new IllegalArgumentException("Unknown app icon");
        // Enable the replacement first; the active launcher alias is retired only
        // after backgrounding, never while unsaved gameplay is on screen.
        a.getPackageManager().setComponentEnabledSetting(component(a,index),PackageManager.COMPONENT_ENABLED_STATE_ENABLED,PackageManager.DONT_KILL_APP);
        AtomicFile store=file(a);FileOutputStream out=store.startWrite();
        try{out.write(Integer.toString(index).getBytes(StandardCharsets.UTF_8));store.finishWrite(out);}catch(IOException e){store.failWrite(out);throw e;}
        applyTaskIcon(a);
    }
    static void applyTaskIcon(Activity a){
        int id=a.getResources().getIdentifier("launcher_icon_"+selected(a),"drawable",a.getPackageName());
        if(id!=0)a.setTaskDescription(new ActivityManager.TaskDescription("Majora ReMasked",BitmapFactory.decodeResource(a.getResources(),id)));
    }
    static void reportPreviews(Activity a){
        for(int i=0;i<5;i++){
            int id=a.getResources().getIdentifier("launcher_icon_"+i,"drawable",a.getPackageName());
            android.graphics.Bitmap bitmap=BitmapFactory.decodeResource(a.getResources(),id);
            int width=bitmap.getWidth(),height=bitmap.getHeight();int[] argb=new int[width*height];bitmap.getPixels(argb,0,width,0,0,width,height);
            byte[] rgba=new byte[argb.length*4];for(int p=0;p<argb.length;p++){
                int v=argb[p];rgba[p*4]=(byte)(v>>>16);rgba[p*4+1]=(byte)(v>>>8);rgba[p*4+2]=(byte)v;rgba[p*4+3]=(byte)(v>>>24);
            }
            ReMaskedGameActivity.reportAppIconPixels(i,width,height,rgba);bitmap.recycle();
        }
    }
    static void reconcile(Context c){
        int selected=selected(c);PackageManager pm=c.getPackageManager();
        ArrayList<PackageManager.ComponentEnabledSetting> changes=new ArrayList<>();
        for(int i=0;i<5;i++){
            int desired=i==selected?PackageManager.COMPONENT_ENABLED_STATE_ENABLED:PackageManager.COMPONENT_ENABLED_STATE_DISABLED;
            if(pm.getComponentEnabledSetting(component(c,i))!=desired){
                if(android.os.Build.VERSION.SDK_INT>=33)changes.add(new PackageManager.ComponentEnabledSetting(component(c,i),desired,PackageManager.DONT_KILL_APP));
                else pm.setComponentEnabledSetting(component(c,i),desired,PackageManager.DONT_KILL_APP);
            }
        }
        if(android.os.Build.VERSION.SDK_INT>=33&&!changes.isEmpty())pm.setComponentEnabledSettings(changes);
    }
}
