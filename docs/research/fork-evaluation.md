# Fork evaluation: which FM-1 firmware to fork

A code-level comparison of the GPL-3.0 fork candidates, read on 2026-10-10/11 from the SHAs below. Nothing was built or run. Every one of these repos changes daily, so re-check SHAs before acting on this page.

Related: [ADR-0001](../decisions/0001-base-platform.md) · [firmware landscape](firmware-landscape.md) · [dev tooling](dev-tooling.md) · [architecture](../engineering/architecture.md)

| Repo | SHA | Version |
|---|---|---|
| hugelton/Felucca | `a4d7ca2` | 1.5.1 |
| charlesvestal/fm1-x0x | `6cffa40` | 1.0.5 |
| keremimo/melodee | `1a71d99` (`next`) | 1.1.0 |
| isod89/sloop-fm1 | `4365a1d` | 2.6 |
| Nicodesy06/FM1-MOVE | `0f213e8` (`fm1-move/dev`) | unreleased |
| w0ts/optimist | `47b8421` | 0.1 |
| zvenson/dinghy | `9297300` | 0.3 |

## Recommendation: fork X0X

**Fork X0X.** Then:
- **Port** FM6 and NOISE from Felucca, along with Felucca's newer gfx, HAL and loader fixes.
- **Borrow** queue semantics from Melodee, and the clip-bank and key-light ideas from FM1 Move.
- **Strip** the BREAK generator.

X0X is the only candidate that already has everything below. It also runs on hardware (1.0.5).
- **Per-part playing and cued patterns** (`ppat[]` / `pcue[]`).
- **Song record** of cues and mutes.
- **Per-pattern motion lanes.**
- **An engine command queue**, which is a natural tap point for a session log.
- **The `plat.h` boundary**, with context-struct sequencer and motion code.
- **A scripted headless scenario runner** with `expect`, PNG, WAV and LED output.
- **A WASM emulator with a native-vs-WASM parity test.**

**X0X's gap is sound.** It has no FM6 and no poly synth. FM6 in Felucca is a self-contained, integer-only, Apache-2.0 module, so this is the cheapest gap of any candidate. The other candidates' gaps are structural: data model, test harness, architecture.

## 1. Hardware maturity

- **Shared loader design.** Every fork inherits Felucca's loader. It runs from RAM and writes only `[0x4000, 0x93000)`. A power cut mid-update re-runs the loader. The mask-ROM UBOOT can't be overwritten.
- **Recovery without a working app** needs jl-uboot-tool or the Transporter (Felucca #61).

| Repo | Hardware | Rescue | Known problems |
|---|---|---|---|
| Felucca | Largest user base | bootguard: 2 boots dying within 30 s go to UBOOT. **Newest loader does a pre-erase CRC of the whole package.** No in-app rescue screen. | #61 (fixed), #211 drum crash (open) |
| **X0X** | Yes, 1.0.5 | **SAFE MODE** after 2 crashed boots (USB only, no audio). OCT−+OCT+ held 5 s goes to UBOOT. | Some units misread RAM on core 1 (x0x#10). 1.0.4/1.0.5 added a check word and canary, plus **automatic fallback to one core**. |
| Melodee | Yes, crash-prone on dual core | Felucca-1.0 bootguard | #18 red-screen crashes. #21 **raises core supply voltage** (do not port). |
| SLOOP | Yes | **Best rescue:** OCT− at power-on, or 2 failed boots, starts an in-app USB-MIDI updater | #93 Windows enumeration, #83 audio stall; 65 open issues |
| Dinghy | Yes (template) | SLOOP's recovery | — |
| Optimist | One unit | SLOOP's (untested on a device) | Own BLE stack |
| FM1 Move | **Never run on hardware** | SLOOP's | Unknown |

**Every fork branched from Felucca around 1.0 (2026-10-05) and none tracks upstream.** HAL and loader diffs against 1.5.1 run 712–1076 lines. Felucca has changed 12 platform files since 1.0:
- `fm1_input.h` debounce and encoder rework (+307 lines)
- `fm1_led_anim.h`
- the loader's pre-erase CRC
- `usb.c` (+431): UAC at 48 kHz, and descriptor order fixed for macOS 15
- `gfx.c` anti-aliased fonts

**Felucca publishes squashed one-commit-per-release dumps**, so pulling these is a hand port in every case.

## 2. Upstream health (GitHub API, 2026-10-10)

| Repo | Commits | Contributors | Releases | Open issues | Notes |
|---|---|---|---|---|---|
| Felucca | ~37 (squashed) | kurogedelic | 17 | 10 | AI disclaimer in README; PRs rarely merged |
| **X0X** | ~159 | charlesvestal 143, kurogedelic 15 | 19 | 5 | Commits co-authored with Claude. Fast responses (x0x#10 fixed in a day). Single maintainer. |
| Melodee | ~268 | keremimo 249 | 8 | 8 | Many dual-core fix branches |
| SLOOP | ~12 (squashed) | isod89 | 9 | 65 | Large backlog |
| Optimist | ~977 in 3 days | hdavid | 0 | 0 | Extreme churn |
| FM1 Move | ~36 | "Claude" 26 | 0 | 0 | Created 2026-10-10 |
| Dinghy | 7 | zvenson | 3 | 1 | Small, stable |

## 3. Sequencer model: cost of N clips per role with quantized launch

**X0X**
```c
typedef struct { dpart_t drum[NKIT]; bpart_t bass[NBASS]; brkpart_t brk; uint8_t swing; uint8_t rsv[3]; } pattern_t;  // NPAT 32
volatile uint8_t ppat[NTRACKS];   /* the pattern each part plays */
volatile uint8_t pcue[NTRACKS];   /* each part's cued pattern (0xFF = none) */
void seq_cue_part(seq_t *s, int part, int pattern);
```
- Cues are taken in `bar_end()` (`sequencer.c:230-290`). A part that changes restarts at step 1 while the others run on.
- In effect this is already 32 clips per role, with a per-role queue and launch on the bar.

Work needed:
- **Bar boundary.** The bar comes from the 909 part wrapping. Replace it with a global transport (beat, bar, phrase).
- **Per-clip settings.** Add launch quantize and loop/one-shot/gate modes.
- **Roles.** Turn the fixed parts into a role table.
- **Storage.** Store clips per role, not as whole-set patterns.

**Melodee**
- A `pattern_t {step_t step[NSTEP]; ...}` holds 8 banks per track, using `pattern` / `pattern_next`.
- Switches happen at each track's own loop end (`seq.c:961`). Re-selecting the active bank cancels the queue (`motion.c:417`).
- These semantics are good, but they are welded to Felucca's `track_t`, and RAM is tight.

**FM1 Move**
- An 8 KB packed `clip_mem` with `clip_off`/`clip_len[NTRK][NCLIP]`, `clip_cur` and `clip_next[]`.
- Launch is at the next bar in the ISR. The outgoing clip is stored, then the new one is decoded. Scenes and key-light states are included.
- A good memory design, but unverified on hardware.

**Optimist**
- 16 patterns per track. Launch defaults to the end of the pattern; OCT− held launches at the next bar, and OCT+ held launches now (`docs/PATTERNS-DESIGN.md`).
- A useful vocabulary of gestures.

**Felucca / SLOOP / Dinghy**
- One pattern per track (`track_t` embeds `step[NSTEP]`).
- Getting to clips means a new model.

| Base | Effort |
|---|---|
| **X0X** | **Low–medium** |
| Melodee | Medium |
| FM1 Move | Low on paper, but the base is untested |
| Felucca / SLOOP / Dinghy | High |

## 4. Recording → session log
- **X0X**
  - **Song record.** `song_on`+`song_rec` writes the live cues and mutes into 192 `song_bar_t`.
  - **Motion lanes.** `lane_t` × 160, recorded per step.
  - **Engine command queue.** `engine.c:100` is an SPSC queue of `uint32` (`type:4|a:8|b:8|c:12`) that carries *every* parameter change and note.
  - **Session log.** Timestamping that stream, plus cues and mutes, against the sample counter gives the take format, in about 300 lines.
- **Melodee:** `jam_loop()` snapshots banks into 16 rows at loop end. `recording.c` records notes.
- **Felucca:** 128 `{place,param,value}` motion events per project.
- **SLOOP:** SONG REC records the order sections are played in.

None of the four records a single linear timeline. X0X's command queue is the best tap point.

## 5. Engines, CPU, second core

| Repo | Engines | FM6 | Core 1 | Maths |
|---|---|---|---|---|
| Felucca | 13, 8 voices total | **Yes**: `fm6_core.c` (msfa, **Apache-2.0**, integer), `eng_fm6.c`, `fm6_vbank.c` | No | Integer |
| **X0X** | 909, 808, 2× 303 (Open303 + TB-3PO), BREAK | No | Yes (909/808 on core 1, with canary fallback) | **Hardware FPU** (`-mcpu=r3 -mfprev1 -ffp-contract=off`) |
| Melodee | PROPHET, FM6 (16 voices), CZ-1, SID, DRUM | Yes | Yes by default | FPU |
| SLOOP family | ~12, FM6 included | Yes | No | Integer |

- **Correction to the earlier hardware notes:** the AC79 **has a single-precision FPU** (dinghy#1, X0X build flags).
- **Design rule:** budget for **single-core worst case**. Some units can't run core 1, so treat it as headroom only.

## 6. Test and emulator infrastructure

| Repo | Host tests | Scenario runner | Screen checks | WASM | CI |
|---|---|---|---|---|---|
| **X0X** | 25 suites (seq vs TB-3PO reference, engines, motion, undo, lz, storage, OTA, loader) | **Yes**: `host/x0x_host.c` with `wait press tap tapkey turn spin midi wav shot leds expect reboot projimg`; 30 scenarios; `expect` keys include `playing pattern cue ppat mute songpos lanes noteons cpu2 view` | PNG shots, not golden-compared | **Yes**, plus a WASM-vs-native WAV parity test | None |
| Felucca | 70 files (storage, formats, fuzzers, engines, OTA) | No | **Yes**: `ui_render.c` sweeps every screen and lints for overlap and clipping; `regress.c` audio-hash goldens | Yes | None |
| Melodee | 94 (incl. dual-core TSan, FM6 parity vs Dexed) | No | ui_render | **No** | None |
| FM1 Move | 44 | No | Reference renders | Yes | None |
| Optimist | 192 | No | — | QEMU fm1-emulator | **Yes** |

## 7. Code quality
- **X0X**
  - About 25 kLOC, split into `app/`, `dsp/` and `seq/`. The 82-line `plat.h` is the only hardware boundary.
  - Sequencer and motion are context structs with sink callbacks. The engine is reached only through the queue.
  - Weak spots:
    - **`app/ui.c` is a 4,119-line monolith.**
    - It uses a unity build.
    - Floats in the sequencer and engine make bit-exact output depend on `-ffp-contract=off`.
  - **The BREAK generator is used "with the author's permission" from a repo with no licence**, so strip it.
- **Felucca:** about 34 kLOC across about 90 flat files. Heavy globals, with the sequencer coupled to tracks in the ISR. Dense, accurate comments and very clean licensing.
- **Melodee:** about 38 kLOC; Felucca-style globals plus features. Many fixes in flight.
- **Optimist:** about 78 kLOC and extreme churn. Hard to own.
- **FM1 Move:** AI-authored and never run on hardware. Treat it as a design reference.
- **Dinghy:** 2.7 kLOC and clean. Its "no FPU" rule is wrong.

## 8. UI framework
- **All candidates use Felucca's strip-canvas `gfx.c`.**
  - Felucca 1.5.1 adds 4-bit-alpha anti-aliased Inter Tight text.
  - X0X still has the older 1-bit fonts. **Porting Felucca's `gfx.c` and `gen_aa_font.py` is a self-contained legibility upgrade.**
- **X0X views**
  - An enum (`V_HOME, V_PART, V_GEN, V_FX, V_MIX, V_GLO, V_SONG, V_PERF, V_SEQ`) plus overlays `O_LIST`/`O_ASK`.
  - Four bands, each blitted only when it changes.
  - Grammar: ALGORITHM = part, PRESETS = pattern, SELECT = move, SEL = yes, HOME = back.
  - Hold any button for 1 s to get a help card.
  - LEDs go through `plat_leds`, `plat_keys_dim` and `plat_play_red`, which is enough for clip states.
  - **LIVE becomes a new view with a layer flag.** Split `ui.c` first.
- **Melodee** draws queued banks dashed (`ui_patterns.c`), which is a good visual reference.

## Ranking

| # | Base | Reason |
|---|---|---|
| **1** | **X0X** | The right model, architecture and test infrastructure, and proven on hardware. Gaps: no FM6 or poly synth, an older HAL and gfx, the `ui.c` monolith. |
| 2 | Felucca | The strongest platform and sound, and the upstream for all the others. But it means rewriting the sequencer, and it has no scenario runner. |
| 3 | Melodee | Banks with a queue, FM6 and dual core, but no WASM, crash reports, and voltage tweaks. Use it as a source of ideas. |
| 4 | Dinghy | Safe and minimal, but we'd write everything ourselves. |
| 5 | SLOOP | One pattern per track, no emulator, and a long issue backlog. |
| 6 | Optimist | Churn, BLE, profile complexity. |
| 7 | FM1 Move | Never run on hardware. Use as a design reference only. |

## Fork plan

**Fork `charlesvestal/fm1-x0x` at `6cffa40` (v1.0.5) with full history.** Add Felucca as a second remote so its platform changes can be diffed.

- **M0, restructure:**
  - `seq/`, `dsp/`, the model and the UI move to `core/`. `plat.h` becomes `core/plat.h`.
  - `plat_fm1`, `main_fm1`, `panel`, `hal/` and `loader/` move to `targets/fm1`.
  - `host/x0x_host.c` moves to `targets/host`, with JSON state dumps added.
  - `web/emu` moves to `targets/wasm`.
  - Add a Felucca-style screen lint and audio-hash goldens.
- **M1, model:**
  - Replace the fixed parts with a role table: kind is drum, mono, poly or noise, plus an engine id.
  - Store a per-role clip pool, using FM1 Move's packed bank.
  - Run a global transport.
  - Add per-clip launch quantize, using Melodee's loop-end semantics, and a per-clip mode.
  - Keep the `pcue` cancel behaviour.
  - Store songs in N slots via `plat_store_*` + `lz.c`.
- **M2, takes:** timestamp everything that crosses the command queue, plus launches, mutes and FX, using the sample counter. Reuse the `song_rec` playback path.

| Port | From | Licence / attribution |
|---|---|---|
| FM6 (`fm6_core.c`, `eng_fm6.c`, `fm6_vbank.c`, voice allocation) | Felucca | `fm6_core.c` is Apache-2.0 (msfa: Google / Pascal Gauthier); keep the header and `LICENSES/Apache-2.0-msfa.txt`. The rest is GPL-3.0 © Leo Kuroshita / Hügelton Instruments. |
| NOISE engine (`eng_noise.c`) | Felucca | GPL-3.0 |
| AA `gfx.c` + `gen_aa_font.py` + Inter Tight | Felucca | GPL; ship `LICENSES/OFL-InterTight.txt` (OFL-1.1) |
| HAL fixes since 1.0 (input, loader CRC, `usb.c`) | Felucca | GPL; hand-port each one and diff-review it |
| Dual-core hardening | Already in X0X (from Melodee #21 and Jangada) | Keep credits. **Do not** port `fm1_power.h` voltage changes. |
| OCT−-at-power-on rescue (optional) | SLOOP/Dinghy `recovery.c`, `bootguard.h` | GPL; credit isod89 and zvenson |
| Ideas only | FM1 Move key-lights and scenes; Optimist OCT−/OCT+ launch modifiers; Melodee dashed queued banks | Credit in the docs |

**Strip:**
- the BREAK generator (permission-only licence)
- break sample slots and upload tools
- TB-3PO auto-mutate (optional)
- the 808, if flash is tight
- the PERF screen, in release builds

**Natural role mapping after the port:**

| Role | Engine |
|---|---|
| Drums | 909 (and 808) |
| Bass | 303 |
| Lead / Chords | FM6 |
| Noise/FX | NOISE |

## Risks
1. **Platform drift.** We inherit a Felucca-1.0-era HAL. Fixes have to be hand-ported, and upstream only publishes squashed dumps.
2. **Single maintainer upstream.** Plan to own the code.
3. **Floats in seq and engine.** Prefer integers in new `core/` code.
4. **Unmeasured CPU and RAM** for FM6 poly + 909 + 303 on one core. X0X's build demands 8 KiB of pool spare. Measure early on the PERF screen.
5. **Core-1 silicon variance.** Never require core 1.
6. **`ui.c` monolith.** Split it before adding LIVE.
7. **Recovery.** Keep the `ldr_test`, `ota_test` and `install_test` tests green. Agents never flash.

## Sources
- Repos:
  - https://github.com/charlesvestal/fm1-x0x
  - https://github.com/hugelton/Felucca
  - https://github.com/keremimo/melodee
  - https://github.com/isod89/sloop-fm1
  - https://github.com/Nicodesy06/FM1-MOVE
  - https://github.com/w0ts/optimist
  - https://github.com/zvenson/dinghy
  - https://github.com/zednaked/jangada
- Issues:
  - [x0x#10](https://github.com/charlesvestal/fm1-x0x/issues/10)
  - [melodee#17](https://github.com/keremimo/melodee/issues/17)
  - [melodee#18](https://github.com/keremimo/melodee/issues/18)
  - [Felucca#61](https://github.com/hugelton/Felucca/issues/61)
  - [Felucca#211](https://github.com/hugelton/Felucca/issues/211)
  - [dinghy#1](https://github.com/zvenson/dinghy/issues/1)
- Method: line diffs of `firmware/hal/*` and `firmware/loader/*` against each Felucca release, with Felucca unshallowed.
