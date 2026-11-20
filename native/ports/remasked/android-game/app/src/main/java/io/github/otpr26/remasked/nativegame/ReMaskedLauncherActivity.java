// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;

import android.app.Activity;
import android.app.ActivityManager;
import android.os.Bundle;
import android.content.Intent;
import android.database.Cursor;
import android.provider.OpenableColumns;
import android.widget.*;
import android.net.Uri;
import com.chaquo.python.Python;
import com.chaquo.python.android.AndroidPlatform;
import org.json.JSONObject;
import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;

/** The ReMasked launcher keeps model conversion out of the game process. */
public final class ReMaskedLauncherActivity extends Activity {
    private static final int PICK_PAK=100;
    private static final int PICK_GAME_FOLDER=101;
    private static final int PICK_GAME_ROM=102;
    private static volatile boolean importing;
    private TextView status;
    private File root() {return org.triaevum.android.GameDataDirectory.root(this);}
    private boolean gameRunning() {
        ActivityManager manager=(ActivityManager)getSystemService(ACTIVITY_SERVICE);
        for(ActivityManager.RunningAppProcessInfo process:manager.getRunningAppProcesses())
            if(process.uid==android.os.Process.myUid() && process.processName.equals(getPackageName()+":game"))return true;
        return false;
    }
    private void report(String message) {runOnUiThread(()->status.setText(message));}
    private int dp(int value){return Math.round(value*getResources().getDisplayMetrics().density);}
    private android.graphics.Typeface bold,normal;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);buildLauncher();openRequestedPage();
    }
    private void play(boolean settings){
        if(ReMaskedDataFolderActivity.activeMove){report("Wait for the data move to finish.");return;}
        if(importing){report("Wait for import to finish.");return;}
        if(settings&&!gameRunning()){startActivity(new Intent(this,ReMaskedSettingsActivity.class));return;}
        if(!new File(root(),"GameContent/ReMaskedNativeGame.launch.json").isFile()){
            new android.app.AlertDialog.Builder(this).setTitle("Set up game files")
                .setMessage("Choose your decrypted Majora's Mask 3D ROM. ReMasked prepares the files on this device and downloads the small Project Restoration HUD resources during setup.")
                .setPositiveButton("Choose ROM", (dialog, which) ->
                    startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                        .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), PICK_GAME_ROM))
                .setNeutralButton("Prepared folder", (dialog, which) ->
                    startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_GAME_FOLDER))
                .setNegativeButton("Cancel", null).show();
            return;
        }
        startActivity(new Intent(this,ReMaskedGameActivity.class).putExtra("open_settings",settings)
            .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT|Intent.FLAG_ACTIVITY_SINGLE_TOP));
    }
    private void buildLauncher(){
        bold=getResources().getFont(org.triaevum.android.R.font.dekui_sans_bold);
        normal=getResources().getFont(org.triaevum.android.R.font.dekui_sans_regular);
        getWindow().getDecorView().setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_FULLSCREEN|android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE|android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION|android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        FrameLayout frame=new FrameLayout(this);frame.addView(new ReMaskedLauncherBackdrop(this),new FrameLayout.LayoutParams(-1,-1));
        LinearLayout pane=new LinearLayout(this);pane.setOrientation(LinearLayout.VERTICAL);pane.setGravity(android.view.Gravity.END);pane.setPadding(dp(12),dp(16),dp(28),dp(12));
        frame.addView(pane,new FrameLayout.LayoutParams((int)(getResources().getDisplayMetrics().widthPixels*.69f),-1,android.view.Gravity.END));
        TextView title=new TextView(this);title.setText("Majora ReMasked");title.setTypeface(bold);title.setTextSize(34);title.setTextColor(0xfff2f2f2);title.setGravity(android.view.Gravity.END);title.setLetterSpacing(.03f);title.setMaxLines(1);title.setAutoSizeTextTypeUniformWithConfiguration(22,34,1,android.util.TypedValue.COMPLEX_UNIT_SP);pane.addView(title,new LinearLayout.LayoutParams(-1,dp(52)));
        TextView subtitle=new TextView(this);subtitle.setText("A DekUI Fork");subtitle.setTypeface(android.graphics.Typeface.create(normal,android.graphics.Typeface.ITALIC));subtitle.setTextSize(16);subtitle.setTextColor(0xffb97df2);subtitle.setGravity(android.view.Gravity.END);subtitle.setLetterSpacing(.07f);pane.addView(subtitle,new LinearLayout.LayoutParams(-1,dp(24)));
        pane.addView(new android.view.View(this),new LinearLayout.LayoutParams(1,0,1));
        Button start=menuButton(pane,"Start game",()->play(false));
        menuButton(pane,"Settings",()->play(true));
        menuButton(pane,"About",()->new android.app.AlertDialog.Builder(this).setTitle("Majora ReMasked").setMessage("A DekUI Fork\n\nMajora’s Mask 3D\n\nSettings are available from the startup menu and during play.\n\n"+org.triaevum.android.BuildConfig.VERSION_NAME).setPositiveButton("Close",null).show());
        menuButton(pane,"Exit",this::finishAndRemoveTask);
        status=new TextView(this);status.setTypeface(normal);status.setTextSize(12);status.setTextColor(0xffcccccc);status.setGravity(android.view.Gravity.END);status.setPadding(0,dp(6),dp(12),0);status.setAccessibilityLiveRegion(android.view.View.ACCESSIBILITY_LIVE_REGION_POLITE);pane.addView(status,new LinearLayout.LayoutParams(-1,dp(32)));
        TextView version=new TextView(this);version.setText("MAJORA REMASKED  ·  ANDROID  ·  v"+org.triaevum.android.BuildConfig.VERSION_NAME);version.setTypeface(normal);version.setTextSize(10);version.setLetterSpacing(.1f);version.setTextColor(0xff9993a5);version.setPadding(dp(12),0,0,dp(8));frame.addView(version,new FrameLayout.LayoutParams(-2,-2,android.view.Gravity.BOTTOM|android.view.Gravity.START));
        setContentView(frame);start.requestFocus();
    }
    private Button menuButton(LinearLayout pane,String label,Runnable action){
        Button b=new Button(this);b.setText(label);b.setAllCaps(false);b.setTypeface(bold);b.setTextSize(17);b.setFocusable(true);b.setFocusableInTouchMode(true);b.setLetterSpacing(.07f);b.setGravity(android.view.Gravity.END|android.view.Gravity.CENTER_VERTICAL);b.setMinHeight(0);b.setMinimumHeight(0);b.setIncludeFontPadding(false);b.setPadding(dp(12),0,dp(12),0);
        b.setTextColor(new android.content.res.ColorStateList(new int[][]{{android.R.attr.state_focused},{android.R.attr.state_pressed},{}},new int[]{0xffb97df2,0xffb97df2,0xffcccccc}));
        android.graphics.drawable.GradientDrawable highlight=new android.graphics.drawable.GradientDrawable(android.graphics.drawable.GradientDrawable.Orientation.LEFT_RIGHT,new int[]{android.graphics.Color.TRANSPARENT,0x33b97df2});highlight.setCornerRadius(dp(4));
        android.graphics.drawable.StateListDrawable background=new android.graphics.drawable.StateListDrawable();background.addState(new int[]{android.R.attr.state_focused},highlight);background.addState(new int[]{android.R.attr.state_pressed},highlight.getConstantState().newDrawable());background.addState(new int[]{},new android.graphics.drawable.ColorDrawable(android.graphics.Color.TRANSPARENT));b.setBackground(background);b.setOnClickListener(v->action.run());pane.addView(b,new LinearLayout.LayoutParams(-1,dp(36)));return b;
    }
    @Override public void onConfigurationChanged(android.content.res.Configuration configuration){super.onConfigurationChanged(configuration);String message=status==null?"":status.getText().toString();buildLauncher();report(message);}
    @Override protected void onNewIntent(Intent intent){super.onNewIntent(intent);setIntent(intent);openRequestedPage();}
    private void openRequestedPage(){
        String page=getIntent().getStringExtra("page");if(page==null)return;
        if(gameRunning()){new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(this::openRequestedPage,200);return;}
        getIntent().removeExtra("page");
        if(page.equals("remasked-pak"))openPak();
        else if(page.equals("remasked-driver"))org.triaevum.android.GraphicsDriver.chooseAvailable(this,this::report);
    }
    private void openPak(){
        if(importing||gameRunning()){report("Quit the game before importing a player model.");return;}
        if(!new File(root(),"PrivateInputs/mm3d.3ds").isFile()){report("Set up your MM3D ROM before importing a model.");return;}
        Intent picker=new Intent(Intent.ACTION_OPEN_DOCUMENT);picker.setType("*/*");picker.addCategory(Intent.CATEGORY_OPENABLE);startActivityForResult(picker,PICK_PAK);
    }
    public static final class Progress {
        private final ReMaskedLauncherActivity owner;
        Progress(ReMaskedLauncherActivity owner){this.owner=owner;}
        public void progress(String text){owner.report(text);}
    }
    private void selectPak(boolean selected)throws Exception {selectModel(selected?"pak_link_model":"");}
    private void selectModel(String selected)throws Exception {
        File settings=new File(root(),"GameContent/presentation.json");
        JSONObject document=new JSONObject(new String(Files.readAllBytes(settings.toPath()),StandardCharsets.UTF_8));
        for(String key:new String[]{"pak_link_model","adult_link_model","dark_link_model","twilight_link_model"})document.put(key,key.equals(selected));
        File temporary=new File(settings.getParentFile(),"presentation.json.partial");
        Files.write(temporary.toPath(),document.toString(2).getBytes(StandardCharsets.UTF_8));
        Files.move(temporary.toPath(),settings.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if (request == PICK_GAME_FOLDER || request == PICK_GAME_ROM) {
            if (result != RESULT_OK || data == null || data.getData() == null) return;
            if (importing || gameRunning() || ReMaskedDataFolderActivity.activeMove) {
                report("Return to the launcher and wait for any other data operation to finish.");
                return;
            }
            importing = true;
            Uri input = data.getData();
            Path dataRoot = root().toPath();
            Path game = dataRoot.resolve("GameContent");
            boolean isRom = request == PICK_GAME_ROM;
            report("Importing game files…");
            new Thread(() -> {
                try {
                    byte[] presentationDefaults;
                    try (InputStream defaults = getAssets().open("presentation.json")) {
                        presentationDefaults = defaults.readAllBytes();
                    }
                    if (isRom) {
                        if (!Python.isStarted()) Python.start(new AndroidPlatform(getApplicationContext()));
                        PreparedGameImport.installRom(getContentResolver(), input, game, dataRoot.resolve("PrivateInputs"), presentationDefaults,
                            (rom, staging, codeHash) -> {
                                Python.getInstance().getModule("prepare_rom")
                                    .callAttr("prepare", rom.toString(), staging.toString(), codeHash);
                                HudSetup.prepare(staging);
                            });
                    } else {
                        PreparedGameImport.install(getContentResolver(), input, game, presentationDefaults);
                    }
                    report("Game files imported. You can now start the game.");
                } catch (Exception error) {
                    report("Game import failed: " + error.getMessage());
                } finally {
                    importing = false;
                }
            }, "remasked-game-import").start();
            return;
        }

        if(request==org.triaevum.android.GraphicsDriver.PICK_REQUEST){
            org.triaevum.android.GraphicsDriver.result(data);report(org.triaevum.android.GraphicsDriver.status(this));return;
        }
        if(request!=PICK_PAK||result!=RESULT_OK||data==null||data.getData()==null)return;
        if(importing||gameRunning()){report("Quit the game before importing a player model.");return;}
        importing=true;Uri uri=data.getData();
        new Thread(()->{
            File temporary=null;
            try {
                String name="";
                try(Cursor cursor=getContentResolver().query(uri,new String[]{OpenableColumns.DISPLAY_NAME},null,null,null)){
                    if(cursor!=null&&cursor.moveToFirst())name=cursor.getString(0);
                }
                if(name==null||!name.toLowerCase(java.util.Locale.ROOT).endsWith(".pak"))throw new IOException("Select a .pak player-model package.");
                temporary=File.createTempFile("player-model-",".pak",getCacheDir());
                report("Reading the selected PAK…");
                try(InputStream input=getContentResolver().openInputStream(uri);OutputStream output=new FileOutputStream(temporary)){
                    if(input==null)throw new IOException("Cannot read the selected PAK.");
                    byte[] buffer=new byte[65536];long total=0;int count;
                    while((count=input.read(buffer))!=-1){total+=count;if(total>32L*1024*1024)throw new IOException("Model package exceeds 32 MiB.");output.write(buffer,0,count);}
                }
                if(!Python.isStarted())Python.start(new AndroidPlatform(getApplicationContext()));
                JSONObject imported=new JSONObject(Python.getInstance().getModule("zobj_import.android").callAttr("import_model",root().getAbsolutePath(),new File(root(),"PrivateInputs/mm3d.3ds").getAbsolutePath(),temporary.getAbsolutePath(),new Progress(this)).toString());
                if(imported.has("error"))throw new IOException(imported.getString("error"));
                selectPak(true);report("Imported "+imported.getString("name")+". Start the game to use it.");
            }catch(Exception error){report("Model import failed: "+error.getMessage());}
            finally {if(temporary!=null)temporary.delete();importing=false;}
        },"Player-model-import").start();
    }
}
