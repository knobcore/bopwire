/**
 * Minimal QR code generator. Pure TypeScript, no external dependencies.
 *
 * Public API:
 *
 *     import { qrcodeSVG } from './vendor/qrcode';
 *     const svg = qrcodeSVG("0xAbC123...", { ecc: 'L', scale: 4, margin: 4 });
 *
 * Algorithm follows ISO/IEC 18004:2015 (the QR standard) plus the
 * public-domain QR code reference by Project Nayuki
 * (https://www.nayuki.io/page/qr-code-generator-library, MIT) for the
 * spec tables and overall structure. This file is an independent re-
 * implementation in TypeScript; the bit-stream / Reed-Solomon math and
 * mask-penalty rules match the spec so the output is byte-for-byte
 * compatible with any reference QR scanner.
 *
 * Supports:
 *   - byte mode only (sufficient for EIP-55 addresses, URIs, JSON, etc.)
 *   - all four error-correction levels (L / M / Q / H)
 *   - QR versions 1..10 (up to 33×33 -> 57×57 modules)
 *   - all eight mask patterns with full ISO penalty scoring
 *
 * Why version 1..10: the wallet uses ECC L for an EIP-55 address
 * (42 bytes), which fits comfortably in version 4 (33×33 modules);
 * larger versions are included for future use (e.g. JSON-encoded
 * payment URIs). Versions >=11 are not implemented because their
 * alignment-pattern layout adds complexity we don't need yet.
 */

export type ECLevel = 'L' | 'M' | 'Q' | 'H';

export interface QrOptions {
  /** Error-correction level. Default 'L' (most payload, least redundancy). */
  ecc?: ECLevel;
  /** Pixels per QR module. Default 4. */
  scale?: number;
  /** Quiet-zone width in modules. QR spec requires >=4. Default 4. */
  margin?: number;
}

/**
 * Generate a QR code for `data` and return a standalone SVG string.
 * Picks the smallest QR version that fits the payload at the requested
 * error-correction level.
 */
export function qrcodeSVG(data: string, opts: QrOptions = {}): string {
  const ecc = opts.ecc ?? 'L';
  const scale = Math.max(1, opts.scale ?? 4);
  const margin = Math.max(0, opts.margin ?? 4);
  const qr = generate(data, ecc);
  return toSvg(qr, scale, margin);
}

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

interface QrMatrix {
  size: number;
  /** Indexed as modules[y][x]. true = dark. */
  modules: boolean[][];
}

// ---------------------------------------------------------------------------
// Galois field for Reed-Solomon. Primitive polynomial = 0x11d.
// ---------------------------------------------------------------------------

const GF_EXP = new Uint8Array(512);
const GF_LOG = new Uint8Array(256);
(function initGF() {
  let x = 1;
  for (let i = 0; i < 255; i++) {
    GF_EXP[i] = x;
    GF_LOG[x] = i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11d;
  }
  for (let i = 255; i < 512; i++) GF_EXP[i] = GF_EXP[i - 255];
})();

function gfMul(a: number, b: number): number {
  if (a === 0 || b === 0) return 0;
  return GF_EXP[(GF_LOG[a] + GF_LOG[b]) % 255];
}

function rsGeneratorPoly(degree: number): number[] {
  let poly = [1];
  for (let i = 0; i < degree; i++) {
    const next = new Array(poly.length + 1).fill(0);
    for (let j = 0; j < poly.length; j++) {
      next[j] ^= poly[j];
      next[j + 1] ^= gfMul(poly[j], GF_EXP[i]);
    }
    poly = next;
  }
  return poly;
}

function rsRemainder(data: number[], degree: number): number[] {
  const gen = rsGeneratorPoly(degree);
  const rem = new Array(degree).fill(0);
  for (const b of data) {
    const factor = b ^ rem.shift()!;
    rem.push(0);
    for (let i = 0; i < gen.length - 1; i++) {
      rem[i] ^= gfMul(gen[i + 1], factor);
    }
  }
  return rem;
}

// ---------------------------------------------------------------------------
// Version metadata (v1..v10). Each entry:
//   [totalDataCodewords, ecCodewordsPerBlock, [g1Blocks, g1Words, g2Blocks, g2Words]]
// Pulled from ISO/IEC 18004 Annex D.
// ---------------------------------------------------------------------------

type EcBlockSpec = readonly [number, number, readonly [number, number, number, number]];

const EC_TABLE: Record<ECLevel, readonly EcBlockSpec[]> = {
  L: [
    [19, 7, [1, 19, 0, 0]],
    [34, 10, [1, 34, 0, 0]],
    [55, 15, [1, 55, 0, 0]],
    [80, 20, [1, 80, 0, 0]],
    [108, 26, [1, 108, 0, 0]],
    [136, 18, [2, 68, 0, 0]],
    [156, 20, [2, 78, 0, 0]],
    [194, 24, [2, 97, 0, 0]],
    [232, 30, [2, 116, 0, 0]],
    [274, 18, [2, 68, 2, 69]],
  ],
  M: [
    [16, 10, [1, 16, 0, 0]],
    [28, 16, [1, 28, 0, 0]],
    [44, 26, [1, 44, 0, 0]],
    [64, 18, [2, 32, 0, 0]],
    [86, 24, [2, 43, 0, 0]],
    [108, 16, [4, 27, 0, 0]],
    [124, 18, [4, 31, 0, 0]],
    [154, 22, [2, 38, 2, 39]],
    [182, 22, [3, 36, 2, 37]],
    [216, 26, [4, 43, 1, 44]],
  ],
  Q: [
    [13, 13, [1, 13, 0, 0]],
    [22, 22, [1, 22, 0, 0]],
    [34, 18, [2, 17, 0, 0]],
    [48, 26, [2, 24, 0, 0]],
    [62, 18, [2, 15, 2, 16]],
    [76, 24, [4, 19, 0, 0]],
    [88, 18, [2, 14, 4, 15]],
    [110, 22, [4, 18, 2, 19]],
    [132, 20, [4, 16, 4, 17]],
    [154, 24, [6, 19, 2, 20]],
  ],
  H: [
    [9, 17, [1, 9, 0, 0]],
    [16, 28, [1, 16, 0, 0]],
    [26, 22, [2, 13, 0, 0]],
    [36, 16, [4, 9, 0, 0]],
    [46, 22, [2, 11, 2, 12]],
    [60, 28, [4, 15, 0, 0]],
    [66, 26, [4, 13, 1, 14]],
    [86, 26, [4, 14, 2, 15]],
    [100, 24, [4, 12, 4, 13]],
    [122, 28, [6, 15, 2, 16]],
  ],
};

// Alignment-pattern center coordinates per version (v1..v10).
const ALIGN_POSITIONS: readonly (readonly number[])[] = [
  [],
  [6, 18],
  [6, 22],
  [6, 26],
  [6, 30],
  [6, 34],
  [6, 22, 38],
  [6, 24, 42],
  [6, 26, 46],
  [6, 28, 50],
];

// 15-bit format-info codes by (ECLevel, mask) — pre-XOR'd with 0x5412.
// Lifted from ISO/IEC 18004 Annex C.
const FORMAT_BITS: Record<ECLevel, readonly number[]> = {
  L: [0x77c4, 0x72f3, 0x7daa, 0x789d, 0x662f, 0x6318, 0x6c41, 0x6976],
  M: [0x5412, 0x5125, 0x5e7c, 0x5b4b, 0x45f9, 0x40ce, 0x4f97, 0x4aa0],
  Q: [0x355f, 0x3068, 0x3f31, 0x3a06, 0x24b4, 0x2183, 0x2eda, 0x2bed],
  H: [0x1689, 0x13be, 0x1ce7, 0x19d0, 0x0762, 0x0255, 0x0d0c, 0x083b],
};

// 18-bit version-info BCH codes for v7..v10.
const VERSION_INFO: Record<number, number> = {
  7: 0x07c94,
  8: 0x085bc,
  9: 0x09a99,
  10: 0x0a4d3,
};

function dataCapacityBits(version: number, ec: ECLevel): number {
  const spec = EC_TABLE[ec][version - 1];
  const totalDatawords = spec[2][0] * spec[2][1] + spec[2][2] * spec[2][3];
  return totalDatawords * 8;
}

function pickVersion(payloadLen: number, ec: ECLevel): number {
  for (let v = 1; v <= 10; v++) {
    const lenBits = v < 10 ? 8 : 16;
    const needBits = 4 + lenBits + payloadLen * 8;
    if (dataCapacityBits(v, ec) >= needBits) return v;
  }
  throw new Error(`qrcodeSVG: payload too large (${payloadLen} bytes) for v1..v10`);
}

// ---------------------------------------------------------------------------
// Bit-stream construction
// ---------------------------------------------------------------------------

function pushBits(out: number[], value: number, width: number): void {
  for (let i = width - 1; i >= 0; i--) out.push((value >> i) & 1);
}

function encodeByteMode(data: Uint8Array, version: number, ec: ECLevel): number[] {
  const cap = dataCapacityBits(version, ec);
  const lenBits = version < 10 ? 8 : 16;
  const bits: number[] = [];
  pushBits(bits, 0b0100, 4);          // byte-mode indicator
  pushBits(bits, data.length, lenBits);
  for (const b of data) pushBits(bits, b, 8);

  // Terminator + byte alignment.
  pushBits(bits, 0, Math.min(4, cap - bits.length));
  while (bits.length % 8 !== 0) bits.push(0);

  // Pad codewords (alternate 0xEC / 0x11).
  const pad = [0xec, 0x11];
  let i = 0;
  while (bits.length < cap) {
    pushBits(bits, pad[i % 2], 8);
    i++;
  }
  return bits;
}

function bitsToBytes(bits: number[]): number[] {
  const out: number[] = [];
  for (let i = 0; i < bits.length; i += 8) {
    let b = 0;
    for (let j = 0; j < 8; j++) b = (b << 1) | (bits[i + j] ?? 0);
    out.push(b);
  }
  return out;
}

function interleaveCodewords(dataBytes: number[], version: number, ec: ECLevel): number[] {
  const spec = EC_TABLE[ec][version - 1];
  const ecPerBlock = spec[1];
  const [g1Blocks, g1Words, g2Blocks, g2Words] = spec[2];

  const dataBlocks: number[][] = [];
  const ecBlocks: number[][] = [];
  let cursor = 0;
  for (let b = 0; b < g1Blocks; b++) {
    const slice = dataBytes.slice(cursor, cursor + g1Words);
    cursor += g1Words;
    dataBlocks.push(slice);
    ecBlocks.push(rsRemainder(slice, ecPerBlock));
  }
  for (let b = 0; b < g2Blocks; b++) {
    const slice = dataBytes.slice(cursor, cursor + g2Words);
    cursor += g2Words;
    dataBlocks.push(slice);
    ecBlocks.push(rsRemainder(slice, ecPerBlock));
  }

  // Interleave column-by-column (data first, then EC).
  const out: number[] = [];
  const maxData = Math.max(...dataBlocks.map(b => b.length));
  for (let i = 0; i < maxData; i++) {
    for (const block of dataBlocks) if (i < block.length) out.push(block[i]);
  }
  for (let i = 0; i < ecPerBlock; i++) {
    for (const block of ecBlocks) out.push(block[i]);
  }
  return out;
}

function bytesToBits(bytes: number[]): number[] {
  const bits: number[] = [];
  for (const b of bytes) {
    for (let i = 7; i >= 0; i--) bits.push((b >> i) & 1);
  }
  return bits;
}

// ---------------------------------------------------------------------------
// Module matrix construction
// ---------------------------------------------------------------------------

function newGrid(size: number): boolean[][] {
  const g: boolean[][] = new Array(size);
  for (let y = 0; y < size; y++) g[y] = new Array(size).fill(false);
  return g;
}

function placeFinder(g: boolean[][], reserved: boolean[][], cx: number, cy: number): void {
  for (let dy = -1; dy <= 7; dy++) {
    for (let dx = -1; dx <= 7; dx++) {
      const x = cx + dx;
      const y = cy + dy;
      if (x < 0 || y < 0 || x >= g.length || y >= g.length) continue;
      const inBox = dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6;
      let dark = false;
      if (inBox) {
        const onBorder = dx === 0 || dx === 6 || dy === 0 || dy === 6;
        const inCenter = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
        dark = onBorder || inCenter;
      }
      g[y][x] = dark;
      reserved[y][x] = true;
    }
  }
}

function placeAlignment(g: boolean[][], reserved: boolean[][], version: number): void {
  const pos = ALIGN_POSITIONS[version - 1];
  for (const cy of pos) {
    for (const cx of pos) {
      // Skip alignment patterns that would overlap the three finders.
      const size = g.length;
      const overlapsFinder =
        (cy <= 8 && cx <= 8) ||
        (cy <= 8 && cx >= size - 9) ||
        (cy >= size - 9 && cx <= 8);
      if (overlapsFinder) continue;
      for (let dy = -2; dy <= 2; dy++) {
        for (let dx = -2; dx <= 2; dx++) {
          const onBorder = dx === -2 || dx === 2 || dy === -2 || dy === 2;
          const center = dx === 0 && dy === 0;
          g[cy + dy][cx + dx] = onBorder || center;
          reserved[cy + dy][cx + dx] = true;
        }
      }
    }
  }
}

function placeTiming(g: boolean[][], reserved: boolean[][]): void {
  const size = g.length;
  for (let i = 8; i < size - 8; i++) {
    const dark = i % 2 === 0;
    g[6][i] = dark;
    reserved[6][i] = true;
    g[i][6] = dark;
    reserved[i][6] = true;
  }
}

function reserveFormatAreas(reserved: boolean[][], version: number): void {
  const size = reserved.length;
  // Format-info strips around the three finders.
  for (let i = 0; i < 9; i++) {
    reserved[8][i] = true;
    reserved[i][8] = true;
  }
  for (let i = 0; i < 8; i++) {
    reserved[8][size - 1 - i] = true;
    reserved[size - 1 - i][8] = true;
  }
  // Dark module (always-on bit at (8, size-8)).
  reserved[size - 8][8] = true;

  // Version info blocks for v7+.
  if (version >= 7) {
    for (let y = 0; y < 6; y++) {
      for (let x = 0; x < 3; x++) {
        reserved[y][size - 11 + x] = true;
        reserved[size - 11 + x][y] = true;
      }
    }
  }
}

function placeFormatBits(g: boolean[][], ec: ECLevel, mask: number): void {
  const bits = FORMAT_BITS[ec][mask];
  const size = g.length;
  // Copy 1 (around top-left finder).
  for (let i = 0; i < 15; i++) {
    const b = ((bits >> i) & 1) === 1;
    let x: number, y: number;
    if (i < 6) { x = 8; y = i; }
    else if (i === 6) { x = 8; y = 7; }
    else if (i === 7) { x = 8; y = 8; }
    else if (i === 8) { x = 7; y = 8; }
    else { x = 14 - i; y = 8; }
    g[y][x] = b;
  }
  // Copy 2 (split across top-right and bottom-left).
  for (let i = 0; i < 15; i++) {
    const b = ((bits >> i) & 1) === 1;
    let x: number, y: number;
    if (i < 8) { x = size - 1 - i; y = 8; }
    else { x = 8; y = size - 15 + i; }
    g[y][x] = b;
  }
  // Always-dark module.
  g[size - 8][8] = true;
}

function placeVersionBits(g: boolean[][], version: number): void {
  if (version < 7) return;
  const bits = VERSION_INFO[version];
  const size = g.length;
  for (let i = 0; i < 18; i++) {
    const b = ((bits >> i) & 1) === 1;
    const a = Math.floor(i / 3);
    const c = (i % 3) + size - 11;
    g[a][c] = b;
    g[c][a] = b;
  }
}

function placeData(g: boolean[][], reserved: boolean[][], dataBits: number[]): void {
  const size = g.length;
  let bitIdx = 0;
  let upward = true;
  // Snake from bottom-right upward, two columns at a time, skipping the
  // vertical timing column at x=6.
  for (let right = size - 1; right >= 1; right -= 2) {
    if (right === 6) right = 5;
    for (let step = 0; step < size; step++) {
      const y = upward ? size - 1 - step : step;
      for (let dx = 0; dx < 2; dx++) {
        const x = right - dx;
        if (reserved[y][x]) continue;
        const bit = bitIdx < dataBits.length ? dataBits[bitIdx] : 0;
        g[y][x] = bit === 1;
        bitIdx++;
      }
    }
    upward = !upward;
  }
}

const MASK_FNS: readonly ((x: number, y: number) => boolean)[] = [
  (x, y) => (x + y) % 2 === 0,
  (_x, y) => y % 2 === 0,
  (x, _y) => x % 3 === 0,
  (x, y) => (x + y) % 3 === 0,
  (x, y) => (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0,
  (x, y) => ((x * y) % 2 + (x * y) % 3) === 0,
  (x, y) => (((x * y) % 2 + (x * y) % 3) % 2) === 0,
  (x, y) => (((x + y) % 2 + (x * y) % 3) % 2) === 0,
];

function applyMask(g: boolean[][], reserved: boolean[][], mask: number): void {
  const fn = MASK_FNS[mask];
  const size = g.length;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      if (!reserved[y][x] && fn(x, y)) g[y][x] = !g[y][x];
    }
  }
}

function scoreMask(g: boolean[][]): number {
  const size = g.length;
  let score = 0;

  // Rule 1: runs of 5+ same-color modules.
  for (let y = 0; y < size; y++) {
    let color = g[y][0], run = 1;
    for (let x = 1; x < size; x++) {
      if (g[y][x] === color) {
        run++;
        if (run === 5) score += 3;
        else if (run > 5) score += 1;
      } else { color = g[y][x]; run = 1; }
    }
  }
  for (let x = 0; x < size; x++) {
    let color = g[0][x], run = 1;
    for (let y = 1; y < size; y++) {
      if (g[y][x] === color) {
        run++;
        if (run === 5) score += 3;
        else if (run > 5) score += 1;
      } else { color = g[y][x]; run = 1; }
    }
  }
  // Rule 2: solid 2x2 blocks.
  for (let y = 0; y < size - 1; y++) {
    for (let x = 0; x < size - 1; x++) {
      const c = g[y][x];
      if (c === g[y][x + 1] && c === g[y + 1][x] && c === g[y + 1][x + 1]) {
        score += 3;
      }
    }
  }
  // Rule 3: finder-like pattern 1011101 with 4-light margin (either side).
  const pat: readonly boolean[] = [
    true, false, true, true, true, false, true,
    false, false, false, false,
  ];
  const patRev: readonly boolean[] = [
    false, false, false, false,
    true, false, true, true, true, false, true,
  ];
  for (let y = 0; y < size; y++) {
    for (let x = 0; x <= size - 11; x++) {
      if (matchRow(g, x, y, pat) || matchRow(g, x, y, patRev)) score += 40;
    }
  }
  for (let x = 0; x < size; x++) {
    for (let y = 0; y <= size - 11; y++) {
      if (matchCol(g, x, y, pat) || matchCol(g, x, y, patRev)) score += 40;
    }
  }
  // Rule 4: balance of dark vs light.
  let dark = 0;
  for (let y = 0; y < size; y++)
    for (let x = 0; x < size; x++) if (g[y][x]) dark++;
  const pct = (dark * 100) / (size * size);
  score += Math.floor(Math.abs(pct - 50) / 5) * 10;
  return score;
}

function matchRow(g: boolean[][], x: number, y: number, pat: readonly boolean[]): boolean {
  for (let i = 0; i < pat.length; i++) if (g[y][x + i] !== pat[i]) return false;
  return true;
}
function matchCol(g: boolean[][], x: number, y: number, pat: readonly boolean[]): boolean {
  for (let i = 0; i < pat.length; i++) if (g[y + i][x] !== pat[i]) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Top-level generator + SVG renderer
// ---------------------------------------------------------------------------

function generate(text: string, ec: ECLevel): QrMatrix {
  const bytes = new TextEncoder().encode(text);
  const version = pickVersion(bytes.length, ec);
  const size = 17 + 4 * version;

  const dataBits = encodeByteMode(bytes, version, ec);
  const dataBytes = bitsToBytes(dataBits);
  const interleaved = interleaveCodewords(dataBytes, version, ec);
  const finalBits = bytesToBits(interleaved);

  const base = newGrid(size);
  const reserved = newGrid(size);
  placeFinder(base, reserved, 0, 0);
  placeFinder(base, reserved, size - 7, 0);
  placeFinder(base, reserved, 0, size - 7);
  placeAlignment(base, reserved, version);
  placeTiming(base, reserved);
  reserveFormatAreas(reserved, version);
  placeData(base, reserved, finalBits);
  placeVersionBits(base, version);

  let best: { grid: boolean[][]; score: number } | null = null;
  for (let mask = 0; mask < 8; mask++) {
    const g = newGrid(size);
    for (let y = 0; y < size; y++)
      for (let x = 0; x < size; x++) g[y][x] = base[y][x];
    applyMask(g, reserved, mask);
    placeFormatBits(g, ec, mask);
    const s = scoreMask(g);
    if (!best || s < best.score) best = { grid: g, score: s };
  }
  return { size, modules: best!.grid };
}

function toSvg(qr: QrMatrix, scale: number, margin: number): string {
  const dim = (qr.size + margin * 2) * scale;
  const parts: string[] = [];
  parts.push(
    `<svg xmlns="http://www.w3.org/2000/svg" width="${dim}" height="${dim}" viewBox="0 0 ${dim} ${dim}" shape-rendering="crispEdges">`
  );
  parts.push(`<rect width="${dim}" height="${dim}" fill="#ffffff"/>`);
  for (let y = 0; y < qr.size; y++) {
    for (let x = 0; x < qr.size; x++) {
      if (qr.modules[y][x]) {
        const px = (x + margin) * scale;
        const py = (y + margin) * scale;
        parts.push(
          `<rect x="${px}" y="${py}" width="${scale}" height="${scale}" fill="#000000"/>`
        );
      }
    }
  }
  parts.push('</svg>');
  return parts.join('');
}
