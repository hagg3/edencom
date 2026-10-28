// CreatureModelSelftest.cpp — --creature-selftest (LG.7, 2026-10-27): the gate on the PowerVR SDK's
// removal.
//
// For each of the seven creatures it loads the `.edm`, then for every frame in quarter-frame steps
// (every animation's every frame, the blends between, and past the last frame into the clamp) it
// poses the model the way Model.cpp's DrawModel does (SetFrame, then GetBoneWorldMatrix for every
// bone of every batch of every mesh node, so the world-matrix caches are exercised too) and skins
// each batch's vertices on the CPU as the matrix-palette shader does: sum of weight x (bone matrix x
// position / normal). It hashes the bone matrices and the skinned positions and normals, plus a
// fixed battery of VecMath.h operations.
//
// LG.7 built this while the SDK was still compiled and ran it with LG7_COMPARE_PVRT, which also
// loaded each `.pod` through CPVRTModelPOD and required every matrix and skinned float to be
// byte-identical (0 differences on all 7 creatures, macOS arm64, Apple clang). The hashes below are
// that run's output, so they are the SDK's own numbers.
//
// The pins hold for the target and compiler they were measured on: libm's sin/cos/acos and whether
// a*b+c is fused into one rounding (clang contracts on arm64; x86-64 SSE2 has no FMA) both move the
// last bit. So a mismatch fails only on macOS arm64; elsewhere the run prints its hashes and still
// fails on anything structural (a model that does not load, a non-finite float).
#include "CreatureModel.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <math.h>

#ifndef EMOD_EXPORT
#define EMOD_EXPORT
#endif

namespace {

const char* const kCreatureNames[7] = {"Moof", "Batty", "Green", "Nergle", "Stumpy", "Charger", "Stalker"};   // M_MOOF..M_STALKER

struct Hash {
    unsigned long long h;
    Hash() : h(1469598103934665603ULL) {}
    void add(const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    }
    void addf(float f) { add(&f, 4); }
};

// A palette for one batch: bone matrices by palette slot.
struct Pose {
    std::vector<Mat4f> bones;
};

bool allFinite(const float* f, int n) {
    for (int i = 0; i < n; i++) if (!(f[i] == f[i]) || f[i] > 3.0e38f || f[i] < -3.0e38f) return false;
    return true;
}

// Skin batch `b` of `mesh` with `pal` (palette slot -> matrix) and hash the result. The vertex
// reads mirror the draw: interleaved block, attribute offsets/strides, u8 bone indices, float weights.
void skinBatch(const EdmMesh& mesh, int b, const std::vector<Mat4f>& pal, Hash& hash, int& nonFinite) {
    const EdmBoneBatches& bb = mesh.sBoneBatches;
    int first = bb.pnBatchOffset[b];
    int tris = (b + 1 < bb.nBatchCnt) ? bb.pnBatchOffset[b + 1] - first : (int)mesh.nNumFaces - first;
    const unsigned char* base = &mesh.pInterleaved[0];
    for (int t = 0; t < tris * 3; t++) {
        unsigned short vi = mesh.pFaces[(size_t)first * 3 + t];
        const float* p = (const float*)(base + (size_t)vi * mesh.sVertex.nStride + mesh.sVertex.nOffset);
        const float* n = (const float*)(base + (size_t)vi * mesh.sNormals.nStride + mesh.sNormals.nOffset);
        const unsigned char* bi = base + (size_t)vi * mesh.sBoneIdx.nStride + mesh.sBoneIdx.nOffset;
        const float* bw = (const float*)(base + (size_t)vi * mesh.sBoneWeight.nStride + mesh.sBoneWeight.nOffset);
        Vec4f sp(0.0f, 0.0f, 0.0f, 0.0f), sn(0.0f, 0.0f, 0.0f, 0.0f);
        for (unsigned int k = 0; k < mesh.sBoneIdx.n; k++) {
            int slot = bi[k];
            if (slot >= (int)pal.size()) slot = 0;
            Vec4f vp = pal[slot] * Vec4f(p[0], p[1], p[2], 1.0f);
            Vec4f vn = pal[slot] * Vec4f(n[0], n[1], n[2], 0.0f);
            sp = sp + vp * bw[k];
            sn = sn + vn * bw[k];
        }
        if (!allFinite(&sp.x, 4) || !allFinite(&sn.x, 4)) nonFinite++;
        hash.add(&sp.x, 12);
        hash.add(&sn.x, 12);
    }
}

// VecMath.h battery: a fixed pseudo-random set of the operations Model.cpp, BlockBreak.cpp and
// VectorUtil.cpp perform.
struct Lcg {
    unsigned int s;
    float next(float lo, float hi) { s = s * 1664525u + 1013904223u; return lo + (hi - lo) * (float)(s >> 8) / 16777216.0f; }
};

unsigned long long vecmathHash() {
    Hash h;
    Lcg r = {12345u};
    for (int i = 0; i < 2000; i++) {
        float ax = r.next(-7, 7), ay = r.next(-7, 7), az = r.next(-7, 7);
        Mat4f rot = Mat4f::RotationX(ax) * Mat4f::RotationY(ay) * Mat4f::RotationZ(az);
        Mat4f tr = Mat4f::Translation(r.next(-500, 500), r.next(0, 256), r.next(-500, 500)) * Mat4f::RotationY(ay);
        Vec4f v(r.next(-2, 2), r.next(-2, 2), r.next(-2, 2), 1.0f);
        Vec4f a = rot * v, b = tr * v, c = v * tr;
        Mat4f inv = (tr * rot).inverse().transpose();
        Vec3f p(r.next(-50, 50), r.next(-50, 50), r.next(-50, 50)), q(r.next(-1, 1), r.next(-1, 1), r.next(-1, 1));
        Vec3f d = (p - q).normalized(), e = p.cross(q) * 0.5f + q / 3.0f - (-p);
        Vec3f f = q; f.normalize();
        float dl = p.dot(q), len = e.length(), l4 = a.length(), d4 = a.dot(b);
        h.add(rot.f, 64); h.add(tr.f, 64); h.add(inv.f, 64);
        h.add(&a, 16); h.add(&b, 16); h.add(&c, 16);
        h.add(&d, 12); h.add(&e, 12); h.add(&f, 12);
        h.addf(dl); h.addf(len); h.addf(l4); h.addf(d4);
    }
    return h.h;
}


struct Result {
    unsigned long long hash;
    long samples, vertices;
    int nonFinite;
};

}  // namespace

// The pinned hashes (macOS arm64, Apple clang, Release): see the header.
static const unsigned long long kPinnedCreature[7] = {
    0x1623271d42c72131ULL,   // Moof
    0xdf98a5770179d88bULL,   // Batty
    0xeb6a86e9b8ce3347ULL,   // Green
    0xb424b18d49c08338ULL,   // Nergle
    0x5c550827a6514722ULL,   // Stumpy
    0x09c70e00a6da92e8ULL,   // Charger
    0x4a91b73630698c4eULL,   // Stalker
};
static const unsigned long long kPinnedVecMath = 0x8374a6de7ca6ebe6ULL;

extern "C" EMOD_EXPORT int eden_creature_selftest_run(const char* bundleRoot) {
    int bad = 0;
    std::string root = bundleRoot ? bundleRoot : ".";
    if (!root.empty() && root[root.size() - 1] != '/') root += "/";
#if defined(__APPLE__) && defined(__aarch64__)
    const bool enforcePins = true;
#else
    const bool enforcePins = false;
#endif

    unsigned long long vh = vecmathHash();
    printf("[eden-creature] vecmath hash %016llx\n", vh);
    if (enforcePins && vh != kPinnedVecMath) {
        printf("[eden-creature] FAIL: vecmath hash, pinned %016llx\n", kPinnedVecMath);
        bad++;
    }

    for (int c = 0; c < 7; c++) {
        CreatureModel m;
        std::string path = root + kCreatureNames[c] + ".edm";
        if (!m.ReadFromFile(path.c_str())) {
            printf("[eden-creature] FAIL: %s did not load\n", path.c_str());
            bad++;
            continue;
        }
        Result res = {0, 0, 0, 0};
        Hash h;
        std::vector<Mat4f> pal;
        // Quarter frames from 0 through two frames past the end (the clamp), then a few revisits so a
        // cached world matrix is read back after other frames were computed.
        std::vector<float> frames;
        for (int q = 0; q <= (int)(m.nNumFrame + 1) * 4; q++) frames.push_back(q * 0.25f);
        frames.push_back(3.0f); frames.push_back(3.0f); frames.push_back(0.0f); frames.push_back(1.5f);
        for (size_t s = 0; s < frames.size(); s++) {
            float fr = frames[s];
            m.SetFrame(fr);
            for (unsigned int iNode = 0; iNode < m.nNumMeshNode; iNode++) {
                const EdmNode& node = m.pNode[iNode];
                const EdmMesh& mesh = m.pMesh[node.nIdx];
                if (mesh.sVertex.n == 0 || mesh.sUVW.n == 0) continue;
                const EdmBoneBatches& bb = mesh.sBoneBatches;
                for (int b = 0; b < bb.nBatchCnt; b++) {
                    pal.resize(bb.pnBatchBoneCnt[b]);
                    for (int j = 0; j < bb.pnBatchBoneCnt[b]; j++) {
                        int id = bb.pnBatches[b * bb.nBatchBoneMax + j];
                        m.GetBoneWorldMatrix(pal[j], node, m.pNode[id]);
                        h.add(pal[j].f, 64);
                        if (!allFinite(pal[j].f, 16)) res.nonFinite++;
                    }
                    skinBatch(mesh, b, pal, h, res.nonFinite);
                    res.vertices += 3L * ((b + 1 < bb.nBatchCnt) ? bb.pnBatchOffset[b + 1] - bb.pnBatchOffset[b] : (int)mesh.nNumFaces - bb.pnBatchOffset[b]);
                }
            }
            res.samples++;
        }
        res.hash = h.h;
        printf("[eden-creature] %-7s frames %3u  samples %4ld  skinned vertices %8ld  hash %016llx\n",
               kCreatureNames[c], m.nNumFrame, res.samples, res.vertices, res.hash);
        if (res.nonFinite) { printf("[eden-creature] FAIL: %s has %d non-finite values\n", kCreatureNames[c], res.nonFinite); bad++; }
        if (res.vertices == 0) { printf("[eden-creature] FAIL: %s skinned nothing\n", kCreatureNames[c]); bad++; }
        if (enforcePins && res.hash != kPinnedCreature[c]) {
            printf("[eden-creature] FAIL: %s hash, pinned %016llx\n", kCreatureNames[c], kPinnedCreature[c]);
            bad++;
        }
    }
    if (!enforcePins) printf("[eden-creature] hashes are pinned for macOS arm64 only; structural checks only here\n");
    printf("[eden-creature] %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
