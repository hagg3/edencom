//
//  DebugMenu.cpp
//  Eden -- the Debug menu (DebugMenu.h). Every page is a GLDialog; every button acts, then re-shows its page.
//
#include "DebugMenu.h"
#include "World.h"
#include "Hud.h"
#include "Player.h"
#include "Terrain.h"
#include "FileManager.h"
#include "GLDialog.h"
#include "Console.h"
#include "MpWorld.h"
#include "Renderer.h"
#include "port/src/shim/foundation/eden_fs.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

bool DEBUG_MENU_ALLOWED = false;

bool photoEncodePNG(const unsigned char* rgb, int w, int h, std::vector<unsigned char>& out);   // Util.cpp

// Host seam (port/src/seam/WindowState_native.cpp); no-ops (0) where there is no window manager.
extern "C" int  eden_window_set_pixel_size(int w, int h);
extern "C" int  eden_clipboard_set(const char* text);
extern "C" int  eden_reveal_folder(const char* path);
extern "C" void eden_console_menu_open(void);   // port/src/seam/Menu.cpp

namespace DebugMenu {

enum { PAGE_MAIN, PAGE_SHOT, PAGE_TIME, PAGE_PLAYER, PAGE_VIEW };

static std::string s_note;        // the last action's result, shown in the page's body
static std::string s_lastShot;
static float s_shotDelay = -1;    // seconds until the pending shot; < 0 = none
static int   s_shotFrames = 0;    // frames to wait after the delay (the dialog and pause scrim gone)
static bool  s_capture = false;   // the take_screenshot in flight is ours

static void showPage(int page);

static World* w() { return World::getWorld; }
static bool inPlay() { return w() && w()->game_mode == GAME_MODE_PLAY && w()->player && w()->hud; }

bool shown() { return DEBUG_MENU_ALLOWED && !MpWorld::active(); }
const std::string& lastShot() { return s_lastShot; }

// A console line, with the last line it printed as the note (errors included: "Needs Gameplay > Weather").
static void run(const char* line) {
    Console::init();
    const unsigned before = Console::scrollbackSerial();
    Console::run(line);
    s_note.clear();
    if (Console::scrollbackSerial() != before && !Console::scrollback().empty())
        s_note = Console::scrollback().back().text;
}
static std::string cvar(const char* name) {
    Console::init();
    return Console::cvarValue(Console::cvarIndex(name));
}
static void setCvar(const char* name, const char* value) {
    Console::init();
    std::string err = Console::cvarSet(Console::cvarIndex(name), value, NULL);
    s_note = err.empty() ? std::string(name) + " = " + cvar(name) : err;
}

static std::string poseText() {
    Player* p = w()->player;
    char b[160];
    std::snprintf(b, sizeof b, "--at=%.2f,%.2f,%.2f --look=%.1f,%.1f", p->pos.x, p->pos.y, p->pos.z, p->yaw, p->pitch);
    return b;
}

static std::string body(int page) {
    char b[512];
    Hud* h = w()->hud;
    switch (page) {
    case PAGE_MAIN: {
        Player* p = w()->player;
        std::snprintf(b, sizeof b, "%s  |  %d fps  |  x %.1f  y %.1f  z %.1f  |  yaw %.0f  pitch %.0f",
                      cvar("world.name").c_str(), h->fps, p->pos.x, p->pos.y, p->pos.z, p->yaw, p->pitch);
        break;
    }
    case PAGE_SHOT: {
        int fw = 0, fh = 0;
        R::drawableSize(fw, fh);
        std::snprintf(b, sizeof b, "Full-size PNG, no HUD, in Documents/Screenshots. Frame %dx%d px.", fw, fh);
        break;
    }
    case PAGE_TIME:
        std::snprintf(b, sizeof b, "Time %s (0.5 is noon)  |  weather %s", cvar("world.time").c_str(), cvar("weather.now").c_str());
        break;
    default: b[0] = 0; break;
    }
    std::string s = b;
    if (!s_note.empty()) s += (s.empty() ? "" : "  |  ") + s_note;   // a body Label is one wrapped paragraph (no \n)
    return s;
}

// ---- the screenshot -------------------------------------------------------------------------------------
static void queueShot(float delay) {
    s_shotDelay = delay;
    s_shotFrames = 3;
    s_note.clear();
}

void tick(float etime) {
    if (s_shotDelay < 0 || !inPlay()) return;
    if (GLDialog::active() || w()->hud->inmenu) return;   // reopened something: wait until it closes
    if (s_shotDelay > 0) { s_shotDelay -= etime; if (s_shotDelay > 0) return; s_shotDelay = 0; }
    if (s_shotFrames-- > 0) return;
    s_shotDelay = -1;
    s_capture = true;
    w()->hud->mode = MODE_CAMERA;          // Hud::render's camera branch draws no HUD and takes the shot
    w()->hud->take_screenshot = true;
}

bool wantsCapture() { return s_capture; }

void captureNow() {
    s_capture = false;
    int fw = 0, fh = 0;
    R::drawableSize(fw, fh);
    if (fw <= 0 || fh <= 0) return;
    std::vector<unsigned char> px((size_t)fw * fh * 4);
    R::readPixels(0, 0, fw, fh, px.data());
    std::vector<unsigned char> rgb((size_t)fw * fh * 3);
    for (int y = 0; y < fh; y++) {                 // GL rows run bottom-up
        const unsigned char* s = &px[(size_t)(fh - 1 - y) * fw * 4];
        unsigned char* d = &rgb[(size_t)y * fw * 3];
        for (int x = 0; x < fw; x++) { d[x*3] = s[x*4]; d[x*3+1] = s[x*4+1]; d[x*3+2] = s[x*4+2]; }
    }
    std::vector<unsigned char> png;
    if (!photoEncodePNG(rgb.data(), fw, fh, png)) return;
    const std::string dir = w()->fm->documents + "/Screenshots";
    if (!eden_fs_make_dirs(dir)) { if (w()->hud->sb) w()->hud->sb->setStatus("Screenshot: cannot make the folder", 3); return; }
    char stamp[32];
    const time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
    std::string path = dir + "/emod-" + stamp + ".png";
    for (int n = 2; eden_fs_exists(path); n++) path = dir + "/emod-" + stamp + "-" + std::to_string(n) + ".png";
    if (!eden_fs_write_atomic(path, png.data(), png.size())) return;
    s_lastShot = path;
    const std::string pose = poseText();
    printg("[debug] screenshot %s (%dx%d) %s\n", path.c_str(), fw, fh, pose.c_str());
    Console::init();
    Console::print(CmdScript::Sink::OUT, "Screenshot " + path + " " + pose);
    if (w()->hud->sb) w()->hud->sb->setStatus("Saved Screenshots/" + path.substr(dir.size() + 1), 3);
}

// ---- the pages --------------------------------------------------------------------------------------------
static int s_page = PAGE_MAIN;

static void onMain(int j) {
    s_note.clear();
    switch (j) {
    case 0: showPage(PAGE_SHOT); return;
    case 1: showPage(PAGE_TIME); return;
    case 2: showPage(PAGE_PLAYER); return;
    case 3: showPage(PAGE_VIEW); return;
    case 4: {
        const std::string p = poseText();
        const bool clip = eden_clipboard_set(p.c_str()) != 0;
        Console::init();
        Console::print(CmdScript::Sink::OUT, "Pose " + p);
        printg("[debug] pose %s\n", p.c_str());
        s_note = p + (clip ? "  (copied)" : "  (in the console)");
        showPage(PAGE_MAIN);
        return;
    }
    case 5: eden_console_menu_open(); return;
    default: return;                       // Close
    }
}

static void onShot(int j) {
    switch (j) {
    case 0: queueShot(0); return;          // the dialog closes; tick() shoots a few frames later
    case 1: queueShot(5); if (w()->hud->sb) w()->hud->sb->setStatus("Screenshot in 5 s", 2); return;
    case 2: case 3: {
        const int ok = j == 2 ? eden_window_set_pixel_size(1280, 720) : eden_window_set_pixel_size(1920, 1080);
        s_note = ok ? "Window resized." : "This device has no resizable window (or it is fullscreen).";
        break;
    }
    case 4: {
        const std::string dir = w()->fm->documents + "/Screenshots";
        eden_fs_make_dirs(dir);
        s_note = eden_reveal_folder(dir.c_str()) ? "Opened " + dir : dir;
        break;
    }
    default: s_note.clear(); showPage(PAGE_MAIN); return;
    }
    showPage(PAGE_SHOT);
}

static void onTime(int j) {
    static const char* kWeather[] = { "Clear", "Overcast", "Rain", "Storm", "Snow" };
    switch (j) {
    case 0: run("time 0.27"); break;
    case 1: run("noon"); break;
    case 2: run("time 0.73"); break;
    case 3: run("midnight"); break;
    case 4: run(cvar("world.locktime") == "on" ? "timefreeze off" : "timefreeze on"); break;
    case 5: {
        const std::string now = cvar("weather.now");
        int k = 0;
        for (int i = 0; i < 5; i++) if (now == kWeather[i]) k = (i + 1) % 5;
        setCvar("weather.now", kWeather[k]);
        break;
    }
    default: s_note.clear(); showPage(PAGE_MAIN); return;
    }
    showPage(PAGE_TIME);
}

static void onPlayer(int j) {
    switch (j) {
    case 0: {                              // Off -> Fly -> Noclip, whatever Allow fly says
        const int s = (fly_get_state() + 1) % 3;
        fly_set_state(s);
        s_note = s == 1 ? "Fly on" : s == 2 ? "Noclip on" : "Fly off";
        break;
    }
    case 1: run("heal"); break;
    case 2: run("gold 100"); break;
    case 3: run("save"); if (s_note.empty()) s_note = "Saved."; break;
    default: s_note.clear(); showPage(PAGE_MAIN); return;
    }
    showPage(PAGE_PLAYER);
}

static void onView(int j) {
    switch (j) {
    case 0: w()->hud->hideui = !w()->hud->hideui; s_note = w()->hud->hideui ? "HUD hidden (F1 also toggles)." : "HUD shown."; break;
    case 1: run("toggle video.camera_view"); break;
    case 2: {
        static const char* kFov[] = { "60", "70", "80", "95", "110" };
        const std::string now = cvar("video.fov");
        int k = 0;   // the next preset above the current value, wrapping to the first
        for (int i = 4; i >= 0; i--) if (std::atof(now.c_str()) < std::atof(kFov[i]) - 0.5) k = i;
        setCvar("video.fov", kFov[k]);
        break;
    }
    case 3: run("toggle atmo.clouds"); break;
    default: s_note.clear(); showPage(PAGE_MAIN); return;
    }
    showPage(PAGE_VIEW);
}

static void showPage(int page) {
    if (!inPlay()) return;
    s_page = page;
    static std::string label[8];
    const char* b[8];
    int n = 0;
    void (*cb)(int) = onMain;
    const char* title = "Debug";
    switch (page) {
    case PAGE_MAIN:
        label[0] = "Screenshot"; label[1] = "Time & weather"; label[2] = "Player"; label[3] = "View";
        label[4] = "Copy pose"; label[5] = "Console"; label[6] = "Close"; n = 7;
        break;
    case PAGE_SHOT:
        title = "Screenshot"; cb = onShot;
        label[0] = "Shot now"; label[1] = "Shot in 5 s"; label[2] = "1280x720 px"; label[3] = "1920x1080 px";
        label[4] = "Open folder"; label[5] = "Back"; n = 6;
        break;
    case PAGE_TIME:
        title = "Time & weather"; cb = onTime;
        label[0] = "Dawn"; label[1] = "Noon"; label[2] = "Dusk"; label[3] = "Night";
        label[4] = cvar("world.locktime") == "on" ? "Unfreeze time" : "Freeze time";
        label[5] = "Next weather"; label[6] = "Back"; n = 7;
        break;
    case PAGE_PLAYER: {
        title = "Player"; cb = onPlayer;
        const int s = fly_get_state();
        label[0] = s == 0 ? "Fly: off" : s == 1 ? "Fly: on" : "Noclip: on";
        label[1] = "Heal"; label[2] = "Gold +100"; label[3] = "Save world"; label[4] = "Back"; n = 5;
        break;
    }
    case PAGE_VIEW:
        title = "View"; cb = onView;
        label[0] = w()->hud->hideui ? "Show HUD" : "Hide HUD";
        label[1] = "Camera: " + cvar("video.camera_view");
        label[2] = "FOV: " + cvar("video.fov");
        label[3] = "Clouds: " + cvar("atmo.clouds");
        label[4] = "Back"; n = 5;
        break;
    }
    for (int i = 0; i < n; i++) b[i] = label[i].c_str();
    const std::string text = body(page);
    GLDialog::show(title, text.empty() ? NULL : text.c_str(), b, n, cb);
}

void open() {
    s_note.clear();
    showPage(PAGE_MAIN);
}

}  // namespace DebugMenu
