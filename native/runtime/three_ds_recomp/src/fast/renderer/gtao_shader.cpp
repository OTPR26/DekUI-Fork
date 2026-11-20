#include "fast/renderer/gtao_shader.h"

// GTAO horizon integration follows Jimenez et al., Practical Real-Time
// Strategies for Accurate Indirect Occlusion (2016), and XeGTAO's normal-relative
// radius falloff. The latter is adapted from XeGTAO:
// Copyright (C) 2016-2021, Intel Corporation
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

namespace Fast::Renderer {
namespace {
constexpr const char* kInputs = R"glsl(#version 450
layout(set=0,binding=0) uniform texture2D native_depth;
layout(set=0,binding=1) uniform texture2D view_normals;
layout(set=0,binding=2) uniform texture2D raw_occlusion;
layout(set=0,binding=4) uniform sampler point_sampler;
layout(push_constant) uniform GtaoState {
    uvec2 extent;
    uint padding;
    uint quality;
    vec4 view_x;
    vec4 view_y;
    vec4 depth;
    vec4 radius_fade;
    vec4 response;
    vec4 edge_filter;
} pc;
float view_depth(ivec2 pixel) {
    float d=texelFetch(sampler2D(native_depth,point_sampler),pixel,0).r;
    if(d>=1.0 || d<0.0 || isnan(d)) return pc.radius_fade.w;
    return pc.depth.w!=0.0 ? pc.depth.x+d*pc.depth.z :
        pc.depth.x/max(pc.depth.y-d*pc.depth.z,1.0e-12);
}
vec3 position(ivec2 pixel,float z) {
    vec3 uv=vec3((vec2(pixel)+0.5)/vec2(pc.extent),1.0);
    return vec3(dot(pc.view_x.xyz,uv)*z,dot(pc.view_y.xyz,uv)*z,-z);
}
ivec2 source_pixel(ivec2 low_pixel) {
    return min(low_pixel*2,ivec2(pc.extent)-1);
}
vec3 unit_normal(vec3 encoded) {
    vec3 n=encoded*2.0-1.0;
    return n*inversesqrt(max(dot(n,n),1.0e-12));
}
)glsl";
} // namespace

std::string BuildMultiscaleGtaoShader() {
    std::string source = kInputs;
    source += R"glsl(
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
// FP32 depth remains in the original input: neither half-depth quantization
// nor an independently advanced CPU uniform ring can perturb this estimator.
layout(set=0,binding=3,rg16f) uniform writeonly image2D ao_output;
const float pi=3.14159265358979323846;
float integral(vec2 horizon,float normal_angle,float cos_normal,float normal_length) {
    vec2 h=vec2(-acos(clamp(horizon.y,-1.0,1.0)),acos(clamp(horizon.x,-1.0,1.0)));
    h=normal_angle+clamp(h-vec2(normal_angle),vec2(-pi*.5),vec2(pi*.5));
    vec2 arc=(cos_normal+2.0*h*sin(normal_angle)-cos(2.0*h-normal_angle))*.25;
    return normal_length*(arc.x+arc.y);
}
void main() {
    ivec2 low=ivec2(gl_GlobalInvocationID.xy);
    ivec2 low_extent=(ivec2(pc.extent)+1)/2;
    if(any(greaterThanEqual(low,low_extent))) return;
    ivec2 pixel=source_pixel(low);
    float z=view_depth(pixel);
    vec4 guide=texelFetch(sampler2D(view_normals,point_sampler),pixel,0);
    if(z>=pc.radius_fade.w || z<=0.0 || guide.a<=0.0 ||
       max(pc.response.x,pc.response.y)<=0.0) {
        imageStore(ao_output,low,vec4(0.0)); return;
    }
    vec3 center=position(pixel,z);
    vec3 view=normalize(-center);
    vec3 normal=unit_normal(guide.xyz);
    if(dot(normal,view)<0.0) normal=-normal;
    // A fixed spatial pattern: frame timing, TAA availability and presentation
    // count do not change the sample set. Both scales use the same slices.
    float noise=fract(52.9829189*fract(dot(vec2(low),vec2(.06711056,.00583715))));
    int slices=pc.quality==0u ? 2 : 3;
    int steps=pc.quality==0u ? 2 : 3;
    vec2 occlusion=vec2(0.0);
    float normalization=0.0;
    vec2 radius=max(pc.radius_fade.xy,vec2(.0001));
    for(int slice=0;slice<slices;++slice) {
        float phi=(float(slice)+noise)*pi/float(slices);
        vec2 screen_dir=vec2(cos(phi),sin(phi));
        vec3 direction=vec3(dot(pc.view_x.xy,screen_dir/vec2(pc.extent)),
                            dot(pc.view_y.xy,screen_dir/vec2(pc.extent)),0.0);
        float pixel_world=length(direction)*z;
        direction=normalize(direction);
        vec3 ortho=direction-view*dot(direction,view);
        vec3 axis=normalize(cross(ortho,view));
        vec3 projected=normal-axis*dot(normal,axis);
        float projected_length=length(projected);
        float cos_normal=clamp(dot(projected,view)/max(projected_length,1.0e-6),0.0,1.0);
        float angle=sign(dot(ortho,projected))*acos(cos_normal);
        float sin_normal=sin(angle);
        vec2 low_horizon=vec2(-sin_normal,sin_normal);
        vec2 fine_horizon=low_horizon;
        vec2 broad_horizon=low_horizon;
        for(int band=0;band<2;++band) {
            float screen_radius=radius[band]/max(pixel_world,1.0e-6);
            int band_steps=band==0 ? 1 : steps;
            for(int step=0;step<band_steps;++step) {
                float t=(float(step)+.5)/float(band_steps);
                float radial_fraction=band==0 ? .5 : t*t;
                vec2 offset=round(screen_dir*max(1.0,radial_fraction*screen_radius));
                for(int side=0;side<2;++side) {
                    ivec2 sample_pixel=pixel+ivec2(side==0 ? offset : -offset);
                    if(any(lessThan(sample_pixel,ivec2(0))) ||
                       any(greaterThanEqual(sample_pixel,ivec2(pc.extent)))) continue;
                    float sample_z=view_depth(sample_pixel);
                    if(sample_z>=pc.radius_fade.w || sample_z<=0.0) continue;
                    vec3 delta=position(sample_pixel,sample_z)-center;
                    // Respect the precision of the encoded normal. A sample
                    // inside its angular uncertainty cannot establish an
                    // occluding hemisphere (notably for native RGB8 guides).
                    float hemisphere=dot(delta,normal);
                    float distance_squared=dot(delta,delta);
                    if(hemisphere<=max(z*1.0e-5,1.0e-5) ||
                       hemisphere*hemisphere<=distance_squared*pc.edge_filter.y) continue;
                    float distance=sqrt(distance_squared);
                    if(distance<=1.0e-6) continue;
                    float horizon=dot(delta/distance,view);
                    vec2 weight=clamp((radius-vec2(distance))/(radius*.75),0.0,1.0);
                    fine_horizon[side]=max(fine_horizon[side],mix(low_horizon[side],horizon,weight.x));
                    broad_horizon[side]=max(broad_horizon[side],mix(low_horizon[side],horizon,weight.y));
                }
            }
        }
        float unoccluded=projected_length*(cos_normal+angle*sin_normal);
        occlusion+=max(vec2(unoccluded)-vec2(
            all(equal(fine_horizon,low_horizon)) ? unoccluded :
                integral(fine_horizon,angle,cos_normal,projected_length),
            all(equal(broad_horizon,low_horizon)) ? unoccluded :
                integral(broad_horizon,angle,cos_normal,projected_length)),vec2(0.0));
        normalization+=unoccluded;
    }
    // Normalize the finite slice quadrature against the same unobstructed
    // hemisphere: an empty sloped plane must not darken as the camera rotates.
    occlusion=clamp(occlusion/max(normalization,1.0e-6),0.0,1.0);
    // The large band is incremental, not a second darkening of fine contacts.
    vec2 bands=vec2(occlusion.x,max(occlusion.y-occlusion.x,0.0));
    imageStore(ao_output,low,vec4(bands,0.0,0.0));
}
)glsl";
    return source;
}

std::string BuildMultiscaleGtaoResolveShader() {
    std::string source = kInputs;
    source += R"glsl(
layout(local_size_x=16,local_size_y=8,local_size_z=1) in;
layout(set=0,binding=3,r32f) uniform writeonly image2D ao_output;
// One cooperative load per half-resolution texel, rather than nine repeated
// depth/normal loads per output pixel. The halo also covers odd output extents.
shared vec4 tile_position[77];
shared vec3 tile_normal[77];
shared vec2 tile_bands[77];
shared uint tile_has_occlusion;
void main() {
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);
    ivec2 low_extent=textureSize(sampler2D(raw_occlusion,point_sampler),0);
    const int tile_width=11;
    ivec2 tile_origin=ivec2(gl_WorkGroupID.xy*uvec2(8,4))-1;
    if(gl_LocalInvocationIndex==0u) tile_has_occlusion=0u;
    barrier();
    for(uint i=gl_LocalInvocationIndex;i<77u;i+=128u) {
        ivec2 q=clamp(tile_origin+ivec2(int(i)%tile_width,int(i)/tile_width),
                      ivec2(0),low_extent-1);
        tile_bands[i]=texelFetch(sampler2D(raw_occlusion,point_sampler),q,0).rg;
        if(dot(tile_bands[i],pc.response.xy)>0.0) atomicOr(tile_has_occlusion,1u);
    }
    barrier();
    if(tile_has_occlusion==0u) {
        if(all(lessThan(pixel,ivec2(pc.extent)))) imageStore(ao_output,pixel,vec4(1.0));
        return;
    }
    for(uint i=gl_LocalInvocationIndex;i<77u;i+=128u) {
        ivec2 q=clamp(tile_origin+ivec2(int(i)%tile_width,int(i)/tile_width),
                      ivec2(0),low_extent-1);
        ivec2 p=source_pixel(q);
        vec4 g=texelFetch(sampler2D(view_normals,point_sampler),p,0);
        tile_position[i]=vec4(position(p,view_depth(p)),g.a>0.0 ? 1.0 : 0.0);
        tile_normal[i]=unit_normal(g.xyz);
    }
    barrier();
    if(any(greaterThanEqual(pixel,ivec2(pc.extent)))) return;
    float z=view_depth(pixel);
    vec4 guide=texelFetch(sampler2D(view_normals,point_sampler),pixel,0);
    if(z>=pc.radius_fade.w || z<=0.0 || guide.a<=0.0) {
        imageStore(ao_output,pixel,vec4(1.0)); return;
    }
    vec3 center=position(pixel,z);
    vec3 normal=unit_normal(guide.xyz);
    vec2 low_position=vec2(pixel)*.5;
    ivec2 base=ivec2(round(low_position));
    vec2 sum=vec2(0.0);
    float weights=0.0;
    float pixel_world=z*max(length(pc.view_x.xy/vec2(pc.extent)),
                            length(pc.view_y.xy/vec2(pc.extent)));
    float tolerance=max(pixel_world*mix(4.0,.5,pc.edge_filter.x),1.0e-5);
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        ivec2 q=clamp(base+ivec2(x,y),ivec2(0),low_extent-1);
        ivec2 tile_pixel=base+ivec2(x,y)-tile_origin;
        int index=tile_pixel.y*tile_width+tile_pixel.x;
        vec3 sample_normal=tile_normal[index];
        vec3 delta=tile_position[index].xyz-center;
        // Symmetric point-to-plane rejection follows slopes without bridging
        // foreground/background discontinuities during bilateral upsampling.
        float plane_error=max(abs(dot(normal,delta)),abs(dot(sample_normal,delta)));
        float agreement=abs(dot(normal,sample_normal));
        agreement*=agreement; agreement*=agreement; agreement*=agreement;
        vec2 d=vec2(q)-low_position;
        float weight=exp2(-dot(d,d)-plane_error/tolerance)*agreement*tile_position[index].w;
        sum+=tile_bands[index]*weight;
        weights+=weight;
    }
    vec2 bands=weights>1.0e-8 ? sum/weights : vec2(0.0);
    float occlusion=dot(bands,pc.response.xy);
    float fade=clamp((pc.radius_fade.w-z)/max(pc.radius_fade.w-pc.radius_fade.z,1.0e-5),0.0,1.0);
    float visibility=pow(1.0-clamp(occlusion*fade,0.0,pc.response.w),pc.response.z);
    imageStore(ao_output,pixel,vec4(visibility));
}
)glsl";
    return source;
}
} // namespace Fast::Renderer
