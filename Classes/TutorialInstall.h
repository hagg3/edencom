// UX.10-2: the tutorial world's install, marker and restore (docs/save-load.md "The tutorial world").
//
// The bundled `Tutorial.emod` (media/, built by port/tools/tutorial.py) is copied into a normal folder world,
// `Worlds/<id>/world.emod`, through the same staging dir and one no-replace rename as an import
// (FileManager::beginStaging / commitStaging). What makes a world "the tutorial" is its sidecar
// `Worlds/<id>/tutorial.json` = {"v":1,"done":[...],"hidden":false}: a rename keeps it, and export, upload and
// a Get Worlds copy never carry it (they move world.emod and photo.png only), so no other world is ever one.
// Once per install (prefs key `tutorial.installed`, set only after the commit rename), then only by the
// Settings > Gameplay "Restore tutorial world" row. Main thread only.
#ifndef EDEN_TUTORIAL_INSTALL_H
#define EDEN_TUTORIAL_INSTALL_H

#include <string>
#include <vector>

namespace TutorialInstall {

extern bool autoInstall;   // false: installOnce() does nothing. The harness clears it for every scripted mode but --tutorial-selftest.
extern int  testMutation;  // --tutorial-mutate=: 0 none, 1 nomarker (no sidecar written), 2 keyfirst (the pref is set before the commit)

const char* prefsKey();    // "tutorial.installed"
const char* markerFile();  // "tutorial.json"

// The sidecar's content. `v` is 1; a file with a higher "v" or that does not parse is not a marker.
struct Marker {
    std::vector<std::string> done;   // step ids (UX.10-3)
    bool hidden = false;             // the player hid all hints (UX.10-3); restore clears it
};
// Marker of the world folder `dir` (absolute); false if there is no readable v1 marker.
bool readMarker(const std::string& dir, Marker* out);
// Writes `dir`/tutorial.json (tmp + rename). `dir` must exist.
bool writeMarker(const std::string& dir, const Marker& m);

// Is the world with this key ("Worlds/<id>/world.emod") the tutorial world? False for a loose or session world.
bool isTutorialWorld(const std::string& documents, const std::string& key);
// The key of every marked world, in folder-name order.
std::vector<std::string> markedWorlds(const std::string& documents);

// Installs the bundled world as a fresh folder world (a fresh id on a clash). Returns its key, "" on failure
// (logged); nothing half-installed is ever listed. Does not touch prefs.
std::string install(const std::string& documents);
// The Settings button: installs a fresh copy, then deletes the marked world(s) it replaces. "" on failure
// (the old world is then untouched). Caller must ensure no world is open.
std::string restore(const std::string& documents);

// The once-per-install hook (Menu::loadWorlds, before the world list is read): installs and sets prefs
// `tutorial.installed` only if the prefs key is unset, autoInstall is on and no conversion/download is live.
// A failure is logged and retried at the next launch. Returns true if it installed.
bool installOnce(const std::string& documents);

}  // namespace TutorialInstall

#endif
