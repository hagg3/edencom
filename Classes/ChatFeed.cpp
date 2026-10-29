//
//  ChatFeed.mm
//  Eden
//
//  MP.7: see ChatFeed.h. A line costs one Label, built the first frame the HUD feed shows it and kept
//  until it falls out of the feed (the Console's rule, docs/console.md "Cost"); the panel's scrollback
//  labels are ConsoleMenu's own cache.
//
#include "ChatFeed.h"
#include "MpWorld.h"
#include "Globals.h"
#include "UIIcons.h"
#include "Resources.h"
#include "Graphics.h"   // Pen (G.5b-7)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

namespace ChatFeed {

namespace {

struct Entry { int kind; std::string text; float age; };

std::vector<Console::Line> g_lines;
std::vector<float> g_ages;               // parallel to g_lines: seconds since each arrived
unsigned g_serial = 0;
unsigned g_seenSerial = 0;               // markSeen(): lines up to here have been seen
std::vector<std::string> g_history;
double g_tokens = 5.0;                   // the client-side bucket: 0.5/s, burst 5 (protocol § Chat)
long long g_lastMs = 0;
bool g_haveClock = false;

struct FeedLabel { GLW::Label label; std::string key; };
std::map<unsigned, FeedLabel*> g_labels;   // serial -> label, only for lines still in the feed
CGRect g_feedRect = CGRectMake(0, 0, 0, 0);
int g_feedLines = 0;

const float kLifeSec = 10.0f, kFadeSec = 0.5f;

long long realMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void push(int kind, const std::string& text) {
    Console::Line l;
    l.kind = kind;
    l.text = text;
    g_lines.push_back(l);
    g_ages.push_back(0.0f);
    ++g_serial;
    if ((int)g_lines.size() > MAX_LINES) {
        g_lines.erase(g_lines.begin());
        g_ages.erase(g_ages.begin());
    }
}

void freeLabels() {
    for (auto& kv : g_labels) delete kv.second;
    g_labels.clear();
}

// The wire's text is already stripped of control bytes by the server; ours might not be (a paste).
std::string clean(const std::string& in) {
    std::string out;
    for (unsigned char c : in) if (c >= 0x20 && c != 0x7f) out += (char)c;
    return out;
}

}  // namespace

long long (*clockMs)() = nullptr;

void reset() {
    g_lines.clear();
    g_ages.clear();
    g_history.clear();
    g_serial = 0;
    g_seenSerial = 0;
    g_tokens = 5.0;
    g_haveClock = false;
    freeLabels();
    g_feedRect = CGRectMake(0, 0, 0, 0);
    g_feedLines = 0;
}

void addWire(bool fromServer, const std::string& name, const std::string& text) {
    if (fromServer) push(K_SERVER, "[Server] " + text);
    else push(K_CHAT, name.empty() ? text : name + ": " + text);
}

void addNotice(const std::string& text) { push(K_NOTICE, text); }

const std::vector<Console::Line>& lines() { return g_lines; }
unsigned serial() { return g_serial; }

SendResult send(const std::string& text0) {
    std::string text = clean(text0);
    size_t a = 0, b = text.size();
    while (a < b && text[a] == ' ') ++a;
    while (b > a && text[b - 1] == ' ') --b;
    text = text.substr(a, b - a);
    if (text.empty()) return EMPTY;
    if ((int)text.size() > MAX_BYTES) {
        size_t n = MAX_BYTES;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) --n;   // never split a UTF-8 character
        text.resize(n);
    }
    MpWorld::Link* link = MpWorld::link();
    if (!link || !MpWorld::active()) return NO_SESSION;
    const bool command = text[0] == '/';
    if (!command) {   // `/` lines have their own server budget (protocol § Chat)
        const long long now = clockMs ? clockMs() : realMs();
        if (g_haveClock) g_tokens = std::min(5.0, g_tokens + (now - g_lastMs) * 0.0005);
        g_haveClock = true;
        g_lastMs = now;
        if (g_tokens < 1.0) return TOO_FAST;
        g_tokens -= 1.0;
    }
    link->sendLine(("MSG:" + text).c_str());
    const bool login = text.compare(0, 7, "/login ") == 0;
    if (command) push(K_CMD, login ? "> /login ****" : "> " + text);
    else push(K_SELF, "You: " + text);
    if (!login && (g_history.empty() || g_history.back() != text)) {
        g_history.push_back(text);
        if (g_history.size() > 50) g_history.erase(g_history.begin());
    }
    return SENT;
}

size_t historyCount() { return g_history.size(); }
std::string historyGet(size_t i) { return i < g_history.size() ? g_history[i] : std::string(); }

bool unseen() { return g_serial != g_seenSerial; }
void markSeen() { g_seenSerial = g_serial; }

void update(float etime) {
    for (float& a : g_ages) a += etime;
    // Free labels whose line fell out of the feed (or out of the kept range).
    const unsigned base = g_serial - (unsigned)g_lines.size();
    for (auto it = g_labels.begin(); it != g_labels.end();) {
        const unsigned s = it->first;
        const bool gone = s < base || g_ages[s - base] > kLifeSec;
        if (gone) { delete it->second; it = g_labels.erase(it); }
        else ++it;
    }
}

int feedLines() { return g_feedLines; }
CGRect feedRect() { return g_feedRect; }

// X.31: both draws run inside the HUD's bracket (Graphics::beginHud), so they must not use
// GLW::begin2D/end2D: its kitEnd() leaves blend OFF and depth ON for the rest of the HUD (the toasts
// then draw their label rects opaque). Push GLW's ortho, set the pen, and pop only the matrices.
static void hudBegin() {
    const float s = IS_IPAD ? SCALE_WIDTH : 1.0f;
    R::begin2D(SCREEN_WIDTH * s, SCREEN_HEIGHT * s, 1.0f);
    Pen::depthTest(false);
    Graphics::kitState();
}
static void hudEnd() { R::end2D(); }

static GLW::Color kindColor(int kind) {
    switch (kind) {
        case K_SERVER: return GLW::rgb(0xffb74d);               // amber
        case K_SELF:   return GLW::rgb(0xc6f175);               // lime
        case K_CMD:    return GLW::rgb(0xbdbdbd);
        case K_NOTICE: return GLW::rgb(0xff8a80);
        default:       return GLW::kWhite;
    }
}

void renderHud(bool panelOpen) {
    g_feedRect = CGRectMake(0, 0, 0, 0);
    g_feedLines = 0;
    if (panelOpen || g_lines.empty()) return;
    using namespace GLW;
    const CGRect safe = safeRect();
    const float maxW = std::min((float)(safe.size.width * 0.45f), (float)du(420));
    // 3 lines on a screen under 360 pt high (a phone in landscape), else 5.
    const int cap = SCREEN_HEIGHT < 360.0f ? 3 : 5;
    const unsigned base = g_serial - (unsigned)g_lines.size();
    // The newest `cap` lines still alive, oldest first.
    std::vector<int> pick;
    for (int i = (int)g_lines.size() - 1; i >= 0 && (int)pick.size() < cap; --i)
        if (g_ages[i] <= kLifeSec) pick.insert(pick.begin(), i);
    if (pick.empty()) return;

    hudBegin();

    float y = safe.origin.y + safe.size.height - du(45) - du(6);   // under the pause gear
    const float x = safe.origin.x + du(6);
    const float pad = du(4);
    float top = y, bottom = y;
    for (int i : pick) {
        const unsigned ser = base + (unsigned)i;
        FeedLabel*& fl = g_labels[ser];
        if (!fl) fl = new FeedLabel();
        const std::string key = g_lines[i].text + "|" + std::to_string(u()) + "|" + std::to_string((int)maxW);
        if (fl->key != key) {
            fl->key = key;
            fl->label.set(g_lines[i].text.c_str(), du(15), UITextAlignmentLeft, maxW - pad * 2, FACE_BODY);
        }
        float alpha = 1.0f;
        const float left = kLifeSec - g_ages[i];
        if (left < kFadeSec) alpha = left / kFadeSec;
        const float h = fl->label.height() + pad;
        const float w = std::min(maxW, fl->label.width() + pad * 2);
        fill(x, y - h, w, h, rgb(0x000000, 0.85f * alpha));
        Color c = kindColor(g_lines[i].kind);
        c.a = alpha;
        fl->label.draw(x + pad, y - pad * 0.5f, c);
        y -= h + du(1);
        bottom = y;
        ++g_feedLines;
    }
    g_feedRect = CGRectMake(x, bottom, maxW, top - bottom);
    Pen::color(1.0f, 1.0f, 1.0f, 1.0f);
    hudEnd();
}

void renderButton(CGRect r, bool pressed) {
    using namespace GLW;
    hudBegin();
    const float pad = r.size.width * 0.16f;
    // The Maysunbo chat_button art (2026-10-27; was a dark plate + UIICON_MESSAGE_SQUARE), dimmed
    // while held.
    Pen::textured(true);
    if (pressed) Pen::color(0.7f, 0.7f, 0.7f, 1.0f);
    else Pen::color(1.0f, 1.0f, 1.0f, 1.0f);
    Resources::getResources->getArtTex(ART_CHAT)->drawInRect(r);
    if (unseen()) {   // a lime dot: lines arrived while the feed was hidden
        const float d = r.size.width * 0.22f;
        fill(r.origin.x + r.size.width - d - pad * 0.3f, r.origin.y + r.size.height - d - pad * 0.3f, d, d, rgb(0x89c31f));
    }
    Pen::color(1.0f, 1.0f, 1.0f, 1.0f);
    hudEnd();
}

}  // namespace ChatFeed
