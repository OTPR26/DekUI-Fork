package io.github.otpr26.remasked.nativegame;
import android.app.Instrumentation;
import android.content.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
final class DataMenuChecks {
 static void check(boolean good,String why){if(!good)throw new AssertionError(why);}
 static byte[] zip(byte[] save)throws Exception{ByteArrayOutputStream out=new ByteArrayOutputStream();try(java.util.zip.ZipOutputStream zip=new java.util.zip.ZipOutputStream(out)){zip.putNextEntry(new java.util.zip.ZipEntry("save0.bin"));zip.write(save);zip.closeEntry();}return out.toByteArray();}
 static java.util.Map<File,byte[]> captureUserSettings(Instrumentation test)throws Exception{
  java.util.Map<File,byte[]> result=new java.util.HashMap<>();File root=org.triaevum.android.GameDataDirectory.root(test.getTargetContext());
  for(String name:new String[]{"presentation.json","runtime-presentation.json","native-game.json","oot3d_controls.json"}){File file=new File(root,"GameContent/"+name);if(file.isFile())result.put(file,Files.readAllBytes(file.toPath()));}
  return result;
 }
 static void verifyUserSettings(java.util.Map<File,byte[]> snapshot)throws Exception{for(var entry:snapshot.entrySet())check(Arrays.equals(entry.getValue(),Files.readAllBytes(entry.getKey().toPath())),"A test modified a real user settings file");}
 static void run(Instrumentation test)throws Exception{
  File fixture=new File(test.getTargetContext().getCacheDir(),"data-folder-check-"+UUID.randomUUID());Path base=fixture.toPath();Files.createDirectories(base);
  File actualSelection=new File(test.getTargetContext().getFilesDir(),"game-data-directory");byte[] originalSelection=actualSelection.exists()?Files.readAllBytes(actualSelection.toPath()):null;
  try{
   java.lang.reflect.Method rewrite=ReMaskedDataFolderActivity.class.getDeclaredMethod("rewrite",Object.class,String.class,String.class);rewrite.setAccessible(true);
   org.json.JSONObject paths=new org.json.JSONObject("{\"path\":\"/old/game/file\",\"similar\":\"/older/keep\",\"value\":7}");
   check((Boolean)rewrite.invoke(null,paths,"/old","/new"),"Path references must be rewritten");check(paths.getString("path").equals("/new/game/file")&&paths.getString("similar").equals("/older/keep"),"Only exact root prefixes can change");
   check(!(Boolean)rewrite.invoke(null,paths,"/unrelated","/next"),"Unchanged JSON assets must remain byte-identical");
   byte[] save=new byte[0x13618];save[0]=0x4d;save[1]=0x4a;save[2]=0x52;save[3]=8;save[0x1a88]=(byte)0xd2;save[0x1a89]=0x2b;
   org.triaevum.android.saves.SaveLayout layout=new org.triaevum.android.saves.SaveLayout(base.resolve("codec").toFile(),base.resolve("codec/saves").toFile(),base.resolve("codec/states").toFile(),true);
   var archive=org.triaevum.android.saves.SaveArchive.INSTANCE;
   var plan=archive.stage(new ByteArrayInputStream(zip(save)),layout);archive.commit(plan,layout,System.currentTimeMillis());archive.discard(plan);
   check(Arrays.equals(save,Files.readAllBytes(base.resolve("codec/saves/save0.bin"))),"MM3D import did not preserve the slot");
   ByteArrayOutputStream exported=new ByteArrayOutputStream();archive.write(archive.collect(layout,false),exported,i->kotlin.Unit.INSTANCE);
   var roundtrip=archive.stage(new ByteArrayInputStream(exported.toByteArray()),layout);archive.discard(roundtrip);
   save[100]^=1;boolean refused=false;try{archive.stage(new ByteArrayInputStream(zip(save)),layout);}catch(Exception expected){refused=true;}check(refused,"Corrupted MM3D saves must be refused");
   Path source=base.resolve("old"),next=base.resolve("new");Files.createDirectories(source.resolve("GameContent/saves"));byte[] payload=new byte[12345];new Random(99).nextBytes(payload);Files.write(source.resolve("GameContent/saves/fixture.bin"),payload);
   File privateFiles=new File(fixture,"selection");privateFiles.mkdirs();
   Context isolated=new ContextWrapper(test.getTargetContext()){@Override public File getFilesDir(){return privateFiles;}@Override public File getExternalFilesDir(String type){return source.toFile();}};
   ReMaskedDataMigration.move(source,next,(stage,a,b)->{},path->org.triaevum.android.GameDataDirectory.select(isolated,path.toFile()),(done,total)->{});
   check(Arrays.equals(Files.readAllBytes(next.resolve("GameContent/saves/fixture.bin")),payload),"Android relocation lost data");
   check(org.triaevum.android.GameDataDirectory.root(isolated).getCanonicalFile().equals(next.toFile().getCanonicalFile()),"Atomic root not selected");
   check(!Files.exists(source.resolve("GameContent/saves/fixture.bin")),"Old verified copy retained");
   check(Arrays.equals(originalSelection,actualSelection.exists()?Files.readAllBytes(actualSelection.toPath()):null),"Test changed the real folder selection");
  }finally{try(var paths=Files.walk(base)){for(Path p:paths.sorted(Comparator.reverseOrder()).collect(java.util.stream.Collectors.toList()))Files.delete(p);}}
 }
}
