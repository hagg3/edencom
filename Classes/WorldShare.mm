//
//  WorldShare.mm — see WorldShare.h.
//
#import "WorldShare.h"
#import "GLDialog.h"
#import "World.h"
#import "Menu.h"
#import "FileManager.h"
#import "WorldBrowser.h"                 // eden_net_* (the upload's POST)
#include "EdenWorldExport.h"

#include <cstdio>
#include <random>
#include <string>
#include <sys/stat.h>
#if defined(_WIN32)
#  include <direct.h>
#endif

namespace {

enum Stage { S_IDLE, S_EXPORT, S_BODY, S_POST };

struct ShareState {
    Stage stage;
    std::string src, display, png, out, body, result;
    bool upload;
    int server, target, signs, netJob;
    emod::ExportFileJob job;
    ShareState() : stage(S_IDLE), upload(false), server(0), target(0), signs(0), netJob(0) {}
};
ShareState& st() { static ShareState s; return s; }

const char* const kTargets[] = {"Eden (own format)", "Legacy64z", "NewDawn256z", "NewFormat256z", "Cancel"};

void status(const std::string& s, float secs) {
    if (World::getWorld && World::getWorld->menu)
        World::getWorld->menu->sbar->setStatus([NSString stringWithUTF8String:s.c_str()], secs);
}

std::string docs() { return cpstring(World::getWorld->fm->documents); }

void make_dir(const std::string& d) {
#if defined(_WIN32)
    _mkdir(d.c_str());
#else
    mkdir(d.c_str(), 0755);
#endif
}

bool exists(const std::string& p) { struct stat sb; return stat(p.c_str(), &sb) == 0; }

std::string safe_name(const std::string& n) {
    std::string o;
    for (unsigned char c : n) o += (c < 32 || std::strchr("/\\:*?\"<>|", c)) ? '_' : (char)c;
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    return o.empty() ? std::string("world") : o;
}

std::string mb(uint64_t b) {
    char t[32];
    if (b >= (1ull << 30)) std::snprintf(t, sizeof t, "%.2f GB", b / 1073741824.0);
    else std::snprintf(t, sizeof t, "%.1f MB", b / 1048576.0);
    return t;
}

std::string uuid() {
    std::random_device rd;
    std::mt19937_64 g(((uint64_t)rd() << 32) ^ rd());
    uint64_t a = g(), b = g();
    char t[40];
    std::snprintf(t, sizeof t, "%08X-%04X-4%03X-%04X-%012llX", (unsigned)(a >> 32), (unsigned)(a >> 16) & 0xffff,
                  (unsigned)a & 0xfff, 0x8000u | ((unsigned)(b >> 48) & 0x3fff), (unsigned long long)(b & 0xffffffffffffull));
    return t;
}

void finish(const std::string& msg, bool ok) {
    ShareState& s = st();
    s.job.abort();
    if (s.netJob) { eden_net_release(s.netJob); s.netJob = 0; }
    if (!s.body.empty()) { std::remove(s.body.c_str()); s.body.clear(); }
    s.stage = S_IDLE;
    s.result = (ok ? "ok: " : "failed: ") + msg;
    status(msg, ok ? 6 : 8);
}

bool start() {
    ShareState& s = st();
    emod::ExportOptions o;
    o.target = s.target;
    o.signs = s.signs;
    std::string err;
    if (s.upload) {
        s.body = s.src + ".upload-body";
        if (!emod::begin_upload_body(s.job, s.server, s.src, s.png, o, s.body, &err)) { s.body.clear(); finish("Upload: " + err, false); return false; }
        s.stage = S_BODY;
        status("Preparing upload...", 9999);
        return true;
    }
    const std::string dir = docs() + "/Exports";
    make_dir(dir);
    const std::string stem = safe_name(s.display);
    for (int k = 1; k < 1000; k++) {
        s.out = dir + "/" + stem + (k == 1 ? std::string() : "-" + std::to_string(k)) + ".eden.gz";
        if (!exists(s.out)) break;
    }
    if (!s.job.begin(s.src, s.out, o, true)) { finish("Export: " + s.job.error(), false); return false; }
    s.stage = S_EXPORT;
    status("Exporting...", 9999);
    return true;
}

// ---- the dialog chain -------------------------------------------------------------------------
void confirm_cb(int chosen) { if (chosen == 0) start(); }

void confirm() {
    ShareState& s = st();
    emod::ExportOptions o;
    o.target = s.upload && s.server == emod::UPLOAD_LEGACY ? emod::X_LEGACY64Z : s.target;
    o.signs = s.signs;
    emod::EdenExporter ex;                 // the pre-flight: exact size + every loss, before a byte is written
    if (!ex.begin(s.src, o)) { finish(std::string(s.upload ? "Upload: " : "Export: ") + ex.error(), false); return; }
    const emod::ExportReport& r = ex.report();
    std::string body = std::string(emod::export_target_name(o.target)) + ", " + mb(r.bytes) + " before compression. " + r.summary();
    if (s.upload && s.server == emod::UPLOAD_LEGACY) body = "The Legacy server takes Legacy64z worlds only. " + body;
    static const char* const kGo[] = {"Export", "Cancel"};
    static const char* const kUp[] = {"Upload", "Cancel"};
    static std::string title;
    title = s.upload ? std::string("Upload to the ") + (s.server == emod::UPLOAD_LEGACY ? "Legacy" : "Current") + " server?"
                     : "Export this world?";
    GLDialog::show(title.c_str(), body.c_str(), s.upload ? kUp : kGo, 2, confirm_cb);
}

void signs_cb(int chosen) {
    if (chosen == 2) return;
    st().signs = chosen == 1 ? emod::SIGNS_PRUNE : emod::SIGNS_KEEP;
    confirm();
}

// Asked only when the target keeps the trailer and pruning would actually drop something.
void signs() {
    ShareState& s = st();
    s.signs = emod::SIGNS_KEEP;
    if (s.target == emod::X_OWN || s.target == emod::X_NEWFORMAT256Z) {
        emod::ExportOptions o;
        o.target = s.target;
        o.signs = emod::SIGNS_PRUNE;
        emod::EdenExporter ex;
        if (ex.begin(s.src, o)) {
            const emod::ExportReport& r = ex.report();
            const uint32_t drop = (r.signsIn - r.signsOut) + (r.cmdsIn - r.cmdsOut);
            if (drop) {
                static std::string body;
                char t[200];
                std::snprintf(t, sizeof t, "%u of the %u signs and command blocks lie outside this world's columns "
                              "(a world copied from a server carries the whole server's).", drop, r.signsIn + r.cmdsIn);
                body = t;
                static const char* const kB[] = {"Keep all", "Prune outside", "Cancel"};
                GLDialog::show("Signs", body.c_str(), kB, 3, signs_cb);
                return;
            }
        }
    }
    confirm();
}

void target_cb(int chosen) {
    if (chosen < 0 || chosen > 3) return;
    st().target = chosen;
    signs();
}

void pick_target() {
    GLDialog::show(st().upload ? "Upload format" : "Export format",
                   st().upload ? "The world's own format is the default." : "Saved as a .eden.gz in the Exports folder.",
                   kTargets, 5, target_cb);
}

void server_cb(int chosen) {
    ShareState& s = st();
    if (chosen == 2) return;
    s.server = chosen == 1 ? emod::UPLOAD_LEGACY : emod::UPLOAD_CURRENT;
    if (s.server == emod::UPLOAD_LEGACY) { s.target = emod::X_LEGACY64Z; s.signs = emod::SIGNS_KEEP; confirm(); }
    else pick_target();
}

void remove_cb(int chosen) {
    if (chosen != 0) return;
    Menu* m = World::getWorld->menu;
    WorldNode* n = m->selected_world;
    if (n && World::getWorld->fm->removeOriginal(n->file_name)) {
        n->original_bytes = 0;
        status("Original removed", 4);
    } else {
        status("Couldn't remove the original", 4);
    }
}

void share_cb(int chosen) {
    ShareState& s = st();
    WorldNode* n = World::getWorld->menu->selected_world;
    if (!n) return;
    if (chosen == 0) { s.upload = false; pick_target(); return; }
    if (chosen == 1) {
        s.upload = true;
        if (!exists(s.png)) { status("Take a picture of this world in camera mode first: an upload needs a preview", 6); return; }
        static const char* const kS[] = {"Current server", "Legacy server", "Cancel"};
        GLDialog::show("Upload to", "The Legacy server is for very old devices: it takes 64-block-tall worlds only.", kS, 3, server_cb);
        return;
    }
    if (chosen == 2 && n->original_bytes > 0) {
        static std::string body;
        body = "Delete the original .eden (" + mb((uint64_t)n->original_bytes) + ") kept beside this world? The converted world stays.";
        static const char* const kR[] = {"Delete", "Cancel"};
        GLDialog::show("Remove original", body.c_str(), kR, 2, remove_cb);
    }
}

}  // namespace

namespace WorldShare {

bool offered(WorldNode* node) {
#if defined(__EMSCRIPTEN__)
    (void)node;
    return false;
#else
    return node && !node->needs_convert && st().stage == S_IDLE;
#endif
}

bool busy() { return st().stage != S_IDLE; }
const char* lastResult() { return st().result.c_str(); }

static void prime(WorldNode* node) {
    ShareState& s = st();
    s.src = docs() + "/" + cpstring(node->file_name);
    s.display = cpstring(node->display_name);
    s.png = s.src + ".png";
    s.result.clear();
}

void begin(WorldNode* node) {
    if (!offered(node)) return;
    prime(node);
    static const char* const kA[] = {"Export", "Upload", "Remove original", "Cancel"};
    static const char* const kB[] = {"Export", "Upload", "Cancel"};
    const bool orig = node->original_bytes > 0;
    GLDialog::show("Share world", orig ? "Remove original deletes the .eden this world was converted from." : NULL,
                   orig ? kA : kB, orig ? 4 : 3, share_cb);
}

bool startExport(WorldNode* node, int target, int signs) {
    if (!offered(node)) return false;
    prime(node);
    ShareState& s = st();
    s.upload = false; s.target = target; s.signs = signs;
    return start();
}

bool startUpload(WorldNode* node, int server, int target, int signs) {
    if (!offered(node)) return false;
    prime(node);
    ShareState& s = st();
    s.upload = true; s.server = server; s.target = target; s.signs = signs;
    return start();
}

void update() {
    ShareState& s = st();
    if (s.stage == S_EXPORT || s.stage == S_BODY) {
        const int r = s.job.step(12);
        if (r == emod::ExportFileJob::FAILED) { finish(std::string(s.upload ? "Upload: " : "Export: ") + s.job.error(), false); return; }
        if (r == emod::ExportFileJob::RUNNING) {
            char t[64];
            std::snprintf(t, sizeof t, "%s %d%%", s.upload ? "Preparing upload..." : "Exporting...", s.job.percent());
            status(t, 9999);
            return;
        }
        if (s.stage == S_EXPORT) {
            const size_t sl = s.out.find_last_of('/');
            finish("Exported to Exports/" + s.out.substr(sl + 1) + " (" + mb(s.job.written()) + ")", true);
            return;
        }
        s.job.abort();
        s.netJob = eden_net_post_file(emod::upload_url(s.server, uuid()).c_str(), emod::upload_content_type().c_str(), s.body.c_str());
        if (!s.netJob) { finish("Upload: no network on this device", false); return; }
        s.stage = S_POST;
        status("Uploading...", 9999);
        return;
    }
    if (s.stage == S_POST) {
        long long got = 0, total = 0;
        const int r = eden_net_poll(s.netJob, &got, &total);
        if (r == 0) return;
        if (r < 0) { finish(std::string("Upload failed: ") + eden_net_error(s.netJob), false); return; }
        int len = 0;
        const unsigned char* b = eden_net_body(s.netJob, &len);
        const std::string reply(b ? (const char*)b : "", (size_t)(len > 0 ? len : 0));
        if (reply.compare(0, 3, "YES") == 0) finish("Uploaded", true);
        else finish("Upload refused by the server" + (reply.empty() ? std::string() : ": " + reply.substr(0, 60)), false);
    }
}

}  // namespace WorldShare
