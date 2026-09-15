#include "main_native.h"
#include "EdenAppDelegate_native.h"

namespace eden_native {

static EdenAppDelegate* g_appDelegate = nullptr;  // process-lifetime, as on iOS and on web

// Defined in SDLView_native.mm. Must run before the World exists — see that file for why.
extern "C" void eden_seam_create_eagl_view(void);

void eden_seam_main() {
    if (g_appDelegate) return;
    // Publishes G_EAGL_VIEW and the screen-metric globals. ORDER IS LOAD-BEARING: World::World()
    // reads SCREEN_WIDTH/SCREEN_HEIGHT/P_ASPECT_RATIO.
    eden_seam_create_eagl_view();
    g_appDelegate = new EdenAppDelegate();
    g_appDelegate->didFinishLaunching();
}

EdenAppDelegate* eden_seam_get_app_delegate() {
    return g_appDelegate;
}

}  // namespace eden_native
