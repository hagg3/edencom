#include "main_web.h"
#include "EdenAppDelegate_web.h"

namespace eden_web {

static EdenAppDelegate* g_appDelegate = nullptr; // process-lifetime, mirrors the original's
                                                   // UIWindow-owned singleton app delegate.

// Defined in EAGLView_web.mm. Must run before the World exists — see that file for why.
extern "C" void eden_seam_create_eagl_view(void);

void eden_seam_main() {
    if (g_appDelegate) return; // idempotent — TODO P1: revisit if hot-reload during dev needs
                                // a real teardown/reconstruct path.
    // Publishes G_EAGL_VIEW and the screen-metric globals (SCREEN_WIDTH/SCREEN_HEIGHT/
    // IS_WIDESCREEN/P_ASPECT_RATIO). ORDER IS LOAD-BEARING: World::World() reads them.
    eden_seam_create_eagl_view();
    g_appDelegate = new EdenAppDelegate();
    g_appDelegate->didFinishLaunching();
}

EdenAppDelegate* eden_seam_get_app_delegate() {
    return g_appDelegate;
}

} // namespace eden_web
