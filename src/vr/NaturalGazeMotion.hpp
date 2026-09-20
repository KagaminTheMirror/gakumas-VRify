#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "NaturalGazePolicy.hpp"
namespace gakumas::vr::gaze {
struct V3 { double x=0,y=0,z=0; };
struct Q4 { double x=0,y=0,z=0,w=1; };
constexpr double Pi=3.14159265358979323846;
inline Q4 Mul(Q4 a,Q4 b) {return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
    a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
inline Q4 Inverse(Q4 q) {return {-q.x,-q.y,-q.z,q.w};}
inline Q4 EulerZXY(V3 v) {
    const double x=v.x*Pi/360,y=v.y*Pi/360,z=v.z*Pi/360;
    return Mul(Mul({0,std::sin(y),0,std::cos(y)},{std::sin(x),0,0,std::cos(x)}),{0,0,std::sin(z),std::cos(z)});
}
inline V3 Rotate(Q4 q,V3 v) {auto r=Mul(Mul(q,{v.x,v.y,v.z,0}),Inverse(q));return {r.x,r.y,r.z};}
inline double Delta(double a,double b) {return std::remainder(a-b,360.0);}
inline double Arc(Q4 a,Q4 b) {return 2*std::acos(std::clamp(std::abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w),0.0,1.0))*180/Pi;}
struct NaturalMotion {
    bool previous=false;
    Q4 head;
    double yaw=0,pitch=0;
    void Reset(){*this=NaturalMotion{};}
    // Authored orientation is independent of the gaze-corrected neck. Position
    // is the current head origin, so root translation isn't mistaken for yaw.
    NaturalInput Sample(NaturalInput in,V3 authoredEuler,V3 headPosition,V3 target) {
        auto q=EulerZXY(authoredEuler);
        auto d=Rotate(Inverse(q),{target.x-headPosition.x,target.y-headPosition.y,target.z-headPosition.z});
        const double horizontal=std::hypot(d.x,d.z),length=std::hypot(horizontal,d.y);
        const double y=std::atan2(d.x,d.z)*180/Pi,p=std::atan2(d.y,horizontal)*180/Pi;
        in.degenerateDirection=length<0.01 || horizontal<0.05*length || d.z<=0;
        in.yaw=y;in.pitch=p;
        if(previous && in.dt>0 && in.dt<=0.1) {
            in.animationSpeed=Arc(q,head)/in.dt;
            in.yawRate=Delta(y,yaw)/in.dt;in.pitchRate=Delta(p,pitch)/in.dt;
        } else if(!previous || in.dt>0.1) in.valid=false;
        if(in.dt>0) {head=q;yaw=y;pitch=p;previous=true;}
        return in;
    }
};
}
