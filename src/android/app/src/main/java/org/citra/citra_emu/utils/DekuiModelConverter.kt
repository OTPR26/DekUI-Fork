// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.utils

import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.*

/** Model conversion only. Contains no game assets; consumes the player's own dumps. */
object DekuiModelConverter {
    fun u(d:ByteArray,o:Int)=ByteBuffer.wrap(d,o,4).order(ByteOrder.LITTLE_ENDIAN).int
    fun h(d:ByteArray,o:Int)=(d[o].toInt() and 255) or ((d[o+1].toInt() and 255) shl 8)
    private fun s(d:ByteArray,o:Int)=h(d,o).toShort().toInt()
    fun f(d:ByteArray,o:Int)=Float.fromBits(u(d,o)).toDouble()
    fun put(d:ByteArray,o:Int,v:Int,n:Int=4){for(i in 0 until n)d[o+i]=(v ushr (8*i)).toByte()}
    private fun tag(d:ByteArray,o:Int=0)=String(d,o,4,Charsets.US_ASCII)
    private fun slice(d:ByteArray,o:Int,n:Int):ByteArray {require(o>=0&&n>=0&&o.toLong()+n<=d.size){"Invalid model range"};return d.copyOfRange(o,o+n)}
    class Buf(initial:ByteArray=byteArrayOf()) {
        private var data=initial.copyOf(max(256,initial.size));var size=initial.size;private set
        fun add(b:ByteArray):Int{val p=size;if(size+b.size>data.size)data=data.copyOf(max(data.size*2,size+b.size));b.copyInto(data,size);size+=b.size;return p}
        fun align(n:Int):Int{while(size%n!=0)add(byteArrayOf(0));return size}
        fun int(o:Int,v:Int,n:Int=4)=put(data,o,v,n)
        fun bytes()=data.copyOf(size)
    }
    private fun chunk(tag:String,payload:ByteArray)=Buf(tag.toByteArray()+ByteArray(4)).apply{add(payload);int(4,size)}.bytes()
    private fun identity()=DoubleArray(16){if(it%5==0)1.0 else 0.0}
    private fun mul(a:DoubleArray,b:DoubleArray)=DoubleArray(16){i->(0..3).sumOf{k->a[(i/4)*4+k]*b[k*4+i%4]}}
    private fun point(a:DoubleArray,v:DoubleArray,w:Double=1.0)=DoubleArray(3){i->(0..2).sumOf{a[i*4+it]*v[it]}+a[i*4+3]*w}
    private fun inverse(a:DoubleArray):DoubleArray {
        val rows=Array(4){r->DoubleArray(8){c->if(c<4)a[r*4+c] else if(c-4==r)1.0 else 0.0}}
        for(c in 0..3){val p=(c..3).maxBy{abs(rows[it][c])};val tmp=rows[p];rows[p]=rows[c];rows[c]=tmp;val scale=rows[c][c];require(abs(scale)>1e-12);for(i in 0..7)rows[c][i]/=scale;for(r in 0..3)if(r!=c){val k=rows[r][c];for(i in 0..7)rows[r][i]-=k*rows[c][i]}}
        return DoubleArray(16){rows[it/4][4+it%4]}
    }
    data class Entry(val name:String,val offset:Int,val bytes:ByteArray)
    fun entries(d:ByteArray):List<Entry>{require(tag(d)=="GAR\u0002"||tag(d)=="ZAR\u0001"){"Unsupported actor archive"};require(u(d,4)==d.size);val mm=tag(d)=="GAR\u0002";val meta=u(d,16);val offsets=u(d,20);return (0 until h(d,10)).map{i->val at=meta+i*(if(mm)12 else 8);val np=u(d,at+if(mm)8 else 4);var end=np;while(end<d.size&&d[end]!=0.toByte())end++;val off=u(d,offsets+4*i);Entry(String(slice(d,np,end-np),Charsets.UTF_8),off,slice(d,off,u(d,at)))}}
    fun decompress(d:ByteArray):ByteArray {
        require(tag(d)=="LzS\u0001"&&u(d,12)==d.size-16){"Unsupported compressed actor"};val size=u(d,8);require(size in 1..64*1024*1024)
        val out=ByteArray(size);val ring=ByteArray(4096);var write=0xfee;var at=16;var pos=0
        while(at<d.size&&pos<size){val flags=d[at++].toInt() and 255;for(bit in 0..7){if(pos==size)break;if(flags and (1 shl bit)!=0){require(at<d.size);val v=d[at++];out[pos++]=v;ring[write]=v;write=(write+1)and 4095}else{require(at+1<d.size);val a=d[at++].toInt()and 255;val b=d[at++].toInt()and 255;var read=a or ((b and 240)shl 4);repeat((b and 15)+3){require(pos<size);val v=ring[read];read=(read+1)and 4095;out[pos++]=v;ring[write]=v;write=(write+1)and 4095}}}}
        require(pos==size);return out
    }
    private fun literals(raw:ByteArray,header:ByteArray):ByteArray{val payload=Buf();var p=0;while(p<raw.size){val n=min(8,raw.size-p);payload.add(byteArrayOf(((1 shl n)-1).toByte()));payload.add(slice(raw,p,n));p+=n};return Buf(header.copyOf(8)+ByteArray(8)).apply{int(8,raw.size);int(12,payload.size);add(payload.bytes())}.bytes()}
    private fun repack(gar:ByteArray,model:ByteArray):ByteArray{val es=entries(gar);val out=Buf(gar.copyOf(es.minOf{it.offset}));for((i,e)in es.withIndex()){out.align(4);out.int(u(gar,20)+i*4,out.size);val data=if(e.name=="child/model/link_child.cmb")model else e.bytes;out.int(u(gar,16)+i*12,data.size);out.add(data)};out.int(4,out.size);return out.bytes()}
    data class Prim(val offset:Int,val data:ByteArray,val mode:Int,val bones:IntArray,val indices:IntArray)
    class Model(val data:ByteArray){
        val mm=u(data,8)==10;val attrCount=if(mm)9 else 8
        val keys=if(mm)listOf("skl","qtrs","mats","tex","sklm","luts","vatr","indices","textures")else listOf("skl","mats","tex","sklm","luts","vatr","indices","textures")
        val offsets=keys.mapIndexed{i,k->k to u(data,0x24+4*i)}.toMap()
        val chunks=keys.associateWith{k->val o=offsets.getValue(k);val end=when(k){"mats"->offsets.getValue("tex");"indices"->offsets.getValue("textures");"textures"->data.size;else->o+u(data,o+4)};slice(data,o,end-o)}
        private val matStride=if(mm)0x16c else 0x15c
        val materials=(0 until u(chunks.getValue("mats"),8)).map{slice(chunks.getValue("mats"),12+it*matStride,matStride)}
        val combiners=chunks.getValue("mats").copyOfRange(12+materials.size*matStride,chunks.getValue("mats").size)
        val tex=(0 until u(chunks.getValue("tex"),8)).map{slice(chunks.getValue("tex"),12+it*36,36)}
        val buffers=(0 until attrCount).map{i->val v=chunks.getValue("vatr");slice(v,u(v,16+i*8),u(v,12+i*8))}
        private val sk=chunks.getValue("sklm");val meshHeader=slice(sk,u(sk,8),16);val shapeHeader=slice(sk,u(sk,12),16)
        val meshes=(0 until u(meshHeader,8)).map{slice(sk,u(sk,8)+16+it*(if(mm)12 else 4),if(mm)12 else 4)}
        val shapes=(0 until u(shapeHeader,8)).map{val o=u(sk,12)+h(sk,u(sk,12)+16+it*2);slice(sk,o,u(sk,o+4))}
        val world=mutableListOf<DoubleArray>()
        init{require(tag(data)=="cmb "&&u(data,8)in listOf(6,10));val skl=chunks.getValue("skl");for(i in 0 until u(skl,8)){val o=16+i*(if(mm)44 else 40);val parent=s(skl,o+2);val sx=f(skl,o+4);val sy=f(skl,o+8);val sz=f(skl,o+12);val rx=f(skl,o+16);val ry=f(skl,o+20);val rz=f(skl,o+24)
            val x=identity();x[5]=cos(rx);x[6]=-sin(rx);x[9]=sin(rx);x[10]=cos(rx)
            val y=identity();y[0]=cos(ry);y[2]=sin(ry);y[8]=-sin(ry);y[10]=cos(ry)
            val z=identity();z[0]=cos(rz);z[1]=-sin(rz);z[4]=sin(rz);z[5]=cos(rz)
            val scale=identity();scale[0]=sx;scale[5]=sy;scale[10]=sz;val local=mul(mul(mul(z,y),x),scale);local[3]=f(skl,o+28);local[7]=f(skl,o+32);local[11]=f(skl,o+36);world.add(if(parent>=0)mul(world[parent],local)else local)
        }}
        fun primitives(sh:ByteArray):List<Prim>{val base=if(mm)0x124 else 0x108;return(0 until h(sh,8)).map{i->val o=h(sh,base+i*2);val p=slice(sh,o,u(sh,o+4));require(tag(p)=="prms");val bo=u(p,16);val bones=IntArray(h(p,14)){h(p,bo+it*2)};val r=u(p,20);val count=h(p,r+20);val at=h(p,r+22)*2;val typ=h(p,r+16);val idx=IntArray(count){readValue(chunks.getValue("indices"),at+it*typeSize(typ),typ).toInt()};Prim(o,p,h(p,12),bones,idx)}}
        fun values(sh:ByteArray,attr:Int,n:Int,dim:Int):Array<DoubleArray>{val o=0x24+attr*28;val scale=f(sh,o+4);val typ=h(sh,o+8);val mode=h(sh,o+10);return Array(n){v->DoubleArray(dim){c->if(mode==1)f(sh,o+12+c*4)else{require(mode==0);readValue(buffers[attr],u(sh,o)+(v*dim+c)*typeSize(typ),typ)*scale}}}}
    }
    private fun typeSize(t:Int)=when(t){0x1400,0x1401->1;0x1402,0x1403->2;0x1404,0x1405,0x1406->4;else->error("Unsupported vertex type")}
    private fun readValue(d:ByteArray,o:Int,t:Int):Double{require(o>=0&&o+typeSize(t)<=d.size){"Invalid vertex stream"};return when(t){0x1400->d[o].toDouble();0x1401->(d[o].toInt()and 255).toDouble();0x1402->s(d,o).toDouble();0x1403->h(d,o).toDouble();0x1406->f(d,o);else->u(d,o).toDouble()}}
    private val mapping=IntArray(25){if(it<13)it else it+1}
    private const val height=6555.463137/3986.693367
    data class Geo(val pos:Array<DoubleArray>,val normal:Array<DoubleArray>,val bi:Array<DoubleArray>,val bw:Array<DoubleArray>,val prims:List<Prim>)
    private fun geometry(m:Model,si:Int):Geo{val sh=m.shapes[si];val pr=m.primitives(sh);val n=1+pr.maxOf{it.indices.maxOrNull()?:0};val dim=h(sh,if(m.mm)0x120 else 0x104);return Geo(m.values(sh,0,n,3),m.values(sh,1,n,3),m.values(sh,if(m.mm)7 else 6,n,dim),m.values(sh,if(m.mm)8 else 7,n,dim),pr)}
    private fun retarget(a:Model,m:Model,si:Int):Pair<Array<DoubleArray>,Array<DoubleArray>>{val g=geometry(a,si);val p=Array(g.pos.size){g.pos[it].copyOf()};val normal=Array(g.normal.size){g.normal[it].copyOf()};val scale=identity().apply{this[0]=.62;this[5]=.62;this[10]=.62};val matrices=a.world.indices.map{mul(mul(m.world[mapping[it]],scale),inverse(a.world[it]))}
        for(pr in g.prims)for(vi in pr.indices.distinct()){if(pr.mode in 0..1){p[vi]=DoubleArray(3){g.pos[vi][it]*.62}}else{val sum=g.bw[vi].sum();require(sum>0);val out=DoubleArray(3);val n=DoubleArray(3);for(j in g.bi[vi].indices){val w=g.bw[vi][j]/sum;if(w==0.0)continue;val matrix=matrices[pr.bones[g.bi[vi][j].toInt()]];val pp=point(matrix,g.pos[vi]);val nn=point(matrix,g.normal[vi],0.0);for(k in 0..2){out[k]+=pp[k]*w;n[k]+=nn[k]*w}};p[vi]=out;val length=sqrt(n.sumOf{it*it});normal[vi]=DoubleArray(3){if(length==0.0)0.0 else n[it]/length}}};return p to normal}
    private fun worldPoints(m:Model,si:Int):List<DoubleArray>{val g=geometry(m,si);val out=g.pos.map{it.copyOf()}.toTypedArray();for(pr in g.prims)for(v in pr.indices.distinct()){if(pr.mode==2)continue;val b=pr.bones[if(pr.mode==0)0 else g.bi[v][0].toInt()];out[v]=point(m.world[b],g.pos[v])};return out.toList()}
    private fun handPlan(a:Model,m:Model):Pair<Map<Int,List<Int>>,Set<Int>>{
        val candidates=mutableMapOf<Int,Pair<Boolean,Array<DoubleArray>>>()
        for(me in a.meshes)if((me[2].toInt()and 255)==22){val si=h(me,0);val bones=a.primitives(a.shapes[si]).flatMap{it.bones.toList()}.toSet();val left=bones.all{it in setOf(15,16)};if(!left&&!bones.all{it in setOf(19,20)})continue;candidates[si]=left to retarget(a,m,si).first}
        val plan=mutableMapOf<Int,MutableList<Int>>();val removed=mutableSetOf<Int>()
        for(vis in m.meshes.map{it[3].toInt()and 255}.distinct().sorted()){
            val points=mutableListOf<DoubleArray>();val ids=mutableListOf<Int>();val bones=mutableSetOf<Int>()
            for((i,me)in m.meshes.withIndex()){if((me[3].toInt()and 255)!=vis||(me[2].toInt()and 255) !in setOf(2,3))continue;val bs=m.primitives(m.shapes[h(me,0)]).flatMap{it.bones.toList()}.toSet();if(!bs.all{it in setOf(16,17)}&&!bs.all{it in setOf(20,21)})continue;points.addAll(worldPoints(m,h(me,0)));bones.addAll(bs);ids.add(i)}
            if(points.isEmpty())continue;val unique=points.distinctBy{it.toList()};val left=bones.all{it in setOf(16,17)}
            fun distance(x:DoubleArray,y:DoubleArray)=(0..2).sumOf{(x[it]-y[it]).pow(2)}
            val best=candidates.filter{it.value.first==left}.minBy{(_,pair)->val cp=pair.second;cp.sumOf{x->unique.minOf{distance(x,it)}}/cp.size+unique.sumOf{x->cp.minOf{distance(x,it)}}/unique.size}.key
            plan.getOrPut(best){mutableListOf()}.add(vis);removed.addAll(ids)
        };return plan to removed
    }
    private fun floats(values:Array<DoubleArray>):ByteArray{val out=ByteArray(values.sumOf{it.size}*4);var at=0;for(row in values)for(v in row){put(out,at,v.toFloat().toBits());at+=4};return out}
    private fun descriptor(start:Int)=ByteArray(28).apply{put(this,0,start);put(this,4,1f.toBits());put(this,8,0x1406,2)}
    private fun modelBytes(base:Model,chunks:Map<String,ByteArray>,indices:Int):ByteArray{val out=Buf(base.data.copyOf(0x4c));out.int(0x20,indices);for((i,key)in base.keys.withIndex()){out.align(if(key=="textures")128 else 4);out.int(0x24+i*4,out.size);out.add(chunks.getValue(key))};out.int(4,out.size);return out.bytes()}
    /** Color 0=green, 1=blue, 2=red: use original OoT3D CMAB payloads verbatim. */
    fun tunicModel(adult:ByteArray,bodyAnimation:ByteArray,color:Int):ByteArray{
        if(color==0)return adult
        require(color in 1..2&&tag(bodyAnimation)=="cmab");val m=Model(adult);val t=m.tex[1];val raw=u(t,0);require(raw==8192&&h(t,8)==128&&h(t,10)==128)
        val rawStart=u(bodyAnimation,28);val source=rawStart+(if(color==1)2 else 1)*raw
        val out=adult.copyOf();slice(bodyAnimation,source,raw).copyInto(out,m.offsets.getValue("textures")+u(t,16));return out
    }
    fun convert(adultData:ByteArray,nativeArchive:ByteArray,master:Boolean):ByteArray{
        val gar=decompress(nativeArchive);val mm=Model(entries(gar).first{it.name=="child/model/link_child.cmb"}.bytes);val a=Model(adultData)
        require(a.world.size==25&&mm.world.size==26&&a.materials.size==36&&mm.materials.size==42){"Unsupported Link model revision"}
        // Native sword_a/b/c slots are 12/14/16; slot 18 is the separate Great Fairy item.
        val (hands,removed)=handPlan(a,mm);val eqGroups=if(master)mapOf(16 to listOf(12,14,16),31 to listOf(5,6,7),42 to listOf(13,15,17))else emptyMap()
        val equipment=a.meshes.filter{(it[3].toInt()and 255)in eqGroups&&(it[2].toInt()and 255)!=22}.associate{h(it,0)to eqGroups.getValue(it[3].toInt()and 255)}
        val baseSelected=a.meshes.filter{(it[3].toInt()and 255)in setOf(45,46)||h(it,0)in hands};val selected=baseSelected+a.meshes.filter{h(it,0)in equipment}
        val baseMats=baseSelected.map{it[2].toInt()and 255}.distinct().sorted();val usedMats=baseMats+(selected.map{it[2].toInt()and 255}.toSet()-baseMats.toSet()).sorted();val matMap=usedMats.mapIndexed{i,v->v to mm.materials.size+i}.toMap()
        val buffers=mm.buffers.map{Buf(it)};val adultBuffers=listOf(byteArrayOf(),byteArrayOf(),byteArrayOf())+a.buffers.drop(2);val offsets=buffers.mapIndexed{i,b->b.align(4).also{b.add(adultBuffers[i])}}
        val indices=Buf(mm.chunks.getValue("indices"));val indexBase=indices.align(2);indices.add(a.chunks.getValue("indices"))
        val materials=mm.materials.map{it.copyOf()}.toMutableList();val combBase=1+mm.materials.maxOf{ma->(0 until u(ma,0x120)).maxOf{h(ma,0x124+2*it)}};val combiners=Buf(mm.combiners.copyOf(combBase*40))
        fun textures(mats:List<Int>)=mats.flatMap{mi->(0..2).map{s(a.materials[mi],16+it*24)}}.filter{it>=0}.toSet()
        val baseTex=textures(baseMats).sorted();val usedTex=baseTex+(textures(usedMats)-baseTex.toSet()).sorted();val texMap=usedTex.mapIndexed{i,v->v to mm.tex.size+i}.toMap();val texData=Buf(mm.chunks.getValue("textures"));val tex=mm.tex.map{it.copyOf()}.toMutableList()
        for(ti in usedTex){val t=a.tex[ti].copyOf();val old=u(t,16);put(t,16,texData.align(128));texData.add(slice(a.chunks.getValue("textures"),old,u(t,0)));tex.add(t)}
        for(mi in usedMats){val ma=a.materials[mi].copyOf();for(j in 0..2){val at=16+j*24;val ti=s(ma,at);if(ti>=0)put(ma,at,texMap.getValue(ti),2)};for(j in 0 until u(ma,0x120))put(ma,0x124+2*j,h(ma,0x124+2*j)+combBase,2);materials.add(ma+mm.materials[1].copyOfRange(0x15c,0x168)+ByteArray(4).apply{put(this,0,0xada10000.toInt()+mi)})};combiners.add(a.combiners)
        val shapes=mm.shapes.map{it.copyOf()}.toMutableList();val meshes=mutableListOf<ByteArray>();val templates=mm.meshes.associateBy{it[3].toInt()and 255};val swordGroups=setOf(5,6,7,12,13,14,15,16,17)
        for((i,me)in mm.meshes.withIndex()){val vis=me[3].toInt()and 255;if(vis in 28..33||i in removed)continue;if(master&&vis in swordGroups&&(me[2].toInt()and 255) !in setOf(2,3))continue;meshes.add(me.copyOf())}
        for(si in meshes.map{h(it,0)}.distinct().sorted()){val g=geometry(mm,si);require(g.prims.all{it.mode in 0..1});val at=buffers[0].align(4);buffers[0].add(floats(Array(g.pos.size){v->DoubleArray(3){g.pos[v][it]/height}}));descriptor(at).copyInto(shapes[si],0x24)}
        for(me in selected){val si=h(me,0);val mat=me[2].toInt()and 255;val vis=me[3].toInt()and 255;val target=equipment[si]?:hands[si]?:if(vis==45)listOf(28,29)else listOf(33);val source=a.shapes[si];val (pos,norm)=retarget(a,mm,si)
            val tangent=descriptor(0).apply{put(this,10,1,2)};val sh=source.copyOfRange(0,0x5c)+tangent+source.copyOfRange(0x5c,source.size);val flags=h(sh,10);put(sh,10,(flags and 3)or((flags and 3.inv())shl 1),2)
            for(attr in 0..8){if(attr==2)continue;val at=0x24+attr*28;put(sh,at,u(sh,at)+offsets[attr])}
            for((attr,values)in listOf(0 to pos,1 to norm)){val at=buffers[attr].align(4);buffers[attr].add(floats(values));descriptor(at).copyInto(sh,0x24+attr*28)}
            for((j,pr)in a.primitives(source).withIndex()){val no=pr.offset+28;put(sh,0x124+2*j,no,2);val bo=no+u(pr.data,16);for((k,b)in pr.bones.withIndex())put(sh,bo+2*k,mapping[b],2);val po=no+u(pr.data,20);val start=h(sh,po+22)+indexBase/2;require(start<=65535);put(sh,po+22,start,2)}
            put(sh,4,sh.size);val newSi=shapes.size;shapes.add(sh)
            for(group in target){val output=templates.getValue(group).copyOf();put(output,0,newSi,2);output[2]=matMap.getValue(mat).toByte();meshes.add(output)}
        }
        if(!master){val replacements=mapOf(5 to 5,6 to 5,7 to 5,13 to 13,15 to 13,17 to 13,12 to 12,14 to 12,16 to 12)
        fun weapon(me:ByteArray)=(me[3].toInt()and 255)in replacements&&(me[2].toInt()and 255)<42&&(me[2].toInt()and 255) !in setOf(2,3)
        val before=meshes.toList();meshes.removeAll{weapon(it)};for((target,source)in replacements)for(me in before)if((me[3].toInt()and 255)==source&&weapon(me)){val output=templates.getValue(target).copyOf();me.copyOfRange(0,3).copyInto(output);output[3]=target.toByte();meshes.add(output)}}
        val oldOpaque=h(mm.meshHeader,12);val transparent=mm.meshes.drop(oldOpaque).map{h(it,0)to(it[2].toInt()and 255)}.toSet();fun translucent(me:ByteArray)=(h(me,0)to(me[2].toInt()and 255))in transparent
        meshes.sortWith(compareBy<ByteArray>({translucent(it)},{it[2].toInt()and 255}))
        val msh=Buf(mm.meshHeader);msh.int(8,meshes.size);msh.int(12,meshes.count{!translucent(it)},2);for(me in meshes)msh.add(me);msh.int(4,msh.size)
        val shp=Buf(mm.shapeHeader);shp.int(8,shapes.size);shp.add(ByteArray(shapes.size*2));shp.align(4);for((i,sh)in shapes.withIndex()){require(shp.size<65536);shp.int(16+2*i,shp.size,2);shp.add(sh);shp.align(4)};shp.int(4,shp.size)
        val sklm=chunk("sklm",ByteArray(8).apply{put(this,0,16);put(this,4,16+msh.size)}+msh.bytes()+shp.bytes())
        val vatr=Buf("vatr".toByteArray()+ByteArray(80));vatr.int(8,buffers[0].size/12);for((i,b)in buffers.withIndex()){vatr.align(4);vatr.int(12+8*i,b.size);vatr.int(16+8*i,vatr.size);vatr.add(b.bytes())};vatr.int(4,vatr.size)
        val mats=chunk("mats",ByteArray(4).apply{put(this,0,materials.size)}+materials.fold(byteArrayOf()){x,y->x+y}+combiners.bytes());put(mats,4,12+materials.size*(0x16c+0x24))
        val textures=chunk("tex ",ByteArray(4).apply{put(this,0,tex.size)}+tex.fold(byteArrayOf()){x,y->x+y})
        val chunks=mm.chunks.toMutableMap().apply{this["sklm"]=sklm;this["vatr"]=vatr.bytes();this["mats"]=mats;this["tex"]=textures;this["indices"]=indices.bytes();this["textures"]=texData.bytes()}
        val model=modelBytes(mm,chunks,indices.size/2);Model(model).also{check(it.world.size==26)};val rebuilt=repack(gar,model);val packed=literals(rebuilt,nativeArchive);check(decompress(packed).contentEquals(rebuilt));return packed
    }
}
