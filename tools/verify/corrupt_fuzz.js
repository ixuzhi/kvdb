#!/usr/bin/env node
// corrupt_fuzz.js - structure-aware corruption fuzzer. The property under
// test is NOT "matches leveldb's answers" (that is what the ported suite
// checks) but ROBUSTNESS: after arbitrary byte damage to a closed database,
// reopening must never crash, never hang, and never abort - the only legal
// outcomes are a clean open (possibly with a shorter read or an iterator
// error) or a refused open with a Corruption/IO status.
//
// Method: copy a healthy fixture, flip K seeded-random bytes in one random
// file (weighted toward .ldb data regions, WALs and the MANIFEST), then run
// reopen_probe.exe in a child process with a timeout. Exit codes >= 128 or a
// timeout are FAILURES regardless of what the engine "meant" to do.
//
// Usage: node corrupt_fuzz.js [iterations] [probe-exe]
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const REPO = path.resolve(__dirname, '..', '..');
const FIXTURE = path.join(REPO, 'build', 'verify', 'fx-kvdb');
const WORK = path.join(REPO, 'build', 'verify', 'fuzz-work');
const PROBE = process.argv[3] || path.join(REPO, 'build', 'verify', 'reopen_probe.exe');
const N = parseInt(process.argv[2] || '150', 10);

let seed = 0x9e3779b9;
const rnd = () => {
  seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
  return (seed >>> 0) / 4294967296;
};
const pick = (a) => a[Math.floor(rnd() * a.length) % a.length];

const hist = { open_ok: 0, open_refused: 0, crash: 0, hang: 0, other: 0 };
const failures = [];

for (let iter = 0; iter < N; iter++) {
  fs.rmSync(WORK, { recursive: true, force: true });
  fs.cpSync(FIXTURE, WORK, { recursive: true });
  const files = fs.readdirSync(WORK).filter(f => /^\d{6}\.(ldb|log)$/.test(f) || /^MANIFEST-/.test(f));
  if (!files.length) { console.error('fixture has no data files'); process.exit(2); }
  // weight: MANIFEST 25%, .ldb 45%, .log 30%
  const r = rnd();
  const name = r < 0.25 ? files.find(f => /^MANIFEST-/.test(f))
              : r < 0.70 ? files.find(f => f.endsWith('.ldb'))
              : files.find(f => f.endsWith('.log'));
  const fp = path.join(WORK, name);
  const buf = fs.readFileSync(fp);
  const flips = 1 + Math.floor(rnd() * 8);
  const spots = [];
  for (let k = 0; k < flips; k++) {
    const off = Math.floor(rnd() * buf.length);
    buf[off] ^= 1 << Math.floor(rnd() * 8);
    spots.push(off);
  }
  fs.writeFileSync(fp, buf);

  let rc = -1, out = '';
  try {
    out = execFileSync(PROBE, [WORK], { timeout: 15000, encoding: 'utf8' });
    rc = 0;
  } catch (e) {
    if (e.killed || e.signal) { rc = 124; }
    else { rc = e.status === undefined ? -1 : e.status; out = (e.stdout || '') + (e.stderr || ''); }
  }
  if (rc === 0) hist.open_ok++;
  else if (rc === 4) hist.open_refused++;
  else if (rc === 124) { hist.hang++; failures.push({ iter, name, spots, why: 'TIMEOUT' }); }
  else if (rc < 0 || rc >= 128) { hist.crash++; failures.push({ iter, name, spots, rc, out: out.slice(0, 200) }); }
  else { hist.other++; failures.push({ iter, name, spots, rc, out: out.slice(0, 200) }); }
}

console.log(`corrupt_fuzz: ${N} iterations over ${path.basename(FIXTURE)}`);
console.log(`  opened:            ${hist.open_ok}`);
console.log(`  refused (legal):   ${hist.open_refused}`);
console.log(`  CRASHED (illegal): ${hist.crash}`);
console.log(`  HUNG (illegal):    ${hist.hang}`);
console.log(`  other nonzero:     ${hist.other}`);
for (const f of failures.slice(0, 10))
  console.log(`  FAIL iter=${f.iter} file=${f.name} offsets=[${f.spots}] rc=${f.rc} ${f.why || ''} ${(f.out || '').trim().slice(0, 120)}`);
process.exit(hist.crash + hist.hang + hist.other ? 1 : 0);
