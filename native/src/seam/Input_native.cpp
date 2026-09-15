// Input_native.cpp — SDL3 keyboard/mouse -> the engine's `eden_*` input exports.
// Phase N Stage 2 (WORKING/native-migration-plan-2026-09-04.md, "Input" row).
//
// The plan's instruction was: "SDL events -> the existing eden_input_pointer_event /
// eden_set_move_input / eden_apply_look_delta / eden_click_begin exports. `public/eden-input.js`
// is the reference for the mapping, `eden-keybinds.js` for the defaults." That is exactly what
// this file is — a translator, not a second input model. **Every behavioural decision below is
// already made in web/src/seam/Input_web.mm**, which this target compiles unchanged; nothing here
// reimplements a rule, it only decides which SDL event means which existing call.
//
// THE PLAN ALSO SAYS: "Split Input_web.mm (783 lines) into a shared core + a thin SDL translator
// only once both callers exist." Both callers now exist, and the answer that fell out is that no
// split is needed: Input_web.mm is already the shared core (the synthetic-touch construction, the
// hold-to-act mode tracking, the block preview, the HUD tap rects) and the browser-shaped part was
// never in it — it is in public/eden-input.js, which is a host-page file. So this is the SDL twin
// of eden-input.js, not of Input_web.mm.
//
// FOUR RULES INHERITED FROM THE WEB HOST, all of which have cost that port real time:
//
//  1. **Never write a `hud->` input flag directly.** Every one of them is re-derived from the live
//     touch set each frame, so a direct write is silently overwritten. Drive the HUD with the
//     synthetic-touch exports (`eden_tap_hud_button_*`, `eden_set_jump`, `eden_click_*`). This has
//     bitten the web port three times (web/CLAUDE.md's fast facts).
//  2. **A begin/end pair must straddle at least one engine tick.** `eden_click_begin` posts a
//     touch that `Hud::update` consumes on the NEXT frame; ending it in the same frame means the
//     engine never sees a click at all. Hence `g_pendingClickEnd` below rather than a
//     begin-then-end pulse.
//  3. **Movement is recomputed EVERY frame from held keys**, not pushed per keystroke.
//     `eden_set_move_input` takes raw -1..1 axes, and Movement_web.mm's frame-rate normalisation
//     wants a value every tick.
//  4. **Look deltas are relative-mouse deltas, unscaled here.** Sensitivity and the smoothing that
//     fixed "Mouse look feels laggy" both live behind `eden_apply_look_delta` itself, shared with
//     the gamepad path — applying a second factor here would double-count them.
//
// GAMEPAD IS HERE TOO, and by the same rule: it is a transliteration of public/eden-gamepad.js,
// not a second input model. SDL_Gamepad is the direct counterpart of the browser's "standard
// mapping" — SDL's controller database does the remapping the web file refuses to guess at — so
// the mapping table below is eden-gamepad.js's, symbol for symbol.
//
// WHAT IS NOT HERE YET (Stage 2's remaining rows): text entry for world renaming, and rebindable
// keys.
// The bindings below are `eden-keybinds.js`'s defaults, hard-coded — native has no `localStorage`,
// and the plan puts a real keybinds screen in Stage 2.5 because it needs the settings model to
// grow an int/string row type first.

#include "Input_native.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cmath>

// The engine-side input surface. All of it is web/src/seam/Input_web.mm, compiled into this target
// unchanged — see this file's header.
extern "C" {
void eden_input_pointer_event(int phase, int identity, float x, float y);
void eden_set_move_input(float forward, float strafe, float speedMul);
void eden_set_fly_thrust(int up, int down);
void eden_set_crouch(int on);
void eden_set_jump(int on);
void eden_apply_look_delta(float dx, float dy);
void eden_click_begin(int isBuild);
void eden_click_end(int isBuild);
void eden_hotbar_scroll(int dir);
void eden_select_hotbar_slot(int slot);
int  eden_pick_block_at_crosshair(void);
void eden_tap_hud_button_begin(int which);
void eden_tap_hud_button_end(int which);
void eden_ui_tick(void);
int  eden_get_hold_to_act(void);
double eden_platform_now_ms(void);
// Movement_web.mm's frame-interval EMA. On web the host page polls it once per rAF; here the
// frame loop does. Without it the normalisation factor is stuck at its seed, which is survivable
// but wrong — and on this target that factor is also what gives walk_dir its magnitude at all.
void eden_movement_tick(void);
int  eden_ui_wants_cursor(void);
int  eden_hud_in_menu(void);
int  eden_get_fly_mode(void);
void eden_set_fly_mode(int on);
void eden_set_block_preview(int on);
int  eden_get_block_preview(void);
void eden_gl_context_get_drawable_size(int* width, int* height);
void eden_native_gl_get_letterbox(int* x, int* y, int* w, int* h);

// The keybind model (Stage 5.2), in web/src/seam/Settings_web.mm — a SHARED seam file, so this
// is the same table the web build reads. Its codes are USB HID usage IDs, which is exactly what
// SDL_Scancode values are; see the static_asserts below, which are this file's half of that claim.
int  eden_keybind_count(void);
int  eden_keybind_index(const char* action);
int  eden_keybind_action_after(int prev, int code);
int  eden_keybind_get(int i);
}

// Classes/Globals.h's point space. The engine lays every HUD and menu element out in it, and it is
// DERIVED (window aspect x ui_scale) and can change mid-session — so read it live, never cache it.
extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

// The three gamepad rows from web/src/seam/Settings_web.mm's kSettings[], mirrored into C there
// precisely so this file can read them (row #24). Live values — the player can change them
// mid-session — so they are read per tick, never cached.
extern float eden_gamepad_enabled;
extern float eden_gamepad_look_sensitivity;
extern float eden_gamepad_deadzone;

namespace {

// ---------------------------------------------------------------------------------------------
// Bindings — eden-keybinds.js's DEFAULT_KEYMAP, transliterated to SDL scancodes
// ---------------------------------------------------------------------------------------------
// Scancodes, not keycodes: they are physical positions, so WASD stays WASD on an AZERTY keyboard,
// which is the same thing `KeyW`/`KeyA` mean in the browser's `event.code`. Using SDL_Keycode here
// would silently make the layout locale-dependent — a class of bug that only ever shows up on
// someone else's machine.
enum Action {
    A_FORWARD, A_BACK, A_LEFT, A_RIGHT, A_SPRINT, A_WALK,
    A_JUMP, A_FLYDOWN, A_CROUCH, A_COUNT
};

bool g_down[A_COUNT] = {false};

// The left stick's contribution, written by gamepad_tick() (far below, next to the rest of the
// pad code) and read by recompute_move() (just below). Declared HERE rather than there so the
// read site needs no forward declaration — an `extern` inside an unnamed namespace is legal but
// reads like two variables, and getting it subtly wrong is a silent "the stick does nothing".
float g_padForward = 0.0f;
float g_padStrafe = 0.0f;
float g_padSpeedMul = 1.0f;

// SDL_Scancode IS the USB HID usage ID, and Settings_web.mm's keybind table stores HID usage IDs
// so that one integer means the same physical key in the browser (`event.code`) and here. That
// identity is the whole reason a binding fits in the settings model's int-only persistence — so
// assert it at compile time on a spread of ranges rather than trust a sentence in a comment. An
// SDL that ever renumbered scancodes must break the build, not silently rebind every keyboard.
static_assert((int)SDL_SCANCODE_A == 4,        "SDL scancodes are no longer HID usage IDs");
static_assert((int)SDL_SCANCODE_W == 26,       "SDL scancodes are no longer HID usage IDs");
static_assert((int)SDL_SCANCODE_1 == 30,       "SDL scancodes are no longer HID usage IDs");
static_assert((int)SDL_SCANCODE_ESCAPE == 41,  "SDL scancodes are no longer HID usage IDs");
static_assert((int)SDL_SCANCODE_SPACE == 44,   "SDL scancodes are no longer HID usage IDs");
static_assert((int)SDL_SCANCODE_UP == 82,      "SDL scancodes are no longer HID usage IDs");
static_assert((int)SDL_SCANCODE_LCTRL == 224,  "SDL scancodes are no longer HID usage IDs");
static_assert((int)SDL_SCANCODE_RALT == 230,   "SDL scancodes are no longer HID usage IDs");

// ---------------------------------------------------------------------------------------------
// The model -> this file's dispatch, resolved once
// ---------------------------------------------------------------------------------------------
// Until Stage 5.2 this was a `switch (sc)` over hard-coded scancodes in two places (a continuous
// one here and a one-shot one in the KEY_DOWN handler), transliterated by hand from
// eden-keybinds.js. Both are gone: the bindings live in the shared table, and this file only says
// what each ACTION does. Resolution is BY NAME, never by assuming the table's row order — the
// table is append-friendly on purpose and its indexes are not API.
constexpr int kMaxModelActions = 64;
int  g_contAction[kMaxModelActions];   // model row -> Action, or -1
int  g_miHotbar[9];
int  g_miMenu = -1, g_miBlockPicker = -1, g_miColorPicker = -1, g_miFireTool = -1;
int  g_miFlyToggle = -1, g_miBlockPreview = -1;
bool g_modelResolved = false;

void resolve_model(void) {
    if (g_modelResolved) return;
    g_modelResolved = true;
    for (int i = 0; i < kMaxModelActions; ++i) g_contAction[i] = -1;
    // Continuous actions: a held STATE, tracked in g_down[] and read by recompute_move()/tick().
    static const struct { const char* action; int act; } kCont[] = {
        { "moveForward", A_FORWARD }, { "moveBack",  A_BACK  },
        { "moveLeft",    A_LEFT    }, { "moveRight", A_RIGHT },
        { "sprint",      A_SPRINT  }, { "walk",      A_WALK  },
        { "jump",        A_JUMP    }, { "flyDown",   A_FLYDOWN },
        { "crouch",      A_CROUCH  },
    };
    for (const auto& e : kCont) {
        const int i = eden_keybind_index(e.action);
        if (i >= 0 && i < kMaxModelActions) g_contAction[i] = e.act;
    }
    // One-shot actions: an EDGE, dispatched at the keydown site.
    g_miMenu         = eden_keybind_index("menu");
    g_miBlockPicker  = eden_keybind_index("blockPicker");
    g_miColorPicker  = eden_keybind_index("colorPicker");
    g_miFireTool     = eden_keybind_index("fireTool");
    g_miFlyToggle    = eden_keybind_index("flyToggle");
    g_miBlockPreview = eden_keybind_index("blockPreview");
    // Resolved one by one rather than as "hotbar1 + n": the table's rows happen to be contiguous
    // today and nothing promises they stay that way.
    for (int n = 0; n < 9; ++n) {
        char name[16];
        std::snprintf(name, sizeof(name), "hotbar%d", n + 1);
        g_miHotbar[n] = eden_keybind_index(name);
    }
}

// `mi` is a model row; -1 if it is not one of the continuous nine.
int continuous_action(int mi) {
    return (mi >= 0 && mi < kMaxModelActions) ? g_contAction[mi] : -1;
}

// ---------------------------------------------------------------------------------------------
// Mouse / pointer state
// ---------------------------------------------------------------------------------------------
bool g_relativeMouse = false;
bool g_haveWindow = false;
SDL_Window* g_window = nullptr;

// THE iOS DEVICE PASS'S "ONE TAP BREAKS A BLOCK IN TWO PLACES AT ONCE", and half of "the joystick
// is broken" (STATUS §2.0.12). SDL_HINT_TOUCH_MOUSE_EVENTS defaults to "1" on every platform, so
// **every finger also arrives a second time as a mouse** — down/motion/up with
// `which == SDL_TOUCH_MOUSEID`. On a desktop leg that is invisible (there are no fingers) and in
// --touch-selftest it is invisible too (SDL_PushEvent synthesises nothing), which is why four
// green CI jobs and a passing gate said nothing about it. On a device it means:
//   * a tap mines/builds where the finger landed (the touch path) AND at the crosshair (the mouse
//     path's hold-to-act, because while playing `g_relativeMouse` is true) — two edits, one tap;
//   * dragging the on-screen joystick also feeds eden_apply_look_delta, so the camera swings
//     while you are trying to walk.
// The hint is turned off in main() as well, but a hint is a request and this is the assertion: a
// mouse event that is really a finger has already been handled by the finger path, and the reverse
// (SDL_MOUSE_TOUCHID, a mouse pretending to be a finger) would double the desktop path the same
// way if SDL_HINT_MOUSE_TOUCH_EVENTS were ever switched on.
bool is_synthetic_mouse(SDL_MouseID which) { return which == SDL_TOUCH_MOUSEID; }
bool is_synthetic_finger(SDL_TouchID which) { return which == SDL_MOUSE_TOUCHID; }

// ---------------------------------------------------------------------------------------------
// Hold-to-mine/build — a transliteration of public/eden-input.js's holdAct* state machine
// ---------------------------------------------------------------------------------------------
// **Mining is a per-TAP action in this engine, not a per-hold one.** Input_web.mm's tap-mine/build
// fires on M_DOWN and then M_RELEASE, so one begin held down forever removes at most one block's
// worth of progress. The web host answers that with a repeating pulse — fire immediately on
// mousedown, then after a ~250 ms initial delay repeat on a ~200 ms period, **each repeat spending
// one frame in "begin" and the next in "end"** so the pair always straddles an engine tick
// (rule 2). A first native attempt held a single begin instead, and the symptom was exactly what
// you would expect if you did not know the above: the click registered (hud->mode became
// MODE_MINE) and the block simply never broke, on about half of runs.
//
// The numbers and the structure are eden-input.js's, not new ones — including honouring the
// `hold_to_act` setting, which when off makes a press exactly one shot.
constexpr double kHoldInitialDelayMs = 250.0;
constexpr double kHoldRepeatPeriodMs = 200.0;

struct HoldAction {
    bool active = false;
    bool isBuild = false;
    bool pendingEnd = false;
    bool firstFireDone = false;
    double nextFireAtMs = 0.0;
};
HoldAction g_hold;

// eden_set_jump is a synthetic touch held on the jump button's rect, so it has the same
// straddle-a-tick requirement.
int  g_jumpFrames = 0;

// The mouse identity the menu path uses. Negative on purpose: UITouch::isRealTouch answers
// "identity >= 0", which is how engine code (Hud.mm, Joystick.mm) tells a real touchscreen gesture
// from one this layer manufactures. Matches Input_web.mm's own MOUSE_IDENTITY convention.
constexpr int kMouseIdentity = -20;

// TOUCH (Phase N Stage 4.3, first half — the SDL translator). `eden_input_pointer_event` takes an
// `int` identity and SDL_FingerID is a Uint64 that is not small and not dense, so fingers get a
// slot rather than a cast: a cast can alias two live fingers onto one slot (and, on the mouse's
// value, onto the mouse), which the shared touch core would read as one pointer teleporting.
//
// Eight slots, because the engine's own touch table (Classes/Input.h) is finite and nobody plays
// a voxel game with nine fingers. A ninth finger is DROPPED rather than given a recycled slot —
// silently reusing one is how a stuck touch happens, and Input_web.mm's header is emphatic about
// that class of bug.
constexpr int kMaxFingers = 8;
SDL_FingerID g_fingerSlot[kMaxFingers] = {0};

int finger_slot(SDL_FingerID id, bool acquire) {
    for (int i = 0; i < kMaxFingers; ++i) if (g_fingerSlot[i] == id) return i;
    if (!acquire) return -1;
    for (int i = 0; i < kMaxFingers; ++i) {
        if (g_fingerSlot[i] == 0) { g_fingerSlot[i] = id; return i; }
    }
    return -1;
}

void finger_release(SDL_FingerID id) {
    for (int i = 0; i < kMaxFingers; ++i) if (g_fingerSlot[i] == id) { g_fingerSlot[i] = 0; return; }
}

// Window pixels -> the engine's POINT space. The browser does this with getBoundingClientRect();
// here the drawable size is the box. It must be recomputed per event rather than cached, because
// the point space is derived and moves with the window (web/CLAUDE.md's display-profile rule).
// Window coordinates -> the engine's point space, THROUGH THE LETTERBOX.
//
// Phase N Stage 4.3: this used to scale the window's full size onto the point space, which is only
// right when the rendered box IS the whole window. Since the box is now fitted to the engine's
// aspect (src/seam/DisplayMode_native.cpp), a touch has to be measured against the box — otherwise
// every coordinate is stretched by exactly the ratio the letterbox exists to remove, which is what
// an iPad Air 2 reported as "switches don't respond to touch at all".
//
// A point outside the box (a tap on a black bar) maps outside the point space, which is correct:
// the engine's hit tests will find nothing there, which is what is under the finger.
void window_to_point(float px, float py, float* ox, float* oy) {
    int bx = 0, by = 0, bw = 0, bh = 0;
    eden_native_gl_get_letterbox(&bx, &by, &bw, &bh);
    if (bw <= 0 || bh <= 0) { *ox = px; *oy = py; return; }

    // The box is in PIXELS and SDL reports the pointer in POINTS, so convert the box into points
    // with the window's own pixel/point ratio rather than assuming a scale factor.
    int ww = 0, wh = 0, pw = 0, ph = 0;
    if (g_window) {
        SDL_GetWindowSize(g_window, &ww, &wh);
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    }
    float sx = (pw > 0 && ww > 0) ? (float)pw / (float)ww : 1.0f;
    float sy = (ph > 0 && wh > 0) ? (float)ph / (float)wh : 1.0f;
    if (sx <= 0.0f) sx = 1.0f;
    if (sy <= 0.0f) sy = 1.0f;

    const float boxX = (float)bx / sx, boxY = (float)by / sy;
    const float boxW = (float)bw / sx, boxH = (float)bh / sy;
    if (boxW <= 0.0f || boxH <= 0.0f) { *ox = px; *oy = py; return; }

    *ox = (px - boxX) / boxW * SCREEN_WIDTH;
    *oy = (py - boxY) / boxH * SCREEN_HEIGHT;
}

void recompute_move() {
    float forward = (g_down[A_FORWARD] ? 1.0f : 0.0f) - (g_down[A_BACK] ? 1.0f : 0.0f);
    float strafe  = (g_down[A_RIGHT] ? 1.0f : 0.0f) - (g_down[A_LEFT] ? 1.0f : 0.0f);
    // 1.3 / 0.5 are eden-input.js's multipliers, not new numbers.
    float speedMul = g_down[A_SPRINT] ? 1.3f : (g_down[A_WALK] ? 0.5f : 1.0f);
    // Fold in the stick. Whichever source is asking for more wins per axis, rather than summing —
    // a summed pair would let "W plus full-forward stick" ask for 2.0, and eden_set_move_input's
    // axes are -1..1 by contract. This is the one place the two sources compose, which is the
    // same guarantee eden-st.html's recomputeMove() gives on web.
    if (std::fabs(g_padForward) > std::fabs(forward)) forward = g_padForward;
    if (std::fabs(g_padStrafe)  > std::fabs(strafe))  strafe  = g_padStrafe;
    if (g_padSpeedMul > speedMul) speedMul = g_padSpeedMul;
    eden_set_move_input(forward, strafe, speedMul);
}

// Pointer capture. `SDL_SetWindowRelativeMouseMode` is SDL3's spelling, and it incidentally
// resolves the A8 pointer-lock item the web port could never test — browser automation gets
// WrongDocumentError on requestPointerLock(), so only a real human click could ever exercise it
// there. Here it is just an API call.
//
// The predicate is the engine's own: eden_ui_wants_cursor() is true in the menu and in the block/
// colour pickers, false while playing. Driving capture from that (rather than from a key) means
// the cursor appears exactly when something is there to point at.
// NOTE THE SPLIT: `g_relativeMouse` is the LOGICAL mode ("is the mouse driving the camera?"), and
// the SDL capture call is the platform effect of it. They are separated so the logical half is
// still correct with no window at all — which is what makes --input-selftest possible headlessly,
// and is also just the more honest model.
void sync_mouse_capture() {
    // `eden_ui_wants_cursor()` covers the main menu and the block/colour pickers. It deliberately
    // does NOT cover the in-game (ESC) menu, because on web that menu is a DOM overlay
    // (public/eden-pausemenu.js) which releases pointer lock itself. Native's pause menu IS the
    // engine's own GL one — drawn inside the captured window — so without this second term the
    // player opens the menu and then cannot point at it. Found by reading the capture predicate
    // against the native UI rather than by playing, which is the kind of thing a headless
    // self-test cannot catch.
    const bool wantRelative = (eden_ui_wants_cursor() == 0) && (eden_hud_in_menu() == 0);
    if (wantRelative == g_relativeMouse) return;
    g_relativeMouse = wantRelative;
    if (g_haveWindow && g_window) SDL_SetWindowRelativeMouseMode(g_window, wantRelative);
}

// A one-shot HUD tap. Begin and end must straddle a tick (rule 2), so this posts the begin and
// leaves the end to the next tick — same shape as the click path.
int g_pendingHudTapEnd = -1;
int g_pendingHudTapWhich = 0;

void hud_tap(int which) {
    if (g_pendingHudTapEnd >= 0) return;   // one at a time; the engine consumes one per frame
    eden_tap_hud_button_begin(which);
    g_pendingHudTapWhich = which;
    g_pendingHudTapEnd = 2;                // >= 1 engine tick, with a frame of slack
}


// ---------------------------------------------------------------------------------------------
// Gamepad — the SDL twin of public/eden-gamepad.js
// ---------------------------------------------------------------------------------------------
// Every number and every binding below is that file's; see its header for why each is what it is
// (the 225 u/s look rate, the 2.0 response curve, the radial rescaling deadzone, the 0.5 trigger
// threshold, "only one trigger owns the hold-to-act slot"). What is NOT ported is its
// non-standard-mapping guard: the browser hands you a raw button array and refuses to guess, but
// SDL_Gamepad only reports pads it has a mapping for, so SDL_IsGamepad() IS that check.
//
// One real difference, and it is a simplification rather than a divergence: on web the stick axes
// are stashed and eden-st.html's own recomputeMove() folds them in, because that function runs
// unconditionally each frame and would otherwise stomp them. Here recompute_move() is in this
// file, so the fold happens directly in it.
SDL_Gamepad* g_pad = nullptr;
SDL_JoystickID g_padId = 0;

constexpr float kLookRate = 225.0f;      // eden_apply_look_delta units/second at full tilt
constexpr float kLookExpo = 2.0f;
constexpr double kMaxLookDtMs = 100.0;   // a long stall must not snap the camera around
constexpr float kTriggerThreshold = 0.5f;

double g_padLastLookMs = 0.0;
// Edge tracking. Indexed by SDL_GamepadButton, so it is dense and needs no map.
bool g_padPrev[SDL_GAMEPAD_BUTTON_COUNT] = {false};
int  g_padHeldTrigger = -1;              // SDL_GAMEPAD_AXIS_LEFT/RIGHT_TRIGGER, or -1

bool gamepad_on() { return g_pad != nullptr && eden_gamepad_enabled != 0.0f; }

float axis01(SDL_GamepadAxis a) {
    // SDL reports sticks as -32768..32767 and triggers as 0..32767.
    return (float)SDL_GetGamepadAxis(g_pad, a) / 32767.0f;
}

// Radial deadzone with rescaling, applied to a stick as a PAIR. Per-axis deadzones notch the
// diagonals; rescaling keeps full speed reachable at the deadzone edge. eden-gamepad.js's
// applyDeadzone, verbatim in behaviour.
void apply_deadzone(float x, float y, float dz, float* ox, float* oy, float* omag) {
    const float mag = std::sqrt(x * x + y * y);
    if (mag <= dz || mag <= 0.0f) { *ox = *oy = *omag = 0.0f; return; }
    float scaled = (mag - dz) / (1.0f - dz);
    if (scaled > 1.0f) scaled = 1.0f;
    const float k = scaled / mag;
    *ox = x * k;
    *oy = y * k;
    *omag = scaled;
}

bool trigger_pressed(SDL_GamepadAxis a) { return axis01(a) > kTriggerThreshold; }

// Release everything the pad is holding. Fires on disconnect and whenever the pad stops being
// the input source — leaving a stick latched behind a closed menu is the same stuck-input bug
// Input_web.mm's header warns about for touches.
void gamepad_release_all() {
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) g_padPrev[b] = false;
    if (g_padHeldTrigger >= 0) {
        if (g_hold.active) {
            if (g_hold.pendingEnd) eden_click_end(g_hold.isBuild ? 1 : 0);
            g_hold = HoldAction{};
        }
        g_padHeldTrigger = -1;
    }
    g_padForward = g_padStrafe = 0.0f;
    g_padSpeedMul = 1.0f;
    g_padLastLookMs = 0.0;
}

void gamepad_open(SDL_JoystickID id) {
    if (g_pad) return;                        // first pad wins, same as eden-gamepad.js
    SDL_Gamepad* p = SDL_OpenGamepad(id);
    if (!p) return;
    g_pad = p;
    g_padId = id;
    const char* name = SDL_GetGamepadName(p);
    std::fprintf(stderr, "[eden-input] gamepad connected: %s\n", name ? name : "?");
}

void gamepad_close(SDL_JoystickID id) {
    if (!g_pad || id != g_padId) return;
    gamepad_release_all();
    SDL_CloseGamepad(g_pad);
    g_pad = nullptr;
    g_padId = 0;
    std::fprintf(stderr, "[eden-input] gamepad disconnected\n");
}

// Edge-triggered button work. Returns nothing; the continuous ones (sprint, jump, crouch,
// flyDown) are folded into the per-frame state in gamepad_tick() instead, because that is what
// rule 3 wants for anything the engine re-derives each frame.
void gamepad_button_edges() {
    struct Bind { SDL_GamepadButton btn; int hudTap; };
    // The one-shots, mapped onto the SAME HUD button rects the keyboard uses (indices are
    // Input_web.mm's: 0 = menu, 1 = blocks, 2 = colours, 7 = fire) — so a pad press and a key
    // press are indistinguishable from the engine's side.
    static const Bind kTaps[] = {
        {SDL_GAMEPAD_BUTTON_WEST,  1},   // X -> block picker
        {SDL_GAMEPAD_BUTTON_START, 0},   // Start -> in-game menu
        {SDL_GAMEPAD_BUTTON_BACK,  0},   // Back/Select -> same; native has no separate settings UI
        {SDL_GAMEPAD_BUTTON_DPAD_UP,   7},   // fire tool
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 2},   // colour picker
    };
    for (const Bind& b : kTaps) {
        const bool down = SDL_GetGamepadButton(g_pad, b.btn);
        if (down && !g_padPrev[b.btn]) hud_tap(b.hudTap);
        g_padPrev[b.btn] = down;
    }

    // Y toggles fly, same as V on the keyboard.
    {
        const bool down = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_NORTH);
        if (down && !g_padPrev[SDL_GAMEPAD_BUTTON_NORTH])
            eden_set_fly_mode(eden_get_fly_mode() ? 0 : 1);
        g_padPrev[SDL_GAMEPAD_BUTTON_NORTH] = down;
    }

    // Hotbar: shoulders and d-pad left/right, both edges, both directions.
    struct Scroll { SDL_GamepadButton btn; int dir; };
    static const Scroll kScroll[] = {
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,  1},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  -1},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,      1},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT,      -1},
    };
    int scroll = 0;
    for (const Scroll& sc : kScroll) {
        const bool down = SDL_GetGamepadButton(g_pad, sc.btn);
        if (down && !g_padPrev[sc.btn]) scroll += sc.dir;
        g_padPrev[sc.btn] = down;
    }
    if (scroll) eden_hotbar_scroll(scroll > 0 ? 1 : -1);
}

void gamepad_tick() {
    if (!gamepad_on()) return;

    // The engine's own menus own input while they are up — the native counterpart of
    // eden-gamepad.js's `bridge.isBlocked()`, which asks the same question about DOM panels.
    // Movement and look are released; the buttons are NOT, because Start has to be able to close
    // the menu it opened.
    const bool uiOwnsInput = (eden_ui_wants_cursor() != 0) || (eden_hud_in_menu() != 0);

    const float dz = eden_gamepad_deadzone > 0.0f ? eden_gamepad_deadzone : 0.15f;

    if (uiOwnsInput) {
        g_padForward = g_padStrafe = 0.0f;
        g_padSpeedMul = 1.0f;
        g_padLastLookMs = 0.0;
        if (g_padHeldTrigger >= 0) {
            if (g_hold.active) {
                if (g_hold.pendingEnd) eden_click_end(g_hold.isBuild ? 1 : 0);
                g_hold = HoldAction{};
            }
            g_padHeldTrigger = -1;
        }
        gamepad_button_edges();
        return;
    }

    // --- left stick: move ---
    float sx = 0.0f, sy = 0.0f, smag = 0.0f;
    apply_deadzone(axis01(SDL_GAMEPAD_AXIS_LEFTX), axis01(SDL_GAMEPAD_AXIS_LEFTY),
                   dz, &sx, &sy, &smag);
    g_padStrafe  = sx;
    g_padForward = -sy;                       // stick Y is +down
    g_padSpeedMul = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_LEFT_STICK) ? 1.3f : 1.0f;

    // --- right stick: look, integrated against real elapsed time ---
    const double now = eden_platform_now_ms();
    double dtMs = (g_padLastLookMs > 0.0) ? (now - g_padLastLookMs) : 0.0;
    if (dtMs > kMaxLookDtMs) dtMs = kMaxLookDtMs;
    g_padLastLookMs = now;
    float rx = 0.0f, ry = 0.0f, rmag = 0.0f;
    apply_deadzone(axis01(SDL_GAMEPAD_AXIS_RIGHTX), axis01(SDL_GAMEPAD_AXIS_RIGHTY),
                   dz, &rx, &ry, &rmag);
    if (dtMs > 0.0 && rmag > 0.0f) {
        // Scale the MAGNITUDE by the response curve and keep the direction — powf on the pair
        // separately would bend diagonals.
        const float curve = std::pow(rmag, kLookExpo) / rmag;
        const float sens = eden_gamepad_look_sensitivity > 0.0f ? eden_gamepad_look_sensitivity : 1.0f;
        const float rate = kLookRate * sens * (float)(dtMs / 1000.0) * curve;
        eden_apply_look_delta(rx * rate, ry * rate);
    }

    // --- continuous buttons: fold into the same held-key state the keyboard writes ---
    // A/South is jump (and fly-up), B/East is crouch, R3 is fly-down, L3 is sprint (folded into
    // g_padSpeedMul above). Writing g_down[] rather than a parallel set is what makes a pad and a
    // keyboard compose instead of fight: recompute_move() and the thrust/crouch calls below read
    // one state, not two.
    const bool padJump = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_SOUTH);
    if (padJump != g_padPrev[SDL_GAMEPAD_BUTTON_SOUTH]) {
        g_padPrev[SDL_GAMEPAD_BUTTON_SOUTH] = padJump;
        g_down[A_JUMP] = padJump;
        // The same synthetic-touch pair the Space key posts, straddle rule and all (rule 2).
        if (padJump) { eden_set_jump(1); g_jumpFrames = 2; }
    }
    // NOTE THE EDGE, not a bare `if (down) set = true`. A latch that only ever sets leaves the
    // player permanently crouched after one B press, and it looks exactly like a physics bug. The
    // edge also means a key and a pad button can both hold the same action without one clearing
    // the other's hold — only the source that pressed it releases it.
    const bool padCrouch = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_EAST);
    if (padCrouch != g_padPrev[SDL_GAMEPAD_BUTTON_EAST]) {
        g_padPrev[SDL_GAMEPAD_BUTTON_EAST] = padCrouch;
        g_down[A_CROUCH] = padCrouch;
    }
    const bool padDown = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    if (padDown != g_padPrev[SDL_GAMEPAD_BUTTON_RIGHT_STICK]) {
        g_padPrev[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = padDown;
        g_down[A_FLYDOWN] = padDown;
    }

    gamepad_button_edges();

    // --- triggers -> mine/build, through the MOUSE's hold-to-act machine ---
    // Only one at a time, and the one that started it is the one that ends it: g_hold ignores a
    // second press while one is held, so without tracking the owner the wrong release would clear
    // it. eden-gamepad.js's heldTrigger, exactly.
    const bool rt = trigger_pressed(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    const bool lt = trigger_pressed(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    if (g_padHeldTrigger == (int)SDL_GAMEPAD_AXIS_RIGHT_TRIGGER && !rt) {
        if (g_hold.pendingEnd) eden_click_end(g_hold.isBuild ? 1 : 0);
        g_hold = HoldAction{};
        g_padHeldTrigger = -1;
    } else if (g_padHeldTrigger == (int)SDL_GAMEPAD_AXIS_LEFT_TRIGGER && !lt) {
        if (g_hold.pendingEnd) eden_click_end(g_hold.isBuild ? 1 : 0);
        g_hold = HoldAction{};
        g_padHeldTrigger = -1;
    }
    if (g_padHeldTrigger < 0 && !g_hold.active) {
        if (rt) {
            g_padHeldTrigger = (int)SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            g_hold = HoldAction{};
            g_hold.active = true;
            g_hold.isBuild = false;              // RT mines
        } else if (lt) {
            g_padHeldTrigger = (int)SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
            g_hold = HoldAction{};
            g_hold.active = true;
            g_hold.isBuild = true;               // LT builds
        }
    }
}

}  // namespace

// For --touch-selftest, and only for it: the mouse hold-to-act slot's state. A touch-synthesised
// mouse button claiming this slot IS the "one tap, two block edits" bug, and there is no other way
// to see it from outside without waiting for a terrain diff.
int eden_native_input_debug_hold_active(void) { return g_hold.active ? 1 : 0; }

void eden_native_input_init(SDL_Window* window) {
    g_window = window;
    g_haveWindow = (window != nullptr);

    // A finger must arrive ONCE. See is_synthetic_mouse() above for what SDL's default does
    // otherwise and what it cost on a device. Set here rather than in main() so it travels with
    // the translator that depends on it; the `which`-field guards are the real defence, this is
    // just not asking SDL to generate the events in the first place.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

    // GAMEPAD is its own SDL subsystem and is initialised HERE rather than in main's
    // SDL_InitSubSystem(EVENTS) block, because it is this file's dependency and nothing else's.
    // A failure is not fatal: no pad, keyboard and mouse unaffected.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "[eden-input] SDL_InitSubSystem(GAMEPAD) failed: %s\n", SDL_GetError());
        return;
    }
    // Pads already plugged in when we started do NOT generate an ADDED event on every platform,
    // so enumerate once as well as listening. (SDL3 does post ADDED for these on macOS, which is
    // why gamepad_open() is a no-op once one is held — belt and braces, cheap.)
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) gamepad_open(ids[i]);
        SDL_free(ids);
    }
}

void eden_native_input_handle_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN: {
            if (e.key.repeat) return;          // held keys are state, not repeated edges
            resolve_model();
            // ITERATE, do not take the first match. "One key, two actions" is shipped
            // configuration (Ctrl is flyDown AND crouch out of the box) and a rebind can create
            // more of them at any time, so the old `if (action == A_FLYDOWN) g_down[A_CROUCH]`
            // special case is replaced by simply honouring every row bound to this key.
            const int code = (int)e.key.scancode;
            for (int mi = eden_keybind_action_after(-1, code); mi >= 0;
                 mi = eden_keybind_action_after(mi, code)) {
                const int action = continuous_action(mi);
                if (action >= 0) {
                    g_down[action] = true;
                    if (action == A_JUMP) { eden_set_jump(1); g_jumpFrames = 2; }
                    continue;
                }
                // The one-shot actions. Tools and pickers go through the REAL HUD button rects
                // rather than reimplementing Hud.mm's mode toggles, which live inline in
                // Hud::update's touch loop — tapping the rect tracks any future layout or
                // behaviour change for free. Indices are Input_web.mm's: 0=menu, 1=blocks,
                // 2=colours, 7=fire/burn.
                if      (mi == g_miMenu)         hud_tap(0);
                else if (mi == g_miBlockPicker)  hud_tap(1);
                else if (mi == g_miColorPicker)  hud_tap(2);
                else if (mi == g_miFireTool)     hud_tap(7);
                else if (mi == g_miFlyToggle)    eden_set_fly_mode(eden_get_fly_mode() ? 0 : 1);
                else if (mi == g_miBlockPreview) eden_set_block_preview(eden_get_block_preview() ? 0 : 1);
                else {
                    for (int n = 0; n < 9; ++n)
                        if (mi == g_miHotbar[n]) { eden_select_hotbar_slot(n); break; }
                }
            }
            return;
        }
        case SDL_EVENT_KEY_UP: {
            resolve_model();
            const int code = (int)e.key.scancode;
            for (int mi = eden_keybind_action_after(-1, code); mi >= 0;
                 mi = eden_keybind_action_after(mi, code)) {
                const int action = continuous_action(mi);
                if (action < 0) continue;      // one-shot actions have no release edge
                g_down[action] = false;
                if (action == A_JUMP && g_jumpFrames <= 0) eden_set_jump(0);
            }
            return;
        }

        case SDL_EVENT_MOUSE_MOTION: {
            if (is_synthetic_mouse(e.motion.which)) return;
            if (g_relativeMouse) {
                // Rule 4: hand the raw relative delta over. Sensitivity and smoothing are inside
                // eden_apply_look_delta, shared with the gamepad's right stick.
                eden_apply_look_delta(e.motion.xrel, e.motion.yrel);
            } else {
                // Cursor mode: this is the MENU and the pickers, which are real touch UI. Forward
                // as a pointer MOVE so a drag (the world list scrolls by dragging) works.
                float x, y;
                window_to_point(e.motion.x, e.motion.y, &x, &y);
                eden_input_pointer_event(1, kMouseIdentity, x, y);
            }
            return;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (is_synthetic_mouse(e.button.which)) return;
            if (!g_relativeMouse) {
                float x, y;
                window_to_point(e.button.x, e.button.y, &x, &y);
                eden_input_pointer_event(0, kMouseIdentity, x, y);
                return;
            }
            // Playing: left mines, right builds, middle picks the block under the crosshair.
            if (e.button.button == SDL_BUTTON_MIDDLE) { eden_pick_block_at_crosshair(); return; }
            if (g_hold.active) return;               // a second button while one is held: ignore
            g_hold = HoldAction{};
            g_hold.active = true;
            g_hold.isBuild = (e.button.button == SDL_BUTTON_RIGHT);
            return;
        }

        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (is_synthetic_mouse(e.button.which)) return;
            if (!g_relativeMouse) {
                float x, y;
                window_to_point(e.button.x, e.button.y, &x, &y);
                eden_input_pointer_event(2, kMouseIdentity, x, y);
                return;
            }
            const bool isBuild = (e.button.button == SDL_BUTTON_RIGHT);
            if (!g_hold.active || isBuild != g_hold.isBuild) return;
            // eden-input.js's holdActStop: if a begin is still outstanding, close it, then drop
            // the action. Leaving an unmatched begin behind is a stuck touch — the same class of
            // bug as the stuck-jump one Input_web.mm's header warns about.
            if (g_hold.pendingEnd) eden_click_end(g_hold.isBuild ? 1 : 0);
            g_hold = HoldAction{};
            return;
        }

        // TOUCH. Four events, one line of real work each, because the touch HALF of this port's
        // input is already shared and already exercised: web/src/seam/Input_web.mm builds the
        // UITouch objects and hands them to Input::touchesBegan/Moved/Ended exactly as EAGLView.mm
        // did in 2010, and `public/eden-input.js` drives it from DOM touch events. This is the
        // same call from SDL. So the iOS control scheme is not new code — it is the original one,
        // reached from a different event source.
        //
        // SDL3 reports finger positions NORMALISED to the window (0..1), unlike the mouse's
        // window coordinates, so this multiplies by the engine's point space directly rather than
        // going through window_to_point().
        //
        // NOT GATED ON g_relativeMouse: that flag is the desktop's mouse-look capture, and a
        // touchscreen has no cursor to capture. Every finger goes to the shared core, which is
        // what makes the on-screen sticks, the look-drag and tap-to-mine work.
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            if (is_synthetic_finger(e.tfinger.touchID)) return;
            const bool down   = (e.type == SDL_EVENT_FINGER_DOWN);
            const bool ending = (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED);
            const int slot = finger_slot(e.tfinger.fingerID, down);
            if (slot < 0) return;                       // ninth finger, or a move with no begin
            const int phase = down ? 0 : (ending ? (e.type == SDL_EVENT_FINGER_UP ? 2 : 3) : 1);
            // SDL reports fingers NORMALISED to the window (0..1). Convert to window points and
            // then through window_to_point(), so touch and mouse share ONE mapping — including the
            // letterbox. Multiplying straight into the point space (which this did until the first
            // device pass) silently assumes the rendered box fills the window, and on a 4:3 screen
            // running a 16:9 layout it does not.
            // HEADLESS FALLBACK, and it is not a formality: --touch-selftest runs with no window
            // at all, so "the window" has to be something. The point space is the honest answer —
            // it makes a normalised finger land exactly where the same fraction of the engine's
            // own screen is, which is what the test asserts against. A `1x1` fallback would make
            // every synthetic tap land on the origin and the test would pass by being near enough
            // to a corner button, which is worse than failing.
            int ww = 0, wh = 0;
            if (g_window) SDL_GetWindowSize(g_window, &ww, &wh);
            if (ww <= 0 || wh <= 0) { ww = (int)SCREEN_WIDTH; wh = (int)SCREEN_HEIGHT; }
            float x = 0.0f, y = 0.0f;
            window_to_point(e.tfinger.x * (float)ww, e.tfinger.y * (float)wh, &x, &y);
            eden_input_pointer_event(phase, slot, x, y);
            // AFTER the event, not before: the core needs the slot to still resolve while it is
            // closing the touch out.
            if (ending) finger_release(e.tfinger.fingerID);
            return;
        }

        case SDL_EVENT_GAMEPAD_ADDED:
            gamepad_open(e.gdevice.which);
            return;
        case SDL_EVENT_GAMEPAD_REMOVED:
            gamepad_close(e.gdevice.which);
            return;

        case SDL_EVENT_MOUSE_WHEEL:
            // Only while playing: in the menu the wheel would fight the drag-scroll above.
            if (is_synthetic_mouse(e.wheel.which)) return;
            if (g_relativeMouse && e.wheel.y != 0) eden_hotbar_scroll(e.wheel.y > 0 ? 1 : -1);
            return;

        default:
            return;
    }
}

void eden_native_input_tick() {
    // Before recompute_move(), because that is what consumes the factor this updates.
    eden_movement_tick();
    // Before recompute_move() as well, and for the same reason eden-gamepad.js's tick() runs
    // before eden-st.html's: the stick axes it records are read by recompute_move() below, so a
    // pad moved this frame is acted on this frame rather than one late.
    gamepad_tick();
    // Rule 3: every frame, unconditionally.
    recompute_move();
    eden_set_fly_thrust(g_down[A_JUMP] ? 1 : 0, g_down[A_FLYDOWN] ? 1 : 0);
    eden_set_crouch(g_down[A_CROUCH] ? 1 : 0);

    // The deferred halves of the synthetic-touch pairs (rule 2).
    if (g_jumpFrames > 0 && --g_jumpFrames == 0 && !g_down[A_JUMP]) eden_set_jump(0);

    // eden-input.js's holdActTick, verbatim in structure.
    if (g_hold.active) {
        const double now = eden_platform_now_ms();
        if (g_hold.pendingEnd) {
            eden_click_end(g_hold.isBuild ? 1 : 0);
            g_hold.pendingEnd = false;
            const bool wasFirst = !g_hold.firstFireDone;
            g_hold.firstFireDone = true;
            g_hold.nextFireAtMs = eden_get_hold_to_act()
                ? now + (wasFirst ? kHoldInitialDelayMs : kHoldRepeatPeriodMs)
                : 1e300;   // hold-to-act off: one shot per press, exactly like a plain click
        } else if (!g_hold.firstFireDone || now >= g_hold.nextFireAtMs) {
            eden_click_begin(g_hold.isBuild ? 1 : 0);
            g_hold.pendingEnd = true;
        }
    }
    if (g_pendingHudTapEnd >= 0 && g_pendingHudTapEnd-- == 0) {
        eden_tap_hud_button_end(g_pendingHudTapWhich);
        g_pendingHudTapEnd = -1;
    }

    // Block preview + the tool-mode tracking the pickers need. Input_web.mm owns all of it; the
    // host's only job is to call this once a frame, which is what public/eden-input.js does too.
    eden_ui_tick();

    // Last, because it reads the mode eden_ui_tick() may just have changed.
    sync_mouse_capture();
}
