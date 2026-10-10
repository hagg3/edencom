//
//  WorldShare.h
//  Eden — Stage S / S.5, S.5b, S.5c: the world list's "Share" action, on the GL widget kit.
//
//  Share > Export          a format picker (own format first; Legacy64z / NewDawn256z / NewFormat256z),
//                          the signs question when pruning would drop any, then the pre-flight's loss
//                          report as a confirm, then a time-sliced export to Documents/Exports/
//                          <name>.eden.gz (the default export everywhere, plan §4).
//  Share > Upload          Current server (same format picker) or Legacy server (Legacy64z, forced);
//                          the same confirm; the multipart body is built a slice at a time into a temp
//                          beside the world, POSTed through eden_net_post_file, and deleted.
//  Share > Remove original only for an `.emod` with its original `.eden` kept (the list's badge);
//                          deletes that file after a confirm.
//
//  The engine side is Classes/EdenWorldExport.{h,cpp}; this file is only the dialog chain and the
//  per-frame stepping. One action at a time. Native only: web's Storage tab is its export UI, and
//  web has no upload (the servers send no CORS headers).
//
#ifndef Eden_WorldShare_h
#define Eden_WorldShare_h

#import "Util.h"

namespace WorldShare {
    // Whether the menu offers the Share button at all (native; a world selected; nothing running).
    bool offered(WorldNode* node);
    void begin(WorldNode* node);
    // Advances a running export/upload a time slice; called every menu frame.
    void update();
    bool busy();
    // --ui-selftest: the last finished action's outcome ("" while running / never ran).
    const char* lastResult();
    // --ui-selftest: skip the dialogs (target, signs) and start an export or an upload directly.
    bool startExport(WorldNode* node, int target, int signs);
    bool startUpload(WorldNode* node, int server, int target, int signs);
}

#endif
