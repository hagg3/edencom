

#include "VectorUtil.h"

#include "VecMath.h"

Vector rotateVertice(Vector rot,Vector v){
    Vector r;
    Vec4f pt=Vec4f(v.x,v.y,v.z,1);
    Mat4f mat4=Mat4f::RotationX(rot.x)*Mat4f::RotationY(rot.y)*Mat4f::RotationZ(rot.z);
    //for(int j=0;j<16;j++){
    ///printg("%f ",mat4.f[j]);
    // }
    //printg("---\n");
    pt=mat4*pt;
    r.x=pt.x;
    r.y=pt.y;
    r.z=pt.z;
    return r;
    
}
