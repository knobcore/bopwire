/**
 * Smoke test for the vendored QR code generator.
 *
 * Runs under node directly:
 *
 *     npx tsx tests/qrcode.test.ts
 *     # or
 *     npx ts-node tests/qrcode.test.ts
 *
 * No test runner needed — the file `process.exit(1)`s on the first
 * failed assertion so it can be wired into any CI step that just runs
 * the script.
 */

import { qrcodeSVG } from '../src/vendor/qrcode';

function assert(cond: unknown, msg: string): void {
  if (!cond) {
    console.error(`FAIL: ${msg}`);
    process.exit(1);
  }
}

// EIP-55 style address — 42 chars including "0x".
const ADDR = '0xAbCdEf0123456789AbCdEf0123456789AbCdEf01';

// --- Test 1: produces an SVG with explicit width and height -----------------
const svg = qrcodeSVG(ADDR, { ecc: 'L', scale: 4, margin: 4 });
assert(typeof svg === 'string' && svg.length > 0, 'returned a non-empty string');
assert(svg.startsWith('<svg'), 'output starts with <svg');
assert(svg.endsWith('</svg>'), 'output ends with </svg>');

// Pull width/height out of the SVG root and confirm they match the formula.
// For a 42-byte EIP-55 address at ECC L, the smallest version that fits is
// version 3 (29 modules; v3-L has 55 datawords, payload+header = 44 bytes).
// (29 + 4*2) * 4 = 37 * 4 = 148 pixels.
const widthMatch = svg.match(/<svg[^>]*\swidth="(\d+)"/);
const heightMatch = svg.match(/<svg[^>]*\sheight="(\d+)"/);
assert(widthMatch !== null, 'svg has a width attribute');
assert(heightMatch !== null, 'svg has a height attribute');
const width = Number(widthMatch![1]);
const height = Number(heightMatch![1]);
assert(width === height, `svg is square (got ${width} x ${height})`);
// Expected: (29 modules + 2*4 margin) * 4 scale = 148 px for version 3.
const SIZE_MODULES = 29;
const EXPECTED = (SIZE_MODULES + 4 * 2) * 4;
assert(width === EXPECTED, `width matches v3 formula: expected ${EXPECTED}, got ${width}`);

// --- Test 2: enough <rect> elements to actually be a QR code ---------------
// The output has one big white background rect plus one rect per dark module.
// A 33x33 grid has 1089 cells; even a sparse QR has hundreds of dark modules.
// We just require "lots", which proves data was rendered (not just the bg).
const rectMatches = svg.match(/<rect\b/g);
assert(rectMatches !== null, 'svg contains <rect> elements');
const rectCount = rectMatches!.length;
assert(rectCount > 100, `expected > 100 <rect> elements, got ${rectCount}`);
// Sanity upper bound (1 bg + at most size*size dark modules).
assert(
  rectCount <= 1 + SIZE_MODULES * SIZE_MODULES,
  `expected <= ${1 + SIZE_MODULES * SIZE_MODULES} rects, got ${rectCount}`,
);

// --- Test 3: scaling changes width predictably ------------------------------
const svgBig = qrcodeSVG(ADDR, { ecc: 'L', scale: 8, margin: 4 });
const bigWidth = Number(svgBig.match(/<svg[^>]*\swidth="(\d+)"/)![1]);
assert(bigWidth === EXPECTED * 2, `scale=8 doubles width: expected ${EXPECTED * 2}, got ${bigWidth}`);

// --- Test 4: different ECC levels still produce valid output ---------------
for (const ecc of ['L', 'M', 'Q', 'H'] as const) {
  const s = qrcodeSVG(ADDR, { ecc });
  assert(s.startsWith('<svg') && s.endsWith('</svg>'), `ECC ${ecc} produces an SVG`);
  const rects = s.match(/<rect\b/g)!.length;
  assert(rects > 50, `ECC ${ecc} has plenty of rects (${rects})`);
}

// --- Test 5: default options work ------------------------------------------
const svgDefault = qrcodeSVG(ADDR);
assert(svgDefault.startsWith('<svg'), 'default-options call returns SVG');

// --- Test 6: short payload (numeric-ish) ------------------------------------
const svgShort = qrcodeSVG('hello world', { ecc: 'L' });
assert(svgShort.startsWith('<svg'), 'short payload returns SVG');
const shortRects = svgShort.match(/<rect\b/g)!.length;
assert(shortRects > 20, `short payload has rects (${shortRects})`);

console.log(`OK qrcode.test.ts — all assertions passed`);
console.log(`  EIP-55 address @ ECC L → ${width}x${height} px, ${rectCount} <rect> elements`);
