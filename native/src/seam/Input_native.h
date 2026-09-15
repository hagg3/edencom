// Input_native.h — SDL3 -> the engine's eden_* input exports. See the .cpp for the mapping and for
// the four rules inherited from the web host (never write a hud-> flag directly; a begin/end pair
// must straddle an engine tick; movement is recomputed every frame; look deltas go through
// unscaled). Phase N Stage 2.
#ifndef EDEN_SEAM_INPUT_NATIVE_H
#define EDEN_SEAM_INPUT_NATIVE_H

union SDL_Event;
struct SDL_Window;

// Called once, after the GL context (and therefore the window) exists.
void eden_native_input_init(SDL_Window* window);
// One SDL event. Safe to call before init(); it simply has nothing to capture the mouse into.
void eden_native_input_handle_event(const SDL_Event& e);
// Once per frame, AFTER the event pump and BEFORE World::update — the engine consumes queued
// touches inside update(), so state pushed after it would be a frame late.
void eden_native_input_tick(void);
// --touch-selftest only: is the mouse's hold-to-mine/build slot claimed? See the .cpp.
int eden_native_input_debug_hold_active(void);

#endif
