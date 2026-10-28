// CreatureModel.h — one animated creature model, read from an `.edm` file (LG.7, 2026-10-27).
//
// Replaces the PowerVR SDK's CPVRTModelPOD for Model.cpp. The `.edm` files are converted offline from
// the shipped `.pod` art by port/tools/pod2edm.py (format there and in docs/entities-and-creatures.md).
// Field names follow the SDK's (pMesh, pNode, sBoneBatches, nNumMeshNode, ...) so Model.cpp's draw
// loop reads as it did.
//
// SetFrame + GetBoneWorldMatrix reproduce the SDK's animation evaluation bit for bit: the same
// lerp / slerp / matrix expressions in the same order, the same world-matrix caches, the same
// frame clamp. --creature-selftest pins the result (skinned-vertex hashes per creature x frame).
#ifndef EDEN_CREATUREMODEL_H
#define EDEN_CREATUREMODEL_H

#include <vector>
#include "VecMath.h"

// One vertex attribute inside a mesh's interleaved block. type uses the POD numbering
// (1 float, 3 unsigned short, 10 unsigned byte); n == 0 means absent.
struct EdmAttrib {
    unsigned int type, n, nStride, nOffset;
};

struct EdmBoneBatches {
    int nBatchBoneMax, nBatchCnt;
    std::vector<int> pnBatches;       // nBatchCnt x nBatchBoneMax node indices
    std::vector<int> pnBatchBoneCnt;  // bones used by each batch
    std::vector<int> pnBatchOffset;   // first triangle of each batch
};

struct EdmMesh {
    unsigned int nNumVertex, nNumFaces;
    EdmAttrib sVertex, sNormals, sUVW, sBoneIdx, sBoneWeight;
    std::vector<unsigned char> pInterleaved;
    std::vector<unsigned short> pFaces;   // triangle list, nNumFaces x 3
    EdmBoneBatches sBoneBatches;
};

enum { EDM_HAS_POSITION_ANI = 0x01, EDM_HAS_ROTATION_ANI = 0x02, EDM_HAS_SCALE_ANI = 0x04 };

struct EdmNode {
    int nIdx;            // mesh index for a mesh node, else -1
    int nIdxParent;      // -1 for a root
    unsigned int nAnimFlags;
    std::vector<float> pfPosition, pfRotation, pfScale;   // 3 / 4 (x,y,z,w) / 3 floats per key; 1 key if static
};

class CreatureModel {
public:
    unsigned int nNumMesh, nNumNode, nNumMeshNode, nNumFrame;
    std::vector<EdmMesh> pMesh;
    std::vector<EdmNode> pNode;   // the first nNumMeshNode are the mesh nodes

    CreatureModel();
    bool ReadFromFile(const char* path);   // false (and empty) on any malformed or missing file
    void Destroy();

    void SetFrame(float fFrame);
    // The bone's skinning matrix for this mesh node at the current frame: mesh world at frame 0,
    // times the inverse of the bone's frame-0 world, times the bone's world now.
    void GetBoneWorldMatrix(Mat4f& mOut, const EdmNode& nodeMesh, const EdmNode& nodeBone);

private:
    float m_fFrame, m_fBlend;
    int m_nFrame;
    std::vector<float> m_frameCache;   // frame each m_worldCache entry was computed at
    std::vector<Mat4f> m_worldCache, m_worldZeroCache;

    void flushCache();
    void getWorldMatrix(Mat4f& mOut, const EdmNode& node);
    void getWorldMatrixNoCache(Mat4f& mOut, const EdmNode& node) const;
    void getScalingMatrix(Mat4f& mOut, const EdmNode& node) const;
    void getRotationMatrix(Mat4f& mOut, const EdmNode& node) const;
    void getTranslationMatrix(Mat4f& mOut, const EdmNode& node) const;
};

#endif
