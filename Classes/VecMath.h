// VecMath.h — the engine's small vector/matrix types (LG.7, 2026-10-27).
//
// Replaces the PowerVR SDK's PVRTVec3 / PVRTVec4 / PVRTMat4 (Classes/PVRTVector.h, deleted with the
// rest of the SDK) for their uses in Model.cpp, BlockBreak.cpp and VectorUtil.cpp. Only the members
// those files use are here.
//
// BIT-FOR-BIT CONTRACT: every operator keeps the SDK's exact expression (operand order, grouping,
// float vs double), so a result is the same float the SDK gave on the same compiler and flags. That
// matters because creature boxes, collisions and spawn positions run through these, and because
// --creature-selftest pins the skinned-vertex hashes the SDK produced. Do not "tidy" an expression
// here (reorder a sum, factor a product, hoist a reciprocal): with FMA contraction on (clang's default
// on arm64) a regrouped sum is a different float. The conventions are the SDK's too:
//   Mat4f is column-major (f[c*4+r], what glLoadMatrixf takes), Mat4f * Vec4f is M·v, Vec4f * Mat4f
//   is vᵀ·M, and Mat4f * Mat4f applies the right operand first.
#ifndef EDEN_VECMATH_H
#define EDEN_VECMATH_H

#include <math.h>
#include "Vector.h"

struct Vec4f;
struct Mat4f;

struct Vec3f {
    float x, y, z;

    Vec3f() {}
    Vec3f(float fX, float fY, float fZ) { x = fX; y = fY; z = fZ; }
    Vec3f(Vector v) { x = v.x; y = v.y; z = v.z; }
    Vec3f(const float fValue) { x = fValue; y = fValue; z = fValue; }
    Vec3f(const float* pVec) { x = (*pVec++); y = (*pVec++); z = *pVec; }
    Vec3f(const Vec4f& v4);

    Vec3f operator+(const Vec3f& rhs) const { Vec3f out; out.x = x+rhs.x; out.y = y+rhs.y; out.z = z+rhs.z; return out; }
    Vec3f operator-(const Vec3f& rhs) const { Vec3f out; out.x = x-rhs.x; out.y = y-rhs.y; out.z = z-rhs.z; return out; }
    Vec3f& operator+=(const Vec3f& rhs) { x += rhs.x; y += rhs.y; z += rhs.z; return *this; }
    Vec3f& operator-=(const Vec3f& rhs) { x -= rhs.x; y -= rhs.y; z -= rhs.z; return *this; }
    friend Vec3f operator-(const Vec3f& rhs) { return Vec3f(rhs) *= -1.0f; }
    friend Vec3f operator*(const float lhs, const Vec3f& rhs) { Vec3f out; out.x = lhs*rhs.x; out.y = lhs*rhs.y; out.z = lhs*rhs.z; return out; }
    friend Vec3f operator/(const float lhs, const Vec3f& rhs) { Vec3f out; out.x = lhs/rhs.x; out.y = lhs/rhs.y; out.z = lhs/rhs.z; return out; }
    Vec3f operator*(const float& rhs) const { Vec3f out; out.x = x*rhs; out.y = y*rhs; out.z = z*rhs; return out; }
    Vec3f& operator*=(const float& rhs) { x = x*rhs; y = y*rhs; z = z*rhs; return *this; }
    Vec3f operator/(const float& rhs) const { Vec3f out; out.x = x/rhs; out.y = y/rhs; out.z = z/rhs; return out; }
    Vec3f& operator/=(const float& rhs) { x = x/rhs; y = y/rhs; z = z/rhs; return *this; }
    bool operator==(const Vec3f& rhs) const { return ((x == rhs.x) && (y == rhs.y) && (z == rhs.z)); }
    bool operator!=(const Vec3f& rhs) const { return ((x != rhs.x) || (y != rhs.y) || (z != rhs.z)); }

    float lenSqr() const { return x*x+y*y+z*z; }
    float length() const { return (float)sqrt(x*x + y*y + z*z); }
    Vec3f normalize() { float len = length(); x = x/len; y = y/len; z = z/len; return *this; }
    Vec3f normalized() const { Vec3f out; float len = length(); out.x = x/len; out.y = y/len; out.z = z/len; return out; }
    float dot(const Vec3f& rhs) const { return x*rhs.x+y*rhs.y+z*rhs.z; }
    Vec3f cross(const Vec3f& rhs) const {
        Vec3f out;
        out.x = y*rhs.z-z*rhs.y;
        out.y = z*rhs.x-x*rhs.z;
        out.z = x*rhs.y-y*rhs.x;
        return out;
    }
    float* ptr() { return (float*)this; }
};

struct Vec4f {
    float x, y, z, w;

    Vec4f() {}
    Vec4f(const float v) { x = v; y = v; z = v; w = v; }
    Vec4f(float fX, float fY, float fZ, float fW) { x = fX; y = fY; z = fZ; w = fW; }
    Vec4f(const Vec3f& v3, float fW) { x = v3.x; y = v3.y; z = v3.z; w = fW; }
    Vec4f(const float* pVec) { x = (*pVec++); y = (*pVec++); z = (*pVec++); w = *pVec; }

    Vec4f operator+(const Vec4f& rhs) const { Vec4f out; out.x = x+rhs.x; out.y = y+rhs.y; out.z = z+rhs.z; out.w = w+rhs.w; return out; }
    Vec4f operator-(const Vec4f& rhs) const { Vec4f out; out.x = x-rhs.x; out.y = y-rhs.y; out.z = z-rhs.z; out.w = w-rhs.w; return out; }
    Vec4f operator*(const Mat4f& rhs) const;   // vᵀ·M
    Vec4f operator*(const float& rhs) const { Vec4f out; out.x = x*rhs; out.y = y*rhs; out.z = z*rhs; out.w = w*rhs; return out; }
    Vec4f& operator*=(const float& rhs) { x = x*rhs; y = y*rhs; z = z*rhs; w = w*rhs; return *this; }
    friend Vec4f operator*(const float lhs, const Vec4f& rhs) { Vec4f out; out.x = lhs*rhs.x; out.y = lhs*rhs.y; out.z = lhs*rhs.z; out.w = lhs*rhs.w; return out; }
    bool operator==(const Vec4f& rhs) const { return ((x == rhs.x) && (y == rhs.y) && (z == rhs.z) && (w == rhs.w)); }
    bool operator!=(const Vec4f& rhs) const { return ((x != rhs.x) || (y != rhs.y) || (z != rhs.z) || (w != rhs.w)); }

    float length() const { return (float)sqrt(x*x + y*y + z*z + w*w); }
    float dot(const Vec4f& rhs) const { return x*rhs.x+y*rhs.y+z*rhs.z+w*rhs.w; }
    float* ptr() { return (float*)this; }
};

inline Vec3f::Vec3f(const Vec4f& v4) { x = v4.x; y = v4.y; z = v4.z; }

struct Mat4f {
    float f[16];

    Mat4f() {}
    Mat4f(const float* mat) { float* p = f; for (int i = 0; i < 16; i++) (*p++) = (*mat++); }

    float& operator()(const int r, const int c) { return f[c*4+r]; }
    const float& operator()(const int r, const int c) const { return f[c*4+r]; }

    // this · rhs (rhs applies first).
    Mat4f operator*(const Mat4f& rhs) const {
        Mat4f out;
        out.f[0]  = f[0]*rhs.f[0]+f[4]*rhs.f[1]+f[8]*rhs.f[2]+f[12]*rhs.f[3];
        out.f[1]  = f[1]*rhs.f[0]+f[5]*rhs.f[1]+f[9]*rhs.f[2]+f[13]*rhs.f[3];
        out.f[2]  = f[2]*rhs.f[0]+f[6]*rhs.f[1]+f[10]*rhs.f[2]+f[14]*rhs.f[3];
        out.f[3]  = f[3]*rhs.f[0]+f[7]*rhs.f[1]+f[11]*rhs.f[2]+f[15]*rhs.f[3];
        out.f[4]  = f[0]*rhs.f[4]+f[4]*rhs.f[5]+f[8]*rhs.f[6]+f[12]*rhs.f[7];
        out.f[5]  = f[1]*rhs.f[4]+f[5]*rhs.f[5]+f[9]*rhs.f[6]+f[13]*rhs.f[7];
        out.f[6]  = f[2]*rhs.f[4]+f[6]*rhs.f[5]+f[10]*rhs.f[6]+f[14]*rhs.f[7];
        out.f[7]  = f[3]*rhs.f[4]+f[7]*rhs.f[5]+f[11]*rhs.f[6]+f[15]*rhs.f[7];
        out.f[8]  = f[0]*rhs.f[8]+f[4]*rhs.f[9]+f[8]*rhs.f[10]+f[12]*rhs.f[11];
        out.f[9]  = f[1]*rhs.f[8]+f[5]*rhs.f[9]+f[9]*rhs.f[10]+f[13]*rhs.f[11];
        out.f[10] = f[2]*rhs.f[8]+f[6]*rhs.f[9]+f[10]*rhs.f[10]+f[14]*rhs.f[11];
        out.f[11] = f[3]*rhs.f[8]+f[7]*rhs.f[9]+f[11]*rhs.f[10]+f[15]*rhs.f[11];
        out.f[12] = f[0]*rhs.f[12]+f[4]*rhs.f[13]+f[8]*rhs.f[14]+f[12]*rhs.f[15];
        out.f[13] = f[1]*rhs.f[12]+f[5]*rhs.f[13]+f[9]*rhs.f[14]+f[13]*rhs.f[15];
        out.f[14] = f[2]*rhs.f[12]+f[6]*rhs.f[13]+f[10]*rhs.f[14]+f[14]*rhs.f[15];
        out.f[15] = f[3]*rhs.f[12]+f[7]*rhs.f[13]+f[11]*rhs.f[14]+f[15]*rhs.f[15];
        return out;
    }
    // M·v.
    Vec4f operator*(const Vec4f& rhs) const {
        Vec4f out;
        out.x = rhs.x*f[0]+rhs.y*f[4]+rhs.z*f[8]+rhs.w*f[12];
        out.y = rhs.x*f[1]+rhs.y*f[5]+rhs.z*f[9]+rhs.w*f[13];
        out.z = rhs.x*f[2]+rhs.y*f[6]+rhs.z*f[10]+rhs.w*f[14];
        out.w = rhs.x*f[3]+rhs.y*f[7]+rhs.z*f[11]+rhs.w*f[15];
        return out;
    }

    // Inverse of an affine matrix (upper 3x3 A, translation row C, last column 0 0 0 1), in float.
    // A singular A leaves the result uninitialised, as the SDK's did.
    Mat4f inverse() const {
        Mat4f out;
        float det_1;
        float pos, neg, temp;
        pos = neg = 0.0f;
        temp = (f[ 0]*f[ 5])*f[10];
        if (temp >= 0) pos += temp; else neg += temp;
        temp = (f[ 4]*f[ 9])*f[ 2];
        if (temp >= 0) pos += temp; else neg += temp;
        temp = (f[ 8]*f[ 1])*f[ 6];
        if (temp >= 0) pos += temp; else neg += temp;
        temp = (-f[ 8]*f[ 5])*f[ 2];
        if (temp >= 0) pos += temp; else neg += temp;
        temp = (-f[ 4]*f[ 1])*f[10];
        if (temp >= 0) pos += temp; else neg += temp;
        temp = (-f[ 0]*f[ 9])*f[ 6];
        if (temp >= 0) pos += temp; else neg += temp;
        det_1 = pos + neg;
        if (det_1 != 0.0f) {
            det_1 = 1.0f/det_1;
            out.f[ 0] =   ((f[ 5]*f[10]) - (f[ 9]*f[ 6]))*det_1;
            out.f[ 1] = - (((f[ 1]*f[10]) - (f[ 9]*f[ 2]))*det_1);
            out.f[ 2] =   ((f[ 1]*f[ 6]) - (f[ 5]*f[ 2]))*det_1;
            out.f[ 4] = - (((f[ 4]*f[10]) - (f[ 8]*f[ 6]))*det_1);
            out.f[ 5] =   ((f[ 0]*f[10]) - (f[ 8]*f[ 2]))*det_1;
            out.f[ 6] = - (((f[ 0]*f[ 6]) - (f[ 4]*f[ 2]))*det_1);
            out.f[ 8] =   ((f[ 4]*f[ 9]) - (f[ 8]*f[ 5]))*det_1;
            out.f[ 9] = - (((f[ 0]*f[ 9]) - (f[ 8]*f[ 1]))*det_1);
            out.f[10] =   ((f[ 0]*f[ 5]) - (f[ 4]*f[ 1]))*det_1;
            out.f[12] = - ((f[12]*out.f[ 0]) + (f[13]*out.f[ 4]) + (f[14]*out.f[ 8]));
            out.f[13] = - ((f[12]*out.f[ 1]) + (f[13]*out.f[ 5]) + (f[14]*out.f[ 9]));
            out.f[14] = - ((f[12]*out.f[ 2]) + (f[13]*out.f[ 6]) + (f[14]*out.f[10]));
            out.f[ 3] = 0.0f;
            out.f[ 7] = 0.0f;
            out.f[11] = 0.0f;
            out.f[15] = 1.0f;
        }
        return out;
    }
    Mat4f transpose() const {
        Mat4f out;
        out.f[0]  = f[0];  out.f[1]  = f[4];  out.f[2]  = f[8];  out.f[3]  = f[12];
        out.f[4]  = f[1];  out.f[5]  = f[5];  out.f[6]  = f[9];  out.f[7]  = f[13];
        out.f[8]  = f[2];  out.f[9]  = f[6];  out.f[10] = f[10]; out.f[11] = f[14];
        out.f[12] = f[3];  out.f[13] = f[7];  out.f[14] = f[11]; out.f[15] = f[15];
        return out;
    }
    float* ptr() { return (float*)&f; }

    static Mat4f Identity() {
        Mat4f out;
        out.f[0] = 1.0f; out.f[1] = 0;    out.f[2] = 0;     out.f[3] = 0;
        out.f[4] = 0;    out.f[5] = 1.0f; out.f[6] = 0;     out.f[7] = 0;
        out.f[8] = 0;    out.f[9] = 0;    out.f[10] = 1.0f; out.f[11] = 0;
        out.f[12] = 0;   out.f[13] = 0;   out.f[14] = 0;    out.f[15] = 1.0f;
        return out;
    }
    static Mat4f Translation(const float tx, const float ty, const float tz) {
        Mat4f out = Identity();
        out.f[12] = tx; out.f[13] = ty; out.f[14] = tz;
        return out;
    }
    static Mat4f Scale(const float fx, const float fy, const float fz) {
        Mat4f out = Identity();
        out.f[0] = fx; out.f[5] = fy; out.f[10] = fz;
        return out;
    }
    // Rotations by `angle` radians; cos/sin are taken in float, as the SDK did.
    static Mat4f RotationX(float angle) {
        float c = (float)cos(angle), s = (float)sin(angle);
        Mat4f out = Identity();
        out.f[5] = c;  out.f[9] = s;
        out.f[6] = -s; out.f[10] = c;
        return out;
    }
    static Mat4f RotationY(float angle) {
        float c = (float)cos(angle), s = (float)sin(angle);
        Mat4f out = Identity();
        out.f[0] = c; out.f[8] = -s;
        out.f[2] = s; out.f[10] = c;
        return out;
    }
    static Mat4f RotationZ(float angle) {
        float c = (float)cos(angle), s = (float)sin(angle);
        Mat4f out = Identity();
        out.f[0] = c;  out.f[4] = s;
        out.f[1] = -s; out.f[5] = c;
        return out;
    }
};

inline Vec4f Vec4f::operator*(const Mat4f& rhs) const {
    Vec4f out;
    out.x = x*rhs.f[0]+y*rhs.f[1]+z*rhs.f[2]+w*rhs.f[3];
    out.y = x*rhs.f[4]+y*rhs.f[5]+z*rhs.f[6]+w*rhs.f[7];
    out.z = x*rhs.f[8]+y*rhs.f[9]+z*rhs.f[10]+w*rhs.f[11];
    out.w = x*rhs.f[12]+y*rhs.f[13]+z*rhs.f[14]+w*rhs.f[15];
    return out;
}

#endif
