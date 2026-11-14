//
//  DebugMenu.h
//  Eden -- the Debug menu: a paged GLDialog opened from the pause menu's Debug button, which exists only
//  while Settings > Experiments > Debug menu is on (off by default; never in a multiplayer session).
//
//  It gathers the jobs the agent-led development did through harness flags and console lines into one
//  place a player can reach in a running world (docs/ui.md "Debug menu"):
//    Screenshot   a full-size PNG of the frame without the HUD (the camera mode's path, at full drawable
//                 size, no 3:2 crop) in <Documents>/Screenshots, now or after 5 s; desktop window presets
//                 sized in device pixels (1280x720, 1920x1080) so shots match the site's frames; open the folder.
//    Time/weather dawn / noon / dusk / night, freeze the clock, step the weather (console verbs).
//    Player       fly / noclip (regardless of Settings > Gameplay > Allow fly), heal, gold, save.
//    View         HUD on/off, camera view, field of view, clouds.
//    Copy pose    the player's position and look as `--at=X,Y,Z --look=YAW,PITCH`, to the clipboard and the
//                 console: the harness shot modes take those flags, so a frame found by hand can be re-shot
//                 deterministically.
//  Each page is one GLDialog::show(); a button acts and re-shows the page (with the result in its body), so
//  the world stays frozen under it like any dialog. Escape is the last button (Back / Close).
//
#ifndef Eden_DebugMenu_h
#define Eden_DebugMenu_h

#include <string>

extern bool DEBUG_MENU_ALLOWED;   // Settings > Experiments > Debug menu (port/src/seam/Settings.cpp)

namespace DebugMenu {

bool shown();                 // the pause menu offers the Debug button (allowed, singleplayer)
void open();                  // the main page (the pause menu has closed itself)
void tick(float etime);       // World::update, once a frame in play: the delayed screenshot
// Hud::render's camera branch: true when the pending take_screenshot is the Debug menu's, which
// captureNow() then writes instead of the world thumbnail.
bool wantsCapture();
void captureNow();
const std::string& lastShot(); // the last screenshot's path ("" = none yet this session)

}  // namespace DebugMenu

#endif
