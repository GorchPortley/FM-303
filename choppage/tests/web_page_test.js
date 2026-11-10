// SPDX-License-Identifier: GPL-3.0-only
// web/breaks.html in a real browser (Playwright, headless Chromium) against a pretend FM-1: a WebMIDI
// port that answers as firmware/src/app/sysex.c does. Connect, the slot list, a WAV dropped in,
// sent: BEGIN, DATA in order, END with the CRC of the data sent; no page errors.
//   node tests/web_page_test.js WAV
"use strict";
const path = require("path");
const { chromium } = require("playwright");
const P = require("../web/chp_proto.js");

(async () => {
  const wav = process.argv[2];
  const browser = await chromium.launch();
  const page = await browser.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.addInitScript(() => {
    window.__sent = [];
    const listeners = [];
    const inp = { name: "CHOPPAGE FM-1", set onmidimessage(f) { listeners.push(f); } };
    const reply = (bytes) => setTimeout(() => listeners.forEach((f) => f({ data: new Uint8Array(bytes) })), 2);
    const out = {
      name: "CHOPPAGE FM-1",
      send(msg) {
        const m = Array.from(msg);
        window.__sent.push(m);
        const P = window.ChpProto, cmd = m[5], seq = m[6];
        if (cmd === 1) {                       // HELLO: the factory breaks
          const body = [1, 8, 0x00, 0xf0, 0x03, 0x00, 0x00, 0x90, 0x02, 0x00, 1];
          for (let i = 0; i < 8; i++) {
            const len = i < 3 ? 60000 : 0, name = ["RINSE", "TUMBLE", "RIDER"][i] || "";
            body.push(len & 255, (len >> 8) & 255, (len >> 16) & 255, 0, 0x22, 0x56, 0, i < 3 ? 4 : 0, i < 3 ? 16 : 0);
            for (let k = 0; k < 12; k++) body.push(k < name.length ? name.charCodeAt(k) : 0);
          }
          reply([0xf0, ...P.ID, 1, seq, ...P.pack7(body), 0xf7]);
        } else {
          reply([0xf0, ...P.ID, 0x7f, seq, ...P.pack7([cmd, 0, 0, 0, 0, 0]), 0xf7]);
        }
      },
    };
    navigator.requestMIDIAccess = async () => ({ outputs: new Map([["o", out]]), inputs: new Map([["i", inp]]) });
    window.confirm = () => true;
  });
  await page.goto("file://" + path.resolve(__dirname, "../web/breaks.html"));
  await page.click("#connect");
  await page.waitForFunction(() => document.querySelectorAll("#slots tr").length === 8);
  const rows = await page.$$eval("#slots tr", (r) => r.map((x) => x.textContent));
  let fails = 0;
  const check = (ok, what) => { console.log(`  ${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) fails++; };
  check(/RINSE/.test(rows[0]) && /TUMBLE/.test(rows[1]), "connect: the slots listed");
  await page.setInputFiles("#file", wav);
  await page.waitForFunction(() => /BPM/.test(document.getElementById("info").textContent));
  check(await page.$eval("#beats", (e) => e.value) === "8", "the beats guessed from the length");
  await page.selectOption("#slot", "2");
  await page.click("#send");
  await page.waitForFunction(() => /is in slot 2/.test(document.getElementById("log").textContent), null, { timeout: 30000 });
  const sent = await page.evaluate(() => window.__sent);
  const frames = sent.filter((m) => m[5] >= 2 && m[5] <= 4).map((m) => ({ cmd: m[5], body: P.unpack7(m.slice(7, -1)) }));
  const begin = frames.find((f) => f.cmd === 2), end = frames.find((f) => f.cmd === 4);
  const data = [];
  let inOrder = true;
  for (const f of frames.filter((f) => f.cmd === 3)) {
    const off = f.body[0] | f.body[1] << 8 | f.body[2] << 16 | f.body[3] << 24;
    if (off !== data.length) inOrder = false;
    data.push(...f.body.slice(4));
  }
  const len = begin.body[1] | begin.body[2] << 8 | begin.body[3] << 16;
  check(begin.body[0] === 1 && begin.body[7] === 0 && begin.body[8] === 8, "BEGIN: slot 2, linear 8-bit, 8 beats");
  check(inOrder && data.length === len, `DATA: ${data.length} bytes in order, as BEGIN said`);
  const crc = (end.body[0] | end.body[1] << 8 | end.body[2] << 16 | end.body[3] << 24) >>> 0;
  check(crc === P.crc32(data), "END: the CRC of what was sent");
  check(data.length > 70000 && data.length < 80000, `about 22050 samples a second (${data.length})`);
  check(errors.length === 0, "no page errors" + (errors.length ? ": " + errors.join("; ") : ""));
  await page.screenshot({ path: path.resolve(__dirname, "../build/host/breaks_page.png"), fullPage: true });
  await browser.close();
  console.log(fails ? "web page: FAILED" : "web page: ok");
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
