// Input_web.mm — Stage P3 seam: the JS-side half of touch/mouse input.
//
// `Classes/Input.h` (ENGINE, never touched) already declares the real signature
// `touchesBegan(NSSet* touches, UIEvent* event)` etc., and `EAGLView_web.mm` already forwards
// `-touchesBegan:withEvent:`/Moved/Ended/Cancelled straight to `Input::getInput()` with no other
// side effect (Stage P1/P2 wiring, done — verified by reading those four methods: each is a
// single-line passthrough). What was missing is the OTHER half: something the browser can
// actually call — there is no CoreOSTouch here to hand the engine real `UITouch`/`UIEvent`
// instances, so this file builds them from plain numbers a JS pointer/touch-event listener
// supplies, and calls `Input::getInput()` directly (bypassing the ObjC view dispatch — there is
// nothing else subscribed to those selectors, so this is equivalent, and avoids having to
// declare `-touchesBegan:withEvent:` etc. on `EAGLView`'s static type just to message it).
//
// Exposed to JS as a single Emscripten-exported C function, `eden_input_pointer_event` —
// EDEN_EXPORT is sufficient by itself (no CMakeLists.txt link-flag change needed): emcc
// auto-adds KEEPALIVE'd symbols to the export list, reachable from JS as `Module._eden_input_
// pointer_event(phase, identity, x, y)` — a raw wasm export call (plain numbers only, no
// ccall/cwrap marshaling needed, so nothing else in the build had to change). See public/
// eden-st.html for the JS-side listener that calls this.
//
// COORDINATE CONTRACT: `x`/`y` must already be in the engine's POINT space (SCREEN_WIDTH ×
// SCREEN_HEIGHT, top-left origin, Y increasing downward) — exactly what a real UIKit
// `-locationInView:` would have returned. `Input::touchesBegan/Moved` do their own `scr_height -
// point.y` flip (Classes/Input.mm) and, on an `IS_IPAD&&!IS_RETINA` profile, their own
// `/SCALE_WIDTH` — this port's profile is retina-iPhone (CLAUDE.md #3: IS_IPAD&&IS_RETINA both
// true), which takes neither of those branches, so callers must NOT pre-divide by SCALE_WIDTH/
// SCALE_HEIGHT themselves. The JS side's only job is DOM-pixel -> point-space (CSS display size
// -> SCREEN_WIDTH x SCREEN_HEIGHT), nothing else.
//
// THAT POINT SPACE IS NO LONGER A CONSTANT (audit D1/D4). It was pinned at 568x320; it is now
// derived from the window aspect and the `ui_scale` setting by src/seam/DisplayProfile_web.mm,
// and can change mid-session (window resize, settings change). Nothing in THIS file may cache it:
// every use below reads SCREEN_WIDTH/SCREEN_HEIGHT or `input->scr_*` live, and the page's
// toEnginePoint() re-reads it from eden_display_point_width/height on every layout pass.
//
// TOUCH IDENTITY: Input::touchesMoved/Ended match by `UITouch*` POINTER IDENTITY (Classes/
// Input.h: `UITouch* touch_id`, compared via `==`, per uikit_stubs.h's header comment) — so the
// SAME UITouch instance must be reused across a begin/move/.../end sequence, not a fresh one per
// call. That is what `activeTouches()` is for: it owns one retained UITouch per live JS pointer
// identity (mouse's constant sentinel, or a touch's `Touch.identifier`) from "start" until
// "end"/"cancel", then releases it. This is the SAME shape as Input::touches[MAX_TOUCHES]'s own
// slot table one layer up — this map is just what feeds it a stable pointer per gesture.
// NSSet/UIEvent must be declared BEFORE Classes/Input.h parses (it uses both in its own
// signatures) — unlike engine .mm files, seam files are NOT force-included with
// Eden_Prefix.pch (CMakeLists.txt: "reproduces Xcode's prefix header for ENGINE sources only,
// not this port's own shim/seam/entry files, which manage their own includes explicitly"), so
// this file has to pull Foundation/UIKit in itself, in the right order, same as
// EAGLView_web.mm does implicitly via Classes/EAGLView.h's own `#import <Foundation/
// Foundation.h>` + `#import <UIKit/UIKit.h>` (both real angle-bracket includes, resolved by the
// framework/ trampoline dirs) before it ever imports Input.h.
#import "../shim/foundation/uikit_stubs.h"
#import "../shim/foundation/NSArray.h"
#import "../shim/foundation/NSAutoreleasePool.h"
#import "../../../Classes/Input.h"
// Pulled in for pass 23 (desktop keyboard/mouse controls) — World.h transitively brings in
// Hud.h/Player.h (MODE_* defines, Hud's public rmenu/rbuild/rpaint Buttons, Player's public
// yaw/pitch/invertcam/setSpeed). Constants.h is NOT reachable through those (Terrain.h only
// comments it out) and, per this file's header comment, seam files don't get the engine's
// force-included prefix header, so it has to be pulled in explicitly for the TYPE_* hotbar list.
#import "../../../Classes/World.h"
#import "../../../Classes/Constants.h"
#import "../../../Classes/GLDialog.h"
#include "../shim/foundation/platform_shims.h"   // EDEN_EXPORT (Phase N Stage 1)
#include <unordered_map>

namespace {

std::unordered_map<int, UITouch*> &activeTouches() {
    static std::unordered_map<int, UITouch*> touches;
    return touches;
}

// Audit row B4: `eden_input_pointer_event` used to allocate a fresh NSSet + UIEvent per call
// (~120 allocations/sec during a drag, each through the shim's ObjC allocator and this frame's
// autorelease pool). Safe to pool globally because Classes/Input.mm's touchesBegan/Moved/Ended/
// Cancelled only ever enumerate the set and take the event as an opaque marker WITHIN the call —
// grep-confirmed neither is retained or read past return, and the engine's own touches[] table
// keys on the individual UITouch*'s pointer identity instead (that one still must NOT be pooled —
// see activeTouches() above and the stuck-jump bug this file's header warns about). Main thread
// only (CLAUDE.md #4), so no synchronization needed for reuse between calls.
NSSet* pooledTouchSet() {
#if defined(EDEN_USE_SYSTEM_FOUNDATION)
    // A real NSSet cannot be refilled; the pooled object is an NSMutableSet, which -isKindOfClass:
    // NSSet and enumerates identically, so Classes/Input.mm cannot tell the difference.
    static NSSet* set = (NSSet*)[[NSMutableSet alloc] init]; // never released: process-lifetime
#else
    static NSSet* set = [[NSSet alloc] init]; // never released: lives for the process
#endif
    return set;
}

// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): replace the pooled set's contents
// with exactly this one touch. It used to be two inline statements poking `set->_items` — the
// public std::vector ivar of the Foundation SHIM's NSSet — which is fine on web and impossible on
// a target with a real Foundation (NSSet is immutable there, and its storage is not ours to see).
// One helper, two implementations, so everything either side of it stays shared. It is NOT a
// general-purpose set API: the pooled set exists to carry a single touch into
// Classes/Input.mm's touchesBegan/Moved/Ended/Cancelled and nothing else, which is exactly why
// the mutable-set allocation on the native path is affordable at pointer-event rates.
void setPooledTouch(NSSet* set, UITouch* touch) {
#if defined(EDEN_USE_SYSTEM_FOUNDATION)
    // NSSet is immutable, so the pooled object is really an NSMutableSet — see pooledTouchSet().
    [(NSMutableSet*)set removeAllObjects];
    if (touch) [(NSMutableSet*)set addObject:touch];
#else
    set->_items.clear();
    if (touch) set->_items.push_back(touch);
#endif
}
UIEvent* pooledEvent() {
    static UIEvent* event = [[UIEvent alloc] init]; // stateless marker type, reused as-is
    return event;
}

} // namespace

extern "C" {

// `phase`: 0=start, 1=move, 2=end, 3=cancel. Deliberately a small JS-facing enum of our own
// rather than reusing UITouchPhase's numbering (uikit_stubs.h) — keeps the JS side ignorant of
// engine-internal enum values, matching how DOM event NAMES (not phase numbers) are what JS
// naturally has on hand (touchstart/touchmove/touchend/touchcancel, mousedown/mousemove/mouseup).
EDEN_EXPORT
void eden_input_pointer_event(int phase, int identity, float x, float y) {
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];

    std::unordered_map<int, UITouch*> &touches = activeTouches();
    std::unordered_map<int, UITouch*>::iterator it = touches.find(identity);
    UITouch* touch = nil;

    if (phase == 0) {
        if (it != touches.end()) {
            // A stale slot (e.g. a dropped touchend during a fast gesture — browsers do not
            // guarantee delivery under all conditions — or OS key-repeat re-firing a "begin" for
            // an action that never got a matching release, e.g. held Space/jump). Must tell the
            // ENGINE this touch ended, not just drop our own bookkeeping: Input::touchesBegan
            // (Classes/Input.mm) claims the first `touches[i].down==M_NONE` slot and only frees it
            // again on a matching touchesEnded/Cancelled keyed by UITouch* pointer identity. Just
            // releasing our side's UITouch* and erasing the map left that engine-side slot
            // permanently down==M_DOWN (a stuck, unmatchable pointer) — and since Hud::update
            // derives m_jump/etc. by scanning ALL touches for down==M_DOWN inside a hit-box
            // regardless of whose pointer it is, a single leaked slot at the jump button was
            // enough to make the player jump forever, one extra phantom slot per repeat keydown.
            // touchesEnded (not Cancelled, which is Input::clearAll() — wipes every live touch,
            // collateral damage to unrelated ones like movement) frees just this one slot.
            setPooledTouch(pooledTouchSet(), it->second);
            Input::getInput()->touchesEnded(pooledTouchSet(), pooledEvent());
            [it->second release];
            touches.erase(it);
        }
        touch = [[UITouch alloc] init];
        touch->_identity = (void*)(intptr_t)identity;
        touches[identity] = touch; // retained ownership: lives here until end/cancel below
    } else {
        if (it == touches.end()) {
            // move/end/cancel with no matching start — e.g. a move that arrived after we already
            // released on end. Nothing to update; ignore rather than inventing a touch.
            [pool release];
            return;
        }
        touch = it->second;
    }

    touch->_location = CGPointMake(x, y);
    touch->_timestamp = eden_platform_now_ms() / 1000.0;
    touch->_phase = (phase == 0) ? UITouchPhaseBegan
                  : (phase == 1) ? UITouchPhaseMoved
                  : (phase == 2) ? UITouchPhaseEnded
                                 : UITouchPhaseCancelled;

    setPooledTouch(pooledTouchSet(), touch);
    NSSet* set = pooledTouchSet();
    UIEvent* event = pooledEvent();

    Input* input = Input::getInput();
    switch (phase) {
        case 0: input->touchesBegan(set, event); break;
        case 1: input->touchesMoved(set, event); break;
        case 2: input->touchesEnded(set, event); break;
        default: input->touchesCancelled(set, event); break;
    }

    if (phase == 2 || phase == 3) {
        touches.erase(identity);
        [touch release]; // balances this function's own alloc/retain on "start"
    }

    [pool release];
}

// ---------------------------------------------------------------------------------------------
// Pass 23: desktop keyboard/mouse controls (WASD/mouse-look/click mine-build/hotbar/pointer-lock).
// Full spec: web/docs/archive/PORT-STATUS-2026-08-13.md "Next tasks (pass 23) §1". These are additional small
// entry points alongside eden_input_pointer_event above, not a replacement for it — the DOM
// mousedown/move/up + touch listeners in public/eden-st.html keep driving the menu/HUD-button
// hit-testing UI exactly as before; these only cover gameplay input that has no natural touch
// equivalent (a real keyboard, or a pointer-locked mouse with no absolute position).
//
// Reserved synthetic touch identities distinct from public/eden-st.html's MOUSE_IDENTITY(-1) and
// from real touch identifiers (always >= 0): -2 for the mine/build crosshair click, -3 for
// keyboard-driven HUD button taps (E/C/ESC). Only one of each is ever live at a time, so sharing
// one identity per category (rather than minting one per key) is safe.
static const int kClickIdentity = -2;
static const int kHudTapIdentity = -3;

// Movement: bypasses Joystick/touch entirely and drives Player::setSpeed directly (the same public
// API Joystick::update calls, Classes/Joystick.mm:60) — walking a WASD key-state into a synthetic
// touch inside the joystick's 88x88 screen region would work too, but would fight any real touch
// landing in that region and adds no fidelity; setSpeed is already the engine's normalized
// direction + magnitude entry point once you're past the touch bookkeeping. `forward`/`strafe`
// are in [-1,1] (JS pre-combines W/S and A/D into net -1/0/1 before calling); `speedMul` is the
// walk_speed engine parameter Joystick.mm feeds after its own NormalizeVector+magnitude clamp
// (SPRINT ~1.3 matches Joystick's clamp ceiling, WALK ~0.5 is this port's own choice for Alt).
//
// hud->use_joystick MUST be off for this to have any effect: Classes/SettingsMenu.mm:189
// unconditionally sets it TRUE on every settings load (not a saved preference — hardcoded), and
// Hud::update (Classes/Hud.mm:794) runs Joystick::update() every single frame whenever it's on;
// with no real touch ever landing in the joystick's screen region, Joystick::update's "not
// handled" branch (Classes/Joystick.mm:81-84) calls setSpeed(zero,0) and silently stomps
// whatever we just set here one line later in the same World::update pipeline (Hud::update runs
// before Player::preupdate — Classes/World.mm:492-499) — found by a headless test showing zero
// displacement after 60 real-time-ticked frames of forward input despite max_walk_speed reading
// correctly right after the call.
//
// Phase 2 (input mode): only force it off in the DESKTOP profile — eden_effective_input_is_touch()
// (Settings_web.mm) arbitrates Auto/Touch/Keyboard+Mouse. In the Touch profile the real on-screen
// joystick owns use_joystick and this keyboard path should not fight it (desktop keys are not
// expected to be live in a touch session, but a stray call here must not stomp the joystick).
EDEN_EXPORT
void eden_set_move_input(float forward, float strafe, float speedMul) {
    if (!World::getWorld || !World::getWorld->player || !World::getWorld->hud) return;
    extern int eden_effective_input_is_touch(void);
    if (!eden_effective_input_is_touch()) World::getWorld->hud->use_joystick = FALSE;
    float len = sqrtf(forward * forward + strafe * strafe);
    if (len < 0.0001f) {
        World::getWorld->player->setSpeed(MakeVector(0, 0, 0), 0);
        return;
    }
    // Player::setSpeed remaps its arg's y -> walk_force.z (forward) and x -> walk_force.x
    // (strafe), see Classes/Player.mm:145-149 — pass a Vector in that (x=strafe, y=forward) shape.
    float vx = strafe / len, vy = forward / len, vz = 0.0f;
#ifndef EDEN_LD_WRAP
    // Phase N Stage 2. On web, Movement_web.mm's `-Wl,--wrap=` of Player::setSpeed applies the
    // frame-rate normalisation and the walk_speed fold on the way through, and doing it here as
    // well would double it. On a target whose linker has no --wrap (macOS ld64 — see the guard
    // comment in Movement_web.mm) nothing else will, and setSpeed reads walk_dir's MAGNITUDE as
    // the target velocity — so a unit vector means a player who does not move at all. Same
    // function the wrap calls, so the two paths cannot drift.
    extern void eden_movement_scale_walk_dir(float*, float*, float*, float);
    eden_movement_scale_walk_dir(&vx, &vy, &vz, speedMul);
#endif
    World::getWorld->player->setSpeed(MakeVector(vx, vy, vz), speedMul);
}

// Fly up/down (this fork ships FLY_MODE=true, Classes/Player.mm:32). FLY_UP/FLY_DOWN are the same
// two externs Classes/Hud.mm's burn/mine HUD buttons flip as a tap-toggle when FLY_MODE is on
// (Classes/Hud.mm ~658-720) — driven here as momentary key state instead, since a keyboard has a
// real up/down rather than a single tap. Classes/Player.mm:1003-1010: vel.y gets +thrust if
// FLY_UP, -thrust if FLY_DOWN, so both-false (neither key held) or both-true (both held) net to a
// hover, and exactly one true gives clean ascend/descend.
EDEN_EXPORT
void eden_set_fly_thrust(int up, int down) {
    extern bool FLY_UP;
    extern bool FLY_DOWN;
    FLY_UP = up != 0;
    FLY_DOWN = down != 0;
}

// Crouch (held), Source-engine style — halves Player::boxheight (Classes/Player.mm's preupdate)
// so the player fits through 1-block gaps/vents. Plain bool, same shape as FLY_UP/FLY_DOWN above.
// Setting CROUCH_HELD when the "Crouch mode" experiment is off is harmless: Player::preupdate
// ANDs it with CROUCH_ENABLED, so this can stay wired unconditionally on the keyboard side.
EDEN_EXPORT
void eden_set_crouch(int down) {
    extern bool CROUCH_HELD;
    CROUCH_HELD = down != 0;
}

// Experiments tab: "Crouch mode" toggle. Mirrors eden_set_fly_mode/eden_get_fly_mode below.
EDEN_EXPORT
void eden_set_crouch_enabled(int on) {
    extern bool CROUCH_ENABLED;
    CROUCH_ENABLED = on != 0;
}

EDEN_EXPORT
int eden_get_crouch_enabled(void) {
    extern bool CROUCH_ENABLED;
    return CROUCH_ENABLED ? 1 : 0;
}

// Mouse look while pointer-locked. A locked pointer only ever reports relative movement (no
// absolute position to diff against a touch's first-down point), so this can't reuse
// eden_input_pointer_event's touch path — Player::processInput's movecam branch (Classes/
// Player.mm ~568-588) both needs an absolute mx/my AND applies a 15px tap-vs-drag deadzone
// relative to the touch's own start point, neither of which makes sense for a stream of deltas.
// This instead reimplements just that branch's yaw/pitch math against Player's public fields.
// dx/dy are DOM MouseEvent.movementX/Y (positive = pointer moved right/down). YAW_SPEED/
// PITCH_SPEED (.4f) and the IS_IPAD ×2 scale are copied from Classes/Player.mm:19-20,572-576 —
// both #define'd there, not exported, so they're duplicated here; keep them in sync if changed.
EDEN_EXPORT
void eden_apply_look_delta(float dx, float dy) {
    if (!World::getWorld || !World::getWorld->player) return;
    Player* p = World::getWorld->player;
    const float YAW_SPEED = 0.4f, PITCH_SPEED = 0.4f;
    // Owned by the settings panel (src/seam/Settings_web.mm); 1.0 until it initialises. Separate
    // X/Y sensitivity (PC controls audit F9g) — Y defaults to the same 1.0 as X, so a player who
    // never opens that row gets identical behaviour to before this setting existed.
    extern float eden_look_sensitivity;
    extern float eden_mouse_sensitivity_y;
    const float ipadScale = IS_IPAD ? 2.0f : 1.0f;
    const float sx = ipadScale * eden_look_sensitivity;
    const float sy = ipadScale * eden_mouse_sensitivity_y;
    if (p->invertcam) {
        p->yaw -= sx * dx * YAW_SPEED;
        p->pitch += sy * dy * PITCH_SPEED;
    } else {
        p->yaw += sx * dx * YAW_SPEED;
        p->pitch -= sy * dy * PITCH_SPEED;
    }
}

// Which action right-click performs. Mirrors whatever tool is currently selected via the fire (F)
// or paint (C, after a colour is picked) HUD buttons, falling back to MODE_BUILD. Read/written
// only through eden_track_tool_mode() below — NOT sampled directly from hud->mode at click time,
// because eden_click_begin/eden_click_end themselves overwrite hud->mode for the duration of the
// click (see below), so hud->mode alone can't tell "the tool the player selected" apart from "the
// tool our own last click happened to use".
static int g_toolMode = MODE_BUILD;

// Called once per frame (from eden_ui_tick) with the engine's current hud->mode, BEFORE any click
// this frame has a chance to overwrite it. MODE_BUILD/MODE_BURN/MODE_PAINT are real tool
// selections, captured as-is. MODE_NONE arises either from the fire tool's own toggle-off
// (Classes/Hud.mm's rburn handler: MODE_BURN -> MODE_NONE) or from eden_click_begin's "empty hand"
// click-on-nothing below — either way it means "no tool selected right now", so the remembered
// right-click tool reverts to the default build tool rather than staying stuck on the last one.
// MODE_MINE/MODE_CAMERA/the pickers are deliberately ignored — they are not right-click tools.
static void eden_track_tool_mode(int hudMode) {
    if (hudMode == MODE_BUILD || hudMode == MODE_BURN || hudMode == MODE_PAINT) {
        g_toolMode = hudMode;
    } else if (hudMode == MODE_NONE) {
        g_toolMode = MODE_BUILD;
    }
}

// Same crosshair raycast eden_pick_block_at_crosshair uses (see its comment for why FC_DESTROY
// and the swapped mx/my are correct here) — factored out so eden_click_begin below can ask "is
// there anything under the crosshair at all" without duplicating the unproject math. A miss
// (sky, or nothing within range) comes back with x==-1, exactly Util.mm's own "no hit" contract.
static Point3D eden_crosshair_raycast(void) {
    Point3D miss; miss.x = -1; miss.y = 0; miss.z = 0;
    Input* input = Input::getInput();
    if (!input) return miss;
    const int cx = (int)(SCREEN_WIDTH / 2.0f);
    const int cy = (int)(input->scr_height - SCREEN_HEIGHT / 2.0f);
    return findWorldCoords(cy, cx, FC_DESTROY);
}

// Left-click always mines. Right-click performs whichever tool is currently selected — normally
// MODE_BUILD, but MODE_BURN/MODE_PAINT while the fire/paint tool is active, so right-click burns
// or paints instead of blindly placing a block over top of what the player meant to torch/paint.
// Synthesizes an ordinary tap at the crosshair (screen center) through the SAME touch path as a
// real tap — Player::processInput's tap-build/mine/paint/burn logic (Classes/Player.mm ~236-420)
// fires on M_DOWN then M_RELEASE of one touch, so the begin and end calls below must land on
// separate engine ticks (JS mousedown/mouseup naturally straddle at least one frame; the headless
// driver must tick between them too).
//
// "Empty hand" mode (2026-08-01): touch's HUD already lets a player tap the active mine icon a
// second time to drop into MODE_NONE, which Player::processInput's mine/build/burn/paint branch
// (Classes/Player.mm ~365) simply doesn't match, so a live MODE_NONE touch is inert by
// construction. Desktop has no icon to re-tap, so a click that lands on nothing (sky, or beyond
// raycast range) does the same thing a re-tap would: drop into MODE_NONE instead of performing
// the click. Once in MODE_NONE, every click is swallowed here (never forwarded, never re-picks a
// tool) until the player explicitly re-selects one via E/C/F — hudTapButtonRect's buttons switch
// mode unconditionally away from whatever it was, so MODE_NONE exits itself just like touch does.
//
// MODE_PAINT is exempt from the miss check: Player::processInput's own point.x==-1 branch
// (Classes/Player.mm ~423) is what paints the SKY — `mode==MODE_PAINT` there is specifically the
// "the raycast hit nothing" case, used to recolor the sky / toggle day-night. Treating that miss
// as "empty hand" would make sky-painting unreachable from the mouse, so a paint click always
// forwards regardless of what (if anything) is under the crosshair.
EDEN_EXPORT
void eden_click_begin(int isBuild) {
    if (!World::getWorld || !World::getWorld->hud) return;
    Hud* hud = World::getWorld->hud;
    if (hud->mode == MODE_NONE) return;
    const int resolvedMode = isBuild ? g_toolMode : MODE_MINE;
    if (resolvedMode != MODE_PAINT && eden_crosshair_raycast().x == -1) {
        hud->mode = MODE_NONE;
        return;
    }
    hud->mode = resolvedMode;
    eden_input_pointer_event(0, kClickIdentity, SCREEN_WIDTH / 2.0f, SCREEN_HEIGHT / 2.0f);
}
EDEN_EXPORT
void eden_click_end(int isBuild) {
    eden_input_pointer_event(2, kClickIdentity, SCREEN_WIDTH / 2.0f, SCREEN_HEIGHT / 2.0f);
}

// E/C/ESC: synthesize a tap on the real HUD button rects (Hud's public rbuild/rpaint/rmenu
// Buttons) rather than duplicating Hud.mm's mode-toggle logic here — that logic lives inline in
// Hud::update's touch-handling loop (not a standalone function), so reusing the actual button
// hit-test via a real tap tracks any future layout/behavior change for free. `which`: 0=menu
// (ESC/rmenu, Classes/Hud.mm:550-560), 1=blocks (E/rbuild, toggles MODE_PICK_BLOCK/MODE_BUILD,
// Classes/Hud.mm:681-696), 2=colors (C/rpaint, toggles MODE_PICK_COLOR/MODE_PAINT, Classes/
// Hud.mm:702-712).
// 3=save, 4=warp home, 5=photo, 6=exit (rsave/rhome/rcam/rexit — Hud::handlePickMenu,
// Classes/Hud.mm:878-931) — pass 30, the DOM pause menu (public/eden-pausemenu.js) that replaced
// the engine's own tiny 4-icon renderMenuScreen. Only take effect while hud->inmenu is true
// (handlePickMenu is itself only reachable from Hud::update's `if(inmenu)` guard), which the
// pause menu's own open()/close() keep in lockstep with by tapping `which=0` (rmenu) first.
// Hud's Button rects are in itouch's bottom-left-origin Y-up space (same space
// Hud.mm's own inbox2(touches[i].mx,...) checks use) — eden_input_pointer_event's contract is
// top-left Y-down (see this file's header comment), and Input::touchesBegan flips Y for us, so
// feed it SCREEN_HEIGHT-y to land back on the button's real Y-up y after that flip.
static Button hudTapButtonRect(Hud* hud, int which) {
    switch (which) {
        case 0: return hud->rmenu;
        case 1: return hud->rbuild;
        case 2: return hud->rpaint;
        case 3: return hud->rsave;
        case 4: return hud->rhome;
        case 5: return hud->rcam;
        case 6: return hud->rexit;
        // 7 = fire/burn tool (rburn) — added so the web port can bind a key to it (F, by
        // default); Hud::update's own tap handler toggles MODE_BURN on/off exactly like a real
        // tap here would (Classes/Hud.mm:663-680). One quirk inherited as-is from that same
        // handler: if FLY_MODE is on, tapping this button ALSO flips FLY_UP — harmless in
        // practice since eden_set_fly_thrust (driven by the actual held keys) overwrites FLY_UP
        // again on the very next frame.
        case 7: return hud->rburn;
        default: return hud->rmenu;
    }
}
EDEN_EXPORT
void eden_tap_hud_button_begin(int which) {
    if (!World::getWorld || !World::getWorld->hud) return;
    if (which < 0 || which > 7) return;
    Button b = hudTapButtonRect(World::getWorld->hud, which);
    float cx = b.origin.x + b.size.width / 2.0f;
    float cy = b.origin.y + b.size.height / 2.0f;
    eden_input_pointer_event(0, kHudTapIdentity, cx, SCREEN_HEIGHT - cy);
}
EDEN_EXPORT
void eden_tap_hud_button_end(int which) {
    if (!World::getWorld || !World::getWorld->hud) return;
    if (which < 0 || which > 7) return;
    Button b = hudTapButtonRect(World::getWorld->hud, which);
    float cx = b.origin.x + b.size.width / 2.0f;
    float cy = b.origin.y + b.size.height / 2.0f;
    eden_input_pointer_event(2, kHudTapIdentity, cx, SCREEN_HEIGHT - cy);
}

// Is the engine's own pause-menu state (Hud::inmenu, toggled by a tap on rmenu) currently on?
// The DOM pause menu (public/eden-pausemenu.js) polls this each frame to know when the player
// tapped the in-game menu corner icon directly on the canvas (bypassing our DOM entirely, same as
// any other HUD button) so it can show its overlay in lockstep with the engine's real state
// rather than keeping its own parallel copy that could drift.
EDEN_EXPORT
int eden_hud_in_menu(void) {
    if (!World::getWorld || !World::getWorld->hud) return 0;
    return World::getWorld->hud->inmenu ? 1 : 0;
}

// Does the current hud->mode want the crosshair hidden? True in "empty hand" (MODE_NONE — see
// eden_click_begin's click-on-nothing handling above) and in photo/camera mode (MODE_CAMERA,
// entered via the in-game menu's Take Photo button, `which=5` in hudTapButtonRect) — neither has
// anything to aim at a block with, same reasoning as eden_update_block_preview's own mode check
// already excluding both from the block-preview ghost. public/eden-st.html ANDs this with the
// player's "Crosshair" settings toggle (eden_get_crosshair) each frame.
EDEN_EXPORT
int eden_crosshair_hidden_by_mode(void) {
    if (!World::getWorld || !World::getWorld->hud) return 0;
    const int m = World::getWorld->hud->mode;
    return (m == MODE_NONE || m == MODE_CAMERA) ? 1 : 0;
}

// 1-9 hotbar and wheel scroll. Eden's actual block picker is a scrolling grid (Hud::
// handlePickBlock, NUM_DISPLAY_BLOCKS=35 cells addressed by a `static int hudBlocks[]` that is
// file-static to Hud.mm and not reachable from here) opened via the 'E' tap above, not a 9-slot
// hotbar strip — there is no engine concept this maps to 1:1. This is a deliberate simplification:
// a fixed, curated 9-block palette (common build blocks, by TYPE_* from Classes/Constants.h) that
// number keys/wheel index into directly via the same public hud->blocktype/hud->mode fields
// Hud::handlePickBlock itself writes on a real pick (Classes/Hud.mm ~811-841), so a build click
// right after still builds the selected type.
// Phase 4 (PC controls audit): no longer `const` — the engine's real 35-cell picker
// (Hud::handlePickBlock) now feeds it, see eden_ui_tick's picker->camera edge below. This array
// is just the INITIAL contents; public/eden-st.html persists whatever the player ends up with to
// localStorage and restores it via eden_set_hotbar_slot_type on the next load.
static int kHotbarBlocks[9] = {
    TYPE_STONE, TYPE_DIRT, TYPE_GRASS, TYPE_WOOD, TYPE_BRICK,
    TYPE_COBBLESTONE, TYPE_SAND, TYPE_GLASS, TYPE_LIGHTBOX,
};
static int hotbarIndex = 0;

EDEN_EXPORT
void eden_select_hotbar_slot(int slot) {
    if (!World::getWorld || !World::getWorld->hud) return;
    if (slot < 0 || slot > 8) return;
    hotbarIndex = slot;
    World::getWorld->hud->blocktype = kHotbarBlocks[slot];
    World::getWorld->hud->mode = MODE_BUILD;
}

EDEN_EXPORT
void eden_hotbar_scroll(int dir) {
    eden_select_hotbar_slot(((hotbarIndex + (dir > 0 ? 1 : -1)) % 9 + 9) % 9);
}

EDEN_EXPORT
int eden_get_hotbar_index(void) { return hotbarIndex; }

EDEN_EXPORT
int eden_get_hotbar_slot_type(int slot) {
    if (slot < 0 || slot > 8) return TYPE_NONE;
    return kHotbarBlocks[slot];
}

// Restores a slot's contents (public/eden-st.html's localStorage round-trip). Does NOT select
// the slot or touch hud->mode/blocktype — only the array cell backing eden_select_hotbar_slot.
EDEN_EXPORT
void eden_set_hotbar_slot_type(int slot, int type) {
    if (slot < 0 || slot > 8) return;
    kHotbarBlocks[slot] = type;
}

// Project audit 2026-07-30, row F3 — middle-click "pick block" (standard voxel-game affordance,
// requested from play, audit row 31). Reads whatever block is under the crosshair via the EXACT
// same raycast Player::processInput's MODE_MINE/PAINT/BURN branch uses (Classes/Player.mm ~313:
// `findWorldCoords(touches[i].my, touches[i].mx, FC_DESTROY)` — note the swapped arg names, matched
// here rather than guessed, and `terrain->getLand(point.x, point.z, point.y)` with NO /2, which is
// specifically the FC_DESTROY-mode reading; FC_PLACE's branch divides by 2 for build_size reasons
// that don't apply here). No Classes/ edit: findWorldCoords computes its own camera matrices via
// cam->render2() inside the call rather than reading render-loop leftovers, so it's safe to call
// from any point in the frame, including here (a JS-invoked seam function, not the update/render
// pipeline itself).
EDEN_EXPORT
int eden_pick_block_at_crosshair(void) {
    if (!World::getWorld || !World::getWorld->hud || !World::getWorld->terrain) return TYPE_NONE;
    Point3D point = eden_crosshair_raycast();
    if (point.x == -1) return TYPE_NONE;
    int type = World::getWorld->terrain->getLand(point.x, point.z, point.y);
    if (type == TYPE_NONE || type == -1) return TYPE_NONE;
    // Same convention Hud::handlePickBlock's real 35-cell picker already established (Phase 4
    // comment above eden_select_hotbar_slot: a real pick overwrites the CURRENTLY selected slot,
    // not just the initial palette) — and switches to build mode so a follow-up click places it.
    eden_set_hotbar_slot_type(hotbarIndex, type);
    World::getWorld->hud->blocktype = type;
    // The 35-cell picker only ever sets a fixed color (0, or 20 for doors) since it picks from a
    // fixed palette, not a placed block. This eyedropper picks a real in-world block, so carry its
    // actual paint color along too — otherwise middle-click-picking a painted block silently drops
    // the color and the next placed block comes out unpainted.
    World::getWorld->hud->block_paintcolor = World::getWorld->terrain->getColor(point.x, point.z, point.y);
    World::getWorld->hud->mode = MODE_BUILD;
    return type;
}

// Does the UI currently need a real, visible mouse cursor? True in the menu and in the block and
// colour pickers, which are grid-of-swatches screens you point at — pointer lock hides the cursor
// and converts mousemove to look deltas, so with it held the pickers are unusable. This is the
// Minecraft convention (inventory releases the mouse, closing it re-grabs). Polled from JS each
// frame rather than pushed, because the pickers can also be opened/closed by engine-side paths
// (the HUD buttons) that never route through our key handlers.
EDEN_EXPORT
int eden_ui_wants_cursor(void) {
    // A GLDialog (native's world-type/height chooser, the ESC-menu Home popup) is a modal you
    // point at, and it can be raised from an in-game path that has already cleared hud->inmenu
    // (Hud::handlePickMenu sets inmenu=false in the same breath as showAlertWarpHome()). Without
    // this term the native mouse-capture predicate re-grabs the pointer the instant the dialog
    // opens and its buttons become unclickable. Permanently false on web (DOM overlays instead).
    if (GLDialog::active()) return 1;
    if (!World::getWorld) return 1;
    if (World::getWorld->game_mode == 0) return 1;  // menu
    if (!World::getWorld->hud) return 1;
    const int m = World::getWorld->hud->mode;
    return (m == MODE_PICK_BLOCK || m == MODE_PICK_COLOR) ? 1 : 0;
}

// Fly mode. `Classes/Player.mm:32` hard-codes FLY_MODE/FLY_UP/FLY_DOWN to true in this fork
// (CLAUDE.md notes stock behaviour is false), and Hud.mm:570 still carries the commented-out
// remains of the toggle button that used to drive it — so there is no way to turn it off from the
// shipped UI. These are plain mutable globals, so the port can own the setting without touching
// the engine: the page defaults it OFF at startup (stock behaviour) and binds a key to toggle.
// FLY_UP/FLY_DOWN move with it; they gate the vertical controls, which are meaningless on foot.
// Jump. Player::preupdate reads `hud->m_jump` (Classes/Player.mm:897) — it is the HUD jump
// button's state, not an input event, so it has to be held for as long as the key is down. With
// fly mode off (now the default) Space had no effect at all, because the only thing bound to it
// was fly thrust.
// Held for as long as the key is down, via a SYNTHETIC TOUCH on the HUD's jump button rather than
// by writing hud->m_jump. Writing the flag directly does nothing: Hud::update clears m_jump at the
// top of every frame (Hud.mm:777) and re-derives it purely from whether a touch is inside
// `rjumphit` — so a value set from JS between frames is gone before Player::preupdate reads it.
// (Exactly the same trap as `use_joystick`; assume any hud-> input flag is frame-derived.)
static const int kJumpTouchIdentity = -7;  // distinct from the mouse (-1) and the HUD-tap identity

EDEN_EXPORT
void eden_set_jump(int down) {
    if (!World::getWorld || !World::getWorld->hud) return;
    extern bool FLY_MODE;
    // While flying, Space is the ascend control (eden_set_fly_thrust owns it) and must not also
    // jump — the two would fight over vertical velocity. Only gate the PRESS: if fly mode toggles
    // on while the synthetic touch is down, the release must still go through, or the touch is
    // stranded on rjumphit and Hud::update re-derives m_jump as held forever once fly toggles
    // back off (the touch never began per FLY_MODE's eyes, so it can never "end" either).
    if (down && FLY_MODE) return;
    Button b = World::getWorld->hud->rjumphit;
    float cx = b.origin.x + b.size.width / 2.0f;
    float cy = b.origin.y + b.size.height / 2.0f;
    eden_input_pointer_event(down ? 0 : 2, kJumpTouchIdentity, cx, SCREEN_HEIGHT - cy);
}

// Per-frame UI bookkeeping, polled from the page alongside eden_ui_wants_cursor().
//
// Opening a picker sets hud->mode to MODE_PICK_BLOCK/MODE_PICK_COLOR, and closing it drops back to
// MODE_CAMERA — so a player who had selected build (or mine/paint/burn) loses that mode just by
// glancing at the block picker, and has to re-pick it every time. On touch that is survivable
// because the mode buttons are right there; with a mouse and a crosshair it makes click-to-build
// unusable. So: remember the last real action mode and restore it on the picker -> camera edge,
// and only on that edge, so any deliberate switch to MODE_CAMERA still sticks.
// ---------------------------------------------------------------------------------------------
// Translucent block preview ("ghost block"), pass 27 — an OPT-IN toggle, default OFF.
//
// The engine already draws this: Player::render (Classes/Player.mm:2069-2087) draws a translucent
// Graphics::drawCube at `touches[i].preview` for any touch that is inuse-by-Player, still DOWN,
// still counts as a tap (`placeBlock`), has a latched `previewtype`, and has been held for a
// nonzero time. On a touchscreen that is a press-and-hold: you see where the block will land
// before you lift. With a mouse a click is a press and a release in the same gesture, so the
// preview only ever flashed — which is why it looked like an undiscoverable quirk rather than a
// feature. This makes it a persistent crosshair ghost instead.
//
// The mechanism is a PERSISTENT SYNTHETIC TOUCH parked at the crosshair, never released:
//   * `inuse` is set to Player's usage_id directly, so Hud::update never gets a chance to claim
//     it (Hud only takes touches with inuse==0) and it cannot trip a HUD button.
//   * `moved` stays TRUE with `movecam` FALSE, which puts it in Player::processInput's re-latch
//     branch (Classes/Player.mm:589-626) EVERY frame — that is what makes the ghost follow the
//     crosshair instead of freezing where it was first placed. (The claim branch at :240 only
//     runs for inuse==0 and would latch once.)
//   * it is never given M_RELEASE, and M_RELEASE is the ONLY thing that places/mines a block
//     (Classes/Player.mm:302-320), so this can never edit the world by itself.
// It occupies the LAST touch slot; Input::touchesBegan allocates from index 0 upward, so real
// touches keep four slots and only collide here under a 5-finger gesture (the setup below
// declines to claim the slot if a real touch already holds it).
//
// ONE KNOWN SIDE EFFECT, and why it is opt-in rather than on by default: a second live touch
// makes Player::processInput's `num > 1` branch (Classes/Player.mm:289-301) clear `movecam` on
// EVERY down touch, which disables touch drag-to-look for as long as the ghost is showing. That
// costs nothing on the desktop control scheme this ships with (looking is pointer-lock mouse
// deltas, which bypass touches entirely — eden_apply_look_delta), but it would matter on a real
// touchscreen, so the setting stays off unless the player asks for it.
//
// Applies to build AND the destructive modes because that is exactly what the engine's own
// preview does: MODE_BUILD ghosts the selected block type at the placement cell, MODE_MINE/
// PAINT/BURN outline the targeted cell with TYPE_CLOUD (Classes/Player.mm:604-613).
static const int kPreviewSlot = MAX_TOUCHES - 1;
// Classes/Player.mm:143 `static const int usage_id=10;` — file-static, so it cannot be linked
// against and has to be duplicated. If that constant ever changes, the preview silently stops
// drawing (Player would ignore the slot); it does not misbehave.
static const int kPlayerUsageId = 10;

static bool g_blockPreview = false;   // opt-in; the page persists the user's choice
static bool g_previewLive = false;    // do we currently own kPreviewSlot?

static void eden_clear_preview_touch(itouch& t) {
    t.down = M_NONE;
    t.inuse = 0;
    t.moved = 0;
    t.movecam = FALSE;
    t.placeBlock = FALSE;
    t.previewtype = TYPE_NONE;
    t.etime = 0;
    t.touch_id = NULL;
}

static void eden_update_block_preview(void) {
    Input* input = Input::getInput();
    if (!input) return;
    itouch& t = input->touches[kPreviewSlot];

    bool want = false;
    if (g_blockPreview && World::getWorld && World::getWorld->hud &&
        World::getWorld->game_mode != 0) {
        const int m = World::getWorld->hud->mode;
        want = (m == MODE_BUILD || m == MODE_MINE || m == MODE_PAINT || m == MODE_BURN);
    }

    if (!want) {
        if (g_previewLive) { eden_clear_preview_touch(t); g_previewLive = false; }
        return;
    }
    if (!g_previewLive) {
        if (t.down != M_NONE) return;   // a real touch holds the slot; try again next frame
        eden_clear_preview_touch(t);    // start from a known state (previewtype/etime cleared)
        g_previewLive = true;
    }

    // Crosshair = screen centre, in the BOTTOM-LEFT-ORIGIN, Y-UP space `itouch` stores (see this
    // file's COORDINATE CONTRACT note): a real touch's y goes through Input::touchesBegan's
    // `scr_height - point.y` flip before it lands in mx/my, and this writes mx/my directly.
    const int cx = (int)(SCREEN_WIDTH / 2.0f);
    const int cy = (int)(input->scr_height - SCREEN_HEIGHT / 2.0f);
    t.touch_id = NULL;
    t.inuse = kPlayerUsageId;
    t.down = M_DOWN;
    t.movecam = FALSE;
    t.placeBlock = TRUE;
    t.moved = 1;
    t.mx = t.pmx = t.fx = cx;
    t.my = t.pmy = t.fy = cy;
    // Clear the latched type every frame so the ghost DISAPPEARS when the crosshair is on nothing
    // placeable (sky, or a cell that is already solid). Player::processInput only ever writes
    // `previewtype` on a successful raycast, never clears it, so without this the ghost would
    // freeze at the last valid cell — which is right for a held finger (the engine's own
    // press-and-drag behaviour) but wrong for a crosshair that is always on. Safe in either rAF
    // ordering: this runs between engine frames, so the clear is always followed by a fresh
    // processInput latch before the next Player::render.
    t.previewtype = TYPE_NONE;
}

EDEN_EXPORT
void eden_set_block_preview(int on) {
    g_blockPreview = (on != 0);
    eden_update_block_preview();
}

EDEN_EXPORT
int eden_get_block_preview(void) { return g_blockPreview ? 1 : 0; }

EDEN_EXPORT
void eden_ui_tick(void) {
    if (!World::getWorld || !World::getWorld->hud) return;
    if (World::getWorld->game_mode == 0) {
        if (g_previewLive) eden_update_block_preview();  // tears the ghost touch down on menu exit
        return;
    }
    eden_update_block_preview();
    static int lastActionMode = -1;
    static int prevMode = -1;
    const int m = World::getWorld->hud->mode;
    eden_track_tool_mode(m);
    if (m == MODE_BUILD || m == MODE_MINE || m == MODE_PAINT || m == MODE_BURN) lastActionMode = m;
    const bool leftPicker = (prevMode == MODE_PICK_BLOCK || prevMode == MODE_PICK_COLOR);
    if (leftPicker && m == MODE_CAMERA && lastActionMode != -1) {
        World::getWorld->hud->mode = lastActionMode;
        // Phase 4 (PC controls audit F9f): a real pick from the engine's own 35-cell picker
        // overwrites the CURRENTLY SELECTED hotbar slot with whatever the player just chose, so
        // the curated kHotbarBlocks[] becomes the initial contents rather than the only ones —
        // same edge lastActionMode's restore already uses, so this fires exactly once per pick.
        if (prevMode == MODE_PICK_BLOCK) kHotbarBlocks[hotbarIndex] = World::getWorld->hud->blocktype;
    }
    prevMode = World::getWorld->hud->mode;
}

EDEN_EXPORT
void eden_set_fly_mode(int on) {
    extern bool FLY_MODE, FLY_UP, FLY_DOWN;
    FLY_MODE = FLY_UP = FLY_DOWN = (on != 0);
}

EDEN_EXPORT
int eden_get_fly_mode(void) {
    extern bool FLY_MODE;
    return FLY_MODE ? 1 : 0;
}

// Audit row 17/G1 (touch-draggable controls card): a "move controls" mode for the on-screen
// joystick pad, driven from eden-input.js's existing touch pipeline rather than a new one — the
// page tracks the drag itself (same touchstart/touchmove/touchend it already has) and only pushes
// the resulting rect origin here. Classes/Joystick.mm's joystickCustomizeMode flag is the actual
// feature (Joystick::update no-ops while it's on, so this can't fight a real touch's movement
// input); the three exports below are just the platform-specific wiring a JS drag gesture needs,
// which is why they live in this seam file and not in Classes/ (web/CLAUDE.md rule 2).
EDEN_EXPORT
void eden_joystick_set_customize_mode(int on) {
    extern bool joystickCustomizeMode;
    joystickCustomizeMode = (on != 0);
    // Entering mid-walk must not leave a stale nonzero walk_dir latched forever — Joystick::update
    // is the only thing that would otherwise zero it (on the next frame with no touch in the pad),
    // and it is about to stop running entirely.
    if (joystickCustomizeMode && World::getWorld && World::getWorld->player) {
        World::getWorld->player->setSpeed(MakeVector(0, 0, 0), 0);
    }
}

EDEN_EXPORT
int eden_joystick_get_customize_mode(void) {
    extern bool joystickCustomizeMode;
    return joystickCustomizeMode ? 1 : 0;
}

// origin = padbounds.origin, i.e. the pad's top-left corner in the engine's POINT space (same
// space Input_web.mm's touch coordinates already arrive in — see toEnginePoint() in
// eden-viewport.js). Plain float returns rather than out-params: nothing else in this port's JS
// exports asks the JS side to allocate wasm memory for a pointer, and two scalar getters are
// simpler for eden-input.js to call than managing a malloc'd float* just to read two numbers back.
// Clamping to stay on-screen is the CALLER's job (JS knows the live ENGINE_WIDTH/HEIGHT and the
// pad's own size; this file has no reason to duplicate that).
EDEN_EXPORT
float eden_joystick_get_origin_x(void) { extern CGRect padbounds; return padbounds.origin.x; }
EDEN_EXPORT
float eden_joystick_get_origin_y(void) { extern CGRect padbounds; return padbounds.origin.y; }
EDEN_EXPORT
float eden_joystick_get_pad_size(void) { extern CGRect padbounds; return padbounds.size.width; }

EDEN_EXPORT
void eden_joystick_set_origin(float x, float y) {
    // Live-browser finding (audit row 17/G1, 2026-08-04): padbounds is the translucent pad ring
    // Joystick::render() draws first; the solid knob it draws second is joystick_pos, which
    // Joystick::update() only ever resets to default_pos (Classes/Joystick.mm's ctor: a FIXED rect
    // that happens to equal padbounds' original (20,20) because both start at the same size, not
    // because anything keeps them in sync). Moving only padbounds here left the ring following the
    // drag while the knob stayed glued to the pre-move spot, on-screen AND after Done (update()'s
    // "no touch active" branch keeps re-snapping joystick_pos to the stale default_pos every
    // frame) — visibly two disconnected controls. Move both, and keep joystick_pos matching so the
    // knob tracks live during the drag instead of only jumping once the player next touches it.
    extern CGRect padbounds, default_pos, joystick_pos;
    padbounds.origin.x = x;
    padbounds.origin.y = y;
    default_pos.origin.x = x;
    default_pos.origin.y = y;
    joystick_pos.origin.x = x;
    joystick_pos.origin.y = y;
}

} // extern "C"
