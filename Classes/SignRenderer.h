//
//  SignRenderer.h
//  Eden — Stage D / D.3b: draws the trailer's signs (WorldTrailer.h) in the 3D pass. Read-only.
//
//  The look (WORKING/stage-d-plan-2026-10-10.md §2.5, from the 2026 game's screenshot):
//    * a STANDING sign (face a = 3, the anchor's top) is a 1 x 0.5 board on a 0.5 wooden post,
//      centred on the top face and turned to face c (Minecraft's proportions: wider than tall);
//    * a WALL sign (a = 0, 1, 4, 5) is always a board, 7/8 wide and 1/2 tall, centred on the face
//      and 1/32 off it; on a command-block face (one with a CMB1 record) the board fills the
//      skin's inner panel instead and the skin's button stands in front of it;
//    * a COMMAND BLOCK (D.4b: any non-air block with a CMB1 record) gets a fixed skin on every
//      exposed face -- orange frame, charcoal panel, a square button that is red when idle and
//      green while CmdTool::running() says its script runs -- out to CMD_RADIUS. Overlays only:
//      the mesher and the atlas never see it, and the voxel underneath stays plain steel;
//    * the board is colorTable[b]; the text is white with a dark outline, every line centred, laid
//      out from the top line down, word-wrapped, shrunk only when it would overflow the board.
//  Everything is drawn unlit and flat. a = 2 (under the block) is never written by the game and
//  is not drawn.
//
//  Culling: within RADIUS blocks of the player only (which also keeps every drawn anchor inside
//  the resident toroidal window -- getLand() wraps outside it); a sign whose anchor is air is
//  hidden (the 2026 client hides those too), and so is one whose front cell (the cell the board
//  faces into, or stands in) holds an opaque block. Hidden is never deleted: records outside the
//  world's columns or behind a wall stay in the trailer untouched.
//
//  Text textures are rasterised through the text-raster seam (eden_rasterize_text_rgba, the one
//  the HUD's labels use -- stb_truetype natively, a canvas on web); the outline is a dilation of
//  the glyph coverage done here, so neither seam needed an outline option. They live in an LRU
//  cache keyed by (board kind, text), nearest signs first, at most MAX_TEXTURES and MAX_TEX_BYTES,
//  and only a few are built per frame so walking up to a wall of signs does not hitch.
//
#ifndef Eden_SignRenderer_h
#define Eden_SignRenderer_h

class SignRenderer {
public:
    enum { RADIUS = 32, CMD_RADIUS = 64, MAX_TEXTURES = 64, MAX_TEX_BYTES = 8 << 20, BUILDS_PER_FRAME = 4 };

    // The pass. World::renderFrame calls it after the opaque terrain and the creatures, before the
    // sorted transparent pass, with the camera's modelview current. Restores what it touches.
    static void render();
    // Frees every cached text texture (a world closing; the next frame rebuilds what it needs).
    static void releaseTextures();
    // Off = the pass draws nothing and builds nothing (the harness's --signs=0, the memory A/B).
    static bool enabled;

    // What the last render() did, for the debug probe (eden_debug_signs) and the D.3b gate.
    struct Stats {
        int records;        // signs in the trailer
        int inRange;        // within RADIUS of the player, face drawable (a != 2)
        int drawn;          // boards drawn
        int hiddenAir;      // in range, anchor is air
        int hiddenFront;    // in range, front cell is opaque
        int onCmd;          // drawn on a command-block face (panel board + button)
        int withText;       // drawn with their text texture this frame
        int textures;       // text textures in the cache
        int textureBytes;   // their size, mip chains included
        int built;          // textures built this frame
        int cmds;           // command blocks in the trailer
        int cmdInRange;     // within CMD_RADIUS, anchor not air: skinned
        int cmdFaces;       // exposed faces skinned
        int cmdRunning;     // of cmdInRange, drawn with the green (running) button
    };
    static const Stats& stats();
    // {"records":N,"inRange":N,...} -- the probe's line.
    static const char* describe();
};

#endif
