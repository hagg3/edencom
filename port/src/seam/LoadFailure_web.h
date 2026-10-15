#pragma once
// Cross-TU hook: Classes/FileManager.mm reports a corrupt/truncated .eden load failure here (a
// NEW call site into a seam file, not a --wrap -- see LoadFailure_web.mm's header for the full
// story). The actual recovery UI lives in JS (public/eden-loaderror.js), which polls the exports
// LoadFailure_web.mm also defines (eden_load_failed() etc).
extern "C" void eden_report_load_failure(const char* world_file_name, const char* reason);
// Queried by World::loadWorld (Classes/World.mm) so a failed FileManager::loadWorld() doesn't get
// treated as a successful one -- see that call site's comment for why this check exists.
extern "C" int eden_load_failed(void);
