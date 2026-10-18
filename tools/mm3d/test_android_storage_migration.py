import shutil,subprocess,tempfile,unittest
from pathlib import Path

class StorageMigrationTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('javac') or Path('/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin/javac').exists(),'JDK required')
    def test_verified_copy_preserves_sources_rejects_conflicts_and_retries(self):
        repo=Path(__file__).resolve().parents[2]
        jdk=Path('/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin')
        javac=str(jdk/'javac') if jdk.exists() else shutil.which('javac');java=str(jdk/'java') if jdk.exists() else shutil.which('java')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);test=root/'MigrationCheck.java'
            test.write_text('''import java.io.*;import java.nio.file.*;import org.citra.citra_emu.utils.DekuiStorageMigration;
public class MigrationCheck {public static void main(String[] args)throws Exception{
 File root=new File(args[0]),source=new File(root,"private"),game=new File(root,"game.cci"),target=new File(root,"mm3dport");source.mkdirs();
 Files.write(new File(source,"save.bin").toPath(),new byte[]{1,2,3});Files.write(game.toPath(),new byte[]{4,5,6});
 DekuiStorageMigration.migrate(source,game,target);
 if(!java.util.Arrays.equals(Files.readAllBytes(new File(source,"save.bin").toPath()),Files.readAllBytes(new File(target,"save.bin").toPath())))throw new AssertionError("save mismatch");
 if(!game.isFile()||!new File(target,"game/game.cci").isFile()||!new File(source,"save.bin").isFile())throw new AssertionError("source lost");
 DekuiStorageMigration.migrate(source,game,target);
 File conflict=new File(root,"existing");conflict.mkdirs();Files.write(new File(conflict,"keep").toPath(),new byte[]{7});
 try{DekuiStorageMigration.migrate(source,game,conflict);throw new AssertionError("conflict accepted");}catch(IOException expected){}
 if(Files.readAllBytes(new File(conflict,"keep").toPath())[0]!=7)throw new AssertionError("conflict lost");
 if(!System.getProperty("os.name").startsWith("Windows")){
 File linked=new File(root,"linked");Files.createSymbolicLink(linked.toPath(),target.toPath());
 try{DekuiStorageMigration.migrate(source,game,linked);throw new AssertionError("symlink accepted");}catch(IOException expected){}
 }
}}''')
            source=repo/'src/android/app/src/main/java/org/citra/citra_emu/utils/DekuiStorageMigration.java'
            subprocess.run([javac,'-d',str(root),str(source),str(test)],check=True,capture_output=True)
            subprocess.run([java,'-cp',str(root),'MigrationCheck',str(root)],check=True,capture_output=True)
