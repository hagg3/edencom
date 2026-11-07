//
//  ModsMenu.cpp
//  Emod — K.3-5/K.3-6. See ModsMenu.h. The shape is WorldBrowser's: wb-style layout() run every frame
//  (pure rect arithmetic from SCREEN_* plus one-time rasters), claim-at-down / act-at-release touches,
//  a row-snapped ScrollView, and a harness surface (controlRect / rowRect / entry*).
//
#include "ModsMenu.h"
#include "ContentPack.h"
#include "PackInstall.h"
#include "WorldBrowser.h"   // eden_net_*
#include "GLDialog.h"
#include "Graphics.h"
#include "Globals.h"
#include "Util.h"
#include "World.h"
#include "Menu.h"
#include "FileManager.h"
#include "eden_fs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;
extern "C" {
const char* eden_mods_catalogue_url(void);   // port/src/seam/Settings.cpp
int  eden_pick_zip_available(void);          // port/src/seam/Input_native.cpp
void eden_pick_zip_begin(void);
const char* eden_pick_zip_take(void);
int  eden_pick_zip_busy(void);
}

#define MM_MAX_ROWS 24
#define MM_MAX_LINES 24

namespace {
const char* const kTabNames[2] = { "Installed", "Get Mods" };
const char* const kFooter = "Changes apply when a world opens";

std::string docs() { return World::getWorld->fm->documents; }
std::string partialPath() { return docs() + "/Packs/.download.partial"; }

const char* stateWord(ContentPack::PackState s) {
    switch (s) {
    case ContentPack::PACK_LOADED:   return "On";
    case ContentPack::PACK_DISABLED: return "Off";
    case ContentPack::PACK_REFUSED:  return "Refused";
    default:                         return "Skipped";   // PACK_NEEDS / PACK_CONFLICT
    }
}

std::string sizeText(long long n) {
    if (n < 0) return std::string();
    char b[32];
    if (n >= (1 << 20)) std::snprintf(b, sizeof(b), "%.1f MB", n / 1048576.0);
    else std::snprintf(b, sizeof(b), "%lld KB", (n + 1023) / 1024);
    return b;
}

std::string joinNeeds(const std::vector<ContentPack::ModRequire>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ", ";
        s += v[i].id;
        if (!v[i].minVersion.empty()) s += ">=" + v[i].minVersion;
    }
    return s;
}

// The name shown for a row: the pack's name, else its id, else its folder.
std::string packTitle(const ContentPack::ModPack& p) {
    if (!p.name.empty()) return p.name;
    if (!p.id.empty()) return p.id;
    return p.folder;
}
}  // namespace

struct ModsImpl;
static ModsImpl* s_removeOwner = NULL;
static std::string s_removeFolder, s_removeName;

struct ModsImpl {
    bool open;
    int  tab;
    std::vector<ContentPack::ModPack> packs;
    int  sel[2], first[2];

    // Get Mods
    std::vector<PackInstall::CatalogueEntry> cat;
    bool catLoaded;
    int  catJob;
    std::string catStatus;
    int  dlJob;
    PackInstall::CatalogueEntry dlEntry;
    float dlFrac;
    std::string status;                       // the outcome line (footer)
    std::vector<std::string> installLines;    // the last failed install's validator lines
    int  pickWait;                            // a file dialog is open

    GLW::Label  title, statusL, empty, emptyHint;
    GLW::Label  lines[MM_MAX_LINES];
    GLW::Color  lineColor[MM_MAX_LINES];
    int         nLines;
    GLW::Button back, toggle, up, down, remove, file, install, refresh;
    GLW::TabRail tabs;
    GLW::ListRow rows[MM_MAX_ROWS];
    std::string rowText[MM_MAX_ROWS];
    bool rowBuilt[MM_MAX_ROWS];
    GLW::ScrollView scroll;
    CGRect panel, list, detail;
    float pitch, btnH, titleY;
    bool built, showTitle, tabsOn, shownTabsOn;
    std::string shownStatus, shownToggle, shownInstall, detailKey, shownEmpty;
    int gen;                                  // bumps on every rescan / catalogue change (the detail's cache key)
    float clock;
    int listTouch, barTouch, downRow, tabTouch;

    ModsImpl() : open(false), tab(0), catLoaded(false), catJob(0), dlJob(0), dlFrac(0), pickWait(0), nLines(0),
                 pitch(30), btnH(30), titleY(0), built(false), showTitle(true), tabsOn(false), shownTabsOn(false),
                 gen(0), clock(0), listTouch(-1), barTouch(-1), downRow(-1), tabTouch(-1) {
        sel[0] = sel[1] = -1;
        first[0] = first[1] = 0;
        panel = list = detail = CGRectMake(0, 0, 0, 0);
        for (int r = 0; r < MM_MAX_ROWS; r++) rowBuilt[r] = false;
        for (int i = 0; i < MM_MAX_LINES; i++) lineColor[i] = GLW::kText;
    }

    bool catalogueOn() const { const char* u = eden_mods_catalogue_url(); return u && *u; }
    int  count() const { return tab == ModsMenu::TAB_INSTALLED ? (int)packs.size() : (int)cat.size(); }
    int& cur() { return sel[tab]; }

    void rescan() {
        std::vector<std::string> loaderLines;
        packs = ContentPack::scanPacks(docs(), &loaderLines);
        gen++;
        if (sel[0] >= (int)packs.size()) sel[0] = (int)packs.size() - 1;
        if (sel[0] < 0 && !packs.empty()) sel[0] = 0;
        for (int r = 0; r < MM_MAX_ROWS; r++) rowBuilt[r] = false;
    }
    // Re-selects the pack that was selected (by folder) after a move or a toggle.
    void rescanKeeping(const std::string& folder) {
        rescan();
        for (size_t i = 0; i < packs.size(); i++)
            if (packs[i].folder == folder) { sel[0] = (int)i; break; }
    }
    const ContentPack::ModPack* pack() const {
        const int s = sel[0];
        return (s >= 0 && s < (int)packs.size()) ? &packs[(size_t)s] : NULL;
    }
    const PackInstall::CatalogueEntry* entry() const {
        const int s = sel[1];
        return (s >= 0 && s < (int)cat.size()) ? &cat[(size_t)s] : NULL;
    }
    // Position among the Packs/ packs (the movable ones), -1 for a fixed row.
    int movablePos(int* total) const {
        int pos = -1, n = 0;
        for (size_t i = 0; i < packs.size(); i++) {
            if (packs[i].fromExtraDir) continue;
            if ((int)i == sel[0]) pos = n;
            n++;
        }
        if (total) *total = n;
        return pos;
    }
    bool canToggle() const {
        const ContentPack::ModPack* p = pack();
        return tab == 0 && p && !p->fromExtraDir && !p->id.empty() && p->state != ContentPack::PACK_REFUSED;
    }
    bool canMove(int delta) const {
        const ContentPack::ModPack* p = pack();
        if (tab != 0 || !p || p->fromExtraDir || p->id.empty()) return false;
        int total = 0;
        const int pos = movablePos(&total);
        return pos >= 0 && pos + delta >= 0 && pos + delta < total;
    }
    bool canRemove() const {
        const ContentPack::ModPack* p = pack();
        return tab == 0 && p && !p->fromExtraDir && !p->folder.empty();
    }
    const char* installed(const std::string& id) const {
        for (size_t i = 0; i < packs.size(); i++)
            if (!packs[i].fromExtraDir && packs[i].id == id) return packs[i].version.c_str();
        return "";
    }

    // --- Get Mods ---------------------------------------------------------------------------
    void startCatalogue() {
        if (catJob) { eden_net_release(catJob); catJob = 0; }
        const char* url = eden_mods_catalogue_url();
        if (!url || !*url) return;
        catStatus.clear();
        catJob = eden_net_fetch(url, NULL);
        if (!catJob) { catStatus = "Couldn't start the request"; catLoaded = true; }
    }
    void setCatalogue(const char* data, size_t len) {
        std::vector<PackInstall::CatalogueEntry> out;
        int dropped = 0;
        std::string err;
        cat.clear();
        sel[1] = -1;
        if (PackInstall::parseCatalogue(data, len, &out, &dropped, &err)) {
            cat = out;
            catStatus.clear();
            if (!cat.empty()) sel[1] = 0;
        } else {
            catStatus = "Couldn't read the catalogue: " + err;
        }
        catLoaded = true;
        gen++;
        for (int r = 0; r < MM_MAX_ROWS; r++) rowBuilt[r] = false;
    }
    void pollCatalogue() {
        if (!catJob) return;
        long long got = 0, total = -1;
        const int st = eden_net_poll(catJob, &got, &total);
        if (st == 0) return;
        if (st > 0) {
            int n = 0;
            const unsigned char* b = eden_net_body(catJob, &n);
            std::string copy(b ? (const char*)b : "", b ? (size_t)n : 0);
            eden_net_release(catJob);
            catJob = 0;
            setCatalogue(copy.data(), copy.size());
        } else {
            const char* e = eden_net_error(catJob);
            catStatus = std::string("Couldn't load the catalogue") + (e && *e ? std::string(": ") + e : std::string());
            eden_net_release(catJob);
            catJob = 0;
            catLoaded = true;
            gen++;
        }
    }
    void startInstall() {
        const PackInstall::CatalogueEntry* e = entry();
        if (!e || dlJob) return;
        installLines.clear();
        if (!eden_fs_make_dirs(docs() + "/Packs")) { status = "Cannot make the Packs folder"; return; }
        eden_fs_remove(partialPath());
        dlEntry = *e;
        dlFrac = -1.0f;
        dlJob = eden_net_fetch(e->url.c_str(), partialPath().c_str());
        status = dlJob ? "Downloading..." : "Couldn't start the download";
        gen++;
    }
    void cancelDownload(const char* why) {
        if (dlJob) eden_net_release(dlJob);
        dlJob = 0;
        eden_fs_remove(partialPath());
        if (why) status = why;
        gen++;
    }
    void pollDownload() {
        if (!dlJob) return;
        long long got = 0, total = -1;
        const int st = eden_net_poll(dlJob, &got, &total);
        if (st == 0) {
            dlFrac = total > 0 ? (float)((double)got / (double)total) : -1.0f;
            return;
        }
        if (st < 0) {
            const char* e = eden_net_error(dlJob);
            status = "Download failed" + (e && *e ? std::string(": ") + e : std::string());
            cancelDownload(NULL);
            return;
        }
        eden_net_release(dlJob);
        dlJob = 0;
        finishInstall(partialPath(), dlEntry.sha256, dlEntry.name.empty() ? dlEntry.id : dlEntry.name);
        eden_fs_remove(partialPath());
    }
    void finishInstall(const std::string& path, const std::string& sha, const std::string& label) {
        PackInstall::Result r = PackInstall::installZip(path, docs(), sha);
        installLines = r.lines;
        if (r.ok) {
            std::string nm = r.name.empty() ? r.id : r.name;
            status = (r.replaced ? "Updated " : "Installed ") + nm + " " + r.version;
            installLines.clear();
            rescan();
            for (size_t i = 0; i < packs.size(); i++) if (packs[i].folder == r.folder) sel[0] = (int)i;
        } else {
            status = (label.empty() ? std::string() : label + ": ") + r.error;
        }
        gen++;
    }

    // --- actions ----------------------------------------------------------------------------
    void doToggle() {
        const ContentPack::ModPack* p = pack();
        if (!canToggle()) return;
        const bool wasOff = p->state == ContentPack::PACK_DISABLED;
        const std::string folder = p->folder, id = p->id, nm = packTitle(*p);
        if (!ContentPack::setPackEnabled(docs(), id, wasOff)) { status = "Couldn't save the mod list"; return; }
        rescanKeeping(folder);
        status = nm + (wasOff ? " turned on" : " turned off");
    }
    void doMove(int delta) {
        const ContentPack::ModPack* p = pack();
        if (!canMove(delta)) return;
        const std::string folder = p->folder, id = p->id;
        if (!ContentPack::movePack(docs(), id, delta)) { status = "Couldn't save the mod list"; return; }
        rescanKeeping(folder);
        status = delta < 0 ? "Moved up (loads earlier)" : "Moved down (loads later)";
    }
    void askRemove() {
        const ContentPack::ModPack* p = pack();
        if (!canRemove()) return;
        s_removeOwner = this;
        s_removeFolder = p->folder;
        s_removeName = packTitle(*p);
        static const char* const kBtn[] = { "Remove", "Cancel" };
        const std::string body = "Delete the folder Packs/" + p->folder + "? Worlds that use this mod will show its blocks as missing.";
        GLDialog::show(("Remove " + s_removeName).c_str(), body.c_str(), kBtn, 2, removeCb);
    }
    static void removeCb(int chosen) {
        ModsImpl* d = s_removeOwner;
        s_removeOwner = NULL;
        if (!d || chosen != 0) return;
        if (!ContentPack::removePackFolder(docs(), s_removeFolder)) { d->status = "Couldn't remove " + s_removeFolder; return; }
        d->rescan();
        d->status = "Removed " + s_removeName;
    }
};

// ---------------------------------------------------------------------------------------------
ModsMenu::ModsMenu() : d(new ModsImpl()) {}
ModsMenu::~ModsMenu() {
    if (d->catJob) eden_net_release(d->catJob);
    d->cancelDownload(NULL);
    if (s_removeOwner == d) s_removeOwner = NULL;
    delete d;
}

bool ModsMenu::isOpen() const { return d->open; }

void ModsMenu::open() {
    d->open = true;
    d->tab = TAB_INSTALLED;
    d->status.clear();
    d->installLines.clear();
    d->rescan();
    d->scroll.setFirst(0);
    d->first[0] = d->first[1] = 0;
    d->detailKey.clear();
    if (d->catalogueOn() && !d->catLoaded && !d->catJob) d->startCatalogue();
}

void ModsMenu::close() {
    if (d->dlJob) d->cancelDownload(NULL);
    if (GLDialog::active() && s_removeOwner == d) GLDialog::dismiss();
    s_removeOwner = NULL;
    d->open = false;
}

bool ModsMenu::back() {
    if (!d->open) return false;
    close();
    return true;
}

static const int usage_id = 20;   // distinct from Menu (7), SettingsMenu (3), GLDialog (9), ... ServerList (19)

static void mm_layout(ModsImpl* d) {
    using namespace GLW;
    if (!d->built) {
        d->built = true;
        d->title.set("Mods", du(28), UITextAlignmentCenter);
        d->back.setLabel("Back", du(20));
        d->up.setLabel("Up", du(18));
        d->down.setLabel("Down", du(18));
        d->remove.setLabel("Remove", du(18));
        d->remove.setTone(GLW::Button::TONE_DANGER);
        d->file.setLabel("Install from file...", du(16));
        d->refresh.setLabel("Refresh", du(18));
        d->tabs.setTabs(kTabNames, 2, du(18));
    }
    d->tabsOn = d->catalogueOn();
    if (!d->tabsOn && d->tab != ModsMenu::TAB_INSTALLED) d->tab = ModsMenu::TAB_INSTALLED;
    d->pitch = controlH(28);
    d->btnH = controlH(30);
    const float m = du(12), pad = du(12), gap = du(8);
    const CGRect safe = safeRect();
    const float pw = std::min((float)safe.size.width - 2.0f * m, du(900));
    const float ph = safe.size.height - 2.0f * m;
    d->panel = CGRectMake(safe.origin.x + (safe.size.width - pw) * 0.5f, safe.origin.y + m, pw, ph);
    const float left = d->panel.origin.x + pad, cw = pw - 2.0f * pad, right = left + cw;
    const float top = d->panel.origin.y + ph - pad;
    const float botY = d->panel.origin.y + pad;

    // Bottom: row 2 = [Install from file...] + the footer/status line; row 1 = the selection's actions.
    const float row2Y = botY, row1Y = botY + d->btnH + gap;
    const float fixedRows = (d->tabsOn ? 4.0f : 3.0f) * d->btnH + 4.0f * gap;
    d->showTitle = (ph - 2.0f * pad - fixedRows) >= 5.0f * d->pitch;
    float y = top - d->btnH;
    const float backW = du(90);
    d->back.setRect(CGRectMake(left, y, backW, d->btnH));
    if (d->showTitle) {
        d->titleY = y + (d->btnH + d->title.height()) * 0.5f;
        if (d->tabsOn) { y -= gap + d->btnH; d->tabs.setRect(CGRectMake(left, y, cw, d->btnH)); }
    } else if (d->tabsOn) {
        d->tabs.setRect(CGRectMake(left + backW + gap, y, cw - backW - gap, d->btnH));
    }
    d->tabs.setSelected(d->tab);

    const bool inst = d->tab == ModsMenu::TAB_INSTALLED;
    // Row 1 buttons. Installed: Turn off | Up | Down | Remove. Get Mods: Refresh | Install.
    const float w1 = std::max(du(60), (cw - 3.0f * gap) / 5.0f);
    if (inst) {
        const float tw = cw - 3.0f * w1 - 3.0f * gap;
        d->toggle.setRect(CGRectMake(left, row1Y, tw, d->btnH));
        d->up.setRect(CGRectMake(left + tw + gap, row1Y, w1, d->btnH));
        d->down.setRect(CGRectMake(left + tw + gap + (w1 + gap), row1Y, w1, d->btnH));
        d->remove.setRect(CGRectMake(left + tw + gap + 2.0f * (w1 + gap), row1Y, w1, d->btnH));
    } else {
        d->refresh.setRect(CGRectMake(left, row1Y, du(120), d->btnH));
        d->install.setRect(CGRectMake(right - du(180), row1Y, du(180), d->btnH));
    }
    const bool canPick = eden_pick_zip_available() != 0;
    const float fileW = canPick && inst ? std::min(du(210), cw * 0.5f) : 0.0f;
    d->file.setRect(CGRectMake(left, row2Y, fileW, d->btnH));

    // Toggle / Install labels flip on state (setLabel is a raster: only on change).
    const ContentPack::ModPack* sp = d->pack();
    const std::string tl = (sp && sp->state == ContentPack::PACK_DISABLED) ? "Turn on" : "Turn off";
    if (tl != d->shownToggle) { d->shownToggle = tl; d->toggle.setLabel(tl.c_str(), du(18)); }
    d->toggle.setTone(tl == "Turn on" ? GLW::Button::TONE_POSITIVE : GLW::Button::TONE_DEFAULT);
    const bool busy = d->dlJob != 0;
    const std::string il = busy ? "Cancel" : "Install";
    if (il != d->shownInstall) {
        d->shownInstall = il;
        d->install.setLabel(il.c_str(), du(20));
        d->install.setTone(busy ? GLW::Button::TONE_DANGER : GLW::Button::TONE_POSITIVE);
    }
    d->toggle.setEnabled(d->canToggle());
    d->up.setEnabled(d->canMove(-1));
    d->down.setEnabled(d->canMove(+1));
    d->remove.setEnabled(d->canRemove());
    d->file.setEnabled(canPick && d->pickWait == 0);
    d->refresh.setEnabled(!d->catJob && !busy);
    d->install.setEnabled(busy || d->entry() != NULL);

    // Body: list + scrollbar, detail beside (wide) or below (narrow).
    const bool narrow = cw < du(540);
    const float bodyTop = (d->tabsOn && d->showTitle ? d->tabs.rect().origin.y : (d->tabsOn ? d->tabs.rect().origin.y : d->back.rect().origin.y)) - gap;
    const float bodyBottom = row1Y + d->btnH + gap;
    const float bodyH = std::max(d->pitch, bodyTop - bodyBottom);
    const float barW = du(20);
    float listW, listAreaH;
    if (narrow) { listW = cw; listAreaH = std::floor(bodyH * 0.5f); }
    else        { listW = std::floor(cw * 0.5f); listAreaH = bodyH; }
    int vis = (int)std::floor(listAreaH / d->pitch);
    if (vis < 1) vis = 1;
    if (vis > MM_MAX_ROWS) vis = MM_MAX_ROWS;
    const float listH = vis * d->pitch;
    d->list = CGRectMake(left, bodyTop - listH, listW - barW - du(6), listH);
    d->scroll.setBarRect(CGRectMake(left + listW - barW, bodyTop - listH, barW, listH));
    d->scroll.setPitch(d->pitch);
    d->scroll.setRows(d->count(), vis);
    d->scroll.setFirst(d->first[d->tab]);
    d->first[d->tab] = d->scroll.first();
    if (d->sel[d->tab] >= d->count()) d->sel[d->tab] = d->count() - 1;
    if (narrow) d->detail = CGRectMake(left, bodyBottom, cw, std::max(0.0f, bodyTop - listH - gap - bodyBottom));
    else        d->detail = CGRectMake(left + listW + gap, bodyBottom, cw - listW - gap, bodyTop - bodyBottom);

    for (int r = 0; r < MM_MAX_ROWS; r++) {
        d->rows[r].setRect(CGRectMake(d->list.origin.x, d->list.origin.y + listH - (r + 1) * d->pitch, d->list.size.width, d->pitch));
        d->rows[r].setLast(r == vis - 1);
    }
    for (int r = 0; r < vis; r++) {
        const int v = d->scroll.first() + r;
        std::string t, dt;
        if (v < d->count()) {
            if (inst) { t = packTitle(d->packs[(size_t)v]); dt = stateWord(d->packs[(size_t)v].state); }
            else {
                t = d->cat[(size_t)v].name.empty() ? d->cat[(size_t)v].id : d->cat[(size_t)v].name;
                const char* iv = d->installed(d->cat[(size_t)v].id);
                dt = *iv ? "Installed" : "";
            }
        }
        const std::string key = (inst ? "i:" : "g:") + t + "\n" + dt;
        if (!d->rowBuilt[r] || key != d->rowText[r]) {
            d->rowBuilt[r] = true;
            d->rowText[r] = key;
            d->rows[r].setTitle(t.c_str(), du(19));
            d->rows[r].setDetail(dt.c_str(), du(14));
        }
        d->rows[r].setSelected(v == d->sel[d->tab] && v < d->count());
    }

    // The detail pane's lines: rebuilt when the selection, the data or the pane width/height changes.
    char key[160];
    std::snprintf(key, sizeof(key), "%d:%d:%d:%.0f:%.0f:%d", d->tab, d->sel[d->tab], d->gen, d->detail.size.width, d->detail.size.height, d->dlJob != 0);
    if (key != d->detailKey) {
        d->detailKey = key;
        const float dp = du(10), wrap = std::max(du(40), (float)d->detail.size.width - 2.0f * dp);
        for (int i = 0; i < MM_MAX_LINES; i++) d->lines[i].clear();
        d->nLines = 0;
        float used = dp;
        const float room = d->detail.size.height - dp;
        int more = 0;
        auto add = [&](const std::string& text, float pt, GLW::Color c, bool bold) {
            if (text.empty()) return;
            if (d->nLines >= MM_MAX_LINES - 1) { more++; return; }
            GLW::Label& L = d->lines[d->nLines];
            L.set(text.c_str(), pt, UITextAlignmentLeft, wrap, bold ? FACE_DISPLAY : FACE_BODY);
            if (used + L.height() > room) { L.clear(); more++; return; }
            used += L.height() + du(2);
            d->lineColor[d->nLines++] = c;
        };
        if (inst) {
            const ContentPack::ModPack* p = d->pack();
            if (p) {
                add(packTitle(*p), du(21), kText, true);
                std::string idv = p->id.empty() ? "(no id)" : p->id;
                if (!p->version.empty()) idv += "  v" + p->version;
                add(idv, du(14), kTextSecondary, false);
                add(p->fromExtraDir ? "From a --packs= folder: always on, can't be moved or removed." : "Folder: Packs/" + p->folder, du(14), kTextSecondary, false);
                char b[64];
                std::snprintf(b, sizeof(b), "%d block%s", (int)p->blocksInFile, p->blocksInFile == 1 ? "" : "s");
                add(std::string(stateWord(p->state)) + ", " + b, du(14), kText, false);
                if (!p->why.empty()) add(p->why, du(14), kTextWarn, false);
                if (!p->needs.empty()) add("Requires: " + joinNeeds(p->needs), du(14), kTextSecondary, false);
                if (!p->conflicts.empty()) add("Conflicts with: " + joinNeeds(p->conflicts), du(14), kTextSecondary, false);
                for (size_t i = 0; i < p->lines.size(); i++)
                    add(p->lines[i], du(13), p->lines[i].find("warning:") != std::string::npos ? kTextSecondary : kTextError, false);
            } else {
                add("No mods installed.", du(18), kText, true);
                add("Put a pack folder in Packs/, or install a .zip.", du(14), kTextSecondary, false);
            }
        } else {
            const PackInstall::CatalogueEntry* e = d->entry();
            if (e) {
                add(e->name.empty() ? e->id : e->name, du(21), kText, true);
                std::string v = e->id + "  v" + e->version;
                add(v, du(14), kTextSecondary, false);
                if (!e->author.empty()) add("By " + e->author, du(14), kTextSecondary, false);
                if (e->size >= 0) add(sizeText(e->size), du(14), kTextSecondary, false);
                const char* iv = d->installed(e->id);
                if (*iv) add(std::string("installed v") + iv, du(14), kText, false);
                if (!e->needs.empty()) { std::string r; for (size_t i = 0; i < e->needs.size(); i++) r += (i ? ", " : "") + e->needs[i]; add("Requires: " + r, du(14), kTextSecondary, false); }
                add(e->description, du(14), kText, false);
            }
        }
        for (size_t i = 0; i < d->installLines.size(); i++)
            add(d->installLines[i], du(13), d->installLines[i].find("warning:") != std::string::npos ? kTextSecondary : kTextError, false);
        if (more > 0 && d->nLines < MM_MAX_LINES) {
            char b[40];
            std::snprintf(b, sizeof(b), "(%d more line%s)", more, more == 1 ? "" : "s");
            d->lines[d->nLines].set(b, du(13), UITextAlignmentLeft, wrap, FACE_BODY);
            d->lineColor[d->nLines++] = kTextSecondary;
        }
    }
}

void ModsMenu::update(float etime) {
    using namespace GLW;
    if (!d->open) return;
    d->clock += etime;
    d->pollCatalogue();
    d->pollDownload();
    if (d->pickWait && !eden_pick_zip_busy()) {
        d->pickWait = 0;
        const char* p = eden_pick_zip_take();
        if (p && *p) installFile(p);
    }
    mm_layout(d);

    if (GLDialog::active()) return;   // the Remove confirmation is modal
    if (int wheel = eden_ui_take_wheel()) { d->scroll.scrollBy(-wheel); d->first[d->tab] = d->scroll.first(); }

    Input* input = Input::getInput();
    itouch* touches = input->getTouches();
    GLW::Button* const btns[] = { &d->back, &d->toggle, &d->up, &d->down, &d->remove, &d->file, &d->install, &d->refresh };
    const int nb = (int)(sizeof(btns) / sizeof(btns[0]));
    const bool inst = d->tab == TAB_INSTALLED;
    auto live = [&](GLW::Button* b) {
        if (b == &d->back) return true;
        if (inst) return b == &d->toggle || b == &d->up || b == &d->down || b == &d->remove || b == &d->file;
        return b == &d->install || b == &d->refresh;
    };
    auto inList = [&](float x, float y) {
        return x >= d->list.origin.x && x <= d->list.origin.x + d->list.size.width &&
               y >= d->list.origin.y && y <= d->list.origin.y + d->list.size.height;
    };
    for (int i = 0; i < MAX_TOUCHES; i++) {
        if (touches[i].inuse == 0 && touches[i].down == M_DOWN) {
            touches[i].inuse = usage_id;
            const float mx = touches[i].mx, my = touches[i].my;
            for (int b = 0; b < nb; b++) btns[b]->setPressed(live(btns[b]) && btns[b]->enabled() && btns[b]->hit(mx, my));
            if (d->tabsOn) { const int t = d->tabs.hit(mx, my); if (t >= 0) { d->tabTouch = i; d->tabs.setHeld(t); } }
            if (d->barTouch < 0 && d->scroll.hitBar(mx, my)) {
                d->barTouch = i;
                d->scroll.barBegin(my);
                d->first[d->tab] = d->scroll.first();
            } else if (d->listTouch < 0 && inList(mx, my)) {
                d->listTouch = i;
                d->scroll.beginContentDrag(my);
                d->downRow = -1;
                for (int r = 0; r < d->scroll.visible(); r++)
                    if (d->rows[r].hit(mx, my)) d->downRow = d->scroll.first() + r;
            }
        }
        if (touches[i].inuse == usage_id && touches[i].down == M_DOWN) {
            if (i == d->barTouch) { d->scroll.barDragTo(touches[i].my); d->first[d->tab] = d->scroll.first(); }
            if (i == d->listTouch && d->scroll.contentDragTo(touches[i].my)) { d->downRow = -1; d->first[d->tab] = d->scroll.first(); }
        }
        if (touches[i].inuse == usage_id && touches[i].down == M_RELEASE) {
            const float mx = touches[i].mx, my = touches[i].my;
            touches[i].inuse = 0;
            touches[i].down = M_NONE;
            GLW::Button* hit = NULL;
            for (int b = 0; b < nb; b++) {
                if (btns[b]->pressed() && btns[b]->hit(mx, my)) hit = btns[b];
                btns[b]->setPressed(false);
            }
            if (i == d->tabTouch) {
                d->tabTouch = -1;
                d->tabs.setHeld(-1);
                const int t = d->tabs.hit(mx, my);
                if (t >= 0 && t != d->tab) {
                    d->tab = t;
                    d->status.clear();
                    if (t == TAB_GET && !d->catLoaded && !d->catJob) d->startCatalogue();
                }
                return;
            }
            if (i == d->barTouch) { d->scroll.endDrag(); d->barTouch = -1; continue; }
            if (i == d->listTouch) {
                const bool scrolled = d->scroll.scrolling();
                d->scroll.endDrag();
                d->listTouch = -1;
                const int row = d->downRow;
                d->downRow = -1;
                if (!scrolled && row >= 0 && row < d->count()) {
                    const int r = row - d->scroll.first();
                    if (r >= 0 && r < d->scroll.visible() && d->rows[r].hit(mx, my)) d->sel[d->tab] = row;
                }
                continue;
            }
            if (hit == &d->back) { close(); return; }
            if (hit == &d->toggle) d->doToggle();
            else if (hit == &d->up) d->doMove(-1);
            else if (hit == &d->down) d->doMove(+1);
            else if (hit == &d->remove) d->askRemove();
            else if (hit == &d->file) { d->pickWait = 1; eden_pick_zip_begin(); if (!eden_pick_zip_busy()) d->pickWait = 0; }
            else if (hit == &d->refresh) { d->catLoaded = false; d->startCatalogue(); }
            else if (hit == &d->install) { if (d->dlJob) d->cancelDownload("Download cancelled"); else d->startInstall(); }
        }
    }
}

void ModsMenu::render() {
    using namespace GLW;
    if (!d->open) return;
    mm_layout(d);
    Graphics::kitState();

    fill(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kScrim);
    bevel(d->panel, BEVEL_WINDOW);
    d->back.render();
    if (d->showTitle) d->title.drawCentered(d->panel.origin.x + d->panel.size.width * 0.5f, d->titleY, kText);
    if (d->tabsOn) d->tabs.render();

    // The list, or what it is empty for.
    bevel(d->list, BEVEL_CONTENT);
    const bool inst = d->tab == TAB_INSTALLED;
    std::string emptyText;
    if (d->count() == 0) {
        if (inst) emptyText = "No mods installed";
        else if (d->catJob) emptyText = "Loading mods...";
        else if (!d->catStatus.empty()) emptyText = "Couldn't load the list";
        else if (d->catLoaded) emptyText = "No mods listed";
    }
    if (emptyText != d->shownEmpty) {
        d->shownEmpty = emptyText;
        if (emptyText.empty()) { d->empty.clear(); d->emptyHint.clear(); }
        else {
            d->empty.set(emptyText.c_str(), du(22), UITextAlignmentCenter);
            const char* hint = inst ? "Put a pack folder in Packs/ or install a .zip." : (!d->catStatus.empty() ? d->catStatus.c_str() : "");
            d->emptyHint.set(hint, du(14), UITextAlignmentCenter, d->list.size.width - du(20), FACE_BODY);
        }
    }
    if (!d->empty.empty()) {
        const float cx = d->list.origin.x + d->list.size.width * 0.5f;
        const float h = d->empty.height() + du(6) + d->emptyHint.height();
        const float yTop = d->list.origin.y + (d->list.size.height + h) * 0.5f;
        d->empty.drawCentered(cx, yTop, kText);
        d->emptyHint.drawCentered(cx, yTop - d->empty.height() - du(6), kTextSecondary);
    } else {
        for (int r = 0; r < d->scroll.visible() && d->scroll.first() + r < d->count(); r++) d->rows[r].render();
    }
    d->scroll.render();

    // The detail pane.
    bevel(d->detail, BEVEL_CONTENT);
    const float dp = du(10);
    float yTop = d->detail.origin.y + d->detail.size.height - dp;
    for (int i = 0; i < d->nLines; i++) {
        d->lines[i].draw(d->detail.origin.x + dp, yTop, d->lineColor[i]);
        yTop -= d->lines[i].height() + du(2);
    }
    if (d->dlJob) {
        progressBar(CGRectMake(d->detail.origin.x + dp, d->detail.origin.y + dp, d->detail.size.width - 2.0f * dp, du(18)),
                    d->dlFrac, d->clock * 0.8f);
    }

    // Action rows.
    if (inst) { d->toggle.render(); d->up.render(); d->down.render(); d->remove.render(); if (d->file.rect().size.width > 0) d->file.render(); }
    else { d->refresh.render(); d->install.render(); }

    // Footer: the outcome of the last action, else the standing note.
    const std::string st = d->status.empty() ? std::string(kFooter) : d->status;
    const float fx = d->file.rect().size.width > 0 ? d->file.rect().origin.x + d->file.rect().size.width + du(12) : d->file.rect().origin.x;
    const float fw = d->panel.origin.x + d->panel.size.width - du(12) - fx;
    if (st != d->shownStatus) {
        d->shownStatus = st;
        d->statusL.set(st.c_str(), du(14), UITextAlignmentLeft, std::max(du(40), fw), FACE_BODY);
    }
    const CGRect fr = d->file.rect();
    d->statusL.draw(fx, fr.origin.y + (fr.size.height + d->statusL.height()) * 0.5f, d->status.empty() ? kTextSecondary : kText);
    Pen::color(1.0f, 1.0f, 1.0f, 1.0f);
}

// --- harness surface -------------------------------------------------------------------------
CGRect ModsMenu::controlRect(const char* which) {
    mm_layout(d);
    if (!std::strcmp(which, "back"))      return d->back.rect();
    if (!std::strcmp(which, "toggle"))    return d->toggle.rect();
    if (!std::strcmp(which, "up"))        return d->up.rect();
    if (!std::strcmp(which, "down"))      return d->down.rect();
    if (!std::strcmp(which, "remove"))    return d->remove.rect();
    if (!std::strcmp(which, "file"))      return d->file.rect();
    if (!std::strcmp(which, "install"))   return d->install.rect();
    if (!std::strcmp(which, "refresh"))   return d->refresh.rect();
    if (!std::strcmp(which, "list"))      return d->list;
    if (!std::strcmp(which, "scrollbar")) return d->scroll.barRect();
    return CGRectMake(0, 0, 0, 0);
}
CGRect ModsMenu::tabRect(int t) { mm_layout(d); return d->tabsOn ? d->tabs.tabRect(t) : CGRectMake(0, 0, 0, 0); }
bool ModsMenu::rowRect(int index, CGRect* r) {
    mm_layout(d);
    const int k = index - d->scroll.first();
    if (index < 0 || k < 0 || k >= d->scroll.visible() || index >= d->scroll.total()) return false;
    if (r) *r = d->rows[k].rect();
    return true;
}
void ModsMenu::showRow(int index) { mm_layout(d); d->scroll.ensureVisible(index); d->first[d->tab] = d->scroll.first(); }
int  ModsMenu::tab() const { return d->tab; }
void ModsMenu::showTab(int t) {
    if (t < 0 || t > 1) return;
    mm_layout(d);
    if (t == TAB_GET && !d->tabsOn) return;
    d->tab = t;
    if (t == TAB_GET && !d->catLoaded && !d->catJob) d->startCatalogue();
}
bool ModsMenu::tabsShown() const { return d->catalogueOn(); }
int  ModsMenu::entryCount() const { return d->count(); }
const char* ModsMenu::entryId(int i) const {
    if (i < 0 || i >= d->count()) return "";
    if (d->tab == TAB_GET) return d->cat[(size_t)i].id.c_str();
    const ContentPack::ModPack& p = d->packs[(size_t)i];
    return p.id.empty() ? p.folder.c_str() : p.id.c_str();
}
const char* ModsMenu::entryFolder(int i) const {
    return (d->tab == TAB_INSTALLED && i >= 0 && i < (int)d->packs.size()) ? d->packs[(size_t)i].folder.c_str() : "";
}
const char* ModsMenu::entryState(int i) const {
    return (d->tab == TAB_INSTALLED && i >= 0 && i < (int)d->packs.size()) ? stateWord(d->packs[(size_t)i].state) : "";
}
int  ModsMenu::selectedIndex() const { return d->sel[d->tab]; }
void ModsMenu::select(int i) { if (i >= 0 && i < d->count()) d->sel[d->tab] = i; }
bool ModsMenu::canToggle() const { return d->canToggle(); }
bool ModsMenu::canMove(int delta) const { return d->canMove(delta); }
bool ModsMenu::canRemove() const { return d->canRemove(); }
bool ModsMenu::busy() const { return d->catJob != 0 || d->dlJob != 0; }
const char* ModsMenu::statusText() const { return d->status.c_str(); }
void ModsMenu::feedCatalogue(const char* data, size_t len) { d->setCatalogue(data, len); }
void ModsMenu::installFile(const char* path) {
    if (!path || !*path) return;
    d->finishInstall(path, "", "");
}
const char* ModsMenu::installedVersion(const char* id) const { return d->installed(id ? id : ""); }
