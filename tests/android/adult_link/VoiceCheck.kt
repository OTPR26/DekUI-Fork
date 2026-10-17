import org.citra.citra_emu.utils.DekuiVoiceConverter as C
import java.io.File
fun main(args:Array<String>){
 require(args.size==3){"Arguments: OoT3D BCSAR, MM3D BCSAR, output BCSAR"}
 val out=C.convert(File(args[0]).readBytes(),File(args[1]).readBytes())
 check(out.clips==38&&out.mappedKeys==42)
 File(args[2]).writeBytes(out.archive)
 println("PASS ${out.clips} clips, ${out.mappedKeys} human voice keys; all unrelated files, original waves, and voice keys preserved")
}
