// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;
import java.io.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
/** Verified staging; selection changes only after every byte and rewritten path is ready. */
public final class ReMaskedDataMigration {
    public interface Rewrite {void apply(Path staged,Path source,Path destination)throws Exception;}
    public interface Publish {void select(Path destination)throws Exception;}
    public interface Progress {void update(long copied,long total);}
    private record Entry(Path relative,long size,byte[] hash){}
    public static boolean move(Path source,Path destination,Rewrite rewrite,Publish publish,Progress progress)throws Exception{
        source=source.toRealPath();destination=destination.toFile().getCanonicalFile().toPath();
        if(source.equals(destination))return true;
        if(source.startsWith(destination)||destination.startsWith(source))throw new IOException("Choose a folder outside the current data folder");
        if(Files.exists(destination)&&(!Files.isDirectory(destination)||!empty(destination)))throw new IOException("The destination already contains files; choose another folder");
        Path parent=destination.getParent();Files.createDirectories(parent);Path stage;
        List<Entry> entries=new ArrayList<>();long total=0;
        try(var paths=Files.walk(source)){for(Path path:paths.collect(java.util.stream.Collectors.toList())){
            if(Files.isSymbolicLink(path))throw new IOException("Data folder contains a symbolic link");
            if(Files.isRegularFile(path)){long size=Files.size(path);total=Math.addExact(total,size);entries.add(new Entry(source.relativize(path),size,null));}
        }}
        if(parent.toFile().getUsableSpace()<total+32L*1024*1024)throw new IOException("Not enough free space to verify the move");
        stage=Files.createTempDirectory(parent,".remasked-moving-");
        boolean published=false,installed=false;List<Entry> verified=new ArrayList<>();
        try{
            long copied=0;for(Entry entry:entries){Path input=source.resolve(entry.relative),output=stage.resolve(entry.relative);Files.createDirectories(output.getParent());
                byte[] before=hash(input);Files.copy(input,output);if(Files.size(output)!=entry.size||!Arrays.equals(before,hash(output))||!Arrays.equals(before,hash(input)))throw new IOException("A data file changed during the move");
                verified.add(new Entry(entry.relative,entry.size,before));copied+=entry.size;progress.update(copied,total);
            }
            // Empty directories are meaningful to the native save and texture owners.
            try(var paths=Files.walk(source)){for(Path p:paths.filter(Files::isDirectory).collect(java.util.stream.Collectors.toList()))Files.createDirectories(stage.resolve(source.relativize(p)));}
            rewrite.apply(stage,source,destination);
            for(Entry entry:verified)if(!Arrays.equals(entry.hash,hash(source.resolve(entry.relative))))throw new IOException("A source file changed before the move completed");
            if(Files.exists(destination))Files.delete(destination);
            Files.move(stage,destination,StandardCopyOption.ATOMIC_MOVE);installed=true;
            publish.select(destination);published=true;
            // Remove only the exact source files verified above; never unrelated parent files.
            boolean cleaned=true;
            try{
                for(Entry entry:verified){Path file=source.resolve(entry.relative);if(Files.isRegularFile(file)&&Arrays.equals(entry.hash,hash(file)))Files.delete(file);}
                try(var paths=Files.walk(source)){for(Path p:paths.sorted(Comparator.reverseOrder()).collect(java.util.stream.Collectors.toList()))if(Files.isDirectory(p)&&empty(p)&&!p.equals(source))Files.delete(p);}
            }catch(IOException cleanup){cleaned=false;}
            return cleaned;
        }finally{
            if(!published){Path owned=installed?destination:stage;if(Files.exists(owned))removeTree(owned);}
        }
    }
    private static boolean empty(Path p)throws IOException{try(var entries=Files.list(p)){return entries.findAny().isEmpty();}}
    private static byte[] hash(Path p)throws Exception{MessageDigest hash=MessageDigest.getInstance("SHA-256");try(InputStream in=Files.newInputStream(p)){byte[] block=new byte[1024*1024];int n;while((n=in.read(block))!=-1)hash.update(block,0,n);}return hash.digest();}
    private static void removeTree(Path p)throws IOException{try(var paths=Files.walk(p)){for(Path item:paths.sorted(Comparator.reverseOrder()).collect(java.util.stream.Collectors.toList()))Files.delete(item);}}
}
