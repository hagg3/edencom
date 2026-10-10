//
//  WorldTrailer.h
//  Eden — Stage D / D.3a: the post-directory trailer as a model (signs + command blocks).
//
//  A `NewFormat256z` world keeps its signs and command blocks in rows tagged `ff ff ff ff` after
//  the real directory rows (docs/eden-file-format.md "The trailer model"). FileManager captures
//  those rows on both containers -- `dir_trailer` for a `.eden`, the SIGN_TRAILER record for an
//  `.emod` -- and until D.3a only ever carried them verbatim. This class parses them into an
//  ordered list of sections so the game can read and edit records:
//
//    SGN1 -> std::vector<TrailerSign>     (120-byte records)
//    CMB1 -> std::vector<TrailerCmd>      (528-byte records)
//    any other magic -> its bytes, kept verbatim in its slot
//
//  THE RULE THAT MATTERS: an untouched trailer encodes to its original bytes exactly (encode()
//  hands back the bytes it was loaded from), and a touched one rebuilds only the sections that
//  were edited -- every other byte comes out as it went in. A trailer this class cannot read
//  (a row not tagged `ff ff ff ff`, a known magic whose sizes do not add up, a second SGN1 ...)
//  is OPAQUE: kept verbatim, every edit refused. web/tools/emod.py's `trailer` command restates
//  the same rules and shares no code with this file.
//
//  Coordinates: a record holds the ANCHOR block in absolute world block coordinates, file order
//  (x, y, z) with x, y horizontal and z the height. That is exactly the argument order of every
//  `Terrain` API, (x, z, y) with y up -- so a record's (x, y, z) passes straight into
//  `getLand(x, z, y)`. TrailerPos::fromEngine / toEngine are the one place the engine's
//  (x, y_up, z) Vector order is converted; use them, never a hand-written swap.
//
//  Plain C++, no ObjC, no engine globals (FileManager owns the instance; WorldTrailer.cpp has no
//  idea what a world is). Not thread-safe, and nothing needs it to be.
//
#ifndef Eden_WorldTrailer_h
#define Eden_WorldTrailer_h

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// A record's anchor, in the FILE's order: x, y horizontal, z the height.
struct TrailerPos {
    int32_t x, y, z;
    // Terrain API order (x, z, y) with y up IS this order: getLand(p.x, p.y, p.z).
    static TrailerPos fromTerrainArgs(int x, int z, int y) { TrailerPos p = {x, z, y}; return p; }
    // The engine's (x, y_up, z) order (Vector, chunk bounds) <-> the file's (x, y, z).
    static TrailerPos fromEngine(int ex, int eyUp, int ez) { TrailerPos p = {ex, ez, eyUp}; return p; }
    void toEngine(int* ex, int* eyUp, int* ez) const { *ex = x; *eyUp = z; *ez = y; }
    bool operator==(const TrailerPos& o) const { return x == o.x && y == o.y && z == o.z; }
};

// Byte-for-byte the 120-byte SGN1 record (little-endian, like every on-disk struct here).
//   a = the anchor face: 0 -x, 1 +x, 2 bottom (never written by the game), 3 top, 4 -y, 5 +y
//   b = board colour, a colorTable index 1..54 (paint swatch + 1)
//   c = the direction the text faces, a quarter turn: 0 -x, 1 -y, 2 +x, 3 +y
//   text = up to 95 characters + NUL; bytes after the NUL are kept as read
struct TrailerSign {
    int32_t x, y, z, a, b, c;
    char text[96];
    TrailerPos pos() const { TrailerPos p = {x, y, z}; return p; }
};
// Byte-for-byte the 528-byte CMB1 record. A command block is a TYPE_STEEL voxel plus one of these.
// flags is 0 in every record seen and its meaning is unknown: kept verbatim.
struct TrailerCmd {
    int32_t x, y, z, flags;
    char script[512];
    TrailerPos pos() const { TrailerPos p = {x, y, z}; return p; }
};
static_assert(sizeof(TrailerSign) == 120, "SGN1 record is 120 bytes on disk");
static_assert(sizeof(TrailerCmd) == 528, "CMB1 record is 528 bytes on disk");

class WorldTrailer {
public:
    // The cap both containers share: FileManager's DIR_TRAILER_MAX, the `.emod` SIGN_TRAILER limit.
    // In rows (16 B, 12 of payload), so ~6,550 signs with no command blocks.
    enum : size_t { MAX_BYTES = 1u << 20, ROW = 16, ROW_PAYLOAD = 12 };
    enum Result {
        TR_OK = 0,
        TR_FULL,          // the encoded trailer would pass MAX_BYTES: refused, nothing changed
        TR_OPAQUE,        // this trailer could not be read: every edit is refused
        TR_EXISTS,        // a sign already hangs on that (anchor, face) / a command block on that block
        TR_NOT_FOUND,     // no such record
        TR_BAD,           // an argument out of range (face, colour, text length)
    };
    // The toast for a refused edit (D.3c shows it through hud->sb->setStatus).
    static const char* resultMessage(int r);

    WorldTrailer();

    // ---- loading and saving
    // A brand-new world: no trailer, editable, clean.
    void clear();
    // The bytes FileManager captured (dir_trailer / the SIGN_TRAILER record). Always keeps them; returns
    // parsed(). Clean afterwards.
    bool load(const uint8_t* p, size_t n);
    // The 2026 game's sidecar files (`signs_<world>.eden.dat`, `cmd_<world>.eden.dat`) are one bare section
    // each -- `MAGIC | u32 version | u32 count` + records, no wrapper. Adopted only when this trailer has
    // no section of that magic yet; marks the trailer dirty so the next save writes it in. TR_OK, TR_EXISTS
    // (the trailer already has one: the trailer wins), TR_BAD (not a section we can read), TR_OPAQUE, TR_FULL.
    Result adoptSidecar(const uint8_t* p, size_t n);
    bool parsed() const { return parsed_; }
    const std::string& why() const { return why_; }          // why it is opaque ("" when parsed)
    // Changed since load()/clear()/markSaved(). FileManager forces a directory rewrite (`.eden`) or a
    // SIGN_TRAILER record (`.emod`) on this alone, with zero dirty columns.
    bool dirty() const { return dirty_; }
    // What the next save must write: the original bytes while clean, the rebuild once dirty. Empty = no trailer.
    std::vector<uint8_t> encode() const { return dirty_ ? rebuild() : original_; }
    // Serialise from the model regardless of dirty (the parse -> serialise gate).
    std::vector<uint8_t> rebuild() const;
    size_t encodedSize() const;
    // After a save landed: the written bytes become the original, and the trailer is clean.
    void markSaved();
    // Test hook (the mutation leg of the D.3a gate): edits stop setting the dirty mark.
    static bool testNoDirtyMark;

    // ---- records (indices are into the section's vector, stable until a removal)
    size_t signCount() const { return signs_.size(); }
    size_t cmdCount() const { return cmds_.size(); }
    const TrailerSign& sign(size_t i) const { return signs_[i]; }
    const TrailerCmd& cmd(size_t i) const { return cmds_[i]; }
    int findSign(const TrailerPos& anchor, int face) const;   // -1 if none
    int findCmd(const TrailerPos& block) const;               // -1 if none
    // O(1): does any sign or command block hang on this block? (the D.3c removal hook's fast path)
    bool hasAnchored(const TrailerPos& block) const;
    // Every sign index on this block (any face), in record order.
    std::vector<int> signsOn(const TrailerPos& block) const;

    Result addSign(const TrailerPos& anchor, int face, int colour, int facing, const char* text);
    Result setSignText(size_t i, const char* text);
    Result setSignColour(size_t i, int colour);
    Result removeSign(size_t i);
    Result addCmd(const TrailerPos& block, const char* script);
    Result setCmdScript(size_t i, const char* script);
    Result removeCmd(size_t i);
    // D.3c's hook: a live edit turned `block` into air -> every sign on it and its command block go.
    // Returns how many records were removed (0 when opaque).
    int removeAnchored(const TrailerPos& block);

    // Summary for logs and the debug probe: {"parsed":1,"dirty":0,"bytes":N,"signs":N,"cmds":N,"sections":"CMB1,SGN1"}
    std::string describe() const;

private:
    struct Section {
        uint8_t magic[4];
        int kind;                 // K_SIGN, K_CMD or K_RAW
        uint32_t w3, version;     // the wrapper's third word and the inner version, kept as read
        bool present;             // emitted at all (a known section created by an edit starts present)
        bool edited;              // an edit touched it: emitted only while it holds records
        std::vector<uint8_t> raw; // K_RAW: the whole section, wrapper included
    };
    enum { K_SIGN, K_CMD, K_RAW };
    static uint64_t key(const TrailerPos& p);
    void reindex();
    Section* section(int kind, bool create);
    bool fits() const;                  // the encoded size is within MAX_BYTES
    size_t payloadSize() const;
    void touch(int kind);
    bool parse(const uint8_t* p, size_t n);

    std::vector<uint8_t> original_;
    std::vector<Section> sections_;
    std::vector<uint8_t> tail_;            // the zero bytes after the last section, verbatim
    std::vector<TrailerSign> signs_;
    std::vector<TrailerCmd> cmds_;
    std::unordered_map<uint64_t, std::vector<int> > signIndex_;
    std::unordered_map<uint64_t, std::vector<int> > cmdIndex_;
    bool parsed_, dirty_;
    std::string why_;
};

#endif
