package io.github.otpr26.remasked.nativegame;
import java.nio.file.*;
import java.util.*;
public final class ReMaskedDataMigrationTest {
 static void check(boolean ok,String why){if(!ok)throw new AssertionError(why);}
 static void refuses(Checked action)throws Exception{try{action.run();throw new AssertionError("Expected refusal");}catch(java.io.IOException expected){}}
 interface Checked{void run()throws Exception;}
 public static void main(String[] args)throws Exception{
  Path base=Files.createTempDirectory("remasked-migration-test-");
  try{
   Path source=base.resolve("old"),dest=base.resolve("new");Files.createDirectories(source.resolve("empty/saves"));
   byte[] payload=new byte[65539];new Random(123).nextBytes(payload);Files.write(source.resolve("owned.bin"),payload);Files.writeString(source.resolve("profile.json"),source.toString());
   Files.writeString(base.resolve("unrelated.txt"),"keep");boolean[] selected={false};
   ReMaskedDataMigration.move(source,dest,(stage,oldRoot,newRoot)->Files.writeString(stage.resolve("profile.json"),newRoot.toString()),path->{check(Files.exists(path.resolve("owned.bin")),"Publish only after staging complete");selected[0]=true;Files.writeString(source.resolve("created-later.txt"),"keep new file");},(done,total)->check(done<=total,"Bounded progress"));
   check(selected[0]&&Arrays.equals(payload,Files.readAllBytes(dest.resolve("owned.bin"))),"Exact binary data transferred");
   check(Files.readString(dest.resolve("profile.json")).equals(dest.toFile().getCanonicalPath()),"Rewritten launch root preserved");
   check(Files.isDirectory(dest.resolve("empty/saves")),"Empty save directories transferred");check(!Files.exists(source.resolve("owned.bin")),"Verified old copies removed");
   check(Files.exists(source.resolve("created-later.txt"))&&Files.exists(base.resolve("unrelated.txt")),"Unrelated and newly-created files retained");
   for(int failure=0;failure<3;failure++){
    Path old=base.resolve("failure-"+failure),next=base.resolve("result-"+failure);Files.createDirectories(old);Files.writeString(old.resolve("save.bin"),"original");int mode=failure;
    refuses(()->ReMaskedDataMigration.move(old,next,(stage,a,b)->{if(mode==0)throw new java.io.IOException("rewrite failed");if(mode==2)Files.writeString(old.resolve("save.bin"),"changed");},path->{if(mode==1)throw new java.io.IOException("selection failed");},(a,b)->{}));
    check(Files.exists(old.resolve("save.bin"))&&!Files.exists(next),"Failed move leaves original and selection intact");
   }
   refuses(()->ReMaskedDataMigration.move(dest,dest.resolve("inside"),(a,b,c)->{},p->{},(a,b)->{}));
   Path occupied=base.resolve("occupied");Files.createDirectories(occupied);Files.writeString(occupied.resolve("other"),"keep");
   refuses(()->ReMaskedDataMigration.move(dest,occupied,(a,b,c)->{},p->{},(a,b)->{}));check(Files.readString(occupied.resolve("other")).equals("keep"),"Existing destination is untouched");
   System.out.println("PASS: verified relocation, launch-root rewrite, empty directories, safe cleanup, source changes, failure rollback, destination/overlap guards.");
  }finally{try(var paths=Files.walk(base)){for(Path p:paths.sorted(Comparator.reverseOrder()).collect(java.util.stream.Collectors.toList()))Files.delete(p);}}
 }
}
