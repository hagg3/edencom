//
//  ChatFeed.h
//  Eden
//
//  MP.7 (WORKING/multiplayer-ui-plan-2026-10-17.md §8): the multiplayer chat. One kept scrollback (the
//  Console screen's Chat tab draws it), the HUD feed (the last lines, fading after 10 s, inert to
//  touch), the sending side (local echo, a client-side rate guard, `/login` masking, Up/Down history)
//  and the touch HUD's chat button. The wire is the host's: lines arrive through addWire() from
//  port/src/seam/MpLink_native.cpp and leave through MpWorld::link()->sendLine("MSG:...").
//
#ifndef Eden_ChatFeed_h
#define Eden_ChatFeed_h

#include <string>
#include <vector>
#include "Console.h"          // Console::Line (the scrollback's line type)
#include "GLWidgets.h"

namespace ChatFeed {

// Console::Line kinds for a chat line (the Console's own are small ints and ECHO = 10).
enum { K_CHAT = 100, K_SERVER = 101, K_SELF = 102, K_CMD = 103, K_NOTICE = 104 };
enum { MAX_LINES = 200, MAX_BYTES = 256 };   // the server's chat length cap is 256 bytes

void reset();                                           // a new session: empty everything
// A relayed line: `name` is the sender ("" for a `[Server]` line, whose prefix is already stripped).
void addWire(bool fromServer, const std::string& name, const std::string& text);
void addNotice(const std::string& text);                // a local line (rate-limit, not connected)

const std::vector<Console::Line>& lines();
unsigned serial();                                      // lines ever added, like Console::scrollbackSerial()

// Sending. The result is what the panel does next: SENT clears the field, TOO_FAST keeps the text.
enum SendResult { SENT, EMPTY, TOO_FAST, NO_SESSION };
SendResult send(const std::string& text);
size_t historyCount();
std::string historyGet(size_t i);

// The HUD feed and its button (Hud::render / Hud::update). All in the HUD's point space, y up.
void update(float etime);
void renderHud(bool panelOpen);                         // top-left of the safe rect, under the pause gear
CGRect feedRect();                                      // where the feed drew last (tests); zero if none
void renderButton(CGRect r, bool pressed);              // the speech-bubble button + the unseen dot
bool unseen();
void markSeen();
int  feedLines();                                       // lines the HUD feed shows now (tests)

// Test hook: the bucket's clock in ms (the harness advances it); NULL = the real clock.
extern long long (*clockMs)();

}  // namespace ChatFeed

#endif
