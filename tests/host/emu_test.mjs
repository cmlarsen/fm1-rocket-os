// SPDX-License-Identifier: GPL-3.0-only
// The browser build (build/emu/x0x.wasm) against the host simulator: run one shared .x0x
// scenario (tests/parity.x0x) through the WASM exports and compare its audio with the WAV the
// native runner wrote from the same file. Both stereo channels, equal length, within 1 LSB; the
// screen and the lights must be drawn.
//   node tests/host/emu_test.mjs build/emu/x0x.wasm HOST.wav [tests/parity.x0x]
import fs from "fs";

const [wasmPath, wavPath, scriptPath = "tests/parity.x0x"] = process.argv.slice(2);
if (!wasmPath || !wavPath) {
  console.error("usage: node tests/host/emu_test.mjs WASM HOST.wav [SCRIPT.x0x]");
  process.exit(2);
}

let mem;
const clock = { clock_time_get: (id, p, out) => { new DataView(mem.buffer).setBigUint64(out, 0n, true); return 0; } };
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), { wasi_snapshot_preview1: clock });
const ex = instance.exports;
mem = ex.memory;
ex._initialize();
ex.web_boot();

// The host script's tables (host/x0x_host.c BTN_N / ENC_N / key_of).
const BTN = { FX: 0, SEL: 1, ENV: 2, LFO: 3, EDIT: 4, GLO: 5, HOME: 6, SAVE: 7, ARP: 8, SEQ: 9,
              PLAY: 10, REC: 11, "OCT-": 12, "OCT+": 13 };
const ENC = { SELECT: 0, ALGO: 1, PRESET: 2, K1: 3, K2: 4, K3: 5, K4: 6 };
const WHITE_K = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26];
const BLACK_K = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25];
const keyOf = (s) => s[0] === "w" ? WHITE_K[parseInt(s.slice(1), 10) & 15]
    : s[0] === "b" ? BLACK_K[parseInt(s.slice(1), 10) % 11] : parseInt(s, 10);

const outL = [], outR = [];
let capturing = false;
let heldBtn = 0, heldKeys = 0;
const apply = () => { ex.web_buttons(heldBtn); ex.web_keys(heldKeys); };

// The host's own audio schedule (host/x0x_host.c run_ms): a 1 ms clock that renders one 256-frame
// half buffer each time the audio deadline passes (256 frames = 5.805 ms), as the I2S DMA does.
// Mirroring it exactly makes the WASM capture the same frames the native WAV holds, so the two
// lengths are equal, not merely close.
let nowMs = 0, audioDueMs = 0;
const render = (ms) => {
  for (let i = 0; i < ms; i++) {
    nowMs++;
    while (audioDueMs <= nowMs) {
      ex.web_render(256);
      if (capturing) {
        outL.push(...new Float32Array(mem.buffer, ex.web_out_l(), 256));
        outR.push(...new Float32Array(mem.buffer, ex.web_out_r(), 256));
      }
      audioDueMs += 256.0 * 1000.0 / 44100.0;
    }
  }
};

// The .x0x subset tests/parity.x0x uses, with the host's exact timings (host/x0x_host.c main).
const text = fs.readFileSync(scriptPath, "utf8");
for (const raw of text.split("\n")) {
  const line = raw.replace(/#.*$/, "").trim();
  if (!line) continue;
  const t = line.split(/\s+/), cmd = t[0];
  if (cmd === "wav") capturing = true;
  else if (cmd === "wavstop") capturing = false;
  else if (cmd === "wait") render(parseInt(t[1] ?? "0", 10));
  else if (cmd === "press" || cmd === "release" || cmd === "tap") {
    const i = BTN[t[1]];
    if (i === undefined) throw new Error(`emu: no button ${t[1]}`);
    if (cmd[0] !== "r") heldBtn |= 1 << i;
    apply();
    if (cmd[0] === "t") render(40);
    if (cmd[0] !== "p") heldBtn &= ~(1 << i);
    apply();
    render(cmd[0] === "t" ? 40 : 2);
  } else if (cmd === "key" || cmd === "tapkey") {
    const k = keyOf(t[1]);
    if (cmd[0] === "t") {
      heldKeys |= 1 << k; apply(); render(60);
      heldKeys &= ~(1 << k); apply(); render(30);
    } else {
      if (t[2] === "down") heldKeys |= 1 << k; else heldKeys &= ~(1 << k);
      apply(); render(2);
    }
  } else if (cmd === "turn") {
    const e = ENC[t[1]], n = parseInt(t[2], 10), gap = t[3] ? parseInt(t[3], 10) : 80, s = n > 0 ? 1 : -1;
    for (let m = n; m; m -= s) { ex.web_enc(e, s); render(gap); }
  } else if (cmd === "spin") {
    ex.web_enc(ENC[t[1]], parseInt(t[2], 10));
    render(1);
  } else {
    throw new Error(`emu: unsupported command '${cmd}' in ${scriptPath}`);
  }
}

// The host WAV is stereo interleaved 16-bit after a 44-byte header.
const raw = fs.readFileSync(wavPath);
const ab = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.length);
const dv = new DataView(ab);
const frames = (ab.byteLength - 44) >> 2;
const hostL = new Int16Array(frames), hostR = new Int16Array(frames);
for (let i = 0; i < frames; i++) {
  hostL[i] = dv.getInt16(44 + 4 * i, true);
  hostR[i] = dv.getInt16(44 + 4 * i + 2, true);
}

const lenDiff = Math.abs(frames - outL.length);
const n = Math.min(frames, outL.length);
let peak = 0, maxd = 0;
for (let i = 0; i < n; i++) {
  for (const [h, o] of [[hostL[i], outL[i]], [hostR[i], outR[i]]]) {
    const d = Math.abs(h - o * 32767);            // host LSBs: the host writes (int16)(l*32767)
    if (d > maxd) maxd = d;
    const a = Math.abs(o);
    if (a > peak) peak = a;
  }
}
const fb = new Uint16Array(mem.buffer, ex.web_fb(), 240 * 240);
const drawn = fb.some((p) => p !== 0), lit = ex.web_lit_keys() !== 0;
console.log(`emu: ${n} samples, peak ${peak.toFixed(3)}, max diff ${(maxd / 32768).toExponential(2)} ` +
            `(${maxd.toFixed(1)} LSB), length delta ${lenDiff}, screen ${drawn}, lights ${lit}`);
// r1: the WASM renders exactly the host's frame count (equal lengths), the run must be long enough
// to be a real comparison (160000 frames), both channels within 1 LSB (the tiny margin absorbs the
// host's float32 rounding of l*32767), and the screen and the lights must be drawn.
const ok = n > 160000 && lenDiff === 0 && peak > 0.05 && maxd <= 1.01 && drawn && lit;
process.exit(ok ? 0 : 1);
