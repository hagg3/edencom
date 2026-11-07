//
//  ModsMenu.h
//  Emod — K.3-5/K.3-6 (WORKING/mod-manager-plan-2026-11-01.md §2): the mod manager screen, modelled on
//  WorldBrowser (a full screen over the menu background; Back, a title, an optional tab rail, a list
//  with a scrollbar, a detail pane, an action bar).
//
//  Installed tab: one row per pack folder `ContentPack::scanPacks` finds (extra --packs= dirs first,
//  then Packs/ in enable-list order): name, state (On / Off / Refused / Skipped). The selection's
//  actions are big buttons (a finger's target, no text input anywhere on the screen): Turn on/off
//  (setPackEnabled), Up / Down (movePack; --packs= rows are fixed and say so), Remove (removePackFolder
//  behind a GLDialog naming the folder), and, on desktop, "Install from file..." (a .zip, installZip).
//  The detail pane shows the pack's id/version/folder/blocks, why it is skipped, requires/conflicts,
//  and the validator's lines (errors red, warnings grey). Changes apply when a world opens.
//
//  Get Mods tab (K.3-6), shown only when the setting `mods_catalogue_url` is set: the catalogue
//  (PackInstall::parseCatalogue) as rows, Install = download to Packs/.download.partial (polled per
//  frame with progress) then installZip with the entry's sha256.
//
//  Never in a session: the menu offers it only on the home screen / main menu.
//
#ifndef Eden_ModsMenu_h
#define Eden_ModsMenu_h

#include "GLWidgets.h"
#include <stddef.h>

class ModsMenu {
public:
    enum Tab { TAB_INSTALLED = 0, TAB_GET = 1 };

    ModsMenu();
    ~ModsMenu();

    void open();              // rescans Packs/, lands on the Installed tab
    void close();
    bool isOpen() const;
    bool back();              // Escape: closes (cancelling a download); 1 = consumed
    void update(float etime);
    void render();

    // --- harness surface (--ui-selftest / --shot): point space, y up ---------------------------
    // controlRect: "back", "toggle", "up", "down", "remove", "file" (Install from file), "install",
    // "refresh" (Get Mods), "list", "scrollbar". tabRect(i) is a tab (zero-size when no tabs).
    CGRect controlRect(const char* which);
    CGRect tabRect(int tab);
    bool   rowRect(int index, CGRect* r);
    void   showRow(int index);
    int    tab() const;
    void   showTab(int t);
    bool   tabsShown() const;                    // the catalogue URL is set
    int    entryCount() const;                   // rows on the current tab
    const char* entryId(int i) const;            // Installed: the pack id (else the folder); Get: the id
    const char* entryFolder(int i) const;        // Installed only
    const char* entryState(int i) const;         // Installed: "On" "Off" "Refused" "Skipped"
    int    selectedIndex() const;
    void   select(int i);
    bool   canToggle() const;
    bool   canMove(int delta) const;
    bool   canRemove() const;
    bool   busy() const;                         // a catalogue fetch or a download is in flight
    const char* statusText() const;
    // Get Mods without a network: hand the tab the bytes a catalogue request would have returned.
    void   feedCatalogue(const char* data, size_t len);
    // The "Install from file..." dialog's outcome, as the dialog hands it back.
    void   installFile(const char* path);
    // The installed pack with this id: its version, else "" (what the Get Mods rows say).
    const char* installedVersion(const char* id) const;

private:
    ModsMenu(const ModsMenu&);
    ModsMenu& operator=(const ModsMenu&);
    struct ModsImpl* d;
};

#endif
