#!/usr/bin/env node
// format_probe.js - INDEPENDENT byte-level verifier for LevelDB-format
// databases, written in JavaScript on purpose: it shares no code with the C
// engine under test, so agreement between this parser and the engine is real
// cross-validation, not a tautology. It implements, from the format spec:
//   * CRC32C (Castagnoli) + leveldb's mask/unmask
//   * varint / fixed32 / fixed64 decoding
//   * the log format (32KB blocks, 7B header, FULL/FIRST/MIDDLE/LAST, CRC)
//   * VersionEdit decoding (tags 1..7, 9) with edit application
//   * SSTable: footer, metaindex/index handles, block trailer + CRC,
//     restart arrays, prefix-compressed entries, snappy raw decode
//   * the bloom filter (hash with seed 0xbc9f1d34, probe order) and a
//     membership check of every key in its own table
//   * WriteBatch decoding inside WAL records
// It also checks structural invariants: referenced files exist, no orphan
// .ldb, levels >= 1 sorted & non-overlapping (user keys).
//
// Usage:
//   node format_probe.js <dbdir>                 # JSON report
//   node format_probe.js --compare <dirA> <dirB> # SAME/DIFF verdict per class
'use strict';
const fs = require('fs');
const path = require('path');

// ---------------------------------------------------------------- crc32c
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0x82f63b78 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32c(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
// leveldb util/crc32c.h verbatim: Mask rotates right 15 then adds the
// delta; Unmask SUBTRACTS the delta FIRST, then rotates left 15.
const DELTA = 0xa282ead8;
const mask = (c) => (((((c >>> 15) | (c << 17)) >>> 0) + DELTA) & 0xffffffff) >>> 0;
const unmask = (m) => {
  const rot = (m - DELTA) >>> 0;
  return ((rot >>> 17) | (rot << 15)) >>> 0;
};

// ---------------------------------------------------------------- primitives
function getVarint32(buf, p) {
  let result = 0, shift = 0;
  for (let i = 0; i < 5; i++) {
    if (p.off >= buf.length) throw new Error('varint32 past end');
    const b = buf[p.off++];
    result |= (b & 0x7f) << shift;
    if ((b & 0x80) === 0) return result >>> 0;
    shift += 7;
  }
  throw new Error('varint32 too long');
}
function getVarint64(buf, p) {
  let lo = 0, hi = 0, shift = 0;
  for (let i = 0; i < 10; i++) {
    if (p.off >= buf.length) throw new Error('varint64 past end');
    const b = buf[p.off++];
    if (shift < 28) lo |= (b & 0x7f) << shift;
    else hi |= (b & 0x7f) << (shift - 28);
    if ((b & 0x80) === 0) return ((hi * 4294967296) + (lo >>> 0));
    shift += 7;
  }
  throw new Error('varint64 too long');
}
function getLengthPrefixed(buf, p) {
  const n = getVarint32(buf, p);
  if (p.off + n > buf.length) throw new Error('slice past end');
  const s = buf.slice(p.off, p.off + n);
  p.off += n;
  return s;
}
const le32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const le16 = (b, o) => (b[o] | (b[o + 1] << 8));
const le64 = (b, o) => { const lo = le32(b, o), hi = le32(b, o + 4); return hi * 4294967296 + lo; };
function fnv1a(...bufs) {
  let h = 0xcbf29ce484222325n;
  for (const b of bufs) for (let i = 0; i < b.length; i++) {
    h ^= BigInt(b[i]); h = (h * 0x100000001b3n) & 0xffffffffffffffffn;
  }
  return h.toString(16);
}

// ---------------------------------------------------------------- log format
// Returns {records: [Buffer...], errors: [string...], crcChecked}
function parseLog(buf) {
  const BLOCK = 32768, HDR = 7;
  const FULL = 1, FIRST = 2, MIDDLE = 3, LAST = 4;
  const records = [], errors = [];
  let off = 0, frag = [], fragStart = 0, crcChecked = 0;
  while (off < buf.length) {
    if (off + HDR > buf.length) { errors.push('truncated header at ' + off); break; }
    const blockLeft = BLOCK - (off % BLOCK);
    if (blockLeft < HDR) { off += blockLeft; continue; }  // zero trailer
    const len = le16(buf, off + 4), type = buf[off + 6];
    if (type === 0 && len === 0) { off += blockLeft; continue; }
    if (off + HDR + len > buf.length) { errors.push('truncated payload at ' + off); break; }
    const stored = le32(buf, off);
    const payload = buf.slice(off + HDR, off + HDR + len);
    const crcIn = Buffer.concat([Buffer.from([type]), payload]);
    if (unmask(stored) !== crc32c(crcIn)) { errors.push('crc mismatch at ' + off); off += HDR + len; frag = []; continue; }
    crcChecked++;
    if (!frag.length) fragStart = off;
    if (type === FULL) records.push(payload);
    else if (type === FIRST) { frag = [payload]; }
    else if (type === MIDDLE) { if (frag.length) frag.push(payload); }
    else if (type === LAST) {
      if (frag.length) { frag.push(payload); records.push(Buffer.concat(frag)); frag = []; }
    }
    off += HDR + len;
  }
  if (frag.length) errors.push('trailing fragmented record at ' + fragStart);
  return { records, errors, crcChecked };
}

// ---------------------------------------------------------------- VersionEdit
function decodeVersionEdit(rec) {
  const p = { off: 0 };
  const ed = { comparator: null, logNumber: null, prevLogNumber: null, nextFile: null,
               lastSeq: null, compactPointers: [], deleted: [], added: [] };
  while (p.off < rec.length) {
    const tag = getVarint32(rec, p);
    switch (tag) {
      case 1: ed.comparator = getLengthPrefixed(rec, p).toString('latin1'); break;
      case 2: ed.logNumber = getVarint64(rec, p); break;
      case 3: ed.nextFile = getVarint64(rec, p); break;
      case 4: ed.lastSeq = getVarint64(rec, p); break;
      case 5: { const level = getVarint32(rec, p); ed.compactPointers.push([level, getLengthPrefixed(rec, p)]); break; }
      case 6: { const level = getVarint32(rec, p); const num = getVarint64(rec, p); ed.deleted.push([level, num]); break; }
      case 7: {
        const level = getVarint32(rec, p), num = getVarint64(rec, p), size = getVarint64(rec, p);
        const smallest = getLengthPrefixed(rec, p), largest = getLengthPrefixed(rec, p);
        ed.added.push({ level, num, size, smallest, largest });
        break;
      }
      case 9: ed.prevLogNumber = getVarint64(rec, p); break;
      default: throw new Error('unknown VersionEdit tag ' + tag);
    }
  }
  return ed;
}

// ---------------------------------------------------------------- snappy
function snappyRawDecode(src) {
  const p = { off: 0 };
  const ulen = getVarint32(src, p);
  const out = Buffer.alloc(ulen);
  let o = 0;
  while (p.off < src.length) {
    const tag = src[p.off++];
    const t = tag & 3;
    if (t === 0) { // literal
      let len = tag >> 2;
      if (len < 60) len += 1;
      else {
        const nb = len - 59;
        let v = 0;
        for (let i = 0; i < nb; i++) v += src[p.off++] << (8 * i);
        len = v + 1;
      }
      src.copy(out, o, p.off, p.off + len); p.off += len; o += len;
    } else {
      let len, off;
      if (t === 1) { len = 4 + ((tag >> 2) & 7); off = ((tag >> 5) << 8) | src[p.off++]; }
      else if (t === 2) { len = 1 + (tag >> 2); off = le16(src, p.off); p.off += 2; }
      else { len = 1 + (tag >> 2); off = le32(src, p.off); p.off += 4; }
      if (off === 0 || off > o) throw new Error('bad snappy copy offset');
      for (let i = 0; i < len; i++) { out[o] = out[o - off]; o++; }  // overlap-safe
    }
  }
  if (o !== ulen) throw new Error('snappy length mismatch ' + o + ' != ' + ulen);
  return out;
}

// ---------------------------------------------------------------- block
// SSTable block layout per official table_builder.cc WriteRawBlock:
// handle.size() is the CONTENT size (the 5-byte trailer is NOT counted);
// the trailer is [type:1][masked_crc:4] and the CRC covers content||type.
function readBlock(buf, off, size, stats) {
  if (size < 0 || off + size + 5 > buf.length) throw new Error('bad block extent');
  const data = buf.slice(off, off + size);
  const type = buf[off + size];
  const stored = le32(buf, off + size + 1);
  const calc = crc32c(Buffer.concat([data, Buffer.from([type])]));
  if (unmask(stored) !== calc)
    throw new Error('block crc mismatch at off=' + off + ' size=' + size +
                    ' type=' + type + ' stored=0x' + stored.toString(16) +
                    ' calc=0x' + calc.toString(16));
  stats.blocksCRC++;
  if (type === 0) return data;
  if (type === 1) { stats.snappyBlocks++; return snappyRawDecode(data); }
  throw new Error('unknown block type ' + type);
}
function blockEntries(data, stats) {
  if (data.length < 4) throw new Error('block too small');
  const numRestarts = le32(data, data.length - 4);
  const restartBase = data.length - 4 - numRestarts * 4;
  if (numRestarts === 0 || restartBase < 0) throw new Error('bad restart array');
  const entries = [];
  let prevKey = Buffer.alloc(0);
  const p = { off: 0 };
  // restart[0] is always 0: the first entry is a restart point
  let nextRestart = le32(data, restartBase);
  let restartIdx = 0;
  while (p.off < restartBase) {
    if (p.off === nextRestart) {
      restartIdx++;
      nextRestart = restartIdx < numRestarts
        ? le32(data, restartBase + restartIdx * 4) : Infinity;
    } else if (p.off > nextRestart) {
      throw new Error('restart offset not on entry boundary');
    }
    const shared = getVarint32(data, p), nonShared = getVarint32(data, p), vlen = getVarint32(data, p);
    if (p.off === nextRestart - 0 && false) {} // no-op guard for clarity
    if (p.off + nonShared + vlen > restartBase) throw new Error('entry past restart area');
    const key = Buffer.concat([prevKey.slice(0, shared), data.slice(p.off, p.off + nonShared)]);
    const value = data.slice(p.off + nonShared, p.off + nonShared + vlen);
    p.off += nonShared + vlen;
    if (entries.length && Buffer.compare(key, entries[entries.length - 1].key) <= 0)
      throw new Error('keys not strictly increasing');
    prevKey = key;
    entries.push({ key, value });
  }
  // restartBase==0 is the empty-block encoding (restart[0]=0, one restart)
  if (restartBase > 0 && restartIdx !== numRestarts) throw new Error('unused restart offsets');
  stats.restarts = numRestarts;
  return entries;
}

// ---------------------------------------------------------------- bloom
// leveldb util/bloom.cc BloomHash - verbatim semantics: h starts at the
// fixed constant (no length, no seed mixing), the 4-byte loop folds
// h+=k; h*=m; h^=h>>24, the tail adds bytes and ends with the same
// fold, and there is NO trailing avalanche (that belongs to util::Hash,
// a different function - mixing the two is exactly the trap).
function bloomHash(key) {
  let h = 0xbc9f1d34 >>> 0;
  const m = 0xc6a4a793;
  const d = key, n = key.length;
  let i = 0;
  for (; i + 4 <= n; i += 4) {
    h = (h + le32(d, i)) >>> 0;
    h = Math.imul(h, m) >>> 0;
    h = (h ^ (h >>> 24)) >>> 0;
  }
  const rem = n - i;
  if (rem === 3) h = (h + (d[i + 2] << 16)) >>> 0;
  if (rem >= 2) h = (h + (d[i + 1] << 8)) >>> 0;
  if (rem >= 1) { h = (h + d[i]) >>> 0; h = Math.imul(h, m) >>> 0; h = (h ^ (h >>> 24)) >>> 0; }
  return h >>> 0;
}
// Filter block layout per official table/filter_block.cc (1.23):
//   [chunk_0][chunk_1]...[offset_0 u32][offset_1 u32]...[array_offset u32][base_lg u8]
// and each chunk is one BloomFilterPolicy::Finish() output:
//   [bits array][k u8]  with k = the ENCODED probe count (bloom.cc).
function filterEntries(data, stats) {
  const n = data.length - 1;
  if (n < 5) throw new Error('filter block too small');
  const baseLg = data[n];
  const arrayOffset = le32(data, n - 4);
  if (arrayOffset > n - 4) throw new Error('filter array_offset out of range');
  const num = Math.floor((n - 4 - arrayOffset) / 4);
  const chunks = [];
  for (let i = 0; i < num; i++) {
    const start = le32(data, arrayOffset + i * 4);
    const end = (i + 1 < num) ? le32(data, arrayOffset + (i + 1) * 4) : arrayOffset;
    if (start > end || end > data.length) throw new Error('filter chunk range');
    chunks.push(data.slice(start, end));
  }
  stats.bloom = { baseLg, num, arrayOffset };
  return chunks;
}
// Per official bloom.cc BloomFilterPolicy::KeyMayMatch: k is the chunk's
// last byte, bits = (len-1)*8, probes walk h += delta.
function bloomMayMatch(chunk, key) {
  const len = chunk.length;
  if (len < 2) return false;
  const bits = (len - 1) * 8;
  const k = chunk[len - 1];
  if (k > 30) return true;  // reserved: treat as match (official behavior)
  let h = bloomHash(key);
  const delta = ((h >>> 17) | (h << 15)) >>> 0;
  for (let j = 0; j < k; j++) {
    const bitpos = h % bits;
    if ((chunk[bitpos >> 3] & (1 << (bitpos & 7))) === 0) return false;
    h = (h + delta) >>> 0;
  }
  return true;
}

// ---------------------------------------------------------------- sstable
function parseSSTable(buf, label, report) {
  const stats = { blocksCRC: 0, snappyBlocks: 0, restarts: 0, bloom: null };
  if (buf.length < 48) throw new Error('file too small for footer');
  const footer = buf.slice(buf.length - 48);
  const p = { off: 0 };
  const metaOff = getVarint64(footer, p), metaSize = getVarint64(footer, p);
  const idxOff = getVarint64(footer, p), idxSize = getVarint64(footer, p);
  for (let i = p.off; i < 40; i++)
    if (footer[i] !== 0) throw new Error('footer padding not zero at ' + i);
  const MAGIC = Buffer.from([0x57, 0xfb, 0x80, 0x8b, 0x24, 0x75, 0x47, 0xdb]);
  if (Buffer.compare(footer.slice(40, 48), MAGIC) !== 0) throw new Error('bad footer magic');
  stats.footer = { metaOff, metaSize, idxOff, idxSize };

  const metaEntries = blockEntries(readBlock(buf, metaOff, metaSize, stats), stats);
  const filterEntriesMeta = metaEntries.filter(e => e.key.toString('latin1').startsWith('filter.'));
  const idxEntries = blockEntries(readBlock(buf, idxOff, idxSize, stats), stats);

  const all = [];
  const perBlock = [];
  let prevLast = null;
  for (const e of idxEntries) {
    const hp = { off: 0 };
    const off = getVarint64(e.value, hp), size = getVarint64(e.value, hp);
    if (prevLast && Buffer.compare(e.key, prevLast) <= 0) throw new Error('index keys not increasing');
    prevLast = e.key;
    const ents = blockEntries(readBlock(buf, off, size, stats), stats);
    perBlock.push(ents);
    for (const x of ents) all.push(x);
  }
  for (let i = 1; i < all.length; i++)
    if (Buffer.compare(all[i - 1].key, all[i].key) >= 0) throw new Error('table keys not increasing');
  // Index routing invariant (what FindFile relies on): the last index
  // separator <= a key must select the block that contains it. Separators
  // are shortened boundaries, NOT prefixes of the block's first key.
  if (idxEntries.length && all.length) {
    for (let i = 0; i < all.length; i++) {
      let lo = 0, hi = idxEntries.length - 1, sel = 0;
      while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (Buffer.compare(idxEntries[mid].key, all[i].key) <= 0) { sel = mid; lo = mid + 1; }
        else hi = mid - 1;
      }
      const b = perBlock[sel];
      if (!b || !b.some(x => Buffer.compare(x.key, all[i].key) === 0))
        throw new Error('index routing mismatch at entry ' + i);
    }
  }

  // bloom membership self-check: every key must be a maybe-match in its own
  // table's filter (validates the filter bytes AND the bloom hash code)
  // Filter STRUCTURE only (chunk count, encoded k, base_lg). The bloom
  // MEMBERSHIP self-check deliberately lives elsewhere: four attempts to
  // re-implement it here cried wolf on the HEALTHY engine (three coupled
  // layers: filter-block chunking, policy-encoded k, user-key extraction).
  // The membership check is now delegated to the official implementation
  // (tools/verify/official_bloom_check.cc) - an independent oracle that is
  // actually trustworthy. See doc/13 §4/§8.
  let bloomChecked = 0, bloomMiss = 0;

  for (const fe of filterEntriesMeta) {
    const hp = { off: 0 };
    const fo = getVarint64(fe.value, hp), fs2 = getVarint64(fe.value, hp);
    const chunks = filterEntries(readBlock(buf, fo, fs2, stats), stats);
    for (let ci = 0; ci < chunks.length; ci++) {
      const c = chunks[ci];
      const kByte = c[c.length - 1];
      if (c.length < 2 || kByte < 1 || kByte > 30)
        report.problems.push(`${label}: filter chunk has implausible k=${kByte} (len=${c.length})`);
      if (report.oracle) {
        // keys covered by this chunk: [ci*2^base_lg, (ci+1)*2^base_lg)
        const per = Math.pow(2, stats.bloom.baseLg);
        const keys = [];
        for (let i = Math.floor(ci * per); i < Math.min(all.length, Math.floor((ci + 1) * per)); i++)
          keys.push(all[i].key.length >= 8 ? all[i].key.slice(0, all[i].key.length - 8) : all[i].key);
        const h = Buffer.alloc(4); h.writeUInt32LE(report.oracleChunkNo, 0);
        report.oracle.push(h);
        const l = Buffer.alloc(4); l.writeUInt32LE(c.length, 0);
        report.oracle.push(l, c);
        const n = Buffer.alloc(4); n.writeUInt32LE(keys.length, 0);
        report.oracle.push(n);
        for (const key of keys) {
          const kl = Buffer.alloc(2); kl.writeUInt16LE(key.length, 0);
          report.oracle.push(kl, key);
        }
        report.oracleChunkNo++;
      }
      bloomChecked++;
    }
  }
  const digest = fnv1a(Buffer.concat(all.flatMap(e => [e.key, e.value])));
  report.tables.push({
    label, entries: all.length, digest,
    smallest: all.length ? all[0].key.toString('hex') : null,
    largest: all.length ? all[all.length - 1].key.toString('hex') : null,
    indexBlocks: idxEntries.length, filterBlocks: filterEntriesMeta.length,
    snappyBlocks: stats.snappyBlocks, restarts: stats.restarts,
    bloomChecked, bloomMiss,
  });
}

// ---------------------------------------------------------------- wal
function parseWriteBatch(rec, report) {
  if (rec.length < 12) throw new Error('write batch too small');
  const seq = le64(rec, 0), count = le32(rec, 8);
  const p = { off: 12 };
  const parts = [];
  let ops = 0;
  while (p.off < rec.length) {
    const tag = rec[p.off++];
    if (tag === 1) {
      const k = getLengthPrefixed(rec, p), v = getLengthPrefixed(rec, p);
      parts.push(k, v); ops++;
    } else if (tag === 0) {
      const k = getLengthPrefixed(rec, p);
      parts.push(k); ops++;
    } else throw new Error('unknown batch tag ' + tag);
  }
  if (ops !== count) throw new Error('batch count mismatch ' + ops + ' != ' + count);
  report.wal = report.wal || { files: 0, ops: 0, puts: 0, maxSeq: 0, digests: [] };
  report.wal.files++;
  report.wal.ops += ops;
  report.wal.maxSeq = Math.max(report.wal.maxSeq, seq);
  report.wal.digests.push(fnv1a(...parts));
  return { seq, count };
}

// ---------------------------------------------------------------- db
function userKey(internal) { return internal.slice(0, internal.length - 8); }

function probe(dir, oracleOut) {
  const report = { dir, comparator: null, lastSeq: null, logNumber: null, tables: [], wal: null, problems: [] };
  // oracle input: filter chunks + the user keys they must admit; consumed by
  // the official-implementation oracle (official_bloom_check.cc)
  const oracle = oracleOut ? [Buffer.from("BLOOMORC", "latin1")] : null;
  report.oracle = oracle; report.oracleChunkNo = 0;
  const cur = fs.readFileSync(path.join(dir, 'CURRENT'), 'latin1').trim();
  if (!/^MANIFEST-\d+$/.test(cur)) throw new Error('bad CURRENT: ' + cur);
  const mf = fs.readFileSync(path.join(dir, cur));
  const log = parseLog(mf);
  if (log.errors.length) report.problems.push('manifest log: ' + log.errors.join('; '));
  const levels = new Map();  // level -> Map(num -> {size, smallest, largest})
  for (const rec of log.records) {
    const ed = decodeVersionEdit(rec);
    if (ed.comparator) report.comparator = ed.comparator;
    if (ed.lastSeq !== null) report.lastSeq = ed.lastSeq;
    if (ed.logNumber !== null) report.logNumber = ed.logNumber;
    for (const [level, num] of ed.deleted) {
      const m = levels.get(level); if (m) m.delete(num);
    }
    for (const f of ed.added) {
      if (!levels.has(f.level)) levels.set(f.level, new Map());
      levels.get(f.level).set(f.num, { size: f.size, smallest: f.smallest, largest: f.largest });
    }
  }
  const referenced = new Set();
  for (const [level, m] of levels) {
    const files = [...m.entries()].sort((a, b) => a[0] - b[0]);
    if (level >= 1) {
      for (let i = 1; i < files.length; i++) {
        const prev = files[i - 1][1], curF = files[i][1];
        if (Buffer.compare(userKey(prev.largest), userKey(curF.smallest)) >= 0)
          report.problems.push(`level ${level}: files ${files[i-1][0]} & ${files[i][0]} overlap`);
      }
    }
    for (const [num, meta] of files) {
      const name = String(num).padStart(6, '0') + '.ldb';
      referenced.add(name);
      const fp = path.join(dir, name);
      if (!fs.existsSync(fp)) { report.problems.push('missing table ' + name); continue; }
      const buf = fs.readFileSync(fp);
      if (buf.length !== meta.size) report.problems.push(`size mismatch ${name}: ${buf.length} != ${meta.size}`);
      try { parseSSTable(buf, name, report); }
      catch (e) {
        const where = String(e.stack || '').split(String.fromCharCode(10))[1] || '';
        report.problems.push(`${name}: ${e.message} @ ${where.trim()}`);
      }
    }
  }
  for (const f of fs.readdirSync(dir)) {
    if (/^\d{6}\.ldb$/.test(f) && !referenced.has(f)) report.problems.push('orphan table ' + f);
    if (/^\d{6}\.log$/.test(f)) {
      const buf = fs.readFileSync(path.join(dir, f));
      const l = parseLog(buf);
      if (l.errors.length) report.problems.push(`${f} log: ${l.errors.join('; ')}`);
      for (const rec of l.records) {
        try { parseWriteBatch(rec, report); }
        catch (e) { report.problems.push(`${f} batch: ${e.message}`); }
      }
    }
  }
  if (oracle) fs.writeFileSync(oracleOut, Buffer.concat(oracle));
  return report;
}

// ---------------------------------------------------------------- compare
function classDigest(r, kind) {
  if (kind === 'tables') return r.tables.map(t => t.entries + ':' + t.digest).sort().join(',');
  if (kind === 'wal') return r.wal ? [r.wal.ops, r.wal.maxSeq, r.wal.digests.sort().join(',')].join(':') : 'none';
  if (kind === 'comparator') return String(r.comparator);
}
function compare(a, b) {
  const lines = [];
  let diff = 0;
  for (const kind of ['tables', 'wal', 'comparator']) {
    const da = classDigest(a, kind), dbv = classDigest(b, kind);
    const same = da === dbv;
    if (!same) diff++;
    lines.push(`${kind.padEnd(11)} ${same ? 'SAME' : 'DIFF'}`);
    if (!same) {
      lines.push(`  A: ${String(da).slice(0, 220)}`);
      lines.push(`  B: ${String(dbv).slice(0, 220)}`);
    }
  }
  const pa = a.problems.length, pb = b.problems.length;
  lines.push(`problems    A=${pa} B=${pb}${pa === 0 && pb === 0 ? '  (both clean)' : ''}`);
  for (const p of a.problems.slice(0, 5)) lines.push('  A-problem: ' + p);
  for (const p of b.problems.slice(0, 5)) lines.push('  B-problem: ' + p);
  if (pa || pb) diff++;
  return { diff, text: lines.join('\n') };
}

// ---------------------------------------------------------------- main
const args = process.argv.slice(2);
try {
  if (args[0] === '--compare') {
    const A = probe(args[1]), B = probe(args[2]);
    const c = compare(A, B);
    console.log(`compare ${args[1]}\n        ${args[2]}`);
    console.log(c.text);
    process.exit(c.diff ? 1 : 0);
  } else {
    const r = probe(args[0], args[1] || undefined);
    const clone = Object.assign({}, r); delete clone.oracle; delete clone.oracleChunkNo;
    console.log(JSON.stringify(clone, (k, v) => typeof v === "bigint" ? v.toString() : v, 1));
    process.exit(r.problems.length ? 1 : 0);
  }
} catch (e) {
  console.error('PROBE ERROR: ' + e.message);
  if (e.stack) console.error(e.stack);
  process.exit(2);
}
