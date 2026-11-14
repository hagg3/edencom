//
//  BlockIcon.h
//  UX.8-1: the fake-isometric block picture of the HUD build button, as a function of (type, colour,
//  box) instead of Hud members, so the hotbar (UX.8-4) and later the pickers can draw any block.
//  2D, drawn in the caller's ortho pass with Pen::strip; no frame art (the caller draws its plate).
//  docs/ui.md "Block icon".
//

#ifndef Eden_BlockIcon_h
#define Eden_BlockIcon_h

#include "Util.h"
#include <string>

namespace BlockIcon {

// Where the cube goes, in the units the caller's pass draws in. The front face spans
// (x+off+offx, y+off+offy) .. (x+size+offx, y+size+offy); the top and side faces lean `depth`
// up-right of it. `cory` lowers the front edge of a ramp's slope (stock: -3).
struct Geo {
    float x, y;
    float off, size, depth;
    float offx, offy;
    float cory;
};

enum {
    FRONT_ONLY = 1,   // the front face alone (the build button while holding a creature)
};

// Flower, golden cube, door, portal: a recoloured picture (Resources::getPaintedTex), not a cube.
bool isFlat(int type);
void flat(int type, int colour, CGRect r);

// The cube (or ramp wedge, or two-texture block-TNT) for a stock/palette block `type` (a table row,
// never a tool or mod sentinel) carrying `colour` (0 = unpainted). Binds the block's atlas, leaves
// the pen colour white.
void cube(int type, int colour, const Geo& g, int flags = 0);

// The same for a mod block, from its BlockDef (faces, atlas, tint); nothing if `name` has no definition.
void cubeMod(const std::string& name,int colour,const Geo& g);
void drawMod(const std::string& name,int colour,CGRect r);

// Hotbar-cell furniture, in the pass's raw units: the small colour chip in a painted block's corner
// (bottom-right of `r`), a colour slot's paint-chip swatch, and an empty slot's dim "+".
void chip(int colour,CGRect r);
void drawSwatch(int colour,CGRect r);
void drawEmpty(CGRect r);

// A Geo that fills box `r` (the build button's proportions at build size 1), whole units.
Geo fit(CGRect r);

// flat() or cube(fit(r)), by type.
void draw(int type, int colour, CGRect r, int flags = 0);

}

#endif
