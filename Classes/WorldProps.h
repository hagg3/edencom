//
//  WorldProps.h
//  Eden
//
//  Stage H / H.15: the world list's Properties screen (Classes/WorldPropertiesMenu.h) and its
//  FileManager pair, readWorldProperties() / applyWorldProperties(). A header of its own because
//  it carries a ClassicGen: FileManager.h cannot include TerrainGenerator.h (TerrainGenerator.h ->
//  Terrain.h -> FileManager.h), so FileManager.h only forward-declares the struct.
//
#ifndef Eden_WorldProps_h
#define Eden_WorldProps_h

#import "TerrainGenerator.h"
#include <string>

// Stage H / H.15: what the world list's Properties screen shows and saves
// (WORKING/feature-archaeology-2026-10-13/world-properties-plan.md §1, §3). Filled by
// FileManager::readWorldProperties() from a CLOSED world; the screen edits `name` and `gen` and
// hands it back to applyWorldProperties(). -1 / "" = not known for this kind of file (hidden rows).
enum { WP_KIND_EMOD=0, WP_KIND_EDEN=1, WP_KIND_ARCHIVE=2 };          // archive: .eden.gz / .zip
enum { WP_TYPE_FLAT=0, WP_TYPE_NORMAL=1, WP_TYPE_CLASSIC=2 };
// Where a Classic world's settings come from: its GENERATOR record, an `.eden` mirror in the
// header, the legacy rule (a 1.x world with no record: caves detected now, as at load), or a record
// this build cannot read (void: the Terrain tab is read-only).
enum { WP_GEN_RECORD=0, WP_GEN_MIRROR=1, WP_GEN_LEGACY=2, WP_GEN_VOID=3 };
// applyWorldProperties()'s result bits (0 = nothing to write; -1 = failed, nothing changed).
enum { WP_WROTE_NAME=1, WP_WROTE_GENERATOR=2, WP_LIFTED_POS=4, WP_LIFTED_HOME=8 };
struct WorldProps{
    int kind;
    std::string name;             // the header's display name
    int seed, height, version, type;
    int genSource;                // WP_GEN_*, Classic only
    ClassicGen gen;               // Classic only
    unsigned long long fileBytes, originalBytes;
    long long createdUnix, savedUnix;   // created: the .emod file header (0 if unknown); saved: mtime
    int columns, signs, goldencubes;
    std::string convertedFrom;    // PROVENANCE's source file name, "" if none
    int convertedVersion;
    Vector pos, home;
    double openMs;                // what reading all this cost (gate 4)
};

#endif
