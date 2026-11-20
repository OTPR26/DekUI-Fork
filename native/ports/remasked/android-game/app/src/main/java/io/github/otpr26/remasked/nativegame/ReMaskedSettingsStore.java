// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;
import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;
final class ReMaskedSettingsStore {
    static JSONObject patch(android.content.Context context,JSONObject changes)throws Exception {
        File directory=new File(org.triaevum.android.GameDataDirectory.root(context),"GameContent");
        File canonicalFile=new File(directory,"presentation.json"),runtimeFile=new File(directory,"runtime-presentation.json");
        byte[] previous=Files.readAllBytes(runtimeFile.toPath());
        JSONObject canonical=new JSONObject(new String(Files.readAllBytes(canonicalFile.toPath()),StandardCharsets.UTF_8));
        JSONObject runtime=new JSONObject(new String(previous,StandardCharsets.UTF_8));
        java.util.Iterator<String> keys=changes.keys();while(keys.hasNext()){
            String key=keys.next();Object value=changes.get(key);canonical.put(key,value);runtime.put(key,value);
        }
        write(runtimeFile,runtime);
        try{write(canonicalFile,canonical);}catch(Exception failure){
            try{write(runtimeFile,new JSONObject(new String(previous,StandardCharsets.UTF_8)));}catch(Exception rollback){failure.addSuppressed(rollback);}throw failure;
        }
        return runtime;
    }
    static void write(File file,JSONObject document)throws Exception {
        File temporary=new File(file.getParentFile(),file.getName()+".partial");
        try{
            Files.write(temporary.toPath(),document.toString(2).getBytes(StandardCharsets.UTF_8));
            Files.move(temporary.toPath(),file.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        }finally{Files.deleteIfExists(temporary.toPath());}
    }
}
