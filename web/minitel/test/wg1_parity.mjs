// wg1_parity.mjs - gate W-G1: the SHIPPED web engine against the native
// Suite harness, cell for cell and pixel for pixel.
//
// Part of the MGT Unicorn Suite. AGPLv3 or later, see /COPYING.
//
// Loads web/minitel/dist/engine.js + engine.wasm (the very files the page
// serves), feeds each .vdt frame from the power-on state, and prints the
// page memory in EXACTLY the line format of `minitel_selftest --dump`
// (tools/minitel_selftest.c dump_file), counters included. Then, if the
// native BMPs are given, renders the frame through mtw_frame in four
// presentations and compares every pixel with the native harness's BMP.
//
//   node wg1_parity.mjs FRAMES_DIR NATIVE_DUMPS_DIR OUT_DIR [NATIVE_BMP_DIR]
//
// NATIVE_DUMPS_DIR holds NAME.txt from `minitel_selftest --dump NAME.VDT`
// (LF line ends). NATIVE_BMP_DIR holds NAME.{c1,m1,c2s,m2s}.bmp from
// `minitel_selftest NAME.VDT OUT.bmp [--mono] [--scale 2 --scanlines]`.
// Exit status 0 only if every frame is identical in every respect.

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const enginePath = path.resolve(here, '..', 'dist', 'engine.js');
const MinitelEngine = require(enginePath);

const [framesDir, nativeDir, outDir, bmpDir] = process.argv.slice(2);
if (!framesDir || !nativeDir || !outDir) {
  console.error('usage: node wg1_parity.mjs FRAMES_DIR NATIVE_DUMPS_DIR OUT_DIR [NATIVE_BMP_DIR]');
  process.exit(2);
}
fs.mkdirSync(outDir, { recursive: true });

const E = await MinitelEngine();
if (!E._mtw_init()) { console.error('mtw_init failed'); process.exit(2); }
E._mtw_set_reply(0);
E._mtw_set_status(0);          // the harness draws no status letter

const KIND = ['A', 'M', 'D'];
const MT_KIND_ALPHA = 0, MT_KIND_MOSAIC = 1, MT_KIND_DELIM = 2;
const MT_F_ZMASK = 0x08, MT_F_ZUNDER = 0x10, MT_BLACK = 0;

function feedAll(bytes) {
  const inb = E._mtw_inbuf(), max = E._mtw_inbuf_size();
  for (let o = 0; o < bytes.length; o += max) {
    const c = bytes.subarray(o, Math.min(o + max, bytes.length));
    E.HEAPU8.set(c, inb);
    E._mtw_feed(c.length);
  }
}

// The same walk as dump_file, including the paper derivation.
function dump() {
  const out = [];
  const masked = E._mtw_screen_masked();
  for (let r = 0; r < 25; r++) {
    let zBg = MT_BLACK, zMask = 0, zUl = 0;
    for (let c = 1; c <= 40; c++) {
      const v = E._mtw_cell(r, c) >>> 0;
      const kind = v & 0xFF, glyph = (v >>> 8) & 0xFF, fg = (v >>> 16) & 15,
            bg = (v >>> 20) & 15, flags = (v >>> 24) & 0xFF;
      const size = E._mtw_cell_size(r, c);
      if (kind === MT_KIND_DELIM) {
        zBg = bg;
        zMask = (flags & MT_F_ZMASK) ? 1 : 0;
        zUl = (flags & MT_F_ZUNDER) ? 1 : 0;
      }
      const paper = kind === MT_KIND_ALPHA ? zBg : bg;
      out.push(`${r} ${c} ${KIND[kind]} ${glyph} ${fg} ${paper} ${flags} ${size} ${zMask && masked ? 1 : 0} ${zUl}`);
      if (kind === MT_KIND_MOSAIC) { zBg = bg; zUl = 0; }
    }
  }
  const s = [];
  for (let i = 0; i < 11; i++) s.push(E._mtw_stat(i) >>> 0);
  out.push(`${s[0]} bytes  ${s[1]} chars  ${s[2]} filtered  ${s[3]} resync  ${s[4]} error  ` +
           `${s[5]} row00  ${s[6]} pro  ${s[7]} unrep  ${s[8]} bit8  ${s[9]} bel  ${s[10]} us-ign`);
  return out.join('\n') + '\n';
}

// A 24-bit bottom-up BMP as written by the harness, to R G B rows top first.
function readBmp(p) {
  const b = fs.readFileSync(p);
  const off = b.readUInt32LE(10), w = b.readInt32LE(18), h = b.readInt32LE(22), bpp = b.readUInt16LE(28);
  if (bpp !== 24) throw new Error(`${p}: ${bpp} bpp`);
  const stride = (w * 3 + 3) & ~3;
  const px = new Uint8Array(w * h * 3);
  for (let y = 0; y < h; y++) {
    const src = off + (h - 1 - y) * stride;
    for (let x = 0; x < w; x++) {
      px[(y * w + x) * 3 + 0] = b[src + x * 3 + 2];
      px[(y * w + x) * 3 + 1] = b[src + x * 3 + 1];
      px[(y * w + x) * 3 + 2] = b[src + x * 3 + 0];
    }
  }
  return { w, h, px };
}

// mtw_frame options: MT_R_MONO 1; MT_P_SCANLINES 1, MT_P_MONO 2.
const PRESENT = [
  { tag: 'c1',  scale: 1, r: 0, p: 0 },
  { tag: 'm1',  scale: 1, r: 1, p: 2 },
  { tag: 'c2s', scale: 2, r: 0, p: 1 },
  { tag: 'm2s', scale: 2, r: 1, p: 3 },
];

function comparePixels(name) {
  const res = [];
  for (const pr of PRESENT) {
    const bp = path.join(bmpDir, `${name}.${pr.tag}.bmp`);
    const nat = readBmp(bp);
    const w = 320 * pr.scale, h = 250 * pr.scale;
    if (nat.w !== w || nat.h !== h) { res.push(`${pr.tag} size ${nat.w}x${nat.h}`); continue; }
    const ptr = E._mtw_frame(pr.scale, pr.r, pr.p);
    const web = E.HEAPU8.subarray(ptr, ptr + w * h * 4);
    let diff = 0;
    for (let i = 0; i < w * h; i++) {
      if (web[i * 4] !== nat.px[i * 3] || web[i * 4 + 1] !== nat.px[i * 3 + 1] ||
          web[i * 4 + 2] !== nat.px[i * 3 + 2] || web[i * 4 + 3] !== 255) diff++;
    }
    res.push(diff ? `${pr.tag} ${diff} px differ` : null);
  }
  return res.filter(Boolean);
}

const frames = fs.readdirSync(framesDir).filter(f => /\.vdt$/i.test(f)).sort();
let bad = 0, cells = 0, pixels = 0;
const report = [];
for (const f of frames) {
  const name = f.replace(/\.vdt$/i, '');
  E._mtw_reset();
  feedAll(new Uint8Array(fs.readFileSync(path.join(framesDir, f))));
  const mine = dump();
  fs.writeFileSync(path.join(outDir, `${name}.txt`), mine);
  const theirs = fs.readFileSync(path.join(nativeDir, `${name}.txt`), 'latin1').replace(/\r\n/g, '\n');
  const problems = [];
  if (mine !== theirs) {
    const a = mine.split('\n'), b = theirs.split('\n');
    let n = 0;
    for (let i = 0; i < Math.max(a.length, b.length); i++) if (a[i] !== b[i]) n++;
    problems.push(`${n} dump lines differ`);
  } else {
    cells += 1000;
  }
  if (bmpDir) {
    const px = comparePixels(name);
    problems.push(...px);
    if (!px.length) pixels += 320 * 250 * 2 + 640 * 500 * 2;
  }
  if (problems.length) bad++;
  report.push(`${name}  ${problems.length ? 'DIFFERS: ' + problems.join('; ') : 'identical'}`);
}
report.push(`${frames.length} frames, ${bad} differ; ${cells} cells and ${pixels} pixels compared identical`);
const text = report.join('\n') + '\n';
fs.writeFileSync(path.join(outDir, 'wg1-report.txt'), text);
process.stdout.write(text);
process.exit(bad ? 1 : 0);
