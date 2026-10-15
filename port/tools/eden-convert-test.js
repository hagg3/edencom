#!/usr/bin/env node
// Self-contained test for tools/eden-convert.js. No engine, no wasm, no fixture files in the repo:
// it synthesises `.eden` worlds byte by byte from docs/eden-file-format.md, converts them, and
// checks the results. Run: node tools/eden-convert-test.js
//
// What it proves:
//   1. 64z -> 256z -> 64z is BYTE-IDENTICAL to the original (the round-trip property the plan asks
//      for -- including the header, since both ends stamp version 4).
//   2. The 256z output is structurally what the format says: version 5, 131072-byte stride,
//      24000-byte (400-slot) creature block, source bands preserved, bands 4..15 all air.
//   3. A deliberately SHORT column (107072 B, the anomaly measured in a real New Dawn world) is
//      read as zero-padded rather than stealing 24000 bytes of its neighbour -- and the neighbour
//      itself survives untouched. This is the failure mode that silently corrupts big worlds.
//   4. 256z -> 64z discards exactly the blocks above y=63, counts them, clears door/portal bottoms
//      whose top half was cut, clamps an out-of-range player/home y, and drops/relocates creatures.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const CONVERT = path.join(__dirname, 'eden-convert.js');
const HEADER_SIZE = 192, BAND_BYTES = 8192, DIR_ENTRY_SIZE = 16, ENTITY_SIZE = 60;
const COL_64 = 4 * BAND_BYTES, COL_256 = 16 * BAND_BYTES;
const H_POS_Y = 8, H_HOME_Y = 20, H_DIR_OFF = 32, H_NAME = 40, H_VERSION = 92;
const E_POS_Y = 4, E_TYPE = 28;

let failures = 0;
function check(label, cond, extra) {
    console.log(`  ${cond ? 'ok  ' : 'FAIL'}  ${label}${cond || extra === undefined ? '' : `  (${extra})`}`);
    if (!cond) failures++;
}

// ------------------------------------------------------------------ fixture construction

function makeHeader({ version, directoryOffset, posY, homeY, name }) {
    const h = Buffer.alloc(HEADER_SIZE);
    h.writeInt32LE(333333, 0);              // level_seed
    h.writeFloatLE(100.5, 4); h.writeFloatLE(posY, H_POS_Y); h.writeFloatLE(-40.25, 12); // pos
    h.writeFloatLE(101, 16); h.writeFloatLE(homeY, H_HOME_Y); h.writeFloatLE(-41, 24);   // home
    h.writeFloatLE(37.5, 28);               // yaw
    h.writeBigUInt64LE(BigInt(directoryOffset), H_DIR_OFF);
    h.write(name, H_NAME, 'utf8');
    h.writeInt32LE(version, H_VERSION);
    h.write('0123456789abcdef0123456789abcdef', 96, 'utf8');   // hash
    for (let i = 0; i < 16; i++) h[132 + i] = i % 5;           // skycolors
    h.writeInt32LE(7, 148);                                    // goldencubes
    return h;
}

// Deterministic pseudo-random fill so runs are reproducible.
function rng(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function makeColumn(bands, seed, opts = {}) {
    const col = Buffer.alloc(bands * BAND_BYTES);
    const r = rng(seed);
    for (let b = 0; b < bands; b++) {
        const base = b * BAND_BYTES;
        for (let i = 0; i < 4096; i++) {
            // keep every id inside the legal 0..111 space so the data is plausible
            col[base + i] = Math.floor(r() * 60);
            col[base + 4096 + i] = Math.floor(r() * 8);
        }
    }
    if (opts.doorAtCeiling) {
        // door bottom at world y=63 (band 3, local y=15) with its top at y=64 (band 4, local y=0)
        const cc = 2 * 256 + 3 * 16;
        col[3 * BAND_BYTES + cc + 15] = 66;      // TYPE_DOOR1
        col[3 * BAND_BYTES + 4096 + cc + 15] = 4;
        if (bands > 4) col[4 * BAND_BYTES + cc] = 70; // TYPE_DOOR_TOP
    }
    return col;
}

function makeCreatures(slots, live) {
    const cre = Buffer.alloc(slots * ENTITY_SIZE);
    for (let i = 0; i < slots; i++) cre.writeInt32LE(-1, i * ENTITY_SIZE + E_TYPE);
    for (const { slot, y, type } of live) {
        const o = slot * ENTITY_SIZE;
        cre.writeFloatLE(10 + slot, o);          // pos.x
        cre.writeFloatLE(y, o + E_POS_Y);
        cre.writeFloatLE(-10 - slot, o + 8);     // pos.z
        cre.writeInt32LE(type, o + E_TYPE);
        cre.writeInt32LE(3, o + 32);             // color
    }
    return cre;
}

// columns: [{x, z, data, span}] -- span < data.length writes a short record (the anomaly).
function writeWorld(file, { version, bands, columns, creatureSlots, live, posY, homeY, name }) {
    const colSize = bands * BAND_BYTES;
    const parts = [];
    let pos = HEADER_SIZE;
    const dir = Buffer.alloc(columns.length * DIR_ENTRY_SIZE);
    columns.forEach((c, i) => {
        const span = c.span === undefined ? colSize : c.span;
        parts.push(c.data.subarray(0, span));
        dir.writeInt32LE(c.x, i * DIR_ENTRY_SIZE);
        dir.writeInt32LE(c.z, i * DIR_ENTRY_SIZE + 4);
        dir.writeBigUInt64LE(BigInt(pos), i * DIR_ENTRY_SIZE + 8);
        pos += span;
    });
    // The last column always occupies its full record, so the creature block lands where the
    // reader's `directory_offset - (last offset + colSize)` derivation expects it.
    const cre = makeCreatures(creatureSlots, live);
    parts.push(cre);
    pos += cre.length;
    const header = makeHeader({ version, directoryOffset: pos, posY, homeY, name });
    fs.writeFileSync(file, Buffer.concat([header, ...parts, dir]));
    return { directoryOffset: pos, colSize };
}

// ------------------------------------------------------------------ reading back

function parse(file) {
    const buf = fs.readFileSync(file);
    const directoryOffset = Number(buf.readBigUInt64LE(H_DIR_OFF));
    const version = buf.readInt32LE(H_VERSION);
    const colSize = version >= 5 ? COL_256 : COL_64;
    const entries = [];
    for (let o = directoryOffset; o + DIR_ENTRY_SIZE <= buf.length; o += DIR_ENTRY_SIZE) {
        entries.push({ x: buf.readInt32LE(o), z: buf.readInt32LE(o + 4), offset: Number(buf.readBigUInt64LE(o + 8)) });
    }
    const lastEnd = Math.max(...entries.map((e) => e.offset)) + colSize;
    const creatureBytes = directoryOffset - lastEnd;
    return {
        buf, version, directoryOffset, entries, colSize, creatureBytes,
        posY: buf.readFloatLE(H_POS_Y), homeY: buf.readFloatLE(H_HOME_Y),
        column: (i) => buf.subarray(entries[i].offset, entries[i].offset + colSize),
        creatures: () => buf.subarray(lastEnd, directoryOffset),
    };
}

function run(args) {
    return execFileSync(process.execPath, [CONVERT, ...args], { encoding: 'utf8' });
}

// ------------------------------------------------------------------ tests

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'eden-convert-'));
console.log(`workdir ${tmp}\n`);

// ---- 1: 64z -> 256z -> 64z round-trip -------------------------------------------------------
console.log('64z -> 256z -> 64z round-trip');
const src64 = path.join(tmp, 'round.eden');
writeWorld(src64, {
    version: 4, bands: 4, creatureSlots: 200, posY: 22.5, homeY: 20, name: 'round trip world',
    columns: [
        { x: 100, z: 200, data: makeColumn(4, 1) },
        { x: 101, z: 200, data: makeColumn(4, 2) },
        { x: 100, z: 201, data: makeColumn(4, 3) },
    ],
    // deliberately leaves holes between live slots: the converter must NOT compact them, or the
    // round-trip stops being byte-identical
    live: [{ slot: 0, y: 30, type: 2 }, { slot: 5, y: 12, type: 4 }, { slot: 199, y: 8, type: 1 }],
});
const mid = path.join(tmp, 'round.256z.eden');
const back = path.join(tmp, 'round.back.eden');
run(['--to-256', src64, '-o', mid]);
run(['--to-64', mid, '-o', back, '--yes']);
check('round-trip is byte-identical', fs.readFileSync(src64).equals(fs.readFileSync(back)));

// ---- 2: the 256z intermediate is structurally correct ----------------------------------------
console.log('\n256z output structure');
const m = parse(mid);
const orig = parse(src64);
check('version is 5', m.version === 5, m.version);
check('column stride is 131072', m.entries[1].offset - m.entries[0].offset === COL_256, m.entries[1].offset - m.entries[0].offset);
check('creature block is 24000 B (400 slots)', m.creatureBytes === 24000, m.creatureBytes);
check('directory keeps its entry order and coords',
    m.entries.map((e) => `${e.x},${e.z}`).join(' ') === orig.entries.map((e) => `${e.x},${e.z}`).join(' '));
check('source bands 0..3 preserved verbatim', m.column(1).subarray(0, COL_64).equals(orig.column(1)));
check('bands 4..15 are all air', m.column(1).subarray(COL_64).every((b) => b === 0));
check('live creature in slot 5 survived', m.creatures().readInt32LE(5 * ENTITY_SIZE + E_TYPE) === 4);
check('padded slots 200..399 are empty', (() => {
    const c = m.creatures();
    for (let i = 200; i < 400; i++) if (c.readInt32LE(i * ENTITY_SIZE + E_TYPE) !== -1) return false;
    return true;
})());

// ---- 3: a real 256z world, including the short-span anomaly ----------------------------------
console.log('\n256z -> 64z (tall world with a short column)');
const tall = path.join(tmp, 'tall.eden');
const colA = makeColumn(16, 11, { doorAtCeiling: true });
const colB = makeColumn(16, 12);      // the short one: only 107072 B on disk
const colC = makeColumn(16, 13);      // its neighbour, which a naive reader would eat into
writeWorld(tall, {
    version: 5, bands: 16, creatureSlots: 400, posY: 65.93, homeY: 64.93, name: 'tall world',
    columns: [
        { x: 4026, z: 3942, data: colA },
        { x: 4027, z: 3942, data: colB, span: 107072 },
        { x: 4028, z: 3942, data: colC },
    ],
    live: [
        { slot: 1, y: 20, type: 2 },     // survives in place
        { slot: 2, y: 128, type: 3 },    // above the 64z ceiling -> dropped
        { slot: 300, y: 30, type: 5 },   // beyond 200 slots -> relocated into a free slot
        { slot: 301, y: 200, type: 6 },  // beyond 200 AND too high -> dropped
    ],
});
const tallOut = path.join(tmp, 'tall.64z.eden');
const report = run(['--to-64', tall, '-o', tallOut, '--yes']);
const t = parse(tallOut);
const tallSrc = fs.readFileSync(tall);

check('version stamped back to 4', t.version === 4, t.version);
check('column stride is 32768', t.entries[1].offset - t.entries[0].offset === COL_64);
check('creature block is 12000 B', t.creatureBytes === 12000, t.creatureBytes);
check('player y clamped into range', t.posY === 63, t.posY);
check('home y clamped into range', t.homeY === 63, t.homeY);

// bands 0..3 of a full-length column come through untouched
check('full-length column survives verbatim', t.column(2).equals(colC.subarray(0, COL_64)));

// the short column: its first 107072 bytes are real, so bands 0..3 (32768 B) are entirely present
check('short column bands 0..3 are its own bytes', t.column(1).equals(colB.subarray(0, COL_64)));
// and the neighbour that a naive 131072-byte read would have swallowed is intact on disk
const cOffset = Number(tallSrc.readBigUInt64LE(Number(tallSrc.readBigUInt64LE(H_DIR_OFF)) + 2 * DIR_ENTRY_SIZE + 8));
check('neighbour column untouched in the source', tallSrc.subarray(cOffset, cOffset + COL_256).equals(colC));

// door/portal orphan handling
const ccDoor = 2 * 256 + 3 * 16 + 15;
check('orphaned door bottom cleared', t.column(0)[3 * BAND_BYTES + ccDoor] === 0, t.column(0)[3 * BAND_BYTES + ccDoor]);
check('its paint byte cleared too', t.column(0)[3 * BAND_BYTES + 4096 + ccDoor] === 0);
check('report counted 1 orphaned door', /orphaned doors\s+1 cleared/.test(report), report.match(/orphaned doors.*/));

// creatures
const tc = t.creatures();
check('slot 1 creature kept in place', tc.readInt32LE(1 * ENTITY_SIZE + E_TYPE) === 2);
check('slot 2 creature (y=128) dropped', tc.readInt32LE(2 * ENTITY_SIZE + E_TYPE) === -1);
check('one creature relocated from slot 300', /1 relocated/.test(report), report.match(/creatures .*/));
check('relocated creature landed in a free slot', (() => {
    for (let i = 0; i < 200; i++) if (tc.readInt32LE(i * ENTITY_SIZE + E_TYPE) === 5) return true;
    return false;
})());
check('report counted 2 creatures dropped', /2 dropped/.test(report), report.match(/creatures .*/));

// Discarded-block accounting: count the non-air type bytes above y=63 ourselves. The part of
// column B past its 107072-byte span was never on disk, so it must read as air and NOT be counted.
let expectDiscarded = 0;
for (const [col, span] of [[colA, COL_256], [colB, 107072], [colC, COL_256]]) {
    for (let b = 4; b < 16; b++) for (let i = 0; i < 4096; i++) {
        const off = b * BAND_BYTES + i;
        if (off < span && col[off] !== 0) expectDiscarded++;
    }
}
const reported = Number((report.match(/non-air blocks\s+([\d,]+)/) || [])[1].replace(/,/g, ''));
check('discarded non-air block count is exact', reported === expectDiscarded, `reported ${reported}, expected ${expectDiscarded}`);

// ---- 4: a 256z world with NO creature block --------------------------------------------------
// The sister editor's own worldgen writes version-5 files with no creature block at all, so the
// derived-size logic must cope with a zero-byte gap rather than assuming 400 slots are there.
console.log('\n256z with no creature block');
const noCre = path.join(tmp, 'nocreatures.eden');
writeWorld(noCre, {
    version: 5, bands: 16, creatureSlots: 0, live: [], posY: 40, homeY: 40, name: 'no creatures',
    columns: [{ x: 1, z: 1, data: makeColumn(16, 21) }, { x: 2, z: 1, data: makeColumn(16, 22) }],
});
check('--info reports 0 slots', /creature block\s+0 slots/.test(run(['--info', noCre])));
const noCreOut = path.join(tmp, 'nocreatures.64z.eden');
run(['--to-64', noCre, '-o', noCreOut, '--yes']);
const nc = parse(noCreOut);
check('a full 200-slot block is synthesised', nc.creatureBytes === 12000, nc.creatureBytes);
check('every synthesised slot is empty', (() => {
    const c = nc.creatures();
    for (let i = 0; i < 200; i++) if (c.readInt32LE(i * ENTITY_SIZE + E_TYPE) !== -1) return false;
    return true;
})());
check('its columns still truncate correctly', nc.column(1).equals(makeColumn(16, 22).subarray(0, COL_64)));

// ---- 5: guards ------------------------------------------------------------------------------
console.log('\nguards');
function expectFailure(label, args, pattern) {
    try { run(args); check(label, false, 'command unexpectedly succeeded'); }
    catch (e) { const out = String(e.stdout || '') + String(e.stderr || ''); check(label, pattern.test(out), out.trim().split('\n').pop()); }
}
expectFailure('refuses to convert 64z with --to-64', ['--to-64', src64, '-o', path.join(tmp, 'x.eden'), '--yes'], /already 64z/);
expectFailure('refuses to convert 256z with --to-256', ['--to-256', tall, '-o', path.join(tmp, 'x.eden')], /already 256z/);
expectFailure('refuses to overwrite its input', ['--to-256', src64, '-o', src64], /over the input/);
const legacy = path.join(tmp, 'legacy.eden');
{
    const b = fs.readFileSync(src64);
    b.writeInt32LE(999999, H_VERSION);
    fs.writeFileSync(legacy, b);
}
expectFailure('refuses a 1.x legacy world', ['--to-256', legacy, '-o', path.join(tmp, 'x.eden')], /outside 1\.\.1000/);
const bundled = path.join(__dirname, '..', '..', 'Eden.eden');
if (fs.existsSync(bundled)) {
    expectFailure('refuses the RLE-compressed bundled Eden.eden', ['--to-256', bundled, '-o', path.join(tmp, 'x.eden')], /RLE-compressed|Refusing/);
}

console.log(`\n${failures === 0 ? 'PASS' : 'FAIL'}: ${failures} failing check(s)`);
if (failures === 0) fs.rmSync(tmp, { recursive: true, force: true });
process.exit(failures === 0 ? 0 : 1);
