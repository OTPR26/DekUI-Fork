package org.triaevum.android;
import android.content.Context;
import android.util.AtomicFile;
import java.io.*;
import java.nio.charset.StandardCharsets;
/** Shared resolution for launch profiles, settings and save transfer. */
public final class GameDataDirectory {
    private GameDataDirectory(){}
    private static AtomicFile selection(Context c){return new AtomicFile(new File(c.getFilesDir(),"game-data-directory"));}
    public static File root(Context c){
        AtomicFile store=selection(c);if(!store.getBaseFile().exists())return c.getExternalFilesDir(null);
        try{String path=new String(store.readFully(),StandardCharsets.UTF_8).trim();File root=new File(path);
            if(!root.isAbsolute()||!root.isDirectory()||!root.canRead()||!root.canWrite())throw new IOException("Data folder unavailable");
            return root;
        }catch(IOException error){throw new IllegalStateException("The selected data folder is unavailable. Reconnect its storage or restore Android file access.",error);}
    }
    public static void select(Context c,File root)throws IOException{
        AtomicFile store=selection(c);File fallback=c.getExternalFilesDir(null);if(fallback!=null&&root.getCanonicalFile().equals(fallback.getCanonicalFile())){store.delete();return;}FileOutputStream out=store.startWrite();
        try{out.write(root.getCanonicalPath().getBytes(StandardCharsets.UTF_8));store.finishWrite(out);}catch(IOException e){store.failWrite(out);throw e;}
    }
}
