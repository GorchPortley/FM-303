// SPDX-License-Identifier: GPL-3.0-only
// CHOPPAGE break loading: the SysEx protocol (firmware/src/app/sysex.c) and the conversion of a
// sound to an FM-1 break (mono, resampled, peak-normalized, 8-bit linear or mu-law). The same as
// tools/chp_breaks.py, byte for byte (tests/web_test.js checks it). Browser: window.ChpProto;
// node: require().
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.ChpProto = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";
  const ID = [0x7d, 0x43, 0x48, 0x50];
  const SLOTS = 8, CAPACITY = 258048, CHUNK = 512;
  const STATUS = { 0: "ok", 1: "bad arguments", 2: "no room", 3: "out of order", 4: "CRC mismatch", 5: "flash error" };

  function pack7(data) {                        // 8 bits to 7, least significant first
    const out = [];
    let acc = 0, nb = 0;
    for (const b of data) {
      acc |= b << nb;
      nb += 8;
      while (nb >= 7) { out.push(acc & 0x7f); acc >>>= 7; nb -= 7; }
    }
    if (nb) out.push(acc & 0x7f);
    return out;
  }

  function unpack7(data) {
    const out = [];
    let acc = 0, nb = 0;
    for (const b of data) {
      acc |= (b & 0x7f) << nb;
      nb += 7;
      if (nb >= 8) { out.push(acc & 0xff); acc >>>= 8; nb -= 8; }
    }
    return out;
  }

  let crcTable = null;
  function crc32(data) {                        // zlib's
    if (!crcTable) {
      crcTable = new Uint32Array(256);
      for (let i = 0; i < 256; i++) {
        let c = i;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        crcTable[i] = c >>> 0;
      }
    }
    let c = 0xffffffff;
    for (const b of data) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  }

  function u32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }

  function frame(cmd, seq, payload) {           // F0 7D 43 48 50 cmd seq <packed> F7
    return [0xf0, ...ID, cmd & 0x7f, seq & 0x7f, ...pack7(payload || []), 0xf7];
  }

  function cleanName(name) {
    const s = String(name || "").toUpperCase().slice(0, 11);
    const out = new Array(12).fill(0);
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      out[i] = c >= 32 && c < 127 ? c : 63;
    }
    return out;
  }

  function breakMessages(slot, data, rate, fmt, beats, name) {
    const msgs = [[0x02, [slot, ...u32(data.length), rate & 0xff, rate >>> 8, fmt, beats, ...cleanName(name)]]];
    for (let off = 0; off < data.length; off += CHUNK)
      msgs.push([0x03, [...u32(off), ...data.slice(off, off + CHUNK)]]);
    msgs.push([0x04, u32(crc32(data))]);
    return msgs;
  }

  function guessBeats(n, rate) {                // nearest 125 BPM, as the FM-1 guesses
    const sec = n / rate;
    let best = 4, bd = 1e9;
    for (const b of [1, 2, 4, 8, 16, 32]) {
      const d = Math.abs(Math.log2(b * 60 / sec / 125));
      if (d < bd) { bd = d; best = b; }
    }
    return best;
  }

  function ulawByte(v) {                        // G.711 mu-law
    let s = Math.trunc(Math.max(-1, Math.min(1, v)) * 32124);
    const sign = s < 0 ? 0x80 : 0;
    s = Math.min(Math.abs(s), 32635) + 0x84;
    let exp = 7;
    for (let e = 7; e >= 0; e--) if (s & (0x4000 >> (7 - e))) { exp = e; break; }
    const mant = (s >> (exp + 3)) & 0x0f;
    return ~(sign | (exp << 4) | mant) & 0xff;
  }

  // a float sound -> the FM-1's bytes: peak-normalized; linear 8-bit (signed) or mu-law
  function toBytes(x, fmt, dither) {
    let peak = 0;
    for (const v of x) peak = Math.max(peak, Math.abs(v));
    peak = peak || 1;
    const out = new Array(x.length);
    let rnd = 1;
    for (let i = 0; i < x.length; i++) {
      const v = x[i] / peak;
      if (fmt === 1) { out[i] = ulawByte(v * 0.999); continue; }
      let q = v * 127;
      if (dither) {                             // TPDF, one LSB (the same numbers as the Python tool)
        rnd = (Math.imul(rnd, 1103515245) + 12345) & 0x7fffffff;
        const r1 = rnd / 0x7fffffff;
        rnd = (Math.imul(rnd, 1103515245) + 12345) & 0x7fffffff;
        q += r1 - rnd / 0x7fffffff;
      }
      q = Math.round(q);
      if (q === -0) q = 0;
      out[i] = Math.max(-127, Math.min(127, q)) & 0xff;
    }
    return out;
  }

  // without a browser's resampler: two one-pole lowpasses, then linear (as chp_breaks.py without numpy)
  function resampleSimple(x, src, dst) {
    if (src === dst) return Array.from(x);
    let y = x;
    if (dst < src) {
      const c = 1 - Math.exp(-2 * Math.PI * 0.42 * dst / src);
      let s1 = 0, s2 = 0;
      y = new Array(x.length);
      for (let i = 0; i < x.length; i++) { s1 += c * (x[i] - s1); s2 += c * (s1 - s2); y[i] = s2; }
    }
    const n = Math.floor(x.length * dst / src), out = new Array(n);
    for (let i = 0; i < n; i++) {
      const t = i * src / dst, k = Math.floor(t), fr = t - k;
      out[i] = y[k] + fr * ((k + 1 < y.length ? y[k + 1] : y[k]) - y[k]);
    }
    return out;
  }

  function parseHello(body) {
    const r = { version: body[0], slots: body[1], capacity: 0, used: 0, factory: !!body[10], breaks: [] };
    const rd32 = (p) => (body[p] | body[p + 1] << 8 | body[p + 2] << 16 | body[p + 3] << 24) >>> 0;
    r.capacity = rd32(2);
    r.used = rd32(6);
    for (let i = 0; i < r.slots; i++) {
      const p = 11 + i * 21, len = rd32(p);
      let name = "";
      for (let k = 0; k < 12 && body[p + 9 + k]; k++) name += String.fromCharCode(body[p + 9 + k]);
      r.breaks.push({ slot: i + 1, len, rate: body[p + 4] | body[p + 5] << 8, fmt: body[p + 6], beats: body[p + 7],
                      slices: body[p + 8], name });
    }
    return r;
  }

  return { ID, SLOTS, CAPACITY, CHUNK, STATUS, pack7, unpack7, crc32, frame, cleanName, breakMessages, guessBeats,
           ulawByte, toBytes, resampleSimple, parseHello };
});
