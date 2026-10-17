import org.citra.citra_emu.utils.DekuiModelConverter as C
import java.io.File
fun main(args:Array<String>){
 require(args.size==4){"Arguments: adult CMB, clothing CMAB, MM3D archive, output directory"}
 File(args[3]).mkdirs()
 val a=File(args[0]).readBytes()
 val body=File(args[1]).readBytes()
 val native=File(args[2]).readBytes()
 for(color in 0..2)for(master in listOf(false,true)){
  val out=C.convert(C.tunicModel(a,body,color),native,master)
  val model=C.Model(C.entries(C.decompress(out)).first{it.name=="child/model/link_child.cmb"}.bytes)
  for(group in listOf(12,14,16)) {
   val weapons=model.meshes.filter{(it[3].toInt()and 255)==group&&(it[2].toInt()and 255) !in setOf(2,3,48)}
   check(weapons.isNotEmpty())
   check(weapons.all{val material=it[2].toInt()and 255;if(master)material>=49 else material in 27..30}){"Regular sword slot $group was not replaced"}
  }
  check(model.meshes.filter{(it[3].toInt()and 255)==18&&(it[2].toInt()and 255)<42}.map{it[2].toInt()and 255}.toSet()==setOf(37,38,39,40,41)){"Great Fairy's Sword was changed"}
  File(args[3],"$color-${if(master)"master" else "kokiri"}.lzs").writeBytes(out)
  println("PASS color=$color master=$master bytes=${out.size}")
 }
}
