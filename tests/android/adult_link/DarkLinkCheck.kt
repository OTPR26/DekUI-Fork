import org.citra.citra_emu.utils.DekuiModelConverter as C
import java.io.File
fun main(args:Array<String>){
 val native=File(args[1]).readBytes()
 val source=C.entries(C.decompress(native));val original=C.Model(source.first{it.name=="child/model/link_child.cmb"}.bytes)
 val out=C.convert(File(args[0]).readBytes(),native,true,true)
 val es=C.entries(C.decompress(out));val model=C.Model(es.first{it.name=="child/model/link_child.cmb"}.bytes)
 check(model.chunks.getValue("skl").contentEquals(original.chunks.getValue("skl")))
 for(entry in source)if(entry.name!="child/model/link_child.cmb")check(es.first{it.name==entry.name}.bytes.contentEquals(entry.bytes))
 for(group in listOf(12,14,16)) {
  val weapons=model.meshes.filter{(it[3].toInt()and 255)==group&&(it[2].toInt()and 255) !in setOf(2,3,47)}
  check(weapons.all{mesh->model.primitives(model.shapes[C.h(mesh,0)]).all{pr->pr.bones.all{it==17}}}){"Sword attached to the wrong hand in slot $group"}
  check(weapons.isNotEmpty()&&weapons.all{(it[2].toInt()and 255)==48}){"Dark sword is missing from slot $group"}
 }
 check(model.meshes.filter{(it[3].toInt()and 255)==18&&(it[2].toInt()and 255)<42}.map{it[2].toInt()and 255}.toSet()==original.meshes.filter{(it[3].toInt()and 255)==18&&(it[2].toInt()and 255) !in setOf(2,3)}.map{it[2].toInt()and 255}.toSet())
 val eyes=model.materials.single{C.u(it,0x168)==0xdad10004.toInt()}
 check(C.u(eyes,0x120)==1)
 val eyeStage=C.h(eyes,0x124)*40
 val eyeComb=model.combiners
 check(C.h(eyeComb,eyeStage)==0x1e01&&C.h(eyeComb,eyeStage+2)==0x1e01)
 check(C.h(eyeComb,eyeStage+12)==0x84c0&&C.h(eyeComb,eyeStage+24)==0x84c0){"Eye output must sample original RGBA, independently of actor tints"}
 check(eyes[0x130].toInt()==1&&C.h(eyes,0x132)==0x0204)
 val sourceModel=C.Model(File(args[0]).readBytes())
 val sourceEye=sourceModel.tex[4];val outputEye=model.tex[C.h(eyes,16)]
 check(sourceModel.chunks.getValue("textures").copyOfRange(C.u(sourceEye,16),C.u(sourceEye,16)+C.u(sourceEye,0)).contentEquals(model.chunks.getValue("textures").copyOfRange(C.u(outputEye,16),C.u(outputEye,16)+C.u(outputEye,0)))){"Original red-eye pixels changed"}
 for(sh in model.shapes)for(pr in model.primitives(sh)) {
  check(pr.bones.all{it in 0..25});check(pr.indices.all{it>=0})
 }
 File(args[2]).writeBytes(out)
 println("PASS Dark Link: ${out.size} bytes, ${model.meshes.size} meshes; native skeleton, non-model assets, and Great Fairy Sword preserved")
}
