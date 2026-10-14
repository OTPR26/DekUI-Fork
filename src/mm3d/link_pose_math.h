// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/camera_midpoint.h"
namespace MM3D::LinkPoseMath {
using namespace CameraMidpoint;
inline Matrix Expand(const std::array<float,12>& a) {
    auto m=Identity();for(unsigned i=0;i<12;++i)m[i/4][i%4]=a[i];return m;
}
inline std::array<float,12> Flatten(const Matrix& m) {
    std::array<float,12> a{};for(unsigned i=0;i<12;++i)a[i]=float(m[i/4][i%4]);return a;
}
inline std::optional<Matrix> Inverse(const Matrix& m) {
    if(!Near(m,m,0))return {};
    const auto a=m[0][0],b=m[0][1],c=m[0][2],d=m[1][0],e=m[1][1],f=m[1][2],g=m[2][0],h=m[2][1],i=m[2][2];
    const auto det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    if(std::abs(det)<1e-12)return {};
    Matrix r=Identity();r[0]={ (e*i-f*h)/det,(c*h-b*i)/det,(b*f-c*e)/det,0};
    r[1]={(f*g-d*i)/det,(a*i-c*g)/det,(c*d-a*f)/det,0};
    r[2]={(d*h-e*g)/det,(b*g-a*h)/det,(a*e-b*d)/det,0};
    for(unsigned row=0;row<3;++row)for(unsigned k=0;k<3;++k)r[row][3]-=r[row][k]*m[k][3];
    return r;
}
inline Matrix Rest(const std::array<float,9>& v) {
    const auto cx=std::cos(v[3]),cy=std::cos(v[4]),cz=std::cos(v[5]);
    const auto sx=std::sin(v[3]),sy=std::sin(v[4]),sz=std::sin(v[5]);
    Matrix m=Identity();
    m[0]={cy*cz*v[0],(sx*sy*cz-cx*sz)*v[1],(cx*sy*cz+sx*sz)*v[2],v[6]};
    m[1]={cy*sz*v[0],(sx*sy*sz+cx*cz)*v[1],(cx*sy*sz-sx*cz)*v[2],v[7]};
    m[2]={-sy*v[0],sx*cy*v[1],cx*cy*v[2],v[8]};return m;
}
inline std::optional<Matrix> Half(const Matrix& a,const Matrix& b,double max_scale_ratio=2.) {
    Matrix ar=a,br=b;std::array<double,3> scales{};
    for(unsigned col=0;col<3;++col) {
        double as=0,bs=0;for(unsigned row=0;row<3;++row){as+=a[row][col]*a[row][col];bs+=b[row][col]*b[row][col];}
        as=std::sqrt(as);bs=std::sqrt(bs);
        if(as<1e-5 || bs<1e-5 || as/bs<1./max_scale_ratio || as/bs>max_scale_ratio)return {};
        scales[col]=(as+bs)/2;
        for(unsigned row=0;row<3;++row){ar[row][col]/=as;br[row][col]/=bs;}
    }
    if(!Rigid(ar)||!Rigid(br))return {};
    auto result=Midpoint(ar,br);
    for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row)result[row][col]*=scales[col];
    return result;
}
inline bool PaletteNear(const Matrix& a,const Matrix& b) {
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<4;++j)
        if(!std::isfinite(a[i][j])||!std::isfinite(b[i][j])||std::abs(a[i][j]-b[i][j])>(j==3?.03:1e-5))return false;
    return true;
}
}
