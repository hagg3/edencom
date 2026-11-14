// LoadFailure.cpp -- the load-failure signal. FileManager::loadWorld/loadEmodWorld (and World::loadWorld)
// call eden_report_load_failure() when a world can't be opened (unreadable .emod, no header, band
// mismatch, unconvertible archive); World::loadWorld's doneLoading==2 check polls eden_load_failed() and
// parks in the menu, showing eden_load_failed_reason() on the status bar. QA.6: the flag is cleared where
// the next load starts, so one failed world never blocks later ones. (The web port's .bak restore dialog
// and its exports were deleted here: dead since S.8.)
#include "LoadFailure.h"
#include <cstdio>
#include <string>

static bool g_failed = false;
static std::string g_reason;

void eden_report_load_failure(const char* world_file_name, const char* reason) {
    g_failed = true;
    g_reason = reason ? reason : "unknown";
    fprintf(stderr, "[eden-load] load failed for '%s': %s\n", (world_file_name ? world_file_name : ""), g_reason.c_str());
}

extern "C" {

int eden_load_failed(void) { return g_failed ? 1 : 0; }

const char* eden_load_failed_reason(void) { return g_reason.c_str(); }

void eden_load_failed_clear(void) { g_failed = false; g_reason.clear(); }

}
