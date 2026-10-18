// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils;
import java.io.*;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.UUID;
/** Copy, verify, then publish. Source data is never removed or modified. */
public final class DekuiStorageMigration {
    public static boolean hasPreparedGame(File target) {
        File game=new File(target,"game/game.cci");
        return new File(target,".dekui-storage-v1").isFile() && game.isFile() && game.length()>0 &&
            new File(target,"load/mods/0004000000125500/code.bps").isFile();
    }
    private static byte[] hash(File file) throws Exception {
        MessageDigest digest=MessageDigest.getInstance("SHA-256");
        try(InputStream in=new FileInputStream(file)){byte[] block=new byte[1024*1024];int n;while((n=in.read(block))!=-1)digest.update(block,0,n);}
        return digest.digest();
    }
    private static void copy(File source,File target,int depth) throws Exception {
        if(depth>64 || Files.isSymbolicLink(source.toPath()))throw new IOException("Unsupported linked or deeply nested game data: "+source);
        if(source.isDirectory()){
            if(!target.isDirectory()&&!target.mkdirs())throw new IOException("Cannot create "+target);
            File[] files=source.listFiles();if(files==null)throw new IOException("Cannot read "+source);
            for(File file:files)copy(file,new File(target,file.getName()),depth+1);
        }else if(source.isFile()){
            if(!target.getParentFile().isDirectory()&&!target.getParentFile().mkdirs())throw new IOException("Cannot create data directory");
            if(target.exists())throw new IOException("Migration refuses to replace "+target);
            try(InputStream in=new FileInputStream(source);FileOutputStream out=new FileOutputStream(target)){
                byte[] block=new byte[1024*1024];int n;while((n=in.read(block))!=-1)out.write(block,0,n);out.getFD().sync();
            }
            if(source.length()!=target.length()||!Arrays.equals(hash(source),hash(target)))throw new IOException("Copy verification failed: "+source);
        }else throw new IOException("Unsupported game data entry: "+source);
    }
    private static void removeStage(File file){File[] children=file.listFiles();if(children!=null)for(File child:children)removeStage(child);file.delete();}
    public static void migrate(File source,File game,File target) throws Exception {
        if(Files.isSymbolicLink(target.toPath()))throw new IOException("Game data folder must not be a symbolic link");
        File marker=new File(target,".dekui-storage-v1");
        if(marker.isFile())return;
        if(target.exists()){
            String[] names=target.list();if(names==null||names.length!=0)throw new IOException("mm3dport already contains files. Existing files were retained; choose an empty destination before migrating.");
        }
        File stage=new File(target.getParentFile(),".mm3dport-migration-"+UUID.randomUUID());
        try{
            if(!stage.mkdirs())throw new IOException("Cannot create shared-storage staging folder");
            if(source.isDirectory())for(File child:source.listFiles())copy(child,new File(stage,child.getName()),0);
            if(game.isFile())copy(game,new File(stage,"game/game.cci"),0);
            try(FileOutputStream out=new FileOutputStream(new File(stage,".dekui-storage-v1"))){out.write("1\n".getBytes("UTF-8"));out.getFD().sync();}
            if(target.exists()&&!target.delete())throw new IOException("Cannot publish shared game data");
            if(!stage.renameTo(target))throw new IOException("Cannot publish shared game data");
        }finally{if(stage.exists())removeStage(stage);}
    }
}
