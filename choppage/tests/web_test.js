// SPDX-License-Identifier: GPL-3.0-only
// The browser loader's protocol and conversion (web/chp_proto.js) against tools/chp_breaks.py:
// the same bytes for the same sound. tests/web_test.py writes the fixture and runs this.
"use strict";
const fs = require("fs");
const P = require("../web/chp_proto.js");
const fx = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
let fails = 0;
function check(ok, what) { console.log(`  ${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) fails++; }
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

check(eq(P.toBytes(fx.x, 0, false), fx.lin), "linear 8-bit: the same bytes");
check(eq(P.toBytes(fx.x, 1, false), fx.ulaw), "mu-law: the same bytes");
check(eq(P.toBytes(fx.x, 0, true), fx.dith), "dithered: the same bytes");
const r = P.resampleSimple(fx.x, 44100, 22050);
check(r.length === fx.res.length && r.every((v, i) => Math.abs(v - fx.res[i]) < 1e-9), "resampling: the same samples");
check(P.crc32(fx.lin) === fx.crc, "CRC-32 as zlib");
check(P.guessBeats(77836, 22050) === 8 && P.guessBeats(38918, 22050) === 4, "beats guessed alike");
const msgs = P.breakMessages(fx.slot, fx.data, fx.rate, fx.fmt, fx.beats, fx.name);
let syx = [];
msgs.forEach(([c, p], i) => { syx = syx.concat(P.frame(c, i + 1, p)); });
check(eq(syx, fx.syx), `the SysEx of a whole break: the same ${syx.length} bytes`);
check(eq(P.unpack7(P.pack7(fx.data)).slice(0, fx.data.length), fx.data), "pack7 round trip");
const hello = P.parseHello(fx.hello);
check(hello.slots === 8 && hello.breaks[1].name === "AMEN" && hello.breaks[1].beats === 8, "HELLO answer read");
console.log(fails ? "web: FAILED" : "web: ok");
process.exit(fails ? 1 : 0);
