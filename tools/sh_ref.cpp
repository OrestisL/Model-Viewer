// CPU reference for the spherical-harmonics colour evaluation the vertex shader
// will do. Validates: (a) constants, (b) degree-0 reproduces the old flat colour
// 0.5 + C0*dc exactly, (c) no NaNs and sane ranges on the real cloud across
// many view directions.
#include "scene/SplatLoader.hpp"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static const float C0 = 0.28209479177387814f;
static const float C1 = 0.4886025119029199f;
static const float C2[5] = {1.0925484305920792f, -1.0925484305920792f, 0.31539156525252005f,
                            -1.0925484305920792f, 0.5462742152960396f};
static const float C3[7] = {-0.5900435899266435f, 2.890611442640554f, -0.4570457994644658f,
                             0.3731763325901154f, -0.4570457994644658f, 1.445305721320277f,
                             -0.5900435899266435f};

struct V3{float x,y,z;};
static V3 add(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
static V3 mul(float s,V3 a){return {s*a.x,s*a.y,s*a.z};}

// sh0 = DC (f_dc). rest = higher-order, coeff-major RGB interleaved: rest[(k)*3+c].
V3 evalSH(int deg, V3 dir, V3 sh0, const float* rest)
{
    V3 r = mul(C0, sh0);
    auto C=[&](int k)->V3{ return {rest[k*3+0], rest[k*3+1], rest[k*3+2]}; };
    if (deg >= 1) {
        float x=dir.x,y=dir.y,z=dir.z;
        r = add(r, mul(-C1*y, C(0)));
        r = add(r, mul( C1*z, C(1)));
        r = add(r, mul(-C1*x, C(2)));
        if (deg >= 2) {
            float xx=x*x,yy=y*y,zz=z*z,xy=x*y,yz=y*z,xz=x*z;
            r=add(r, mul(C2[0]*xy, C(3)));
            r=add(r, mul(C2[1]*yz, C(4)));
            r=add(r, mul(C2[2]*(2.f*zz-xx-yy), C(5)));
            r=add(r, mul(C2[3]*xz, C(6)));
            r=add(r, mul(C2[4]*(xx-yy), C(7)));
            if (deg >= 3) {
                r=add(r, mul(C3[0]*y*(3.f*xx-yy),        C(8)));
                r=add(r, mul(C3[1]*xy*z,                 C(9)));
                r=add(r, mul(C3[2]*y*(4.f*zz-xx-yy),     C(10)));
                r=add(r, mul(C3[3]*z*(2.f*zz-3.f*xx-3.f*yy), C(11)));
                r=add(r, mul(C3[4]*x*(4.f*zz-xx-yy),     C(12)));
                r=add(r, mul(C3[5]*z*(xx-yy),            C(13)));
                r=add(r, mul(C3[6]*x*(xx-3.f*yy),        C(14)));
            }
        }
    }
    return {r.x+0.5f, r.y+0.5f, r.z+0.5f};
}

int main(int argc,char**argv){
    if(argc<2){std::fprintf(stderr,"usage: %s file\n",argv[0]);return 2;}
    mv::SplatCloud c; std::string e;
    if(!mv::SplatLoader::load(argv[1],c,e)){std::fprintf(stderr,"%s\n",e.c_str());return 1;}
    const int deg=c.shDegree, shDim=c.shDim();
    std::printf("cloud: %zu splats, SH degree %d (shDim=%d)\n", c.count(), deg, shDim);

    // (b) degree-0 invariant: eval with deg=0 must equal 0.5 + C0*dc (old flat colour).
    double maxErr=0;
    for(size_t i=0;i<c.count(); i+=997){
        V3 dc{c.colorsDC[i].x,c.colorsDC[i].y,c.colorsDC[i].z};
        V3 got=evalSH(0,{0,0,1},dc,nullptr);
        V3 want{0.5f+C0*dc.x,0.5f+C0*dc.y,0.5f+C0*dc.z};
        maxErr=std::max({maxErr,(double)std::abs(got.x-want.x),(double)std::abs(got.y-want.y),(double)std::abs(got.z-want.z)});
    }
    std::printf("degree-0 invariant max error: %.2e  (%s)\n", maxErr, maxErr<1e-6?"OK":"FAIL");

    // (c) full-degree eval over many directions: NaN check + range stats.
    const V3 dirs[6]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    size_t nan=0,total=0; float lo=1e9f,hi=-1e9f; double sum=0;
    for(size_t i=0;i<c.count(); i+=97){
        V3 dc{c.colorsDC[i].x,c.colorsDC[i].y,c.colorsDC[i].z};
        const float* rest = shDim? &c.sh[i*(size_t)shDim*3] : nullptr;
        for(auto d:dirs){
            V3 col=evalSH(deg,d,dc,rest);
            for(float v:{col.x,col.y,col.z}){
                if(std::isnan(v)||std::isinf(v))nan++;
                else{ lo=std::min(lo,v); hi=std::max(hi,v); sum+=v; total++; }
            }
        }
    }
    std::printf("full eval: %zu samples, NaN/Inf=%zu, range=[%.3f, %.3f], mean=%.3f\n",
                total,nan,lo,hi,sum/total);
    std::printf("%s\n",(maxErr<1e-6 && nan==0)?"SH MATH OK":"PROBLEM");
    return (maxErr<1e-6 && nan==0)?0:1;
}
