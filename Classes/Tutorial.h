// UX.10-3: the tutorial world's in-world hints (docs/ui.md "Tutorial hints").
//
// Active only in a world whose folder carries `tutorial.json` (TutorialInstall::Marker) and never in a multiplayer
// session. One persistent card above the hotbar, styled like GLW::Toast, shows the current step's one-line hint in
// the copy of the input mode in use (keyboard + mouse, touch, gamepad); a step completes when the player does what
// it says, a short "Done" flash follows, then the next step. The steps' predicates read state the engine already
// has (player position and yaw, Hud mode, the edit calls in MpWorld, Hotbar::select): there is no input path of
// its own. The hint copy is Classes/TutorialData.h, generated from port/tools/tutorial/copy.txt. Main thread only.
#ifndef EDEN_TUTORIAL_H
#define EDEN_TUTORIAL_H

#include <string>

namespace Tutorial {

enum InputMode { IM_KBM = 0, IM_TOUCH = 1, IM_PAD = 2 };
enum Event { EV_BREAK = 1, EV_PLACE = 2, EV_PAINT = 4, EV_HOTBAR = 8 };

extern float flashSeconds;      // how long the "Done" flash holds (0.8; the selftest shortens it)
extern bool testHintsByName;   // --tutorial-mutate=hintsbyname: a world is "the tutorial" by its name, not its marker

// World entry (FileManager::finishLoadOnMain). `dir` is the world folder (absolute), "" for a loose key or a
// multiplayer session; `key` is the world's engine key. Reads the marker; no marker = no card.
void worldLoaded(const std::string& dir, const std::string& key);
bool active();                       // this world has the marker, and the hints are not hidden for good

void update(float etime);            // Hud::update, every frame in play
void render();                       // Hud::render, after the status toast
// A tap/click at (x, y) (point space, y up) on the card's close box. tapClose() acts on it (the touch release).
bool closeHit(float x, float y);
void tapClose();
void askHideAll();                   // the "Hide all tutorial hints?" confirm (the pause-menu row, UX.10-5)
// The player did something the steps watch: an edit (MpWorld's intent calls) or a hotbar select.
void note(int event);

int  inputMode();                    // IM_*: the pad when it was the last device used, else touch/kbm by the input profile

// ---- introspection (the selftests)
int         stepCount();
const char* stepId(int i);
bool        stepDone(int i);
int         currentStep();           // the step the card shows (-1 none)
bool        cardShown();             // a card is up this frame (a hint, or the "Done" flash)
std::string cardText();              // what it says, with the live key labels filled in
bool        cardRect(CGRect* card, CGRect* close);   // point space, y up
std::string hintText(int step, int mode);            // the resolved copy of a step in a mode ("" if none)
bool        hidden();                // the player hid every hint

}  // namespace Tutorial

#endif
