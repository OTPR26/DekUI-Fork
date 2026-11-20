// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;
import android.app.Activity;
import android.widget.*;
import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.function.*;
import org.json.JSONObject;
final class ReMaskedGameplayOptions {
    interface LiveUpdate {boolean update(float distance,float walk,float climb,float swim,boolean resetCamera);}
    static void show(Activity activity,BooleanSupplier canSave,LiveUpdate live,Consumer<String> report){
        try {
            File profile=new File(org.triaevum.android.GameDataDirectory.root(activity),"GameContent/presentation.json");
            JSONObject original=new JSONObject(new String(Files.readAllBytes(profile.toPath()),StandardCharsets.UTF_8));
            String[] keys={"camera_distance","walk_speed","climb_speed","swim_speed"};
            String[] labels={"Camera distance","Walking speed","Climbing speed","Swimming speed"};
            final boolean[] resetCamera={false};
            double[] values=new double[4];SeekBar[] bars=new SeekBar[4];
            LinearLayout options=new LinearLayout(activity);options.setOrientation(LinearLayout.VERTICAL);options.setPadding(24,12,24,12);
            for(int i=0;i<4;i++){
                final int index=i;values[i]=Math.max(.5,Math.min(3,original.optDouble(keys[i],1)));
                TextView label=new TextView(activity);label.setText(labels[i]+": "+Math.round(values[i]*100)+"%");options.addView(label);
                SeekBar bar=new SeekBar(activity);bar.setMax(250);bar.setProgress((int)Math.round(values[i]*100)-50);bars[i]=bar;options.addView(bar);
                bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){public void onStartTrackingTouch(SeekBar b){}public void onStopTrackingTouch(SeekBar b){}
                    public void onProgressChanged(SeekBar b,int value,boolean user){values[index]=(value+50)/100.;label.setText(labels[index]+": "+(value+50)+"%");}});
            }
            ScrollView scroll=new ScrollView(activity);scroll.addView(options);
            android.app.AlertDialog dialog=new android.app.AlertDialog.Builder(activity).setTitle("ReMasked camera and movement").setView(scroll)
                .setNegativeButton("Cancel",null).setNeutralButton("Reset",null).setPositiveButton("Save",(d,which)->{
                    if(!canSave.getAsBoolean()){report.accept("Quit the game before changing camera or movement settings.");return;}
                    try {
                        File runtimeFile=new File(org.triaevum.android.GameDataDirectory.root(activity),"GameContent/runtime-presentation.json");
                        byte[] runtimeBefore=Files.readAllBytes(runtimeFile.toPath());
                        JSONObject canonical=new JSONObject(new String(Files.readAllBytes(profile.toPath()),StandardCharsets.UTF_8));
                        JSONObject runtime=new JSONObject(new String(runtimeBefore,StandardCharsets.UTF_8));
                        for(int n=0;n<keys.length;n++){canonical.put(keys[n],values[n]);runtime.put(keys[n],values[n]);}
                        ReMaskedSettingsStore.write(runtimeFile,runtime);
                        try {ReMaskedSettingsStore.write(profile,canonical);}catch(Exception failure){
                            try{ReMaskedSettingsStore.write(runtimeFile,new JSONObject(new String(runtimeBefore,StandardCharsets.UTF_8)));}catch(Exception rollback){failure.addSuppressed(rollback);}throw failure;
                        }
                        if(live!=null&&!live.update((float)values[0],(float)values[1],(float)values[2],(float)values[3],resetCamera[0]))
                            throw new IOException("The native game rejected these settings.");
                        report.accept(live==null?"Camera and movement settings saved for the next game launch.":"Camera and movement settings saved.");
                    }catch(Exception error){report.accept("Cannot save gameplay settings: "+error.getMessage());}
                }).create();
            dialog.setOnShowListener(d->dialog.getButton(android.app.AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v->{resetCamera[0]=true;for(SeekBar bar:bars)bar.setProgress(50);}));dialog.show();
        }catch(Exception error){report.accept("Cannot read gameplay settings: "+error.getMessage());}
    }
}
