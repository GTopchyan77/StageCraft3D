// Copyright 2025-2026 NGG. All Rights Reserved.
// gen-placeholder-icon.mjs — write a valid 128x128 PNG to ../../Resources/Icon128.png.
// This is a PLACEHOLDER so the plugin satisfies Fab's icon requirement and packages
// cleanly. Replace Resources/Icon128.png with real 128x128 artwork before submitting.
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const pluginRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const outDir = path.join(pluginRoot, "Resources");
const W = 128, H = 128;

// CRC table for PNG chunks.
const crcTable = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();
function crc32(buf) {
  let c = ~0;
  for (let i = 0; i < buf.length; i++) c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
  const t = Buffer.from(type, "ascii");
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([t, data])), 0);
  return Buffer.concat([len, t, data, crc]);
}

// Raw RGBA scanlines: a dark slate background with a subtle diagonal accent so the
// placeholder is visibly a placeholder, not a blank square.
const raw = Buffer.alloc((W * 4 + 1) * H);
let p = 0;
for (let y = 0; y < H; y++) {
  raw[p++] = 0; // filter type 0 (None)
  for (let x = 0; x < W; x++) {
    const accent = Math.abs(x - y) < 6 || Math.abs(W - 1 - x - y) < 6;
    if (accent) { raw[p++] = 0x4f; raw[p++] = 0x9c; raw[p++] = 0xff; raw[p++] = 0xff; } // blue
    else        { raw[p++] = 0x1e; raw[p++] = 0x22; raw[p++] = 0x2a; raw[p++] = 0xff; } // slate
  }
}

const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
ihdr[8] = 8;   // bit depth
ihdr[9] = 6;   // color type RGBA
ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

const png = Buffer.concat([
  Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
  chunk("IHDR", ihdr),
  chunk("IDAT", zlib.deflateSync(raw, { level: 9 })),
  chunk("IEND", Buffer.alloc(0)),
]);

fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(path.join(outDir, "Icon128.png"), png);
console.log(`Wrote Resources/Icon128.png (${png.length} bytes, ${W}x${H})`);
