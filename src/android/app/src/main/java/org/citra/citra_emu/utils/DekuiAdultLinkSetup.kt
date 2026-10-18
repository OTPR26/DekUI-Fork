// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.util.AtomicFile
import androidx.documentfile.provider.DocumentFile
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.model.BooleanSetting
import org.citra.citra_emu.features.settings.utils.SettingsFile
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.security.MessageDigest

/** Read the user's ROM and pack in place. No full ROM or game asset is bundled. */
object DekuiAdultLinkSetup {
    private fun prefs(c:Context)=c.getSharedPreferences("dekui-adult-link",Context.MODE_PRIVATE)
    fun description(c:Context):String {
        val p=prefs(c)
        val voices=if(listOf("adult-link","dark-link").any{File(c.filesDir,"dekui/load/mm3d-models/$it/voices/romfs/sound/sequence/JokerSound.bcsar").isFile})"Adult voices: imported"else"Adult voices: generate the mod to import"
        return "OoT3D ROM: ${p.getString("romLabel","Not selected")}\nTexture pack: ${p.getString("packLabel","Not selected (original textures)")}\n$voices\n${p.getString("result","")}"
    }
    fun selectRom(c:Context,uri:Uri){
        c.contentResolver.takePersistableUriPermission(uri,Intent.FLAG_GRANT_READ_URI_PERMISSION)
        val name=DocumentFile.fromSingleUri(c,uri)?.name?:"Selected ROM"
        check(prefs(c).edit().putString("rom",uri.toString()).putString("romLabel",name).commit())
    }
    fun selectPack(c:Context,uri:Uri){
        c.contentResolver.takePersistableUriPermission(uri,Intent.FLAG_GRANT_READ_URI_PERMISSION)
        val root=DocumentFile.fromTreeUri(c,uri)?:error("Cannot read texture folder")
        val candidates=mutableListOf<DocumentFile?>()
        for(title in listOf("0004000000033500","0004000000033600","0004000000033400")){
            candidates.add(root.takeIf{it.name.equals(title,true)})
            candidates.add(root.findFile(title));candidates.add(root.findFile("textures")?.findFile(title));candidates.add(root.findFile("load")?.findFile("textures")?.findFile(title))
        }
        val folder=candidates.firstOrNull{it?.isDirectory==true}?:root.takeIf{FileUtil.listFiles(it.uri).any{d->d.filename=="pack.json"||d.filename.startsWith("tex1_")}}
            ?:error("Choose your OoT3D texture pack, load/textures, or ROMs/n3DS folder")
        check(folder.canRead()){"Android cannot read the texture folder"}
        check(prefs(c).edit().putString("pack",folder.uri.toString()).putString("packLabel",folder.name?:"Selected texture pack").commit())
    }
    fun canGenerate(c:Context)=prefs(c).contains("rom")&&File(c.filesDir,"dekui-game/game.cci").isFile
    private class Reader(private val channel:FileChannel){
        fun read(at:Long,n:Int):ByteArray{require(at>=0&&n in 0..64*1024*1024&&at+n<=channel.size()){"Invalid ROM range"};val b=ByteBuffer.allocate(n);while(b.hasRemaining()){val got=channel.read(b,at+b.position());check(got>0){"Could not read ROM"}};return b.array()}
        fun actor(name:String,oot:Boolean):ByteArray{
            val first=read(0,512);val base=when(String(first,0x100,4,Charsets.US_ASCII)){"NCSD"->DekuiModelConverter.u(first,0x120).toLong()*512;"NCCH"->0L;else->error("Choose a decrypted .3ds or .cci ROM (not ZIP/CIA)")}
            val header=read(base,512);check(String(header,0x100,4,Charsets.US_ASCII)=="NCCH");check(header[0x18f].toInt()and 4!=0){"This ROM is encrypted. Select your decrypted dump."}
            val id=ByteBuffer.wrap(header,0x118,8).order(ByteOrder.LITTLE_ENDIAN).long
            check(if(oot)id in listOf(0x0004000000033500L,0x0004000000033600L,0x0004000000033400L)else id==0x0004000000125500L){if(oot)"Select an Ocarina of Time 3D ROM"else"MM3D USA ROM is required"}
            val romfs=base+DekuiModelConverter.u(header,0x1b0).toLong()*512+0x1000
            val h=read(romfs,40);check(DekuiModelConverter.u(h,0)==40)
            val dirs=read(romfs+DekuiModelConverter.u(h,12),DekuiModelConverter.u(h,16));val files=read(romfs+DekuiModelConverter.u(h,28),DekuiModelConverter.u(h,32))
            var result:ByteArray?=null;val seen=mutableSetOf<Int>()
            fun walk(off:Int,parent:String):Int{
                check(seen.add(off)&&seen.size<50000){"Invalid ROM directory table"};val length=DekuiModelConverter.u(dirs,off+20);val dir=String(dirs,off+24,length,Charsets.UTF_16LE);val path=parent+if(dir.isEmpty())""else"$dir/"
                var entry=DekuiModelConverter.u(dirs,off+12);val fileSeen=mutableSetOf<Int>()
                while(entry!=-1){check(fileSeen.add(entry));val nl=DekuiModelConverter.u(files,entry+28);val filename=path+String(files,entry+32,nl,Charsets.UTF_16LE)
                    if(filename==name){val offset=ByteBuffer.wrap(files,entry+8,8).order(ByteOrder.LITTLE_ENDIAN).long;val size=ByteBuffer.wrap(files,entry+16,8).order(ByteOrder.LITTLE_ENDIAN).long;check(size in 1..64L*1024*1024);result=read(romfs+DekuiModelConverter.u(h,36)+offset,size.toInt())}
                    entry=DekuiModelConverter.u(files,entry+4)
                }
                var child=DekuiModelConverter.u(dirs,off+8);while(child!=-1)child=walk(child,path)
                return DekuiModelConverter.u(dirs,off+4)
            };walk(0,"");return result?:error("Required actor archive is missing: $name")
        }
    }
    private fun atomic(file:File,bytes:ByteArray){check(file.parentFile!!.isDirectory||file.parentFile!!.mkdirs());val a=AtomicFile(file);val out=a.startWrite();try{out.write(bytes);a.finishWrite(out)}catch(t:Throwable){a.failWrite(out);throw t}}
    private data class Texture(val raw:ByteArray,val width:Int,val height:Int,val format:Int)
    private fun textures(archive:ByteArray):List<Texture>{val model=DekuiModelConverter.Model(DekuiModelConverter.entries(DekuiModelConverter.decompress(archive)).first{it.name=="child/model/link_child.cmb"}.bytes);return model.tex.drop(33).map{t->val w=DekuiModelConverter.h(t,8);val h=DekuiModelConverter.h(t,10);val format=when(DekuiModelConverter.h(t,12)){0x675a->12;0x675b->13;else->3};val size=w*h*(if(format==3)16 else if(format==12)4 else 8)/8;val at=DekuiModelConverter.u(t,16);Texture(model.chunks.getValue("textures").copyOfRange(at,at+size),w,h,format)}}
    @Volatile var generating=false
        private set
    private fun writeSwordIcon(stage:File,atlas:android.graphics.Bitmap) {
        check(atlas.width==atlas.height && atlas.width>=512 && atlas.width%512==0){"Unsupported item atlas dimensions"}
        val scale=atlas.width/512
        val icon=android.graphics.Bitmap.createBitmap(atlas,0,210*scale,42*scale,42*scale)
        val output=java.io.ByteArrayOutputStream()
        check(icon.compress(android.graphics.Bitmap.CompressFormat.PNG,100,output)){"Cannot encode Master Sword icon"}
        atomic(File(stage,"master-sword-icon.png"),output.toByteArray());icon.recycle()
    }
    private fun nativeSwordIcon(c:Context,rom:Uri,stage:File):List<String> {
        val bytes=c.contentResolver.openFileDescriptor(rom,"r")?.use{fd->FileInputStream(fd.fileDescriptor).use{stream->
            val reader=Reader(stream.channel)
            var atlas:ByteArray?=null
            for(language in listOf("01_US_ENGLISH","02_EU_ENGLISH","00_JP_JAPANESE")) {
                try { atlas=reader.actor("menu/$language/icon_item_menu00.ctxb",true);break }catch(e:IllegalStateException) { if(e.message?.contains("Required actor archive is missing")!=true)throw e }
            }
            atlas?:error("OoT3D item icon atlas not found")
        }}?:error("Cannot read OoT3D item icons")
        check(bytes.size==262216 && bytes.copyOfRange(0,4).contentEquals("ctxb".toByteArray()) && DekuiModelConverter.h(bytes,0x2c)==512 && DekuiModelConverter.h(bytes,0x30)==0x675b){"Unsupported OoT3D item icon atlas"}
        val rgba=NativeLibrary.dekuiDecodeTextureRgba(bytes.copyOfRange(72,bytes.size),512,512,13)?:error("Cannot decode OoT3D item icons")
        val bitmap=android.graphics.Bitmap.createBitmap(512,512,android.graphics.Bitmap.Config.ARGB_8888)
        try { bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(rgba));writeSwordIcon(stage,bitmap) }finally{bitmap.recycle()}
        return NativeLibrary.dekuiTextureHashes(bytes.copyOfRange(72,bytes.size),512,512,13).map{java.lang.Long.toUnsignedString(it,16).uppercase().padStart(16,'0')}
    }
    @Synchronized fun generate(c:Context,progress:(String)->Unit):String {
        generating=true
        try { return generateImpl(c,progress) } finally { generating=false }
    }
    @Synchronized fun generateDarkLink(c:Context,progress:(String)->Unit):String {
        generating=true
        try { return generateImpl(c,progress,true) } finally { generating=false }
    }
    private fun generateImpl(c:Context,progress:(String)->Unit,dark:Boolean=false):String{
        val modelName=if(dark)"Dark Link"else"Adult Link"
        val modelId=if(dark)"dark-link"else"adult-link"
        check(!NativeLibrary.isRunning()){"Close the game before generating a mod"};check(canGenerate(c)){"Select your OoT3D ROM and set up MM3D first"}
        val p=prefs(c);val uri=Uri.parse(p.getString("rom",null));progress("Reading $modelName from your OoT3D ROM…")
        val actor=c.contentResolver.openFileDescriptor(uri,"r")?.use{fd->FileInputStream(fd.fileDescriptor).use{Reader(it.channel).actor(if(dark)"actor/zelda_torch2.zar"else"actor/zelda_link_boy_new.zar",true)}}?:error("Cannot read the selected ROM")
        val es=DekuiModelConverter.entries(actor);val adult=es.firstOrNull{it.name==(if(dark)"Model/darklink.cmb"else"boy/model/link_v2.cmb")}?.bytes?:error("$modelName model is missing")
        val body=if(dark)byteArrayOf()else es.firstOrNull{it.name=="boy/misc/link_body.cmab"}?.bytes?:error("Original tunic colors are missing")
        val native=FileInputStream(File(c.filesDir,"dekui-game/game.cci")).use{Reader(it.channel).actor("actors/zelda2_link_child_new.gar.lzs",false)}
        val root=File(c.filesDir,"dekui/load/mm3d-models/$modelId");val stage=File(root.parentFile,".$modelId-${java.util.UUID.randomUUID()}");check(stage.mkdirs())
        try{
            progress("Reading the Master Sword HUD icon from your OoT3D ROM…")
            val iconHashes=nativeSwordIcon(c,uri,stage)
            progress("Importing Adult Link's voice clips from your OoT3D ROM…")
            val sourceSound=c.contentResolver.openFileDescriptor(uri,"r")?.use{fd->FileInputStream(fd.fileDescriptor).use{Reader(it.channel).actor("sound/QueenSound.bcsar",true)}}?:error("Cannot read OoT3D voices")
            val nativeSound=FileInputStream(File(c.filesDir,"dekui-game/game.cci")).use{Reader(it.channel).actor("sound/sequence/JokerSound.bcsar",false)}
            val voices=DekuiVoiceConverter.convert(sourceSound,nativeSound)
            atomic(File(stage,"voices/romfs/sound/sequence/JokerSound.bcsar"),voices.archive)
            val allTextures=mutableListOf<Texture>();val report=JSONObject().put("voices",JSONObject().put("schema",1).put("clips",voices.clips).put("human_keys",voices.mappedKeys));var count=0
            for(color in (if(dark)listOf(0)else(0..2).toList()))for(master in (if(dark)listOf(true)else listOf(false,true))){
                progress(if(dark)"Creating the original Dark Link model…"else"Creating ${listOf("green","blue","red")[color]} tunic / ${if(master)"Master Sword"else"Kokiri sword"}…")
                val source=if(dark)adult else DekuiModelConverter.tunicModel(adult,body,color);val archive=DekuiModelConverter.convert(source,native,master,dark)
                val name=if(master)"master"else"kokiri";val suffix=if(color==0)""else if(color==1)"-blue"else"-red"
                val output=File(stage,if(dark)"romfs/actors/zelda2_link_child_new.gar.lzs"else"variants/$name$suffix/romfs/actors/zelda2_link_child_new.gar.lzs");atomic(output,archive)
                if(color==0&&!master)atomic(File(stage,"romfs/actors/zelda2_link_child_new.gar.lzs"),archive)
                report.put(if(dark)"dark-link"else name+suffix,MessageDigest.getInstance("SHA-256").digest(archive).joinToString(""){"%02x".format(it)})
                allTextures.addAll(textures(archive));count++
            }
            val folder=p.getString("pack",null);var copied=0
            if(folder!=null){
                progress("Finding matching HD textures in your selected pack…");val index=mutableMapOf<String,Uri>();var visited=0
                fun scan(at:Uri,depth:Int){check(depth<40&&visited++<50000){"Texture folder is too deep or too large"};for(doc in FileUtil.listFiles(at)){if(doc.mimeType==DocumentsContract.Document.MIME_TYPE_DIR)scan(doc.uri,depth+1)else{val match=Regex("^tex1_\\d+x\\d+_([0-9A-Fa-f]{16})_\\d+_mip0\\.(png|dds|ktx)$").matchEntire(doc.filename);if(match!=null)index.putIfAbsent(match.groupValues[1].uppercase(),doc.uri)}}}
                scan(Uri.parse(folder),0)
                val iconSource=index[iconHashes[1]]?:index[iconHashes[0]]
                if(iconSource!=null && DocumentFile.fromSingleUri(c,iconSource)?.name?.endsWith(".png",true)==true) {
                    val bounds=android.graphics.BitmapFactory.Options().apply{inJustDecodeBounds=true}
                    c.contentResolver.openInputStream(iconSource)?.use{android.graphics.BitmapFactory.decodeStream(it,null,bounds)}
                    check(bounds.outWidth==bounds.outHeight && bounds.outWidth in 512..8192 && bounds.outWidth%512==0){"Unsupported HD item icon atlas"}
                    val options=android.graphics.BitmapFactory.Options().apply{inSampleSize=maxOf(1,bounds.outWidth/4096)}
                    val bitmap=c.contentResolver.openInputStream(iconSource)?.use{android.graphics.BitmapFactory.decodeStream(it,null,options)}?:error("Cannot decode the HD item icons")
                    try{writeSwordIcon(stage,bitmap);copied++}finally{bitmap.recycle()}
                }
                val textureRoot=File(c.filesDir,"dekui/load/textures/0004000000125500");val textureStage=File(stage,"texture-overrides");val unique=mutableSetOf<String>()
                for(t in allTextures){val hashes=NativeLibrary.dekuiTextureHashes(t.raw,t.width,t.height,t.format);check(hashes.size==2);val names=hashes.map{java.lang.Long.toUnsignedString(it,16).uppercase().padStart(16,'0')};if(!unique.add(names[0]))continue
                    val source=index[names[1]]?:index[names[0]]?:continue;val extension=DocumentFile.fromSingleUri(c,source)?.name?.substringAfterLast('.')?.lowercase()?:"png"
                    val bytes=c.contentResolver.openInputStream(source)?.use{input->
                        val output=java.io.ByteArrayOutputStream();val buffer=ByteArray(65536)
                        while(true){val n=input.read(buffer);if(n<0)break;check(output.size().toLong()+n<=64L*1024*1024){"Replacement texture is too large"};output.write(buffer,0,n)}
                        output.toByteArray()
                    }?:error("Cannot read replacement texture")
                    check(bytes.size<=64*1024*1024){"Replacement texture is too large"}
                    for(hash in names.distinct())atomic(File(textureStage,"tex1_${t.width}x${t.height}_${hash}_${t.format}_mip0.$extension"),bytes)
                    copied++
                }
                val config=File(textureRoot,"pack.json");if(!config.exists())atomic(config,JSONObject().put("options",JSONObject().put("skip_mipmap",true).put("flip_png_files",true).put("use_new_hash",true)).toString().toByteArray())
            }
            atomic(File(stage,"generated.json"),report.put("rom",p.getString("romLabel","")).put("hd_textures",copied).toString(2).toByteArray())
            // Publish a complete generation only after every archive has validated.
            val backup=File(root.parentFile,"$modelId-backup-${System.currentTimeMillis()}")
            if(root.exists())check(root.renameTo(backup)){"Could not back up the current model"}
            if(!stage.renameTo(root)){if(backup.exists())backup.renameTo(root);error("Could not install generated models")}
            val generatedTextures=File(root,"texture-overrides")
            if(generatedTextures.isDirectory) {
                val textureRoot=File(c.filesDir,"dekui/load/textures/0004000000125500");val target=File(textureRoot,"$modelId-generated");textureRoot.mkdirs()
                val oldTextures=File(textureRoot,".$modelId-backup-${System.currentTimeMillis()}")
                if(target.exists())check(target.renameTo(oldTextures)){"Could not back up generated textures"}
                if(!generatedTextures.renameTo(target)){if(oldTextures.exists())oldTextures.renameTo(target);error("Could not install generated textures")}
            }
            DekuiPresentationStore(c).update(mapOf("adult_link_model" to !dark,"dark_link_model" to dark))
            BooleanSetting.DISK_SHADER_CACHE.boolean=false
            val settings=mutableListOf<org.citra.citra_emu.features.settings.model.AbstractSetting>(BooleanSetting.DISK_SHADER_CACHE)
            if(copied>0){BooleanSetting.CUSTOM_TEXTURES.boolean=true;settings.add(BooleanSetting.CUSTOM_TEXTURES)}
            check(SettingsFile.saveDekuiSettings(settings)){"Models created, but graphics settings could not be saved"}
            val result="Ready: $modelName, $count model variants, ${voices.clips} adult voice clips, $copied matching HD textures. Restart the game to apply."
            check(p.edit().putString("result",result).commit());return result
        }finally{stage.deleteRecursively()}
    }
}
