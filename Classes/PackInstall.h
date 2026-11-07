//
//  PackInstall.h
//  Emod — K.3 (WORKING/mod-manager-plan-2026-11-01.md M6/M7): installing a mod pack from a zip, and the
//  "Get Mods" catalogue.
//
//  A pack travels as a zip holding one pack folder: `pack.json` at the root, or inside one top-level
//  folder (what "Compress" in Finder/Explorer makes). installZip() checks it before anything reaches
//  Packs/: the entry names (no absolute path, `..`, backslash, drive), the sizes (caps below), the methods
//  (stored or deflate; no encryption), each entry's CRC, an optional SHA-256 of the whole file (the
//  catalogue's), and the pack itself (ContentPack::compileMod: a refused pack is not installed). It unpacks
//  into `Packs/.install.partial/` and commits with directory renames: a new pack becomes `Packs/<id>/`; an
//  installed pack with the same id (any folder name) is replaced in place, its folder name and its place
//  in the enable list kept. Nothing is half-installed: a failure removes the staging folder.
//
//  Synchronous (a pack is <= 16 MiB unpacked); the caller downloads first (eden_net_fetch to a file in
//  `Packs/.download.partial`), then calls this on the main thread outside a world (packs load at world
//  entry, so an install takes effect at the next one).
//
#ifndef EDEN_PACK_INSTALL_H
#define EDEN_PACK_INSTALL_H

#include <stddef.h>
#include <string>
#include <vector>

namespace PackInstall {

enum {
    ZIP_MAX_ENTRIES = 1024,
    ZIP_MAX_ENTRY_BYTES = 4 << 20,    // one unpacked file (pack.json and PNGs are capped at 1 MiB by the loader)
    ZIP_MAX_TOTAL_BYTES = 16 << 20,   // the whole unpacked pack
    ZIP_MAX_FILE_BYTES = 32 << 20,    // the zip itself
};

struct Result {
    bool ok = false;
    std::string id, name, version, folder;   // folder: the Packs/ folder it is now in
    bool replaced = false;                   // an installed pack with this id was replaced
    std::string error;                       // one sentence for the player when !ok
    std::vector<std::string> lines;          // the validator's lines (errors and warnings)
};

// `expectSha256`: 64 lowercase hex, or "" for no check (a file the player picked).
Result installZip(const std::string& zipPath, const std::string& documents, const std::string& expectSha256);

// The catalogue a "Get Mods" source serves (docs/content-packs.md "Getting packs"):
//   {"format": 1, "packs": [{"id": "stairs", "name": "Stairs and Trims", "version": "1.0.0",
//     "description": "...", "author": "...", "url": "https://.../stairs-1.0.0.zip",
//     "sha256": "<64 hex>", "size": 12345, "requires": ["base>=1.0"]}]}
// An entry without a valid id, an http(s) url or a sha256 is dropped (counted in `dropped`).
struct CatalogueEntry {
    std::string id, name, version, description, author, url, sha256;
    long long size = -1;
    std::vector<std::string> needs;   // "requires", as written
};
bool parseCatalogue(const char* data, size_t len, std::vector<CatalogueEntry>* out, int* dropped, std::string* error);

// SHA-256 of a file, lowercase hex; "" if unreadable.
std::string fileSha256(const std::string& path);

}  // namespace PackInstall

#endif
