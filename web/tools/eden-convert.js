#!/usr/bin/env node
// eden-convert.js -- offline converter between the classic 64-block-tall `.eden` format this
// engine reads (4 chunk-bands per column, 32768-byte column records, header version <= 4) and the
// 256-block-tall "New Dawn" variant (16 bands, 131072-byte records, version >= 5).
//
// Stage 1 of WORKING/256z-format-backport-plan-2026-08-05.md. Pure byte surgery -- no wasm, no
// engine, no dependencies -- because every structural difference between the two variants is
// confined to (a) the per-column record length, (b) the creature-block slot count and (c) the
// header `version` byte. Everything else is byte-identical between them and was measured
// first-hand against a real 4.8 GB version-5 world (see docs/eden-file-format.md):
//   - the 192-byte header layout (name @40, version @92, hash @96, skycolors @132, goldencubes
//     @148) is the same,
//   - intra-chunk addressing is the same: band b at +b*8192, types at +CC(x,z,y), paint at +4096,
//   - the block-ID space is the same (0..111), so NO block-ID conversion applies here. The
//     `convertType`/`convertColor` tables in FileManager.mm are for the *other* legacy path,
//     `version` outside 1..1000, which this tool refuses outright.
//
// Two hazards from the sister project's postmortem are handled explicitly, because getting either
// wrong silently corrupts large worlds rather than failing:
//   1. `chunk_offset` is a full u64. Decoded as such (Node file positions are plain numbers, safe
//      to 2^53 -- far past the 4.8 GB worlds that exist).
//   2. A column's readable span is derived from the gap to the NEXT column, never assumed to be
//      the full record size. Real files contain short spans (one observed: 107072 = 131072-24000).
//      We read only the span and zero-pad, and we always *write* full-size records, which both
//      repairs the anomaly and guarantees we never scribble into a neighbour.
//
// Usage:
//   node tools/eden-convert.js --to-256 <in.eden> [-o <out.eden>]
//   node tools/eden-convert.js --to-64  <in.eden> [-o <out.eden>] [--yes]
//   node tools/eden-convert.js --info   <in.eden>
//
//   -o <path>   output file (default: <in>.256z.eden / <in>.64z.eden). Never overwrites the input.
//   --yes       skip the interactive confirmation for the destructive --to-64 direction
//   --dry-run   analyse and report what would be discarded; write nothing
//   --force     proceed despite non-fatal structural warnings (odd stride, odd creature block)
'use strict';
const fs = require('fs');
const path = require('path');
const readline = require('readline');

// ---- format constants (docs/eden-file-format.md; Classes/FileManager.h, Classes/Constants.h) ----
const HEADER_SIZE = 192;
const CHUNK_SIZE = 16;
const BAND_BYTES = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 2; // 4096 types + 4096 colors = 8192
const DIR_ENTRY_SIZE = 16;
const ENTITY_SIZE = 60;
const BANDS_64 = 4;
const BANDS_256 = 16;
const COL_64 = BANDS_64 * BAND_BYTES; // 32768
const COL_256 = BANDS_256 * BAND_BYTES; // 131072
const CREATURES_64 = 200; // MAX_CREATURES_SAVED
const CREATURES_256 = 400; // measured from the Pherbos specimen: 24000 B / 60
const FILE_VERSION_64 = 4; // FILE_VERSION
const FILE_VERSION_256 = 5;

// header field offsets (little-endian, 4-byte alignment, u64 landing at 32)
const H_LEVEL_SEED = 0;
const H_POS_Y = 8;
const H_HOME_Y = 20;
const H_DIRECTORY_OFFSET = 32;
const H_NAME = 40;
const H_VERSION = 92;

// EntityData field offsets
const E_POS_Y = 4;
const E_TYPE = 28;

// block ids whose upper half is a separate voxel (Classes/Constants.h)
const DOOR_BOTTOM = [66, 67, 68, 69];
const DOOR_TOP = 70;
const PORTAL_BOTTOM = [75, 76, 77, 78];
const PORTAL_TOP = 79;

// ---------------------------------------------------------------- reading

function readHeader(fd) {
    const buf = Buffer.alloc(HEADER_SIZE);
    const n = fs.readSync(fd, buf, 0, HEADER_SIZE, 0);
    if (n !== HEADER_SIZE) throw new Error(`file is shorter than a ${HEADER_SIZE}-byte header`);
    const nameEnd = buf.indexOf(0, H_NAME);
    return {
        raw: buf,
        levelSeed: buf.readInt32LE(H_LEVEL_SEED),
        posY: buf.readFloatLE(H_POS_Y),
        homeY: buf.readFloatLE(H_HOME_Y),
        directoryOffset: Number(buf.readBigUInt64LE(H_DIRECTORY_OFFSET)),
        name: buf.toString('utf8', H_NAME, nameEnd < 0 || nameEnd > H_NAME + 50 ? H_NAME + 50 : nameEnd),
        version: buf.readInt32LE(H_VERSION),
    };
}

// version >= 5 -> 256z, <= 4 -> 64z (sister project's `parse_world_inner`). Versions 5 and 6 are
// both seen in the wild; what distinguishes 6 is unknown, so 6 is treated as 256z and PRESERVED on
// write rather than normalised to 5.
function detectBands(header, entries, fileSize) {
    if (header.version < 1 || header.version > 1000) {
        throw new Error(
            `header version ${header.version} is outside 1..1000 -- this is a 1.x legacy world ` +
            `(1 byte/block, no colors). Load it in the game once to let convertFile() upgrade it, then retry.`);
    }
    if (header.version >= FILE_VERSION_256) return { bands: BANDS_256, why: `header version ${header.version} >= 5` };
    // Fallback for a version that lies: the minimum gap between sorted offsets tells the truth.
    const offs = entries.map((e) => e.offset).sort((a, b) => a - b);
    let minGap = Infinity;
    for (let i = 1; i < offs.length; i++) minGap = Math.min(minGap, offs[i] - offs[i - 1]);
    if (offs.length > 1 && minGap >= COL_256) {
        return { bands: BANDS_256, why: `header version ${header.version} but minimum column gap is ${minGap} >= ${COL_256}` };
    }
    void fileSize;
    return { bands: BANDS_64, why: `header version ${header.version} <= 4` };
}

function readDirectory(fd, header, fileSize) {
    if (header.directoryOffset < HEADER_SIZE || header.directoryOffset > fileSize) {
        throw new Error(`directory_offset ${header.directoryOffset} is outside the file (size ${fileSize})`);
    }
    const bytes = fileSize - header.directoryOffset;
    if (bytes % DIR_ENTRY_SIZE !== 0) {
        console.warn(`  warning: directory region is ${bytes} B, not a multiple of ${DIR_ENTRY_SIZE}; trailing bytes ignored`);
    }
    const count = Math.floor(bytes / DIR_ENTRY_SIZE);
    const entries = [];
    const CHUNKED = 1 << 20;
    const buf = Buffer.alloc(Math.min(count * DIR_ENTRY_SIZE, CHUNKED) || DIR_ENTRY_SIZE);
    let read = 0;
    while (read < count) {
        const want = Math.min(count - read, Math.floor(buf.length / DIR_ENTRY_SIZE));
        const got = fs.readSync(fd, buf, 0, want * DIR_ENTRY_SIZE, header.directoryOffset + read * DIR_ENTRY_SIZE);
        if (got <= 0) break;
        for (let i = 0; i * DIR_ENTRY_SIZE < got; i++) {
            const o = i * DIR_ENTRY_SIZE;
            entries.push({
                slot: read + i,
                x: buf.readInt32LE(o),
                z: buf.readInt32LE(o + 4),
                offset: Number(buf.readBigUInt64LE(o + 8)), // full u64 -- see file header comment
            });
        }
        read += Math.floor(got / DIR_ENTRY_SIZE);
    }
    return entries;
}

// The creature block sits between the end of the last column and the directory. Derive its size
// from the file rather than trusting the version: this is the one medium-confidence fact in the
// whole format (400 slots was measured from a single specimen), so make it self-checking.
function deriveCreatureBlock(header, entries, colSize, bands) {
    const defaultSlots = bands === BANDS_256 ? CREATURES_256 : CREATURES_64;
    if (header.version < 3) return { slots: 0, bytes: 0, why: 'header version < 3: no creature block' };
    if (entries.length === 0) {
        return { slots: defaultSlots, bytes: defaultSlots * ENTITY_SIZE, why: 'empty directory: assumed from version' };
    }
    const lastEnd = Math.max(...entries.map((e) => e.offset)) + colSize;
    const bytes = header.directoryOffset - lastEnd;
    if (bytes < 0 || bytes % ENTITY_SIZE !== 0) {
        return {
            slots: defaultSlots,
            bytes: defaultSlots * ENTITY_SIZE,
            why: `gap of ${bytes} B before the directory is not a whole number of 60-byte slots; assumed from version`,
            suspect: true,
        };
    }
    return { slots: bytes / ENTITY_SIZE, bytes, why: `derived from the file: directory_offset - (last column end) = ${bytes} B` };
}

// A column's readable span is the gap to the next column, clamped to the record size. Never assume
// the full record: real files contain short spans, and reading past one steals a neighbour's bytes.
function computeSpans(entries, header, creatureBytes, colSize) {
    const sorted = entries.slice().sort((a, b) => a.offset - b.offset);
    const blockDataEnd = header.directoryOffset - creatureBytes;
    for (let i = 0; i < sorted.length; i++) {
        const next = i + 1 < sorted.length ? sorted[i + 1].offset : blockDataEnd;
        sorted[i].span = Math.max(0, Math.min(colSize, next - sorted[i].offset));
    }
    return sorted;
}

function readColumn(fd, entry, colSize) {
    const buf = Buffer.alloc(colSize); // zero-filled: a short span reads as air, not as a neighbour
    if (entry.span > 0) fs.readSync(fd, buf, 0, entry.span, entry.offset);
    return buf;
}

function readCreatures(fd, header, creature) {
    const buf = Buffer.alloc(creature.bytes);
    if (creature.bytes > 0) {
        const start = header.directoryOffset - creature.bytes;
        if (start >= HEADER_SIZE) fs.readSync(fd, buf, 0, creature.bytes, start);
        else buf.fill(0);
    }
    return buf;
}

function emptySlot(buf, o) {
    buf.fill(0, o, o + ENTITY_SIZE);
    buf.writeInt32LE(-1, o + E_TYPE); // -1 = empty; 0 would be a live creature of type 0
}

// ---------------------------------------------------------------- writing

class Writer {
    constructor(outPath, header) {
        this.fd = fs.openSync(outPath, 'w');
        this.pos = 0;
        this.header = Buffer.from(header.raw); // copied verbatim, then patched
        this.write(this.header); // placeholder; rewritten at close with the final directory_offset
    }
    write(buf) {
        let off = 0;
        while (off < buf.length) off += fs.writeSync(this.fd, buf, off, buf.length - off, this.pos + off);
        this.pos += buf.length;
    }
    finish(directoryOffset, version) {
        this.header.writeBigUInt64LE(BigInt(directoryOffset), H_DIRECTORY_OFFSET);
        this.header.writeInt32LE(version, H_VERSION);
        fs.writeSync(this.fd, this.header, 0, HEADER_SIZE, 0);
        fs.closeSync(this.fd);
    }
}

function writeDirectory(writer, entries) {
    // Directory entries keep their ORIGINAL slot order, only the offsets change. Preserving order
    // is what makes 64z -> 256z -> 64z byte-identical.
    const buf = Buffer.alloc(entries.length * DIR_ENTRY_SIZE);
    for (const e of entries) {
        const o = e.slot * DIR_ENTRY_SIZE;
        buf.writeInt32LE(e.x, o);
        buf.writeInt32LE(e.z, o + 4);
        buf.writeBigUInt64LE(BigInt(e.newOffset), o + 8);
    }
    writer.write(buf);
}

// ---------------------------------------------------------------- conversions

function convertTo256(src, opts) {
    const { fd, header, entries, colSize, creature, sorted } = src;
    if (src.bands === BANDS_256) throw new Error('input is already 256z');

    const outSlots = CREATURES_256;
    const outCreatureBytes = outSlots * ENTITY_SIZE;
    const report = {
        columns: sorted.length,
        shortSpans: sorted.filter((e) => e.span < colSize).length,
        creatureSlotsIn: creature.slots,
        creatureSlotsOut: outSlots,
        outSize: HEADER_SIZE + sorted.length * COL_256 + outCreatureBytes + entries.length * DIR_ENTRY_SIZE,
    };
    if (opts.dryRun) return report;

    const w = new Writer(opts.out, header);
    const pad = Buffer.alloc(COL_256 - COL_64); // bands 4..15: all air, unpainted
    for (const e of sorted) {
        e.newOffset = w.pos;
        w.write(readColumn(fd, e, colSize));
        w.write(pad);
    }
    // creature block: the source's slots verbatim, then empty slots out to 400
    const cre = Buffer.alloc(outCreatureBytes);
    const srcCre = readCreatures(fd, header, creature);
    srcCre.copy(cre, 0, 0, Math.min(srcCre.length, outCreatureBytes));
    for (let i = Math.min(creature.slots, outSlots); i < outSlots; i++) emptySlot(cre, i * ENTITY_SIZE);
    w.write(cre);

    const directoryOffset = w.pos;
    writeDirectory(w, sorted);
    // A version >= 5 file is 256z by definition; there is no "was 3, keep 3" case to preserve here
    // because the creature block we just wrote is unconditionally present.
    w.finish(directoryOffset, FILE_VERSION_256);
    report.directoryOffset = directoryOffset;
    return report;
}

function analyseAndConvertTo64(src, opts) {
    const { fd, header, entries, colSize, creature, sorted } = src;
    if (src.bands === BANDS_64) throw new Error('input is already 64z');

    const outSlots = CREATURES_64;
    const outCreatureBytes = outSlots * ENTITY_SIZE;
    const report = {
        columns: sorted.length,
        shortSpans: sorted.filter((e) => e.span < colSize).length,
        blocksDiscarded: 0,
        columnsAffected: 0,
        doorsOrphaned: 0,
        creaturesDropped: 0,
        creaturesRelocated: 0,
        creaturesOverflow: 0,
        posClamped: false,
        homeClamped: false,
        outSize: HEADER_SIZE + sorted.length * COL_64 + outCreatureBytes + entries.length * DIR_ENTRY_SIZE,
    };

    // The header is written first so the columns can be streamed straight out below -- a 256z world
    // can be gigabytes, so nothing here may accumulate whole columns in memory.
    const w = opts.dryRun ? null : new Writer(opts.out, header);

    for (const e of sorted) {
        const col = readColumn(fd, e, colSize);
        let lost = 0;
        for (let b = BANDS_64; b < BANDS_256; b++) {
            const base = b * BAND_BYTES;
            for (let i = 0; i < BAND_BYTES / 2; i++) if (col[base + i] !== 0) lost++;
        }
        if (lost > 0) { report.blocksDiscarded += lost; report.columnsAffected++; }

        // A door/portal bottom in the top retained layer (world y=63) whose *_TOP lived at y=64 is
        // orphaned by the cut; clear it rather than leave a half-door behind.
        const topBand = (BANDS_64 - 1) * BAND_BYTES;
        const aboveBand = BANDS_64 * BAND_BYTES;
        for (let lx = 0; lx < CHUNK_SIZE; lx++) {
            for (let lz = 0; lz < CHUNK_SIZE; lz++) {
                const cc = lx * 256 + lz * 16;
                const bottom = col[topBand + cc + 15];
                const above = col[aboveBand + cc];
                const orphan = (DOOR_BOTTOM.includes(bottom) && above === DOOR_TOP) ||
                               (PORTAL_BOTTOM.includes(bottom) && above === PORTAL_TOP);
                if (orphan) {
                    col[topBand + cc + 15] = 0;
                    col[topBand + 4096 + cc + 15] = 0; // paint byte for the same voxel
                    report.doorsOrphaned++;
                }
            }
        }
        if (w) {
            e.newOffset = w.pos;
            w.write(col.subarray(0, COL_64)); // bands 0..3 only
        }
    }

    // ---- creatures: keep slot positions (round-trip stability), relocate survivors from >= 200 ----
    const srcCre = readCreatures(fd, header, creature);
    const cre = Buffer.alloc(outCreatureBytes);
    const free = [];
    for (let i = 0; i < outSlots; i++) {
        const o = i * ENTITY_SIZE;
        if (i * ENTITY_SIZE + ENTITY_SIZE <= srcCre.length) {
            srcCre.copy(cre, o, o, o + ENTITY_SIZE);
            const type = cre.readInt32LE(o + E_TYPE);
            const y = cre.readFloatLE(o + E_POS_Y);
            if (type !== -1 && y >= 64) { emptySlot(cre, o); report.creaturesDropped++; free.push(i); }
            else if (type === -1) free.push(i);
        } else { emptySlot(cre, o); free.push(i); }
    }
    for (let i = outSlots; i < creature.slots; i++) {
        const o = i * ENTITY_SIZE;
        if (o + ENTITY_SIZE > srcCre.length) break;
        const type = srcCre.readInt32LE(o + E_TYPE);
        const y = srcCre.readFloatLE(o + E_POS_Y);
        if (type === -1) continue;
        if (y >= 64) { report.creaturesDropped++; continue; }
        const dst = free.shift();
        if (dst === undefined) { report.creaturesOverflow++; continue; }
        srcCre.copy(cre, dst * ENTITY_SIZE, o, o + ENTITY_SIZE);
        report.creaturesRelocated++;
    }

    if (!w) return report;

    // The player and home can legally sit above the 64z ceiling in a tall world (the Pherbos
    // specimen's pos.y is 65.9); left alone they would spawn outside the world. groundPlayer()
    // re-settles on the next load. The header buffer is patched here and flushed by finish().
    if (!(header.posY < 63)) { w.header.writeFloatLE(63, H_POS_Y); report.posClamped = true; }
    if (header.posY < 0) { w.header.writeFloatLE(0, H_POS_Y); report.posClamped = true; }
    if (!(header.homeY < 63)) { w.header.writeFloatLE(63, H_HOME_Y); report.homeClamped = true; }
    if (header.homeY < 0) { w.header.writeFloatLE(0, H_HOME_Y); report.homeClamped = true; }

    w.write(cre);
    const directoryOffset = w.pos;
    writeDirectory(w, sorted);
    w.finish(directoryOffset, FILE_VERSION_64);
    report.directoryOffset = directoryOffset;
    return report;
}

// ---------------------------------------------------------------- driver

function openSource(inPath, opts) {
    const fd = fs.openSync(inPath, 'r');
    const fileSize = fs.fstatSync(fd).size;
    const header = readHeader(fd);
    const entries = readDirectory(fd, header, fileSize);
    const det = detectBands(header, entries, fileSize);
    const bands = det.bands;
    const colSize = bands * BAND_BYTES;
    const creature = deriveCreatureBlock(header, entries, colSize, bands);
    const sorted = computeSpans(entries, header, creature.bytes, colSize);

    // The bundled Eden.eden template stores RLE-compressed columns of wildly varying length; this
    // tool does raw byte surgery only and would mangle it. Any file whose columns are not on a
    // clean record stride is either that, or damaged.
    const odd = sorted.filter((e, i) => i < sorted.length - 1 && e.span !== colSize).length;
    if (odd > 1 && !opts.force) {
        throw new Error(
            `${odd} of ${sorted.length} columns are not ${colSize} B long. This looks like the ` +
            `RLE-compressed bundled template (or a damaged file), not a user save. Refusing; pass --force to override.`);
    }
    if (creature.suspect && !opts.force) {
        console.warn(`  warning: ${creature.why}`);
    }
    return { fd, fileSize, header, entries, bands, colSize, creature, sorted, why: det.why };
}

function describe(src) {
    const h = src.header;
    console.log(`  name              ${JSON.stringify(h.name)}`);
    console.log(`  version           ${h.version}  ->  ${src.bands === BANDS_256 ? '256z' : '64z'} (${src.why})`);
    console.log(`  level_seed        ${h.levelSeed}`);
    console.log(`  player y / home y ${h.posY.toFixed(2)} / ${h.homeY.toFixed(2)}`);
    console.log(`  file size         ${src.fileSize.toLocaleString()} B`);
    console.log(`  directory         ${src.entries.length} columns at ${h.directoryOffset.toLocaleString()}`);
    console.log(`  column record     ${src.colSize.toLocaleString()} B (${src.bands} bands)`);
    const short = src.sorted.filter((e) => e.span < src.colSize);
    if (short.length) console.log(`  SHORT SPANS       ${short.length} (e.g. ${short[0].span} B at ${short[0].offset}) -- zero-padded on read`);
    console.log(`  creature block    ${src.creature.slots} slots / ${src.creature.bytes} B (${src.creature.why})`);
}

function confirm(question) {
    if (!process.stdin.isTTY) {
        return Promise.reject(new Error('destructive conversion needs confirmation; re-run with --yes (stdin is not a TTY)'));
    }
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
    return new Promise((resolve) => rl.question(question, (a) => { rl.close(); resolve(/^y(es)?$/i.test(a.trim())); }));
}

async function main() {
    const argv = process.argv.slice(2);
    const opts = {
        to256: argv.includes('--to-256'),
        to64: argv.includes('--to-64'),
        info: argv.includes('--info'),
        yes: argv.includes('--yes'),
        dryRun: argv.includes('--dry-run'),
        force: argv.includes('--force'),
    };
    const oIdx = argv.indexOf('-o');
    if (oIdx >= 0) opts.out = argv[oIdx + 1];
    const positional = argv.filter((a, i) => !a.startsWith('-') && i !== oIdx + 1);
    const inPath = positional[0];

    const modes = [opts.to256, opts.to64, opts.info].filter(Boolean).length;
    if (!inPath || modes !== 1) {
        console.error('usage: node tools/eden-convert.js (--to-256 | --to-64 | --info) <in.eden> [-o out.eden] [--yes] [--dry-run] [--force]');
        process.exit(2);
    }
    if (!opts.out) {
        const ext = path.extname(inPath);
        opts.out = inPath.slice(0, inPath.length - ext.length) + (opts.to256 ? '.256z' : '.64z') + (ext || '.eden');
    }
    if (!opts.info && path.resolve(opts.out) === path.resolve(inPath)) {
        console.error('refusing to write the output over the input');
        process.exit(2);
    }

    console.log(`reading ${inPath}`);
    const src = openSource(inPath, opts);
    describe(src);
    if (opts.info) { fs.closeSync(src.fd); return; }

    if (opts.to256) {
        const r = convertTo256(src, opts);
        console.log(opts.dryRun ? '\nwould write 64z -> 256z:' : '\nwrote 64z -> 256z:');
        console.log(`  columns           ${r.columns} rewritten at ${COL_256.toLocaleString()} B (12 air bands appended each)`);
        if (r.shortSpans) console.log(`  short spans fixed ${r.shortSpans}`);
        console.log(`  creature block    ${r.creatureSlotsIn} -> ${r.creatureSlotsOut} slots`);
        console.log(`  ${opts.dryRun ? 'size would be' : 'output'}          ${opts.dryRun ? r.outSize.toLocaleString() + ' B' : opts.out + '  (' + r.outSize.toLocaleString() + ' B)'}`);
    } else {
        // Measure first, then ask: the point of the confirmation is the number.
        const preview = analyseAndConvertTo64(src, { ...opts, dryRun: true });
        console.log('\n256z -> 64z DISCARDS everything above y=63:');
        console.log(`  non-air blocks    ${preview.blocksDiscarded.toLocaleString()} destroyed, across ${preview.columnsAffected} of ${preview.columns} columns`);
        console.log(`  creatures         ${preview.creaturesDropped} above the ceiling would be dropped`);
        console.log(`  file size         ${src.fileSize.toLocaleString()} -> ${preview.outSize.toLocaleString()} B`);
        if (!opts.dryRun) {
            if (!opts.yes && !(await confirm('proceed? [y/N] '))) { console.log('aborted'); fs.closeSync(src.fd); return; }
            const r = analyseAndConvertTo64(src, opts);
            console.log('\nwrote 256z -> 64z:');
            console.log(`  columns           ${r.columns} rewritten at ${COL_64.toLocaleString()} B`);
            console.log(`  orphaned doors    ${r.doorsOrphaned} cleared (top half was cut)`);
            console.log(`  creatures         ${r.creaturesDropped} dropped, ${r.creaturesRelocated} relocated into free slots, ${r.creaturesOverflow} lost to overflow`);
            if (r.posClamped || r.homeClamped) console.log(`  clamped           ${[r.posClamped && 'player y', r.homeClamped && 'home y'].filter(Boolean).join(', ')} into [0,63]`);
            console.log(`  output            ${opts.out}  (${r.outSize.toLocaleString()} B)`);
        }
    }
    fs.closeSync(src.fd);
}

main().catch((e) => { console.error('error: ' + e.message); process.exit(1); });
