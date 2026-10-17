// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.security.MessageDigest

/** Import user-owned OoT3D adult voice samples without changing MM3D sound IDs.
 * Format references: Gota7/Citric-Composer specs and thane98/BCSAR-View.
 * Existing music, sequences, groups, NPC samples, and transformation programs stay intact.
 */
object DekuiVoiceConverter {
    data class Result(val archive:ByteArray,val clips:Int,val mappedKeys:Int)
    private fun checkRange(d:ByteArray,at:Int,n:Int){require(at>=0&&n>=0&&at<=d.size-n){"Invalid sound archive range"}}
    private fun u(d:ByteArray,at:Int):Int{checkRange(d,at,4);return (d[at].toInt()and 255)or((d[at+1].toInt()and 255)shl 8)or((d[at+2].toInt()and 255)shl 16)or(d[at+3].toInt()shl 24)}
    private fun h(d:ByteArray,at:Int):Int{checkRange(d,at,2);return(d[at].toInt()and 255)or((d[at+1].toInt()and 255)shl 8)}
    private fun put(d:ByteArray,at:Int,value:Int){checkRange(d,at,4);for(i in 0..3)d[at+i]=(value ushr(i*8)).toByte()}
    private fun slice(d:ByteArray,at:Int,n:Int):ByteArray{checkRange(d,at,n);return d.copyOfRange(at,at+n)}
    private fun magic(d:ByteArray,at:Int,text:String){require(slice(d,at,4).contentEquals(text.toByteArray(Charsets.US_ASCII))){"Unsupported sound file: expected $text"}}
    private class Buffer(initial:ByteArray=byteArrayOf()) {
        val out=ByteArrayOutputStream().apply{write(initial)}
        val size get()=out.size()
        fun add(data:ByteArray){out.write(data)}
        fun align(n:Int=32){repeat((n-size%n)%n){out.write(0)}}
        fun bytes()=out.toByteArray()
    }
    private data class FileEntry(val offset:Int,val size:Int,val reference:Int)
    private class Sar(val data:ByteArray) {
        val fileBlock:Int
        val files:List<FileEntry?>
        val banks:List<Int>
        val archives:List<Int>
        val names:List<String>
        init {
            require(data.size<=64*1024*1024);magic(data,0,"CSAR");require(h(data,4)==0xfeff&&u(data,12)==data.size&&h(data,16)==3){"Unsupported sound archive header"}
            val blocks=(0 until 3).associate{h(data,20+it*12)to u(data,24+it*12)}
            val strg=blocks[0x2000]?:error("Missing sound names");val info=blocks[0x2001]?:error("Missing sound info");fileBlock=blocks[0x2002]?:error("Missing sound files")
            magic(data,strg,"STRG");magic(data,info,"INFO");magic(data,fileBlock,"FILE")
            val nt=strg+8+u(data,strg+12);val count=u(data,nt);require(count in 1..100000)
            names=(0 until count).map{val at=nt+u(data,nt+8+it*12);val len=u(data,nt+12+it*12);require(len in 1..4096);String(slice(data,at,len-1),Charsets.UTF_8)}
            fun table(index:Int):List<Int>{val t=info+8+u(data,info+12+index*8);val n=u(data,t);require(n in 0..100000);return(0 until n).map{t+u(data,t+8+it*8)}}
            banks=table(2);archives=table(3)
            files=table(6).map{at->when(h(data,at)){
                0x220c->{val ref=at+4+u(data,at+4);val offset=fileBlock+8+u(data,ref);val size=u(data,ref+4);checkRange(data,offset,size);require(ref+8<=fileBlock);FileEntry(offset,size,ref)}
                0x220d->null
                else->error("Unsupported sound file reference")
            }}
        }
        fun payload(id:Int):ByteArray{val f=files.getOrNull(id)?:error("Missing embedded sound file");return slice(data,f.offset,f.size)}
        fun voiceBank():Int{val entry=banks.firstOrNull{names.getOrNull(u(data,it+16))=="BANK_VOICE"}?:error("Voice bank not found");return u(data,entry)}
        fun waveFile(archive:Int):Int{require(archive in archives.indices);return u(data,archives[archive])}
    }
    private data class Key(val velocity:Int,val tableIndex:Int,val archive:Int,val wave:Int)
    private fun keys(d:ByteArray,program:Int):Map<Int,Key> {
        magic(d,0,"CBNK");val body=u(d,24)+8;val instruments=body+u(d,body+12);require(program in 0 until u(d,instruments))
        val instrument=instruments+u(d,instruments+8+program*8);require(h(d,instrument)==0x6002){"Unsupported voice instrument"}
        val table=instrument+u(d,instrument+4);checkRange(d,table,4);val start=d[table].toInt()and 255;val end=d[table+1].toInt()and 255;require(end in start..127)
        val waves=body+u(d,body+4);val waveCount=u(d,waves);require(waveCount in 1..10000)
        return buildMap{for(note in start..end){
            val ref=table+4+(note-start)*8;val offset=u(d,ref+4);if(offset==-1)continue
            val key=table+offset;require(h(d,key)==0x6000){"Unsupported voice key region"};val detail=key+u(d,key+4);require(h(d,detail)==0x5902)
            val velocity=detail+u(d,detail+4);val index=u(d,velocity);require(index in 0 until waveCount);val wave=waves+4+8*index
            put(note,Key(velocity,index,u(d,wave)and 0xffffff,u(d,wave+4)))
        }}
    }
    private fun waves(d:ByteArray):List<ByteArray>{
        magic(d,0,"CWAR");require(u(d,12)==d.size);val info=u(d,24)+8;val file=u(d,36)+8;val count=u(d,info);require(count in 1..10000)
        return(0 until count).map{val ref=info+4+12*it;require(h(d,ref)==0x1f00);val at=file+u(d,ref+4);slice(d,at,u(d,ref+8))}
    }
    private fun packWaves(original:ByteArray,waves:List<ByteArray>):ByteArray {
        val b=Buffer(slice(original,0,64));val info=b.size;b.add("INFO".toByteArray());b.add(ByteArray(8+12*waves.size));b.align();val file=b.size;b.add("FILE".toByteArray());b.add(ByteArray(4));b.align()
        val refs=mutableListOf<Pair<Int,Int>>();val written=mutableMapOf<ByteBuffer,Int>()
        for(wave in waves){val key=ByteBuffer.wrap(wave);val at=written[key]?:b.size.also{written[key]=it;b.add(wave);b.align()};refs.add(at to wave.size)}
        val out=b.bytes();put(out,info+8,waves.size)
        for((i,ref)in refs.withIndex()){put(out,info+12+12*i,0x1f00);put(out,info+16+12*i,ref.first-file-8);put(out,info+20+12*i,ref.second)}
        put(out,12,out.size);put(out,24,info);put(out,28,file-info);put(out,36,file);put(out,40,out.size-file);put(out,info+4,file-info);put(out,file+4,out.size-file)
        return out
    }
    fun convert(oot:ByteArray,mm:ByteArray):Result {
        val source=Sar(oot);val target=Sar(mm);val sourceBank=source.payload(source.voiceBank());val bankId=target.voiceBank();val originalBank=target.payload(bankId);val bank=originalBank.copyOf();val sourceKeys=keys(sourceBank,0)
        val targetKeys=mapOf(0 to keys(originalBank,0),3 to keys(originalBank,3));val targetArchive=targetKeys.getValue(0).getValue(0).archive
        val archiveId=target.waveFile(targetArchive);val oldArchive=target.payload(archiveId);val oldWaves=waves(oldArchive);val newWaves=oldWaves.toMutableList()
        val mapping=linkedMapOf<Pair<Int,Int>,Int>()
        // MM3D human Link uses the child sequence family (legacy DUMMY_32 onward),
        // not the retained adult LI_* slots. Bank 0 keys 28..54 are the child core.
        for(note in 28..46)mapping[0 to note]=note-28
        for((to,from)in listOf(47 to 19,48 to 23,49 to 24,50 to 25,51 to 55,52 to 56,53 to 26,54 to 27,
            64 to 77,67 to 80,69 to 81,70 to 82,71 to 83,72 to 84,73 to 85,74 to 86,75 to 87,76 to 88,105 to 21))mapping[0 to to]=from
        // MM3D-specific human idle clips are in program 3; other programs remain intact.
        for((to,from)in listOf(72 to 86,73 to 87,74 to 88,75 to 86))mapping[3 to to]=from
        val body=u(bank,24)+8;val oldTable=body+u(bank,body+4);val oldCount=u(bank,oldTable);val table=Buffer(slice(bank,oldTable,4+8*oldCount));var count=oldCount
        val sourceArchives=mutableMapOf<Int,List<ByteArray>>();val imported=mutableMapOf<Pair<Int,Int>,Int>()
        for((destination,sourceNote)in mapping){
            val old=targetKeys.getValue(destination.first)[destination.second]?:error("Missing MM3D human voice key")
            val adult=sourceKeys[sourceNote]?:error("Missing OoT3D adult voice key");require(old.archive==targetArchive)
            val samples=sourceArchives.getOrPut(adult.archive){waves(source.payload(source.waveFile(adult.archive)))};val sample=samples.getOrNull(adult.wave)?:error("Missing adult voice sample")
            magic(sample,0,"CWAV");require(u(sample,12)==sample.size){"Invalid adult voice sample"}
            val waveId=imported.getOrPut(adult.archive to adult.wave){newWaves.size.also{newWaves.add(sample)}}
            val row=ByteArray(8);put(row,0,0x05000000 or targetArchive);put(row,4,waveId);table.add(row);put(bank,old.velocity,count++)
        }
        val rows=table.bytes();put(rows,0,count);val bankOut=Buffer(bank);bankOut.align(4);val tableAt=bankOut.size;bankOut.add(rows);val newBank=bankOut.bytes();put(newBank,body+4,tableAt-body);put(newBank,12,newBank.size);put(newBank,u(newBank,24)+4,newBank.size-u(newBank,24))
        val replacements=mapOf(bankId to newBank,archiveId to packWaves(oldArchive,newWaves))
        // Several file-table entries point inside group containers. Preserve that
        // nesting so unrelated groups retain their embedded data without duplication.
        val roots=mutableMapOf<Int,Int>()
        for((i,file)in target.files.withIndex())if(file!=null){
            val containers=target.files.withIndex().filter{(j,f)->f!=null&&j!=i&&f.offset<=file.offset&&file.offset+file.size<=f.offset+f.size&&(f.size>file.size||j<i)}
            roots[i]=containers.maxByOrNull{it.value!!.size}?.index?:i
        }
        val result=Buffer(slice(mm,0,target.fileBlock+8));result.align();val positions=mutableMapOf<Int,Int>()
        for(id in roots.values.distinct().sortedBy{target.files[it]!!.offset}){positions[id]=result.size;result.add(replacements[id]?:target.payload(id));result.align()}
        val out=result.bytes()
        for((i,file)in target.files.withIndex())if(file!=null){val root=roots.getValue(i);if(i in replacements)require(root==i);val at=positions.getValue(root)+file.offset-target.files[root]!!.offset;put(out,file.reference,at-target.fileBlock-8);put(out,file.reference+4,replacements[i]?.size?:file.size)}
        put(out,12,out.size);put(out,52,out.size-target.fileBlock);put(out,target.fileBlock+4,out.size-target.fileBlock)
        // Fail before publishing if any original sound file or wave was changed.
        val verified=Sar(out)
        for((i,file)in target.files.withIndex())if(file!=null&&i !in replacements)require(verified.payload(i).contentEquals(target.payload(i))){"An unrelated sound file changed"}
        val checkBank=verified.payload(bankId)
        for(program in 0..3){val before=keys(originalBank,program);val after=keys(checkBank,program);for((note,key)in before)if((program to note) !in mapping)require(slice(originalBank,key.velocity,40).contentEquals(slice(checkBank,after.getValue(note).velocity,40))){"An unrelated voice key changed"}}
        val checkWaves=waves(verified.payload(archiveId));for(i in oldWaves.indices)require(checkWaves[i].contentEquals(oldWaves[i]))
        return Result(out,imported.size,mapping.size)
    }
}
