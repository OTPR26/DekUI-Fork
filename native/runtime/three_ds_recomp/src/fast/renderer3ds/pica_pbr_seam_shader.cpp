#include "fast/renderer3ds/pica_pbr_seams.h"
namespace Fast::Renderer3ds {
std::string PicaPbrSeamShaderLibrary() {
    return R"glsl(
layout(set=0,binding=26,std430) readonly buffer PicaPbrSeams { vec4 words[]; } pbr_seams;
struct PbrSeamValues { vec3 normal; vec3 orm; vec3 emission; float height; };
struct PbrSeamBlend { PbrSeamValues value; float weight; };
vec2 pbr_seam_uv(uint triangle,uint corner) {
    uint base=triangle*7u;
    return corner==0u?pbr_seams.words[base].xy:corner==1u?pbr_seams.words[base].zw:pbr_seams.words[base+1u].xy;
}
vec3 pbr_seam_position(uint triangle,uint corner) {return pbr_seams.words[triangle*7u+2u+corner].xyz;}
PbrSeamValues pbr_seam_zero() { return PbrSeamValues(vec3(0),vec3(0),vec3(0),0.0); }
PbrSeamValues pbr_seam_add(PbrSeamValues a,PbrSeamValues b,float weight) {
    return PbrSeamValues(a.normal+b.normal*weight,a.orm+b.orm*weight,a.emission+b.emission*weight,a.height+b.height*weight);
}
PbrSeamValues pbr_seam_scale(PbrSeamValues a,float weight) {
    return PbrSeamValues(a.normal*weight,a.orm*weight,a.emission*weight,a.height*weight);
}
PbrSeamValues pbr_seam_sample(uint triangle,vec2 uv,vec3 geometric,vec3 dpdx,vec3 dpdy) {
    vec3 e1=pbr_seam_position(triangle,1u)-pbr_seam_position(triangle,0u);
    vec3 e2=pbr_seam_position(triangle,2u)-pbr_seam_position(triangle,0u);
    vec2 u1=pbr_seam_uv(triangle,1u)-pbr_seam_uv(triangle,0u);
    vec2 u2=pbr_seam_uv(triangle,2u)-pbr_seam_uv(triangle,0u);
    float a=dot(e1,e1),b=dot(e1,e2),c=dot(e2,e2),det=a*c-b*b;
    vec2 gx=vec2(0),gy=vec2(0);
    if(det>1e-20) {
        vec2 x=vec2(c*dot(dpdx,e1)-b*dot(dpdx,e2),a*dot(dpdx,e2)-b*dot(dpdx,e1))/det;
        vec2 y=vec2(c*dot(dpdy,e1)-b*dot(dpdy,e2),a*dot(dpdy,e2)-b*dot(dpdy,e1))/det;
        gx=u1*x.x+u2*x.y;gy=u1*y.x+u2*y.y;
    }
    uint channels=floatBitsToUint(pbr_material.seam_settings.z);
    uint maps=pbr_material.control.x;
    PbrSeamValues result=PbrSeamValues(geometric,vec3(1),vec3(1),1.0);
    float uvdet=u1.x*u2.y-u1.y*u2.x;
    if((channels&1u)!=0u && (maps&1u)!=0u && abs(uvdet)>1e-12 && pbr_material.emissive_normal_scale.w!=0.0) {
        vec3 t=(e1*u2.y-e2*u1.y)*sign(uvdet);
        vec3 bvec=(e2*u1.x-e1*u2.x)*sign(uvdet);
        t-=geometric*dot(geometric,t);
        if(dot(t,t)>1e-20 && dot(bvec,bvec)>1e-20) {
            t=normalize(t);vec3 bnormal=cross(geometric,t);
            bnormal*=dot(bnormal,bvec)<0.0?-1.0:1.0;
            vec3 mapped=textureGrad(pbr_normal_map,uv,gx,gy).xyz*2.0-1.0;
            mapped.xy*=vec2(abs(pbr_material.emissive_normal_scale.w),pbr_material.emissive_normal_scale.w);
            vec3 normal=t*mapped.x+bnormal*mapped.y+geometric*mapped.z;
            if(dot(normal,normal)>1e-20)result.normal=normalize(normal);
        }
    }
    if((channels&14u)!=0u && (maps&2u)!=0u)result.orm=textureGrad(pbr_orm_map,uv,gx,gy).rgb;
    if((channels&16u)!=0u && (maps&4u)!=0u)result.emission=pbr_srgb_to_linear(textureGrad(pbr_emissive_map,uv,gx,gy).rgb);
    if((channels&32u)!=0u && (maps&8u)!=0u)result.height=textureGrad(pbr_height_map,uv,gx,gy).r;
    return result;
}
PbrSeamValues pbr_seam_corner(uint triangle,uint corner,vec3 geometric,vec3 dpdx,vec3 dpdy) {
    uint base=triangle*7u;
    uint count=uint(pbr_seams.words[base+2u+corner].w);
    uint start=floatBitsToUint(pbr_seams.words[base+6u][corner]);
    uint fanBase=floatBitsToUint(pbr_material.seam_settings.w)*7u;
    PbrSeamValues sum=pbr_seam_zero();
    for(uint i=0u;i<count;++i){
        uint index=start+i;
        uint code=floatBitsToUint(pbr_seams.words[fanBase+index/4u][index%4u]);
        sum=pbr_seam_add(sum,pbr_seam_sample(code/3u,pbr_seam_uv(code/3u,code%3u),geometric,dpdx,dpdy),1.0);
    }
    return pbr_seam_scale(sum,1.0/max(float(count),1.0));
}
PbrSeamBlend pbr_seam_blend(uint triangle,vec3 position,vec3 geometric,vec3 dpdx,vec3 dpdy) {
    PbrSeamBlend result=PbrSeamBlend(pbr_seam_zero(),0.0);
    uint count=floatBitsToUint(pbr_material.seam_settings.w);
    if(triangle>=count || pbr_material.seam_settings.x<=0.0 || pbr_material.seam_settings.y<=0.0)return result;
    uvec3 neighbors=floatBitsToUint(pbr_seams.words[triangle*7u+5u].xyz);
    float total=0.0;
    for(uint e=0u;e<3u;++e){
        if(neighbors[e]==0u)continue;
        uint a=(e+1u)%3u,b=(e+2u)%3u;
        vec3 origin=pbr_seam_position(triangle,a),edge=pbr_seam_position(triangle,b)-origin;
        float length2=dot(edge,edge);
        if(length2<=1e-20)continue;
        float t=clamp(dot(position-origin,edge)/length2,0.0,1.0);
        float width=pbr_material.seam_settings.y*sqrt(length2);
        float distance=length(position-origin-edge*t)/width;
        if(distance>=1.0)continue;
        // Interpolatory partition: at an edge its shared value dominates, even
        // where another seam's band overlaps. Corner fans give junctions one value.
        float weight=(1.0-distance)*(1.0-distance)/max(distance*distance,1e-12);
        uint code=neighbors[e]-1u,other=code/3u,opposite=code%3u;
        vec2 uv=mix(pbr_seam_uv(triangle,a),pbr_seam_uv(triangle,b),t);
        vec2 otherUv=mix(pbr_seam_uv(other,(opposite+2u)%3u),pbr_seam_uv(other,(opposite+1u)%3u),t);
        PbrSeamValues sharedValue=pbr_seam_scale(pbr_seam_add(pbr_seam_sample(triangle,uv,geometric,dpdx,dpdy),
            pbr_seam_sample(other,otherUv,geometric,dpdx,dpdy),1.0),0.5);
        float cornerDistance=min(t,1.0-t)/pbr_material.seam_settings.y;
        if(cornerDistance<1.0){
            float cornerWeight=1.0-smoothstep(0.0,1.0,cornerDistance);
            sharedValue=pbr_seam_add(pbr_seam_scale(sharedValue,1.0-cornerWeight),
                pbr_seam_corner(triangle,t<0.5?a:b,geometric,dpdx,dpdy),cornerWeight);
        }
        result.value=pbr_seam_add(result.value,sharedValue,weight);total+=weight;
    }
    if(total>0.0){result.value=pbr_seam_scale(result.value,1.0/total);result.weight=pbr_material.seam_settings.x*(total/(1.0+total));}
    return result;
}
)glsl";
}
} // namespace Fast::Renderer3ds
