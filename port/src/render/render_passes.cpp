// render_passes.cpp — G.5b-0 (WORKING/renderer-interface-plan-2026-10-25.md §9): which pass a draw
// belongs to, and the per-pass draw mask --scene-shot captures each pass alone with.
//
// The engine already brackets its passes for the P.1b instruments (Classes/PassTimers.h). With
// g_pass.track set (a harness only), every pass_begin/pass_end also lands here, so the renderer knows
// the innermost open pass at every draw without a single engine draw site changing. A draw is then
// classified into one of the --scene-shot categories (chunks, objects, sky, models, signs,
// transparent, flowers, effects, hud, other) and dropped when the mask excludes it. Update and depth
// state are untouched by the mask: a masked draw is simply not issued.
//
// Plain C++, no GL: both backends and the shim ask it.
#include "render_passes.h"
#include "../../../Classes/PassTimers.h"

#include <cstring>

namespace {
constexpr int kMaxOpen = 32;
int  g_open[kMaxOpen];
int  g_depth = 0;
int  g_overflow = 0;            // marks past kMaxOpen: counted, never indexed
unsigned g_mask = EDEN_RC_ALL;
int  g_unbalanced = 0;          // an end that did not match the innermost begin

bool open(int p) {
    for (int i = 0; i < g_depth; ++i) if (g_open[i] == p) return true;
    return false;
}
}  // namespace

extern "C" void eden_render_pass_contract(int pass, int begin);   // render_core.cpp

extern "C" void eden_render_pass_mark(int pass, int begin) {
    eden_render_pass_contract(pass, begin);
    if (begin) {
        if (g_depth < kMaxOpen) g_open[g_depth++] = pass;
        else ++g_overflow;
        return;
    }
    if (g_overflow > 0) { --g_overflow; return; }
    if (g_depth > 0 && g_open[g_depth - 1] == pass) { --g_depth; return; }
    ++g_unbalanced;
    // Recover by closing the matching pass wherever it is, so one stray end cannot poison the frame.
    for (int i = g_depth - 1; i >= 0; --i)
        if (g_open[i] == pass) { g_depth = i; return; }
}

extern "C" int eden_render_pass_depth(void) { return g_depth; }
extern "C" int eden_render_pass_innermost(void) { return g_depth > 0 ? g_open[g_depth - 1] : -1; }
extern "C" int eden_render_pass_is_open(int pass) { return open(pass) ? 1 : 0; }
extern "C" int eden_render_pass_unbalanced(void) { return g_unbalanced; }
extern "C" void eden_render_pass_reset(void) { g_depth = 0; g_overflow = 0; g_unbalanced = 0; }

extern "C" int eden_render_draw_category(void) {
    // Innermost first: a draw inside P_R_OBJECTS is an object (or a flower) whatever encloses it.
    for (int i = g_depth - 1; i >= 0; --i) {
        switch (g_open[i]) {
            case P_R_CHUNKS:  return EDEN_RC_CHUNKS;
            case P_R_TCHUNKS: return EDEN_RC_TRANSPARENT;
            case P_R_SKY:     return EDEN_RC_SKY;
            case P_R_OBJECTS: return open(P_R_TRANSPARENT) ? EDEN_RC_FLOWERS : EDEN_RC_OBJECTS;
            case P_R_MODELS:  return EDEN_RC_MODELS;
            case P_R_SIGNS:   return EDEN_RC_SIGNS;
            case P_R_EFFECTS: return EDEN_RC_EFFECTS;
            case P_R_HUD:     return EDEN_RC_HUD;
            default: break;
        }
    }
    return EDEN_RC_OTHER;
}

extern "C" const char* eden_render_category_name(int bit) {
    switch (bit) {
        case EDEN_RC_CHUNKS:      return "chunks";
        case EDEN_RC_OBJECTS:     return "objects";
        case EDEN_RC_SKY:         return "sky";
        case EDEN_RC_MODELS:      return "models";
        case EDEN_RC_SIGNS:       return "signs";
        case EDEN_RC_TRANSPARENT: return "transparent";
        case EDEN_RC_FLOWERS:     return "flowers";
        case EDEN_RC_EFFECTS:     return "effects";
        case EDEN_RC_HUD:         return "hud";
        case EDEN_RC_OTHER:       return "other";
        default:                  return "?";
    }
}

extern "C" void eden_render_set_draw_mask(unsigned mask) { g_mask = mask; }
extern "C" unsigned eden_render_draw_mask(void) { return g_mask; }
extern "C" int eden_render_mask_active(void) { return g_mask != EDEN_RC_ALL ? 1 : 0; }

extern "C" int eden_render_draw_allowed(void) {
    if (g_mask == EDEN_RC_ALL) return 1;
    return (g_mask & (unsigned)eden_render_draw_category()) ? 1 : 0;
}

extern "C" const char* eden_render_pass_name(int pass) {
    return (pass >= 0 && pass < P_COUNT) ? kEdenPassNames[pass] : "-";
}
