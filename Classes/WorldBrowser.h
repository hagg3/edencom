//
//  WorldBrowser.h
//  Eden
//
//  ROADMAP 5.9: "Get Worlds" on the GL widget kit — the online world browser, with one tab per
//  SOURCE:
//
//    Archive         hagg3.github.io/edenarchive — the community's static catalogue (one JSON
//                    manifest, ~800 worlds, zipped downloads, HTTPS). The only source the web
//                    port's DOM browser (public/eden-worldbrowser.js) can reach.
//    Current server  app2/files2.edengame.net — the live official service the 2.x game talks to.
//    Legacy server   app/files.edengame.net — the older official service, still answering.
//
//  The two official servers speak the stock client's protocol (docs/networking.md; verified
//  against the live game's traffic in ~/eden-world-editor's DOCUMENTATION/10-features.md):
//    Featured  GET files*/popularlist.txt
//    Recent    GET app*/list2.php?start=N&sort=2       (paged by `start`; ~150 per page)
//    Search    GET app*/list2.php?search=<term>
//  each answering plain text, `<id>.eden` / `<name>.name` line pairs; a world is
//  GET files*/<id>.eden (gzip) and its preview files*/<id>.eden.png. All plain HTTP.
//
//  This replaces the stock SharedList/ShareUtil pair (seam-excluded on every port target: they
//  are NSURLConnection + UIKit) rather than reviving it. What carries over is the protocol and
//  the three stock modes (stock called them Featured / Recent / Search), not the code.
//
//  PLATFORM SPLIT. The engine only ever asks for a URL and polls (the eden_net_* seam below); the
//  HTTP stack is the host's — native/src/seam/Net_native.cpp over NSURLSession / libcurl /
//  WinHTTP. Unpacking (gzip, zip, the archive's zip-in-a-zip) is engine code over zlib, done a few
//  MB per frame so a 500 MB world never stalls the menu. Web's seam reports no network, so the GL
//  menu never offers this screen there; the DOM screen stays the web's browser until 5.10.
//
#ifndef Eden_WorldBrowser_h
#define Eden_WorldBrowser_h

#import "GLWidgets.h"

class WorldBrowser {
public:
    enum Source { SRC_ARCHIVE = 0, SRC_CURRENT = 1, SRC_LEGACY = 2, SRC_COUNT = 3 };
    enum Mode   { MODE_FEATURED = 0, MODE_RECENT = 1, MODE_SEARCH = 2 };   // the official servers'

    WorldBrowser();
    ~WorldBrowser();

    // Whether this host can fetch at all (eden_net_available). The menu offers the screen only
    // when it can.
    static bool available();

    void open();              // shows the screen on its last tab, fetching that tab's list if needed
    void close();
    bool isOpen() const;
    void update(float etime);
    void render();

    // --- harness surface (--ui-selftest / --shot): where things are, and what the model holds ---
    // controlRect: "back", "download", "more", "search", "go", "list", "scrollbar"; tabRect for a
    // source tab, modeRect for an official server's Featured / Recent tile. Point space, y up.
    CGRect controlRect(const char* which);
    CGRect tabRect(int source);
    CGRect modeRect(int mode);
    bool   rowRect(int index, CGRect* r);   // index into the visible (filtered) list
    void   showRow(int index);              // scrolls the least distance that puts `index` on screen
    int    source() const;
    int    mode() const;                    // of the current source; MODE_SEARCH while a search shows
    int    entryCount() const;              // the visible (filtered) list
    const char* entryName(int index) const;
    const char* entryId(int index) const;
    const char* entrySize(int index) const; // the archive's "3.5 MB"; "" when the source has none
    int    selectedIndex() const;
    bool   listLoading() const;             // the current source's list request is in flight
    bool   busy() const;                    // a download or unpack is in flight
    bool   previewShown() const;            // a preview texture is on screen for the selection
    const char* statusText() const;

private:
    WorldBrowser(const WorldBrowser&);
    WorldBrowser& operator=(const WorldBrowser&);
    struct BrowserImpl* d;
};

// The network seam (ROADMAP 5.9). Native: native/src/seam/Net_native.cpp. Web: stubs in
// web/src/seam/seam_link_stubs.mm reporting no network. A fetch never blocks: it starts a job and
// the caller polls it every frame.
extern "C" {
int  eden_net_available(void);
// `destPath` NULL keeps the body in memory (eden_net_body); otherwise it streams to `destPath`,
// which appears only when the whole 2xx body has arrived. Returns a job id, 0 if it cannot start.
int  eden_net_fetch(const char* url, const char* destPath);
// 0 running, 1 done, -1 failed. `total` is -1 when the server did not say.
int  eden_net_poll(int job, long long* got, long long* total);
const char* eden_net_error(int job);
const unsigned char* eden_net_body(int job, int* len);
// Cancels a running job and forgets it; the id is dead afterwards. Safe on 0 and on a dead id.
void eden_net_release(int job);
// Stage S / S.5c: POST the file at `bodyPath` with `contentType`; the reply lands in memory
// (eden_net_body). Native only; web's stub answers 0 (no network).
int  eden_net_post_file(const char* url, const char* contentType, const char* bodyPath);
}

#endif
