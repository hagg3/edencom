// UX.10-3: see Tutorial.h.
#include "Tutorial.h"
#include "TutorialData.h"
#include "TutorialInstall.h"
#include "FileManager.h"
#include "GLDialog.h"
#include "GLWidgets.h"
#include "Hotbar.h"
#include "Hud.h"
#include "MpWorld.h"
#include "Player.h"
#include "World.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
int eden_effective_input_is_touch(void);   // port/src/seam/Settings.cpp
int eden_profile_touch_chrome(void);       // port/src/seam/DisplayProfile.cpp
int eden_ui_wants_cursor(void);            // port/src/seam/Input.cpp: a dialog, the Console, a picker
int eden_settings_menu_open(void);         // port/src/seam/Settings.cpp
int eden_input_pad_is_last(void);          // port/src/seam/Input_native.cpp
int eden_keybind_index(const char* action);
int eden_keybind_get(int i);
const char* eden_keybind_code_label(int code);
}

namespace Tutorial {

float flashSeconds = 0.8f;
bool testHintsByName = false;

namespace {

enum Pred { P_LOOK, P_MOVE, P_JUMP, P_BREAK, P_PLACE, P_PICKER, P_PAINT, P_HOTBAR, P_MENU };
const int kAny = 7;   // bit per InputMode

// One hint. `modes`: the input modes it is for (the copy of each is in TutorialData.h under `id.mode`; a mode a step
// does not apply to is skipped). `station`: the sign it waits for (kTutorialStations key), NULL = from the first frame.
struct Step { const char* id; int modes; const char* station; Pred pred; };
const Step kSteps[] = {
    {"look",   kAny, NULL,     P_LOOK},
    {"move",   kAny, NULL,     P_MOVE},
    {"jump",   kAny, "walk",   P_JUMP},
    {"break",  kAny, "build",  P_BREAK},
    {"place",  kAny, "build",  P_PLACE},
    {"picker", kAny, "blocks", P_PICKER},
    {"paint",  kAny, "blocks", P_PAINT},
    {"hotbar", kAny, "hotbar", P_HOTBAR},
    {"menu",   kAny, "finish", P_MENU},
};
const int kNSteps = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
const float kNearIn = 6.0f, kNearStay = 9.0f;   // blocks: a station's hint appears within 6 and holds to 9
const float kLookDegrees = 45.0f, kMoveBlocks = 3.0f;

struct State {
    bool active = false, hiddenAll = false, ready = false;
    std::string dir;
    bool done[kNSteps] = {}, skipped[kNSteps] = {};
    int cur = -1;                       // the step the card is on
    int flashStep = -1; float flashT = 0;
    int closes = 0;                     // opt-out presses this session
    // per-step baselines, reset whenever the current step changes
    float baseX = 0, baseZ = 0, yawAcc = 0, prevYaw = 0; bool prevJumping = false; int evt = 0;
    // the card
    GLW::Label label, xLabel;
    std::string builtText; float builtW = 0, builtU = 0, builtXU = 0;
    std::string text;                   // what the card shows now ("" = no card this frame)
    bool flashing = false;
    CGRect card = {{0, 0}, {0, 0}}, close = {{0, 0}, {0, 0}};
    bool shown = false;
};
State& S = *new State;   // never destroyed: its Labels hold textures, and static destruction runs after the renderer is gone

bool playing() {
    World* w = World::getWorld;
    return w && w->game_mode == GAME_MODE_PLAY && w->player && w->hud && w->terrain && !MpWorld::active();
}

int stepIndex(const char* id) {
    for (int i = 0; i < kNSteps; i++) if (!std::strcmp(kSteps[i].id, id)) return i;
    return -1;
}

void persist() {
    if (S.dir.empty()) return;
    TutorialInstall::Marker m;
    for (int i = 0; i < kNSteps; i++) if (S.done[i]) m.done.push_back(kSteps[i].id);
    m.hidden = S.hiddenAll;
    TutorialInstall::writeMarker(S.dir, m);
}

const char* hintCopy(const char* id, int mode) {
    static const char* const kMode[3] = {"kbm", "touch", "pad"};
    const std::string k = std::string(id) + "." + kMode[mode];
    for (const TutorialHintText& h : kTutorialHints) if (k == h.key) return h.text;
    return NULL;
}

const TutorialStation* stationOf(const char* key) {
    for (const TutorialStation& s : kTutorialStations) if (!std::strcmp(s.key, key)) return &s;
    return NULL;
}

bool nearStation(const char* key, float radius) {
    const TutorialStation* s = stationOf(key);
    if (!s) return false;
    const Player* p = World::getWorld->player;
    const float dx = p->pos.x - (s->x + 0.5f), dz = p->pos.z - (s->z + 0.5f);
    return dx * dx + dz * dz <= radius * radius && std::fabs(p->pos.y - s->y) <= 8.0f;
}

float wrapDegrees(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

void resetBaseline() {
    const Player* p = World::getWorld->player;
    S.baseX = p->pos.x; S.baseZ = p->pos.z;
    S.yawAcc = 0; S.prevYaw = p->yaw; S.prevJumping = p->jumping; S.evt = 0;
}

bool satisfied(const Step& st) {
    const Player* p = World::getWorld->player;
    const Hud* h = World::getWorld->hud;
    switch (st.pred) {
    case P_LOOK:   return S.yawAcc >= kLookDegrees;
    case P_MOVE: {
        const float dx = p->pos.x - S.baseX, dz = p->pos.z - S.baseZ;
        return dx * dx + dz * dz >= kMoveBlocks * kMoveBlocks;
    }
    case P_JUMP:   return p->jumping && !S.prevJumping;   // the frame the jump starts
    case P_BREAK:  return (S.evt & EV_BREAK) != 0;
    case P_PLACE:  return (S.evt & EV_PLACE) != 0;
    case P_PAINT:  return (S.evt & EV_PAINT) != 0;
    case P_HOTBAR: return (S.evt & EV_HOTBAR) != 0;
    case P_PICKER: return h->mode == MODE_PICK_BLOCK;
    case P_MENU:   return h->inmenu;
    }
    return false;
}

// The first step still to do for this input mode whose station (if any) the player is at. The step the card is on
// keeps its place a little further out, so standing on the edge of the radius does not flicker it.
int pickStep(int mode) {
    for (int i = 0; i < kNSteps; i++) {
        const Step& st = kSteps[i];
        if (S.done[i] || S.skipped[i] || !(st.modes & (1 << mode))) continue;
        if (st.station && !nearStation(st.station, i == S.cur ? kNearStay : kNearIn)) continue;
        return i;
    }
    return -1;
}

// `{action}` -> the key bound to it right now (Settings > Keys).
std::string resolve(const char* t) {
    std::string out;
    for (const char* c = t; *c; c++) {
        if (*c != '{') { out += *c; continue; }
        const char* e = std::strchr(c, '}');
        if (!e) { out += c; break; }
        const std::string name(c + 1, e);
        const int idx = eden_keybind_index(name.c_str());
        const char* lab = idx >= 0 ? eden_keybind_code_label(eden_keybind_get(idx)) : "";
        out += (lab && *lab) ? lab : "?";
        c = e;
    }
    return out;
}

void onHideAll(int chosen) {
    if (chosen != 0) return;   // Keep
    S.hiddenAll = true;
    S.cur = -1; S.flashT = 0; S.text.clear(); S.shown = false;
    persist();
}

// Lays the card out for this frame: above the status toast lane, between the touch chrome. False = it does not fit.
bool layout(const std::string& text, bool flash) {
    using namespace GLW;
    Hud* hud = World::getWorld->hud;
    const CGRect safe = safeRect();
    float lo = safe.origin.x + du(8), hi = safe.origin.x + safe.size.width - du(8);
    const float gap = du(8);
    const float y = safe.origin.y + du(16) + Toast::lift() + du(40) + du(6);
    if (eden_profile_touch_chrome()) {
        CGRect r[16];
        const int n = hud->touchChromeRects(r, 16);
        const float yTop = y + du(64);   // a card is at most two lines
        const float mid = safe.origin.x + safe.size.width * 0.5f;
        for (int i = 0; i < n; i++) {
            if (r[i].origin.y >= yTop + gap || r[i].origin.y + r[i].size.height <= y - gap) continue;
            if (r[i].origin.x + r[i].size.width * 0.5f < mid) lo = std::max(lo, (float)(r[i].origin.x + r[i].size.width) + gap);
            else hi = std::min(hi, (float)r[i].origin.x - gap);
        }
    }
    const float closeW = flash ? 0.0f : std::max(du(30), touchFloor());
    const float padX = du(12);
    const float maxText = hi - lo - 2 * padX - closeW;
    if (maxText < du(100)) return false;
    if (text != S.builtText || std::fabs(maxText - S.builtW) > 0.5f || u() != S.builtU) {
        S.label.set(text.c_str(), du(20), UITextAlignmentCenter, maxText, FACE_DISPLAY);
        S.builtText = text; S.builtW = maxText; S.builtU = u();
    }
    if (!flash && (S.xLabel.empty() || u() != S.builtXU)) {
        S.xLabel.set("X", du(18), UITextAlignmentCenter, 0, FACE_DISPLAY);
        S.builtXU = u();
    }
    const float w = std::min(S.label.width(), maxText) + 2 * padX + closeW;
    const float h = std::max(S.label.height() + 2 * du(4), flash ? 0.0f : touchFloor());
    S.card = CGRectMake(lo + ((hi - lo) - w) * 0.5f, y, w, h);
    S.close = CGRectMake(S.card.origin.x + w - closeW, y, closeW, h);
    return true;
}

}  // namespace

void worldLoaded(const std::string& dir, const std::string& key) {
    S.active = S.hiddenAll = S.ready = false;
    S.dir.clear(); S.cur = -1; S.flashStep = -1; S.flashT = 0; S.closes = 0;
    S.text.clear(); S.shown = false; S.builtText.clear();
    for (int i = 0; i < kNSteps; i++) S.done[i] = S.skipped[i] = false;
    if (dir.empty()) return;   // a loose world or a multiplayer session
    TutorialInstall::Marker m;
    const bool marked = TutorialInstall::readMarker(dir, &m);
    bool byName = false;
    if (!marked && testHintsByName) {   // the mutant: "the tutorial" is the world called "Tutorial world"
        std::vector<FileManager::WorldEntry> e;
        World::getWorld->fm->listWorlds(&e);
        for (const auto& w : e) byName = byName || (w.key == key && w.name == "Tutorial world");
    }
    if (!marked && !byName) return;
    S.active = true;
    S.dir = dir;
    S.hiddenAll = m.hidden;
    for (const std::string& id : m.done) { const int i = stepIndex(id.c_str()); if (i >= 0) S.done[i] = true; }
}

bool active() { return S.active && !S.hiddenAll; }
bool hidden() { return S.hiddenAll; }

int inputMode() {
    if (eden_input_pad_is_last()) return IM_PAD;
    return eden_effective_input_is_touch() ? IM_TOUCH : IM_KBM;
}

void note(int event) { if (S.active) S.evt |= event; }

void update(float etime) {
    S.shown = false;
    if (!S.active || S.hiddenAll || !playing()) return;
    Player* p = World::getWorld->player;
    Hud* hud = World::getWorld->hud;
    if (!S.ready) { S.ready = true; resetBaseline(); }
    S.yawAcc += std::fabs(wrapDegrees(p->yaw - S.prevYaw));
    S.prevYaw = p->yaw;

    const bool blocked = hud->overlayBlocked() || eden_ui_wants_cursor() || eden_settings_menu_open();
    const int mode = inputMode();

    if (S.flashT <= 0) {
        const int next = pickStep(mode);
        if (next != S.cur) { S.cur = next; resetBaseline(); }
        if (S.cur >= 0 && satisfied(kSteps[S.cur])) {   // done, whether or not the card could be seen
            S.done[S.cur] = true;
            persist();
            S.flashStep = S.cur; S.flashT = flashSeconds;
            S.cur = -1;
        }
    }
    S.prevJumping = p->jumping;

    if (blocked) return;   // it waits, it does not queue
    const bool flash = S.flashT > 0;
    if (flash) S.flashT -= etime;
    std::string text;
    if (flash) text = "Done";
    else if (S.cur >= 0) {
        const char* c = hintCopy(kSteps[S.cur].id, mode);
        if (c) text = resolve(c);
    }
    if (text.empty() || !layout(text, flash)) return;
    S.text = text; S.flashing = flash; S.shown = true;
}

void render() {
    using namespace GLW;
    if (!S.shown || S.label.empty() || !playing() || World::getWorld->hud->overlayBlocked()) return;
    const CGRect r = S.card;
    const Color edge = S.flashing ? rgb(0xc6f175) : rgb(0x646464);
    const float t = 2 * u();
    fill(r, rgb(0x2c2b2b, 0.85f));
    fill(r.origin.x, r.origin.y, r.size.width, t, edge);
    fill(r.origin.x, r.origin.y + r.size.height - t, r.size.width, t, edge);
    fill(r.origin.x, r.origin.y, t, r.size.height, edge);
    fill(r.origin.x + r.size.width - t, r.origin.y, t, r.size.height, edge);
    const float textW = r.size.width - (S.flashing ? 0.0f : S.close.size.width);
    S.label.drawCentered(r.origin.x + textW * 0.5f, r.origin.y + (r.size.height + S.label.height()) * 0.5f, S.flashing ? rgb(0xc6f175) : kWhite);
    if (!S.flashing) {
        fill(S.close.origin.x, r.origin.y + t, u(), r.size.height - 2 * t, rgb(0x646464));
        S.xLabel.drawCentered(S.close.origin.x + S.close.size.width * 0.5f,
                              r.origin.y + (r.size.height + S.xLabel.height()) * 0.5f, kTextHint);
    }
}

bool closeHit(float x, float y) {
    if (!S.shown || S.flashing || !playing() || World::getWorld->hud->overlayBlocked()) return false;
    const CGRect& c = S.close;
    return x >= c.origin.x && x < c.origin.x + c.size.width && y >= c.origin.y && y < c.origin.y + c.size.height;
}

// The first press closes the hint on the card for the rest of the session; a later one asks about all of them.
void tapClose() {
    if (!S.shown || S.flashing || S.cur < 0) return;
    S.closes++;
    if (S.closes == 1) {
        S.skipped[S.cur] = true;
        S.cur = -1; S.shown = false;
        return;
    }
    askHideAll();
}

void askHideAll() {
    static const char* const kButtons[2] = {"Hide", "Keep"};
    GLDialog::show("Hide all tutorial hints?", "You can get them back with Restore tutorial world in Settings.", kButtons, 2, onHideAll);
}

int stepCount() { return kNSteps; }
const char* stepId(int i) { return (i >= 0 && i < kNSteps) ? kSteps[i].id : ""; }
bool stepDone(int i) { return i >= 0 && i < kNSteps && S.done[i]; }
int currentStep() { return S.shown && !S.flashing ? S.cur : -1; }
bool cardShown() { return S.shown; }
std::string cardText() { return S.shown ? S.text : std::string(); }
bool cardRect(CGRect* card, CGRect* close) {
    if (!S.shown) return false;
    if (card) *card = S.card;
    if (close) *close = S.close;
    return true;
}
std::string hintText(int step, int mode) {
    if (step < 0 || step >= kNSteps || mode < 0 || mode > 2) return "";
    const char* c = hintCopy(kSteps[step].id, mode);
    return c ? resolve(c) : "";
}

}  // namespace Tutorial
