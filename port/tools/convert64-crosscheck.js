#!/usr/bin/env node
// convert64-crosscheck.js -- ROADMAP W.2 (e): FileManager::convertWorldTo64 (the engine's "Convert to
// 64z", Classes/FileManager.cpp) against this directory's eden-convert.js `--to-64`, the algorithm it
// restates by hand. The native port of the retired web suite headless-256z-convert-crosscheck.js.
//
//   node port/tools/convert64-crosscheck.js <eden_native> <fresh absolute docs dir>
//
// Runs `<eden_native> --convert64-selftest --docs=<dir>`, which authors a 256z world with known
// content above y=63 (bricks, three orphaned door pairs, three creatures), exports it to a
// NewDawn256z `.eden`, converts it with the engine twice (the `.emod`, and a copy of the `.eden`)
// and prints each report plus the path of an untouched reference copy. This converts that copy with
// eden-convert.js and asserts every counter agrees across all three. Exit 0 = all agree.
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync, spawnSync } = require('child_process');

const [bin, docs] = process.argv.slice(2);
if (!bin || !docs || !path.isAbsolute(docs)) {
    console.error('usage: convert64-crosscheck.js <eden_native> <absolute docs dir>');
    process.exit(2);
}
fs.mkdirSync(docs, { recursive: true });
fs.writeFileSync(path.join(docs, '.migrated-from-documents'), '1');   // no ~/Documents scan

const run = spawnSync(bin, ['--convert64-selftest', `--docs=${docs}`], { encoding: 'utf8', maxBuffer: 64 << 20 });
process.stdout.write((run.stdout || '').split('\n').filter((l) => l.startsWith('[eden-convert64]')).join('\n') + '\n');
let failures = 0;
function check(name, ok, detail) {
    console.log(`[convert64-xcheck] ${name.padEnd(52)} ${ok ? 'PASS' : 'FAIL'}${detail ? '  ' + detail : ''}`);
    if (!ok) failures++;
}
check('eden_native --convert64-selftest passes', run.status === 0, `exit ${run.status}`);
if (run.status !== 0) { process.stdout.write(run.stderr || ''); process.exit(1); }

const grab = (re) => { const m = run.stdout.match(re); return m ? m[1] : null; };
const ref = grab(/^\[eden-convert64\] ref (.+)$/m);
const engine = {
    emod: JSON.parse(grab(/^\[eden-convert64\] engine-emod (\{.*\})$/m) || '{}'),
    eden: JSON.parse(grab(/^\[eden-convert64\] engine-eden (\{.*\})$/m) || '{}'),
};
check('the run left a reference .eden', !!ref && fs.existsSync(ref), ref || '(no ref line)');
if (!ref || !fs.existsSync(ref)) process.exit(1);

// The CLI's report lines (eden-convert.js, the `wrote 256z -> 64z` block).
const out64 = ref.replace(/\.eden$/, '.cli64.eden');
const cli = execFileSync(process.execPath, [path.join(__dirname, 'eden-convert.js'), '--to-64', ref, '-o', out64, '--yes'],
                         { encoding: 'utf8' });
const num = (re) => { const m = cli.match(re); return m ? m.slice(1).map((s) => Number(s.replace(/,/g, ''))) : null; };
const blocks = num(/non-air blocks\s+([\d,]+) destroyed, across (\d+) of (\d+) columns/);
const doors = num(/orphaned doors\s+(\d+) cleared/);
const creat = num(/creatures\s+(\d+) dropped, (\d+) relocated into free slots, (\d+) lost to overflow/);
const cliReport = {
    columns: blocks ? blocks[2] : NaN,
    blocksDiscarded: blocks ? blocks[0] : NaN,
    columnsAffected: blocks ? blocks[1] : NaN,
    doorsOrphaned: doors ? doors[0] : NaN,
    creaturesDropped: creat ? creat[0] : NaN,
    creaturesRelocated: creat ? creat[1] : NaN,
    creaturesOverflow: creat ? creat[2] : NaN,
};
console.log('[convert64-xcheck] eden-convert.js --to-64', JSON.stringify(cliReport));
check('the CLI saw real content above y=63', cliReport.blocksDiscarded > 0 && cliReport.creaturesDropped > 0,
      `${cliReport.blocksDiscarded} blocks, ${cliReport.creaturesDropped} creatures`);

for (const leg of ['emod', 'eden']) {
    const e = engine[leg];
    check(`engine (${leg}) reported ok`, e.ok === true, JSON.stringify(e));
    for (const k of Object.keys(cliReport)) {
        check(`${k} agrees (engine ${leg} ${e[k]} == CLI ${cliReport[k]})`, e[k] === cliReport[k]);
    }
}
console.log(`[convert64-xcheck] ${failures ? 'FAILURES' : 'ALL PASS'} (${failures} failure(s))`);
process.exit(failures ? 1 : 0);
