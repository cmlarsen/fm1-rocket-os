# Dev tooling prior art: emulators, host runners, tests, screen design

This page covers how other FM-1 firmwares build and verify UI, UX and sequencer behaviour without flashing hardware. Researched 2026-10-10. The repos were shallow-cloned to a scratchpad and read but never built.

Tags: **[verified]** means read in source. **[reported]** means taken from a README or web page.

Related: [architecture](../engineering/architecture.md) · [agentic dev loop](../engineering/agentic-dev.md) · [screen design workflow](../engineering/screen-design.md) · [hardware](fm1-hardware.md)

## Summary

- **Felucca's web emulator is the real firmware C compiled to WASM** with Emscripten. It is a unity build in which host stubs for the HAL are defined *before* the firmware `.c` files are `#include`d. It runs inside an AudioWorklet, and the audio clock drives the device clock. No firmware source changes. [verified]
- **A common pattern has emerged across FM-1 firmwares:**
  - one C codebase behind a thin platform boundary;
  - a headless host runner that turns a script into PNG, WAV and `expect` results;
  - the same code built as WASM;
  - golden audio hashes;
  - PNG sweeps of every screen, with a layout lint.
- **X0X** (`host/x0x_host.c`) and **ChoralRoot** (`tools/emu`) have the cleanest scenario runners.
- **Two binary-level emulators run unmodified `.fwsc` files:**
  - `simonjohansson/fm1-emulator`: QEMU, GPL-2.0, headless, timed input scripts, LCD capture.
  - `shcherbakov7/m-wave-fm1-browser-emulator`: Rust/WASM.
- **None of the firmwares uses LVGL, u8g2 or Adafruit GFX.** Each ships its own small renderer.
- **The device has no Rust target.** The only compiler is JieLi Clang 4.0.1, so device code is C99/gnu99 or a C++11 subset.

## Felucca emulator internals [verified]

`web/emu/felucca_web.c` (486 lines) includes `tests/hostsim.c`. That file pulls in libc, the engines, voice, fx, usb, midi, song_chain and seq, followed by `audio.c` (the real ISR `fm1_alnk0_irq`), gfx, panel, the ui_* files, storage, project, master and main.

| HAL area | Web substitute |
|---|---|
| Clock | `web_ticks` / `fm1_ms`, advanced in `web_ms()` |
| Audio | Calls the real ISR once per 128-frame half. `web_render(n)` runs the device until `n` frames exist. |
| Main loop | `web_frame()` (master_poll, ui_input, autosave_poll, ui_leds, ui_draw) every 15 ms. `ui_input()` every 1 ms. |
| Input | `web_buttons(mask)`, `web_keys(mask)` (27 keys), `web_enc(role, n)` |
| LEDs | `web_lit_buttons`, `web_lit_keys`, `web_dim_*`, `web_anim_levels` |
| Display | `uint16_t web_fb[240*240]` RGB565; `web_screen()` |
| Storage | 1 MiB NOR array (programming can only clear bits). Dirty sectors are persisted to browser storage. |
| MIDI | `web_midi(status, d1, d2)` |
| Not emulated | USB, TRS MIDI, serial console, battery, crash/UBOOT |

**Build and run**
- Build command: `emcc -O2 -ffp-contract=off -std=gnu99 --no-entry -sSTANDALONE_WASM ...`. No JS glue is generated.
- The build needs `build/gen/felucca_tables.h` from the full firmware build, which means Docker plus JieLi. **We should avoid that coupling.**
- `worklet.js` instantiates the wasm inside the worklet and calls `web_render(128)` per quantum. Inputs are held for at least 24 ms of device time. Screen, LEDs and flash state are posted back about 30 times per second.
- Hosted at <https://hugelton.github.io/Felucca/webapp/try/>.

**`web/emu/emu_test.mjs` drives the wasm from Node, with no browser.** It checks:
- boot;
- that a key sounds and lights up;
- that PLAY starts;
- that a preset persists across instances;
- **bit-identical audio across runs**;
- **native vs WASM bit-exactness** (`native_check.c`);
- a CPU budget of less than 0.5× realtime.

### Felucca host tests (`tests/`, about 60 files) [verified]
- `regress.c` hashes every engine × preset against `golden.txt` and also checks clipping, DC, peak, freed voices and the silent tail. Rerun with `GOLDEN_UPDATE=1` to refresh.
- **CPU budgets.**
  - `cpu_baseline.txt` records host instructions per sample, with ±25% tolerance.
  - `target_budget.py` counts **static loop instructions in the pi32v2 disassembly**, with ±10% tolerance. This gates CPU cost without hardware.
- **`ui_render.c` (2,378 lines) renders every screen in every palette** to PPM. `ui_render.py` converts them to PNGs and contact sheets.
- **Layout lint.** `gfx.c` has compile-time hooks (`GFX_HOOK_TEXT/CELL/INK/ALIGN/RULE`) that flag:
  - text that is off-screen, clipped, overlapping or spilling out of its cell;
  - text drawn in the background colour;
  - misalignment of more than 1 px;
  - palette impurity.

  Any finding exits 1.
- `ui_test.c` tests the UI state machine against stubs.
- Feature tests: input debounce, swing, ratchet, chords, `persistence_test.c` (power cut mid-write), plus fuzzers for project load, the editor and samples.
- **There is no CI.**

## Other firmwares and tools

| Project | Licence | What's reusable |
|---|---|---|
| **X0X** ([repo](https://github.com/charlesvestal/fm1-x0x)) | GPL-3.0 | **The cleanest boundary, `firmware/src/app/plat.h`**, implemented by `plat_fm1.c` on the device and `x0x_host.c` on the host. `host/build_host.sh` needs only `cc` and Python, **with no JieLi toolchain**. 30 scenario scripts in `tests/scenarios/*.x0x` produce WAV, PNG, LED dumps, MIDI logs and `expect` results. Code is split into `src/{app,dsp,seq}/`. |
| **Dinghy** ([repo](https://github.com/zvenson/dinghy)) | GPL-3.0 | **A minimal platform template**: Felucca's OTA, USB rescue, USB-MIDI and audio, panel, screen and settings, with a 4-voice sine on top. Build checks reject 64-bit division, chip-ROM calls, RAM overrun and register access outside `hal/`. |
| **ChoralRoot** ([repo](https://github.com/Quixotic7/ChoralRootFM1)) | GPL-3.0 | `emu --headless --script FILE --shot --wav`: simulated time, bit-exact, `expect` with exit codes. `EMU_PANEL_CHECK` compares partial redraws against full redraws. `wavclicks.py` and `wavpitch.py`. Boundary header `tools/emu/emu_hooks.h`. |
| **Lunar Modulator** ([repo](https://github.com/ip2k/lunar-modulator)) | **MIT** | **The best CI**: Ubuntu and macOS matrix, Playwright on Chromium, Firefox and WebKit, an MIT-only build job, and about 4,500 screens layout-checked. `docs/14-verification-ladder.md` sets out the ladder desktop → -m32 → ASan/UBSan → WASM → board → device, using `-ffp-contract=off` and a pinned libm. Has `CLAUDE.md`/`AGENTS.md`. Engine ABI with MI ports. **Has never run on an FM-1.** |
| **Optimist** ([repo](https://github.com/w0ts/optimist)) | GPL-3.0 | Drives a Rust FM-1 emulator through a line protocol (`run`, `hold`, `turn`, **`peek SYMBOL`** to read firmware RAM by ELF symbol, `nordump`). CI installs the JieLi toolchain. |
| **Jangada** ([repo](https://github.com/zednaked/jangada)) | GPL-3.0 | A CI recipe: firmware build with the toolchain cached, host tests, a reproducibility check, and a zig-cc wasm test. |
| **fm1-emulator** ([repo](https://github.com/simonjohansson/fm1-emulator)) | GPL-2.0 | **QEMU with a pi32v2 CPU and an FM-1 board model.** Runs unmodified `.fwsc` on both cores. Prebuilt for macOS arm64. Runs headless at about 1.5× realtime on an M3 Pro [reported]. `FM1_POC_INPUT=NS:QCODE:1,...` (or `@FILE`) gives timed input. `FM1_POC_CAPTURE_NS` captures the LCD, SRAM and state. QMP, a UART log, and seeded stress tests. |
| **m-wave-fm1-browser-emulator** ([repo](https://github.com/shcherbakov7/m-wave-fm1-browser-emulator)) | GPL-3.0 | Rust core compiled to WASM. Runs `.fwsc` in the browser at about 50% realtime. `cargo run --example diagnose`. |
| **MvaveFM1-Designer** ([repo](https://github.com/Quixotic7/MvaveFM1-Designer)) | **MIT** | Single-file HTML designer for panel LED states, knob labels and **240×240 screen mockups in Felucca palettes**. Its **JSON format is meant to be edited by humans and LLMs** (`FORMAT.md`). PNG export. |
| **FM1VST** ([repo](https://github.com/Quixotic7/FM1VST)) | GPL-3.0 | Wraps unmodified Felucca-family sources behind a C ABI (`core-api/fm1core.h`) inside JUCE. Has a `tests/script_runner`. |
| fm1-workbench | GPL-3.0 | An MCP server for the FM-1 over MIDI (stock firmware). |
| FM-1-RE | WTFPL | Reverse engineering of the stock firmware. |
| fm1-nes | Apache-2.0 | Builds with the JieLi AC79 SDK from source (RTOS path). |
| fm1-polyseq | **No licence** | Reference only. Do not reuse. |
| GrooveOS emulator | Closed | Not documented. |

### Licensing
- Taking Felucca's platform code, directly or through Dinghy or X0X, makes the result **GPL-3.0-only**.
- **Do not link JieLi's proprietary SDK or RTOS libraries into a GPL image.** Felucca stays bare-metal and uses only three Apache-2.0 SDK files at packaging time.
- A permissive project would need a clean-room HAL. Lunar's `docs/01-hardware.md` lists pins and registers with sources. It could then reuse only MIT and Apache code (Lunar, DaisySP, Mutable Instruments, msfa, the Designer).

## Display and UI rendering
- **Panel:** 1.54" 240×240 IPS, ST7789V, RGB565 over SPI1 at about 12 MHz.
- **PA2 controls the backlight and also gates the key matrix and the 595 LED chain.** Driving it to turn the backlight off kills the panel (ChoralRoot README).
- **LEDs:** 27 white key LEDs and 14 button LEDs. REC is red, and PLAY has an extra green LED. Dim and breathe levels come from PWM timing.
- **Felucca's `gfx.c`:**
  - draws into a strip canvas and DMAs it to the panel, with no full framebuffer;
  - uses 4-bit alpha font and icon bitmaps rasterised at build time (Inter Tight, OFL; Fukiai icons, MIT), with sub-pixel phases and kerning;
  - takes palette tokens from `ui_palettes.h`, which `tools/gen_ui_palettes.py` generates.

| Library | Licence | Simulator | Fit |
|---|---|---|---|
| LVGL | MIT | SDL and Emscripten simulators; screenshot tests built in | Costs flash and CPU next to the audio ISR. Clang 4.0.1 support for v9 is unverified. It is a widget toolkit we don't need. |
| u8g2 | BSD-2 | SDL | Mainly monochrome, a poor fit |
| Adafruit GFX | BSD | none | No advantage |

**Recommendation:** use a small custom renderer in Felucca's style, with deterministic output, snapshot tests and lint hooks.

## Prior art beyond the FM-1
| Project | Pattern |
|---|---|
| Deluge community firmware | CppUTest unit tests, 32-bit unit tests, fuzzers, QEMU specs, USB-MIDI integration tests |
| monome teletype | `simulator/tt.c` host REPL, plus the greatest test framework |
| libDaisy / DaisySP | gtest host tests for menu logic; MIT DSP |
| Mutable Instruments → VCV | Unchanged firmware DSP compiled into a desktop plugin |
| Memfault + Renode | Binary-level tests take seconds; host unit tests take milliseconds |

## Sources
- Felucca: `web/emu/felucca_web.c`, `worklet.js`, `emu_test.mjs`, `build.sh`, `tests/*`, `firmware/src/gfx.c`, `firmware/hal/fm1_lcd_hw.h`, `LICENSING.md` (https://github.com/hugelton/Felucca)
- X0X: `host/x0x_host.c`, `host/build_host.sh`, `firmware/src/app/plat.h`, `tests/scenarios/`
- Lunar: `docs/01-hardware.md`, `docs/05-open-source-feasibility.md`, `docs/14-verification-ladder.md`, `.github/workflows/ci.yml`
- fm1-emulator: `README.md`, `docs/development.md`
- Optimist: `tests/emu/run.py`, `tests/emu/drv/main.rs`
- Jangada: `.github/workflows/ci.yml`
- Other references:
  - [Deluge tests](https://github.com/SynthstromAudible/DelugeFirmware/tree/main/tests)
  - [teletype](https://github.com/monome/teletype)
  - [libDaisy tests](https://github.com/electro-smith/libDaisy/tree/master/tests)
  - [LVGL tests](https://github.com/lvgl/lvgl/tree/master/tests)
  - [Memfault Renode](https://community.memfault.com/t/firmware-testing-with-renode-and-github-actions-interrupt/229)
