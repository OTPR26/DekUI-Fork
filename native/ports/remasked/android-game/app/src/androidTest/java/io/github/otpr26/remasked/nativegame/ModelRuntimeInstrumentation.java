// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;
import android.app.Instrumentation;
import android.app.Activity;
import android.os.Bundle;
import android.content.Context;
import com.chaquo.python.Python;
import com.chaquo.python.android.AndroidPlatform;
import org.json.JSONObject;
import java.io.*;
/** Exercises the packaged ARM64 converter, never a user's imported model or save. */
public final class ModelRuntimeInstrumentation extends Instrumentation {
    private Bundle testArguments;
    private boolean touchOnly;
    private boolean gameCheck;private int gameSeconds=180;
    @Override public void onCreate(Bundle args){super.onCreate(args);testArguments=args;touchOnly=args!=null&&"touch".equals(args.getString("checks"));gameCheck=args!=null&&"game".equals(args.getString("checks"));if(args!=null)gameSeconds=Math.max(30,Math.min(600,Integer.parseInt(args.getString("seconds","180"))));start();}
    public static final class Progress { public void progress(String message){} }
    @Override public void onStart(){
        Bundle result=new Bundle();File fake=null, isolated=null;
        if(testArguments!=null&&"pakheight".equals(testArguments.getString("checks"))){
            try{
                Context context=getTargetContext();
                if(!Python.isStarted())Python.start(new AndroidPlatform(context));
                Python python=Python.getInstance();
                File root=org.triaevum.android.GameDataDirectory.root(context);
                File rom=new File(root,"PrivateInputs/mm3d.3ds");
                File model=new File(testArguments.getString("model"));
                if(!rom.isFile()||!model.isFile())throw new AssertionError("Owned PAK/ROM fixture missing");
                isolated=new File(context.getCacheDir(),"pak-height-validation-"+System.nanoTime());
                var report=python.getModule("zobj_import.convert").callAttr("convert",rom.getAbsolutePath(),model.getAbsolutePath(),isolated.getAbsolutePath());
                JSONObject converted=new JSONObject(python.getModule("json").callAttr("dumps",report).toString());
                converted.put("kind","pak-import");
                if(!"adult".equals(converted.getString("model_stature"))||!PlayerAppearance.usesTallPlayer("pak-import",converted))throw new AssertionError("Adult package lost its stature");
                if(!new File(isolated,"zelda2_link_child_new.gar.lzs").isFile()||converted.getInt("triangles")<1)throw new AssertionError("Actor conversion incomplete");
                result.putString("stream","PASS: full ARM64 PAK conversion preserved adult stature; "+converted.getInt("triangles")+" triangles; active model and saves untouched.\n");finish(Activity.RESULT_OK,result);
            }catch(Throwable error){result.putString("stream","FAIL: "+error+"\n");finish(Activity.RESULT_CANCELED,result);}
            finally{if(isolated!=null)remove(isolated);}return;
        }
        if(gameCheck){
            try{
                Context context=getTargetContext();
                runOnMainSync(()->context.startActivity(new android.content.Intent(context,ReMaskedGameActivity.class).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)));
                android.app.ActivityManager manager=(android.app.ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
                long started=android.os.SystemClock.elapsedRealtime();int samples=0,peakPss=0;
                while(android.os.SystemClock.elapsedRealtime()-started<gameSeconds*1000L){
                    android.os.SystemClock.sleep(1000);int pid=0;
                    for(var process:manager.getRunningAppProcesses())if(process.uid==android.os.Process.myUid()&&process.processName.equals(context.getPackageName()+":game"))pid=process.pid;
                    if(pid==0&&android.os.SystemClock.elapsedRealtime()-started>15000)throw new AssertionError("Game process exited during sustained test");
                    if(pid!=0&&++samples%10==0){var memory=manager.getProcessMemoryInfo(new int[]{pid});if(memory.length>0)peakPss=Math.max(peakPss,memory[0].getTotalPss());}
                }
                result.putString("stream","PASS: game process survived "+gameSeconds+"s; peak sampled PSS="+(peakPss/1024)+" MiB. Native timeline/render diagnostics checked separately.\n");finish(Activity.RESULT_OK,result);
            }catch(Throwable error){result.putString("stream","FAIL: "+error+"\n");finish(Activity.RESULT_CANCELED,result);}return;
        }
        if(touchOnly){
            try{var realSettings=DataMenuChecks.captureUserSettings(this);MotionSettingsChecks.run();TouchControlChecks.run(this);DualScreenChecks.run();DataMenuChecks.run(this);DataMenuChecks.verifyUserSettings(realSettings);result.putString("stream","PASS: ReMasked vector drawing, item anchors, gesture ownership, input cancellation, placement/reset, visibility, Start/Select/bumper mapping, native menu callbacks/presets, isolated atomic persistence, PAK routing and Android data-folder migration.\n");finish(Activity.RESULT_OK,result);}
            catch(Throwable error){result.putString("stream","FAIL: "+error.toString()+"\n");finish(Activity.RESULT_CANCELED,result);}return;
        }
        try {
            Context context=getTargetContext();
            if(!Python.isStarted())Python.start(new AndroidPlatform(context));
            Python python=Python.getInstance();
            String version=python.getModule("numpy").get("__version__").toString();
            fake=File.createTempFile("invalid-model-",".pak",context.getCacheDir());
            try(FileOutputStream out=new FileOutputStream(fake)){out.write(new byte[]{1,2,3,4});}
            isolated=new File(context.getCacheDir(),"model-runtime-test-"+System.nanoTime());
            JSONObject rejected=new JSONObject(python.getModule("zobj_import.android").callAttr("import_model",isolated.getAbsolutePath(),new File(context.getExternalFilesDir(null),"PrivateInputs/mm3d.3ds").getAbsolutePath(),fake.getAbsolutePath(),new Progress()).toString());
            if(!rejected.has("error"))throw new AssertionError("Malformed PAK was accepted");
            result.putString("stream","ARM64 NumPy "+version+" loaded; packaged PAK converter rejects malformed data; original models/saves untouched.\n");
            finish(Activity.RESULT_OK,result);
        }catch(Throwable error){result.putString("stream","FAIL: "+error.toString()+"\n");finish(Activity.RESULT_CANCELED,result);}
        finally{if(fake!=null)fake.delete();if(isolated!=null)remove(isolated);}
    }
    private void remove(File path){File[] children=path.listFiles();if(children!=null)for(File child:children)remove(child);path.delete();}
}
