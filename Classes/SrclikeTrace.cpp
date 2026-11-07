//
//  SrclikeTrace.cpp -- Stage SRC.3 (WORKING/srclike-movement-plan-2026-10-31.md, D5): the swept hull trace
//  Srclike movement is built on. See SrclikeMove.h for the API and units.
//
//  Clean room (D4): the classic expanded-plane brush clip, written to match port/tools/srclike-pm-ref.py's
//  trace_hull line for line (that script is the SRC.1 oracle). Each solid voxel is a convex brush: a cube
//  (6 axial planes) or a cube cut by one 45-degree plane (ramp slope or side-ramp diagonal). For a box hull
//  the expanded planes are the exact Minkowski sum, because every edge of these brushes runs along an
//  axis or lies in the plane of a face whose normal is already in the set (no bevel planes needed).
//
//  Brush geometry is derived from the stock shapes (Util.cpp makeRamp/makeSide, as Player::vertc calls
//  them: makeX(x, x+1, z+1, z, y, y+1, type%4)); --srclike-selftest=trace checks the two agree vertex for
//  vertex in all eight orientations.
//
#include "SrclikeMove.h"
#include "Constants.h"
#include "Globals.h"
#include "Terrain.h"
#include <math.h>

namespace SrclikeMove {

const double kUnitsPerBlock = 72.0 / 1.85;
const double kDistEpsilon = 0.03125;
const double kParallelTol = 1e-6;

static const double kS = 0.70710678118654752440;   // 1/sqrt(2): Eden's ".707" made exact
static const double kRampNzMax = 0.99;            // srclike-pm-ref.py RAMP_NZ_MAX

Vec3d toSource(float ex, float ey, float ez) {
    return Vec3d{(double)ex * kUnitsPerBlock, (double)ez * kUnitsPerBlock, (double)ey * kUnitsPerBlock};
}

void toEden(const Vec3d& s, float* ex, float* ey, float* ez) {
    *ex = (float)(s.x / kUnitsPerBlock);
    *ey = (float)(s.z / kUnitsPerBlock);
    *ez = (float)(s.y / kUnitsPerBlock);
}

Shape blockShape(int type) {
    if (type <= 0 || type >= BLOCK_IDS) return SHAPE_EMPTY;
    if (type >= TYPE_STONE_RAMP1 && type <= TYPE_ICE_RAMP4) return SHAPE_RAMP;
    if (type >= TYPE_STONE_SIDE1 && type <= TYPE_ICE_SIDE4) return SHAPE_SIDE;
    const int bi = blockinfo[type];
    if (bi & (IS_LIQUID | IS_DOOR | IS_PORTAL)) return SHAPE_EMPTY;
    if (type == TYPE_FLOWER || type == TYPE_GOLDEN_CUBE) return SHAPE_EMPTY;
    return SHAPE_CUBE;
}

// Slope normals (Source axes), indexed by the stock rotation: makeRamp's t = (type%4 + 3) % 4 gives Eden
// topnormal (.707,.707,0) / (0,.707,.707) / (-.707,.707,0) / (0,.707,-.707), i.e. the HIGH edge is on the
// side the normal points away from. makeSide's t = (type%4 + 1) % 4 gives the diagonal's normal
// (-.707,0,.707) / (-.707,0,-.707) / (.707,0,-.707) / (.707,0,.707), pointing at the missing corner.
// Eden (nx, ny, nz) -> Source (nx, nz, ny).
static const double kRampN[4][3] = {{kS, 0, kS}, {0, kS, kS}, {-kS, 0, kS}, {0, -kS, kS}};
static const double kSideN[4][3] = {{-kS, kS, 0}, {-kS, -kS, 0}, {kS, -kS, 0}, {kS, kS, 0}};

int blockBrush(int type, int bx, int bz, int by, Plane out[7]) {
    const Shape sh = blockShape(type);
    if (sh == SHAPE_EMPTY) return 0;
    const double U = kUnitsPerBlock;
    const double x0 = bx * U, y0 = bz * U, z0 = by * U;
    const double x1 = (bx + 1) * U, y1 = (bz + 1) * U, z1 = (by + 1) * U;
    out[0] = Plane{{1, 0, 0}, x1};  out[1] = Plane{{-1, 0, 0}, -x0};
    out[2] = Plane{{0, 1, 0}, y1};  out[3] = Plane{{0, -1, 0}, -y0};
    out[4] = Plane{{0, 0, 1}, z1};  out[5] = Plane{{0, 0, -1}, -z0};
    if (sh == SHAPE_CUBE) return 6;
    const double* n = sh == SHAPE_RAMP ? kRampN[(type % 4 + 3) % 4] : kSideN[(type % 4 + 1) % 4];
    // Both cuts pass through the cell centre (a slope from a bottom edge to the opposite top edge; a
    // vertical diagonal between two opposite corners).
    const double cx = x0 + U / 2, cy = y0 + U / 2, cz = z0 + U / 2;
    out[6] = Plane{{n[0], n[1], n[2]}, n[0] * cx + n[1] * cy + n[2] * cz};
    return 7;
}

static inline double dot3(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// One brush against the sweep; updates tr. Mirrors trace_hull's per-brush body.
static void clipBrush(const Plane* pl, int np, bool rampBrush, int type, int bx, int bz, int by,
                      const Vec3d& start, const Vec3d& end, const Vec3d& mins, const Vec3d& maxs, Trace& tr) {
    const double eps = kDistEpsilon;
    double enter = -1.0, leave = 1.0;
    const Plane* hit = nullptr;
    bool startout = false, getout = false;
    for (int i = 0; i < np; ++i) {
        const Vec3d& n = pl[i].n;
        const Vec3d ofs{n.x > 0 ? mins.x : maxs.x, n.y > 0 ? mins.y : maxs.y, n.z > 0 ? mins.z : maxs.z};
        const double dist = pl[i].d - dot3(n, ofs);
        const double d1 = dot3(n, start) - dist, d2 = dot3(n, end) - dist;
        if (d2 > 0) getout = true;
        if (d1 > 0) startout = true;
        // Outside this plane and not entering it: the whole brush is missed. d2 >= d1 - tol makes a slide
        // along a plane we rest one epsilon from a miss (plan §8.2.3b).
        if (d1 > 0 && (d2 >= eps || d2 >= d1 - kParallelTol)) return;
        if (d1 <= 0 && d2 <= 0) continue;
        if (d1 > d2) {
            const double f = (d1 - eps) / (d1 - d2);
            if (f > enter) { enter = f; hit = &pl[i]; }
        } else {
            const double f = (d1 + eps) / (d1 - d2);
            if (f < leave) leave = f;
        }
    }
    if (!startout) {
        tr.startsolid = true;
        if (!getout) { tr.allsolid = true; tr.fraction = 0.0; }
        if (tr.type < 0) { tr.type = type; tr.bx = bx; tr.bz = bz; tr.by = by; }
        return;
    }
    if (enter < leave && enter > -1.0 && enter < tr.fraction) {
        tr.fraction = enter < 0.0 ? 0.0 : enter;
        tr.normal = hit->n;
        tr.ramp = rampBrush && hit->n.z >= 0.7 && hit->n.z < kRampNzMax;
        tr.type = type; tr.bx = bx; tr.bz = bz; tr.by = by;
    }
}

Trace traceHull(const Vec3d& start, const Vec3d& end, const Vec3d& mins, const Vec3d& maxs) {
    Trace tr;
    tr.fraction = 1.0;
    tr.normal = Vec3d{0, 0, 0};
    tr.startsolid = tr.allsolid = tr.ramp = false;
    tr.type = -1; tr.bx = tr.bz = tr.by = 0;
    const double U = kUnitsPerBlock, m = 2 * kDistEpsilon;
    const double lox = fmin(start.x, end.x) + mins.x - m, hix = fmax(start.x, end.x) + maxs.x + m;
    const double loy = fmin(start.y, end.y) + mins.y - m, hiy = fmax(start.y, end.y) + maxs.y + m;
    const double loz = fmin(start.z, end.z) + mins.z - m, hiz = fmax(start.z, end.z) + maxs.z + m;
    if (!(isfinite(lox) && isfinite(hix) && isfinite(loy) && isfinite(hiy) && isfinite(loz) && isfinite(hiz))) {
        tr.startsolid = tr.allsolid = true;   // NaN in, nothing sensible out: refuse the move
        tr.fraction = 0.0;
        tr.endpos = start;
        return tr;
    }
    // Eden cell (x, z, y) <-> Source (x, y, z). getLandc2 answers -1 above/below the world (= empty);
    // negative x/z never index the toroidal arrays.
    const int cx0 = (int)floor(lox / U), cx1 = (int)floor(hix / U);
    const int cz0 = (int)floor(loy / U), cz1 = (int)floor(hiy / U);
    const int cy0 = (int)floor(loz / U), cy1 = (int)floor(hiz / U);
    Plane pl[7];
    for (int x = cx0; x <= cx1; ++x) {
        if (x < 0) continue;
        for (int z = cz0; z <= cz1; ++z) {
            if (z < 0) continue;
            for (int y = cy0; y <= cy1; ++y) {
                const int type = getLandc2(x, z, y);
                const int np = blockBrush(type, x, z, y, pl);
                if (!np) continue;
                clipBrush(pl, np, blockShape(type) == SHAPE_RAMP, type, x, z, y, start, end, mins, maxs, tr);
                if (tr.allsolid) { tr.endpos = start; return tr; }
            }
        }
    }
    tr.endpos = Vec3d{start.x + (end.x - start.x) * tr.fraction, start.y + (end.y - start.y) * tr.fraction,
                      start.z + (end.z - start.z) * tr.fraction};
    return tr;
}

}   // namespace SrclikeMove
