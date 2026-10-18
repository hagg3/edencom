//
//  MpServers.h
//  Eden
//
//  MP.6: the C seam between the multiplayer screens (ServerListMenu, ServerFormMenu) and the
//  host's network side (native/src/seam/MpServers_native.mm over port/src/net/ServerList, and
//  MpLink_native.mm's join flow). Classes/ never includes port/src/net: everything crosses here as
//  plain C structs. WORKING/multiplayer-server-list-plan-2026-10-17.md §4-§6 (data),
//  multiplayer-ui-plan-2026-10-17.md §2-§7 (screens).
//
#ifndef Eden_MpServers_h
#define Eden_MpServers_h

// A row's live status (list plan §4-§5). CACHED = no probe this visit yet (the cache is shown dimmed).
enum {
    EDEN_MP_ST_CACHED = 0, EDEN_MP_ST_PROBING, EDEN_MP_ST_ONLINE, EDEN_MP_ST_BASIC, EDEN_MP_ST_FULL,
    EDEN_MP_ST_NOT_RUNNING, EDEN_MP_ST_NO_ANSWER, EDEN_MP_ST_UNKNOWN_HOST
};

typedef struct EdenMpServerRow {
    char label[64];
    char host[256];
    int  port;
    int  hasPassword;          // a remembered password
    char name[64];             // INFO name (live or cached), "" if none
    char motd[160];
    char flags[24];            // INFO letters: t TNT off, f fire off, z protected areas, p PvP
    int  players, max;         // -1 = unknown
    int  locked;               // -1 "?", 0 open, 1 password
    long long seen;            // unix seconds of the last good probe, 0 = never
    int  status;               // EDEN_MP_ST_*
    int  pingMs;               // -1 = unknown
    int  fromCache;            // name/players/motd are the cache's, not this visit's probe
} EdenMpServerRow;

// The join's progress (UI plan §6). Stages in order; FAILED carries the error.
enum {
    EDEN_MP_JOIN_IDLE = 0, EDEN_MP_JOIN_CONNECTING, EDEN_MP_JOIN_JOINING, EDEN_MP_JOIN_RECEIVING,
    EDEN_MP_JOIN_LOADING, EDEN_MP_JOIN_PLAYING, EDEN_MP_JOIN_FAILED
};
// Mirrors eden_net::MpError (port/src/net/MpSession.h), in order.
enum {
    EDEN_MP_ERR_NONE = 0, EDEN_MP_ERR_BAD_INPUT, EDEN_MP_ERR_DNS, EDEN_MP_ERR_REFUSED, EDEN_MP_ERR_TIMEOUT,
    EDEN_MP_ERR_CONNECT_FAILED, EDEN_MP_ERR_SERVER_FULL, EDEN_MP_ERR_WRONG_PASSWORD, EDEN_MP_ERR_INVALID_NAME,
    EDEN_MP_ERR_NO_WELCOME, EDEN_MP_ERR_SERVER_CLOSED, EDEN_MP_ERR_LOST, EDEN_MP_ERR_PROTOCOL,
    EDEN_MP_ERR_TOO_MUCH_DATA
};

typedef struct EdenMpJoinState {
    int   stage;               // EDEN_MP_JOIN_*
    int   err;                 // EDEN_MP_ERR_* when FAILED
    char  text[256];           // the error sentence (UI plan §7) when FAILED
    float progress;            // 0..1, or < 0 = indeterminate
    int   lines;               // welcome / MOTD lines so far (eden_mp_join_line)
    char  host[256];
    int   port;
    int   usedPassword;        // the join sent a password
} EdenMpJoinState;

#ifdef __cplusplus
extern "C" {
#endif

// --- the saved list (servers.txt in the documents root) ---
int  eden_mp_servers_load(void);                              // (re)reads the file; the count
int  eden_mp_servers_count(void);
int  eden_mp_servers_get(int i, EdenMpServerRow* out);        // 0 = no such row
int  eden_mp_servers_password(int i, char* buf, int cap);     // the remembered one; 0 = none
// add/update: label may be blank; password NULL = keep (update) / none (add), "" = forget.
int  eden_mp_servers_add(const char* label, const char* host, int port, const char* password);   // index, -1 = full
int  eden_mp_servers_update(int i, const char* label, const char* host, int port, const char* password);
void eden_mp_servers_remove(int i);
int  eden_mp_servers_move(int i, int delta);                  // 1 = moved
void eden_mp_servers_set_locked(int i, int locked);           // learnt from a join (list plan §5)
int  eden_mp_servers_find(const char* host, int port, int except);   // a duplicate's index, -1 none
int  eden_mp_servers_save(void);                              // 0 = write failed
int  eden_mp_servers_cap(void);                               // 100
// --- probing (≤ 8 in flight, off the main thread) ---
void eden_mp_servers_refresh(int i);                          // -1 = every row
int  eden_mp_servers_poll(void);                              // applies results; probes still pending
// --- validation (port/src/net's rules) ---
int  eden_mp_parse_address(const char* in, char* host, int hostCap, int* port, char* err, int errCap);   // 1 = valid
int  eden_mp_name_problem(const char* name, char* err, int errCap);   // 1 = a problem (in err)
// --- the profile (UI plan §4): port settings in NSUserDefaults ---
int  eden_mp_profile_name(char* buf, int cap);                // 0 = no name saved
void eden_mp_profile_set(const char* name, int character);
int  eden_mp_profile_character(void);                         // 0..6 (characterType)
// --- the join (MpLink_native.mm) ---
int  eden_mp_join(const char* host, int port, const char* name, int characterType, const char* password);
int  eden_mp_join_state(EdenMpJoinState* out);                // the stage
int  eden_mp_join_line(int i, char* buf, int cap);
void eden_mp_join_cancel(void);                               // before LOADING: closes it, back to IDLE
void eden_mp_join_ack(void);                                  // FAILED -> IDLE once the UI has shown it
// --- matchmakers (MP.8): Experiments > Matchmakers (`mp_matchmakers`), LIST results as list rows ---
void eden_mp_matchmakers_reload(void);                        // re-reads the setting (the list screen's open)
int  eden_mp_matchmaker_count(void);                          // addresses set; 0 = the tab does not exist
void eden_mp_matchmakers_fetch(void);                         // LIST to each address, off the main thread
int  eden_mp_matchmakers_poll(void);                          // applies results; fetches still pending
int  eden_mp_matchmaker_rows(void);
int  eden_mp_matchmaker_get(int i, EdenMpServerRow* out);     // label = name, motd = "via host:port"
int  eden_mp_matchmaker_error(char* buf, int cap);            // 1 = some matchmaker failed; the sentence
int  eden_mp_matchmaker_save(int i);                          // into servers.txt; its index, -1 = full
int  eden_mp_matchmakers_validate(const char* text, char* clean, int cleanCap, char* err, int errCap);   // 1 = valid
// --- harness: canned rows + probe results (--mp-mock), no sockets, nothing written ---
void eden_mp_servers_mock(int on);                           // 1 = the 7-row fixture, 2 = empty, 0 = servers.txt
#ifdef __cplusplus
}
#endif

#endif
