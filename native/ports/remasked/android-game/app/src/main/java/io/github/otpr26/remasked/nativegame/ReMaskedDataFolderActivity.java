// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;
import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.*;
import android.provider.*;
import android.os.storage.*;
import android.widget.*;
import java.io.*;
import java.nio.file.*;
import org.json.*;

/** Native game files require ordinary local paths, not document-provider URIs. */
public final class ReMaskedDataFolderActivity extends Activity {
    private static final int ACCESS=4710,FOLDER=4711;
    private TextView status;private boolean moving,picked;
    static volatile boolean activeMove;
    @Override public void onCreate(Bundle state){super.onCreate(state);
        status=new TextView(this);status.setTextColor(0xffeeeeee);status.setBackgroundColor(0xff110b1b);status.setPadding(40,60,40,40);status.setTextSize(18);setContentView(status);
        if(state!=null){status.setText("Folder selection was interrupted. Existing data has been preserved.");return;}
        if(getIntent().getBooleanExtra("use-app-folder",false)){begin(getExternalFilesDir(null));return;}
        requestAccess();
    }
    private boolean allowed(){return Build.VERSION.SDK_INT>=30?Environment.isExternalStorageManager():checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)==android.content.pm.PackageManager.PERMISSION_GRANTED;}
    private void requestAccess(){
        if(allowed()){pick();return;}
        status.setText("");
        new AlertDialog.Builder(this).setTitle("Custom data folder").setMessage("Android requires file access for native game data outside the app folder. After allowing access, choose a local folder. ReMasked creates a Majora ReMasked subfolder and verifies the move before switching.").setNegativeButton("Cancel",(d,w)->returnToData()).setPositiveButton("Continue",(d,w)->{
            if(Build.VERSION.SDK_INT>=30)startActivityForResult(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,Uri.parse("package:"+getPackageName())),ACCESS);
            else requestPermissions(new String[]{android.Manifest.permission.WRITE_EXTERNAL_STORAGE},ACCESS);
        }).show();
    }
    private void pick(){if(picked)return;picked=true;status.setText("Choose a local folder. ReMasked will create a Majora ReMasked subfolder inside it.");
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).putExtra(Intent.EXTRA_LOCAL_ONLY,true).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),FOLDER);
    }
    @Override public void onRequestPermissionsResult(int r,String[] p,int[] grants){super.onRequestPermissionsResult(r,p,grants);if(r==ACCESS){if(allowed())pick();else fail("File access was not granted. Your data folder is unchanged.");}}
    @Override protected void onActivityResult(int request,int result,Intent data){super.onActivityResult(request,result,data);
        if(request==ACCESS){if(allowed())pick();else fail("File access was not granted. Your data folder is unchanged.");return;}
        if(request!=FOLDER)return;if(result!=RESULT_OK||data==null||data.getData()==null){returnToData();return;}
        try{Uri uri=data.getData();getContentResolver().takePersistableUriPermission(uri,data.getFlags()&(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION));
            if(!"com.android.externalstorage.documents".equals(uri.getAuthority()))throw new IOException("Choose a folder on this device or its SD card. Cloud folders cannot host native game data.");
            String id=DocumentsContract.getTreeDocumentId(uri);String[] parts=id.split(":",2);File volume=null;
            if(parts[0].equals("primary"))volume=Environment.getExternalStorageDirectory();
            else if(Build.VERSION.SDK_INT>=30)for(StorageVolume v:getSystemService(StorageManager.class).getStorageVolumes())if(parts[0].equalsIgnoreCase(v.getUuid()))volume=v.getDirectory();
            if(volume==null)throw new IOException("The selected storage volume is unavailable");
            File parent=new File(volume,parts.length>1?parts[1]:"").getCanonicalFile();
            if(!parent.toPath().startsWith(volume.getCanonicalFile().toPath()))throw new IOException("Invalid folder");
            begin(new File(parent,"Majora ReMasked"));
        }catch(Exception error){fail(error.getMessage());}
    }
    private void begin(File destination){begin(destination,0);}
    private void begin(File destination,int attempt){
        if(moving)return;android.app.ActivityManager am=(android.app.ActivityManager)getSystemService(ACTIVITY_SERVICE);
        for(var p:am.getRunningAppProcesses())if(p.uid==android.os.Process.myUid()&&(p.processName.equals(getPackageName()+":game")||p.processName.equals(getPackageName()+":settings"))){if(attempt<50){status.setText("Waiting for the game and settings to close…");new Handler(Looper.getMainLooper()).postDelayed(()->begin(destination,attempt+1),100);}else fail("Close the game and settings before moving data, then try again.");return;}
        moving=true;activeMove=true;getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);status.setText("Verifying and moving ReMasked data…");
        new Thread(()->{try{Path source=org.triaevum.android.GameDataDirectory.root(this).toPath();
            boolean cleaned=ReMaskedDataMigration.move(source,destination.toPath(),(stage,oldRoot,newRoot)->{
                try(var paths=Files.walk(stage)){for(Path p:paths.filter(p->p.toString().endsWith(".json")).collect(java.util.stream.Collectors.toList())){
                    if(Files.size(p)>32L*1024*1024)throw new IOException("A JSON settings file is too large to relocate safely");
                    String text=new String(Files.readAllBytes(p),java.nio.charset.StandardCharsets.UTF_8);Object doc=new JSONTokener(text).nextValue();if(rewrite(doc,oldRoot.toString(),newRoot.toString()))Files.write(p,doc.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                }}
            },path->org.triaevum.android.GameDataDirectory.select(this,path.toFile()),(done,total)->runOnUiThread(()->status.setText("Verifying and moving data: "+(total==0?100:done*100/total)+"%")));
            runOnUiThread(()->{moving=false;activeMove=false;if(!cleaned)Toast.makeText(this,"Folder switched. Some original files could not be removed.",Toast.LENGTH_LONG).show();startActivity(new Intent(this,ReMaskedSettingsActivity.class).putExtra("category","Data"));finish();});
        }catch(Exception error){runOnUiThread(()->{moving=false;activeMove=false;fail("Data move did not complete: "+error.getMessage());});}},"remasked-data-move").start();
    }
    private static String relocated(String text,String oldRoot,String newRoot){return text.equals(oldRoot)||text.startsWith(oldRoot+"/")?newRoot+text.substring(oldRoot.length()):text;}
    private static boolean rewrite(Object doc,String oldRoot,String newRoot)throws JSONException{
        boolean changed=false;
        if(doc instanceof JSONObject){JSONObject object=(JSONObject)doc;var keys=object.keys();while(keys.hasNext()){String key=keys.next();Object value=object.get(key);
            if(value instanceof String){String next=relocated((String)value,oldRoot,newRoot);if(!next.equals(value)){object.put(key,next);changed=true;}}
            else changed|=rewrite(value,oldRoot,newRoot);
        }}else if(doc instanceof JSONArray){JSONArray array=(JSONArray)doc;for(int i=0;i<array.length();i++){Object value=array.get(i);
            if(value instanceof String){String next=relocated((String)value,oldRoot,newRoot);if(!next.equals(value)){array.put(i,next);changed=true;}}
            else changed|=rewrite(value,oldRoot,newRoot);
        }}return changed;
    }
    private void returnToData(){startActivity(new Intent(this,ReMaskedSettingsActivity.class).putExtra("category","Data"));finish();}
    private void fail(String text){new AlertDialog.Builder(this).setTitle("Data folder").setMessage(text).setPositiveButton("Close",(d,w)->returnToData()).setCancelable(false).show();}
    @Override public void onBackPressed(){if(!moving)returnToData();}
}
