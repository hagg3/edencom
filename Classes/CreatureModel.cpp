// CreatureModel.cpp — `.edm` loading and the creature animation evaluator (LG.7, 2026-10-27).
//
// The matrix and quaternion helpers below compute what the PowerVR SDK's PVRTMatrix*F /
// PVRTMatrixQuaternion*F functions computed, expression for expression (VecMath.h explains why the
// grouping must not change). Their convention is the SDK's row-vector one: matMultiply(out, a, b) is
// "a, then b", and a node's world is scale * rotation * translation * parent-world.
#include "CreatureModel.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

namespace {

struct Quat { float x, y, z, w; };

void matIdentity(Mat4f& m) {
    m.f[ 0]=1.0f; m.f[ 4]=0.0f; m.f[ 8]=0.0f; m.f[12]=0.0f;
    m.f[ 1]=0.0f; m.f[ 5]=1.0f; m.f[ 9]=0.0f; m.f[13]=0.0f;
    m.f[ 2]=0.0f; m.f[ 6]=0.0f; m.f[10]=1.0f; m.f[14]=0.0f;
    m.f[ 3]=0.0f; m.f[ 7]=0.0f; m.f[11]=0.0f; m.f[15]=1.0f;
}

void matTranslation(Mat4f& m, const float x, const float y, const float z) {
    matIdentity(m);
    m.f[12]=x; m.f[13]=y; m.f[14]=z;
}

void matScaling(Mat4f& m, const float x, const float y, const float z) {
    matIdentity(m);
    m.f[0]=x; m.f[5]=y; m.f[10]=z;
}

// out = a, then b (row vectors). out may alias a or b.
#define EDM_MUL(r, c) (a.f[r*4+0]*b.f[0*4+c] + a.f[r*4+1]*b.f[1*4+c] + a.f[r*4+2]*b.f[2*4+c] + a.f[r*4+3]*b.f[3*4+c])
void matMultiply(Mat4f& out, const Mat4f& a, const Mat4f& b) {
    Mat4f r;
    r.f[ 0] = EDM_MUL(0,0); r.f[ 1] = EDM_MUL(0,1); r.f[ 2] = EDM_MUL(0,2); r.f[ 3] = EDM_MUL(0,3);
    r.f[ 4] = EDM_MUL(1,0); r.f[ 5] = EDM_MUL(1,1); r.f[ 6] = EDM_MUL(1,2); r.f[ 7] = EDM_MUL(1,3);
    r.f[ 8] = EDM_MUL(2,0); r.f[ 9] = EDM_MUL(2,1); r.f[10] = EDM_MUL(2,2); r.f[11] = EDM_MUL(2,3);
    r.f[12] = EDM_MUL(3,0); r.f[13] = EDM_MUL(3,1); r.f[14] = EDM_MUL(3,2); r.f[15] = EDM_MUL(3,3);
    out = r;
}
#undef EDM_MUL

// Inverse of an affine matrix, with a double-precision determinant. A singular (or nearly so)
// input leaves `out` untouched. out may alias in.
void matInverse(Mat4f& out, const Mat4f& in) {
    Mat4f d;
    double det_1;
    double pos, neg, temp;

    pos = neg = 0.0;
    temp =  in.f[ 0] * in.f[ 5] * in.f[10];
    if (temp >= 0.0) pos += temp; else neg += temp;
    temp =  in.f[ 4] * in.f[ 9] * in.f[ 2];
    if (temp >= 0.0) pos += temp; else neg += temp;
    temp =  in.f[ 8] * in.f[ 1] * in.f[ 6];
    if (temp >= 0.0) pos += temp; else neg += temp;
    temp = -in.f[ 8] * in.f[ 5] * in.f[ 2];
    if (temp >= 0.0) pos += temp; else neg += temp;
    temp = -in.f[ 4] * in.f[ 1] * in.f[10];
    if (temp >= 0.0) pos += temp; else neg += temp;
    temp = -in.f[ 0] * in.f[ 9] * in.f[ 6];
    if (temp >= 0.0) pos += temp; else neg += temp;
    det_1 = pos + neg;

    if (det_1 == 0.0)
        return;
    double rel = det_1 / (pos - neg);
    if ((rel <= 0 ? -rel : rel) < 1.0e-15)
        return;

    det_1 = 1.0 / det_1;
    d.f[ 0] =   ( in.f[ 5] * in.f[10] - in.f[ 9] * in.f[ 6] ) * (float)det_1;
    d.f[ 1] = - ( in.f[ 1] * in.f[10] - in.f[ 9] * in.f[ 2] ) * (float)det_1;
    d.f[ 2] =   ( in.f[ 1] * in.f[ 6] - in.f[ 5] * in.f[ 2] ) * (float)det_1;
    d.f[ 4] = - ( in.f[ 4] * in.f[10] - in.f[ 8] * in.f[ 6] ) * (float)det_1;
    d.f[ 5] =   ( in.f[ 0] * in.f[10] - in.f[ 8] * in.f[ 2] ) * (float)det_1;
    d.f[ 6] = - ( in.f[ 0] * in.f[ 6] - in.f[ 4] * in.f[ 2] ) * (float)det_1;
    d.f[ 8] =   ( in.f[ 4] * in.f[ 9] - in.f[ 8] * in.f[ 5] ) * (float)det_1;
    d.f[ 9] = - ( in.f[ 0] * in.f[ 9] - in.f[ 8] * in.f[ 1] ) * (float)det_1;
    d.f[10] =   ( in.f[ 0] * in.f[ 5] - in.f[ 4] * in.f[ 1] ) * (float)det_1;
    d.f[12] = - ( in.f[12] * d.f[ 0] + in.f[13] * d.f[ 4] + in.f[14] * d.f[ 8] );
    d.f[13] = - ( in.f[12] * d.f[ 1] + in.f[13] * d.f[ 5] + in.f[14] * d.f[ 9] );
    d.f[14] = - ( in.f[12] * d.f[ 2] + in.f[13] * d.f[ 6] + in.f[14] * d.f[10] );
    d.f[ 3] = 0.0f;
    d.f[ 7] = 0.0f;
    d.f[11] = 0.0f;
    d.f[15] = 1.0f;
    out = d;
}

void matRotationQuaternion(Mat4f& m, const Quat& q) {
    m.f[0] = 1.0f - 2.0f*q.y*q.y - 2.0f*q.z*q.z;
    m.f[1] = 2.0f*q.x*q.y - 2.0f*q.z*q.w;
    m.f[2] = 2.0f*q.x*q.z + 2.0f*q.y*q.w;
    m.f[3] = 0.0f;
    m.f[4] = 2.0f*q.x*q.y + 2.0f*q.z*q.w;
    m.f[5] = 1.0f - 2.0f*q.x*q.x - 2.0f*q.z*q.z;
    m.f[6] = 2.0f*q.y*q.z - 2.0f*q.x*q.w;
    m.f[7] = 0.0f;
    m.f[8] = 2.0f*q.x*q.z - 2.0f*q.y*q.w;
    m.f[9] = 2.0f*q.y*q.z + 2.0f*q.x*q.w;
    m.f[10] = 1.0f - 2.0f*q.x*q.x - 2.0f*q.y*q.y;
    m.f[11] = 0.0f;
    m.f[12] = 0.0f;
    m.f[13] = 0.0f;
    m.f[14] = 0.0f;
    m.f[15] = 1.0f;
}

void quatNormalize(Quat& q) {
    double temp = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
    float mag = (float)sqrt(temp);
    if (mag != 0.0f) {
        mag = 1.0f / mag;
        q.x *= mag;
        q.y *= mag;
        q.z *= mag;
        q.w *= mag;
    }
}

// Spherical interpolation along the shorter arc; t outside [0,1] gives the identity rotation.
void quatSlerp(Quat& out, const Quat& a, const Quat& b, const float t) {
    float fCosine, fAngle, A, B;

    if (t < 0.0f || t > 1.0f) {
        out.x = 0; out.y = 0; out.z = 0; out.w = 1;
        return;
    }
    fCosine = a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
    if (fCosine < 0) {
        Quat bi;
        bi.x = -b.x; bi.y = -b.y; bi.z = -b.z; bi.w = -b.w;
        quatSlerp(out, a, bi, t);
        return;
    }
    fCosine = (fCosine < 1.0f) ? fCosine : 1.0f;
    fAngle = (float)acos(fCosine);
    if (fAngle == 0.0f) {
        out = a;
        return;
    }
    A = (float)(((float)sin((1.0f-t)*fAngle)) / ((float)sin(fAngle)));
    B = (float)(((float)sin(t*fAngle)) / ((float)sin(fAngle)));
    out.x = A * a.x + B * b.x;
    out.y = A * a.y + B * b.y;
    out.z = A * a.z + B * b.z;
    out.w = A * a.w + B * b.w;
    quatNormalize(out);
}

void vec3Lerp(float* out, const float* v1, const float* v2, const float s) {
    out[0] = v1[0] + s * (v2[0] - v1[0]);
    out[1] = v1[1] + s * (v2[1] - v1[1]);
    out[2] = v1[2] + s * (v2[2] - v1[2]);
}

// ---- .edm reading --------------------------------------------------------------------------
struct Cursor {
    const unsigned char* p;
    size_t left;
    bool ok;
    bool take(void* dst, size_t n) {
        if (!ok || n > left) { ok = false; return false; }
        memcpy(dst, p, n); p += n; left -= n;
        return true;
    }
    unsigned int u32() { unsigned int v = 0; take(&v, 4); return v; }
    int i32() { int v = 0; take(&v, 4); return v; }
    void pad4(size_t n) { size_t r = (4 - (n & 3)) & 3; if (r > left) ok = false; else { p += r; left -= r; } }
    template <class T> void array(std::vector<T>& v, size_t count) {
        if (!ok || count > left / sizeof(T)) { ok = false; return; }
        v.resize(count);
        if (count) take(&v[0], count * sizeof(T));
    }
};

void readAttrib(Cursor& c, EdmAttrib& a) {
    a.type = c.u32(); a.n = c.u32(); a.nStride = c.u32(); a.nOffset = c.u32();
}

void readChannel(Cursor& c, std::vector<float>& v, unsigned int comps, bool animated, unsigned int frames) {
    unsigned int keys = c.u32();
    if (keys != (animated ? frames : 1u)) { c.ok = false; return; }
    c.array(v, (size_t)keys * comps);
}

}  // namespace

CreatureModel::CreatureModel()
    : nNumMesh(0), nNumNode(0), nNumMeshNode(0), nNumFrame(0), m_fFrame(0), m_fBlend(0), m_nFrame(0) {}

void CreatureModel::Destroy() {
    nNumMesh = nNumNode = nNumMeshNode = nNumFrame = 0;
    pMesh.clear(); pNode.clear();
    m_frameCache.clear(); m_worldCache.clear(); m_worldZeroCache.clear();
}

bool CreatureModel::ReadFromFile(const char* path) {
    Destroy();
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    std::vector<unsigned char> buf;
    unsigned char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), fp)) > 0) buf.insert(buf.end(), chunk, chunk + got);
    fclose(fp);

    Cursor c = { buf.empty() ? NULL : &buf[0], buf.size(), true };
    char magic[4] = {0};
    c.take(magic, 4);
    if (!c.ok || memcmp(magic, "EDM1", 4) != 0 || c.u32() != 1) { Destroy(); return false; }
    nNumFrame = c.u32(); nNumMesh = c.u32(); nNumNode = c.u32(); nNumMeshNode = c.u32();
    if (!c.ok || nNumFrame < 2 || nNumMesh > 64 || nNumNode > 256 || nNumMeshNode > nNumNode) { Destroy(); return false; }

    pMesh.resize(nNumMesh);
    for (unsigned int i = 0; i < nNumMesh && c.ok; ++i) {
        EdmMesh& m = pMesh[i];
        m.nNumVertex = c.u32(); m.nNumFaces = c.u32();
        readAttrib(c, m.sVertex); readAttrib(c, m.sNormals); readAttrib(c, m.sUVW);
        readAttrib(c, m.sBoneIdx); readAttrib(c, m.sBoneWeight);
        unsigned int bytes = c.u32();
        c.array(m.pInterleaved, bytes); c.pad4(bytes);
        unsigned int count = c.u32();
        c.array(m.pFaces, count); c.pad4((size_t)count * 2);
        if (count != m.nNumFaces * 3) c.ok = false;
        EdmBoneBatches& b = m.sBoneBatches;
        b.nBatchBoneMax = c.i32(); b.nBatchCnt = c.i32();
        if (b.nBatchBoneMax < 0 || b.nBatchCnt < 0 || b.nBatchBoneMax > 64 || b.nBatchCnt > 64) { c.ok = false; break; }
        c.array(b.pnBatches, (size_t)b.nBatchCnt * b.nBatchBoneMax);
        c.array(b.pnBatchBoneCnt, b.nBatchCnt);
        c.array(b.pnBatchOffset, b.nBatchCnt);
    }
    pNode.resize(nNumNode);
    for (unsigned int i = 0; i < nNumNode && c.ok; ++i) {
        EdmNode& n = pNode[i];
        n.nIdx = c.i32(); n.nIdxParent = c.i32(); n.nAnimFlags = c.u32();
        readChannel(c, n.pfPosition, 3, (n.nAnimFlags & EDM_HAS_POSITION_ANI) != 0, nNumFrame);
        readChannel(c, n.pfRotation, 4, (n.nAnimFlags & EDM_HAS_ROTATION_ANI) != 0, nNumFrame);
        readChannel(c, n.pfScale,    3, (n.nAnimFlags & EDM_HAS_SCALE_ANI)    != 0, nNumFrame);
        // A parent must come first (the exporter's order), so a world-matrix walk always ends.
        if (n.nIdxParent >= (int)i || (i < nNumMeshNode && (n.nIdx < 0 || n.nIdx >= (int)nNumMesh))) c.ok = false;
    }
    if (!c.ok || c.left != 0) { Destroy(); return false; }

    m_frameCache.assign(nNumNode, 0.0f);
    m_worldCache.resize(nNumNode);
    m_worldZeroCache.resize(nNumNode);
    flushCache();
    return true;
}

// Frame-0 worlds are precomputed; every other frame's are cached per node, keyed by the frame.
void CreatureModel::flushCache() {
    SetFrame(0);
    for (unsigned int i = 0; i < nNumNode; ++i)
        getWorldMatrixNoCache(m_worldZeroCache[i], pNode[i]);
    m_worldCache = m_worldZeroCache;
    for (unsigned int i = 0; i < nNumNode; ++i) m_frameCache[i] = 0;
}

void CreatureModel::SetFrame(float fFrame) {
    m_nFrame = (int)fFrame;
    m_fBlend = fFrame - m_nFrame;
    // The last frame is the end of the final blend (the stock SDK blended one key past the end, a
    // heap over-read that G.2s/ASan found).
    if (nNumFrame >= 2 && m_nFrame > (int)nNumFrame - 2) {
        m_nFrame = (int)nNumFrame - 2;
        m_fBlend = 1.0f;
    }
    m_fFrame = fFrame;
}

void CreatureModel::getScalingMatrix(Mat4f& mOut, const EdmNode& node) const {
    const float* s = &node.pfScale[0];
    if (node.nAnimFlags & EDM_HAS_SCALE_ANI) {
        float v[3];
        vec3Lerp(v, &s[3*(m_nFrame+0)], &s[3*(m_nFrame+1)], m_fBlend);
        matScaling(mOut, v[0], v[1], v[2]);
    } else {
        matScaling(mOut, s[0], s[1], s[2]);
    }
}

void CreatureModel::getRotationMatrix(Mat4f& mOut, const EdmNode& node) const {
    const Quat* q = (const Quat*)&node.pfRotation[0];
    if (node.nAnimFlags & EDM_HAS_ROTATION_ANI) {
        Quat r;
        quatSlerp(r, q[m_nFrame], q[m_nFrame+1], m_fBlend);
        matRotationQuaternion(mOut, r);
    } else {
        matRotationQuaternion(mOut, q[0]);
    }
}

void CreatureModel::getTranslationMatrix(Mat4f& mOut, const EdmNode& node) const {
    const float* p = &node.pfPosition[0];
    if (node.nAnimFlags & EDM_HAS_POSITION_ANI) {
        float v[3];
        vec3Lerp(v, &p[3*(m_nFrame+0)], &p[3*(m_nFrame+1)], m_fBlend);
        matTranslation(mOut, v[0], v[1], v[2]);
    } else {
        matTranslation(mOut, p[0], p[1], p[2]);
    }
}

void CreatureModel::getWorldMatrixNoCache(Mat4f& mOut, const EdmNode& node) const {
    Mat4f mTmp;
    getScalingMatrix(mOut, node);
    getRotationMatrix(mTmp, node);
    matMultiply(mOut, mOut, mTmp);
    getTranslationMatrix(mTmp, node);
    matMultiply(mOut, mOut, mTmp);
    if (node.nIdxParent < 0)
        return;
    getWorldMatrixNoCache(mTmp, pNode[node.nIdxParent]);
    matMultiply(mOut, mOut, mTmp);
}

void CreatureModel::getWorldMatrix(Mat4f& mOut, const EdmNode& node) {
    size_t i = (size_t)(&node - &pNode[0]);
    if (m_fFrame == 0) {
        mOut = m_worldZeroCache[i];
        return;
    }
    if (m_fFrame == m_frameCache[i]) {
        mOut = m_worldCache[i];
        return;
    }
    getWorldMatrixNoCache(mOut, node);
    m_frameCache[i] = m_fFrame;
    m_worldCache[i] = mOut;
}

void CreatureModel::GetBoneWorldMatrix(Mat4f& mOut, const EdmNode& nodeMesh, const EdmNode& nodeBone) {
    Mat4f mTmp;
    float fFrame = m_fFrame;

    SetFrame(0);
    getWorldMatrix(mOut, nodeMesh);
    getWorldMatrix(mTmp, nodeBone);
    matInverse(mTmp, mTmp);
    matMultiply(mOut, mOut, mTmp);

    SetFrame(fFrame);
    getWorldMatrix(mTmp, nodeBone);
    matMultiply(mOut, mOut, mTmp);
}
