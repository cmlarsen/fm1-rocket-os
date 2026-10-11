# T5c: port FM6 and NOISE as standalone engines

Core lane for T5 in [build-plan.md](../build-plan.md#lanes-v3-2026-10-11). Other workers are changing `engine.c`, `ui.c` and the sequencer at the same time, so **only add new files**; do not wire the engines into X0X's engine, UI or project yet (that is T5i).

Read first: `CLAUDE.md`, the worker rules in `docs/engineering/build-plan.md`, `docs/research/fork-evaluation.md` (sections 5 and Fork plan), and how an existing X0X engine in `firmware/src/dsp/` is shaped (its init / note / param / render functions and how `engine.c` calls it).

**Ownership (new files only):** `firmware/src/dsp/fm6_core.*`, `firmware/src/dsp/eng_fm6.*`, `firmware/src/dsp/fm6_vbank.*`, `firmware/src/dsp/eng_noise.*`, `tests/host/fm6_*`, `tests/host/noise_*`, `LICENSES/Apache-2.0-msfa.txt`, and a `make unit-engines` target appended to the Makefile.

## Steps
1. Copy from Felucca (`git show felucca/main:<path>`; find the files with `git ls-tree -r --name-only felucca/main | grep -E "fm6|noise"`): `fm6_core.c/.h` (Apache-2.0, msfa: keep its header verbatim and add `LICENSES/Apache-2.0-msfa.txt`), `eng_fm6.c/.h`, `fm6_vbank.c/.h` and the voice bank data, `eng_noise.c/.h`, plus whatever small helpers they need (copy those under new names if X0X has a different file with the same name). Keep each file's GPL / Apache header and add a line: `Ported from Felucca (hugelton/Felucca <sha>)`.
2. Adapt them to X0X's engine shape with a thin adapter in `eng_fm6.c` / `eng_noise.c`: init, note on/off with velocity, parameter set, render a block into X0X's sample format and block size. FM6 must be polyphonic with voice allocation (Felucca's), with a voice count constant that T5i can tune for CPU.
3. Rules: integer DSP as in Felucca, no heap, no `double`, no 64-bit division, nothing that requires core 1.
4. **Tests (the main acceptance):** a host test builds Felucca's original engine code (from `git show felucca/main:...` into `build/ref/`) and the ported engine, plays the same note script through both for a few presets and the noise engine, and requires **bit-identical output**. If X0X's block size or sample format forces a difference, compare after the same conversion and explain it. Also test: all voices stolen correctly at polyphony limit, silence after note-off + release, no clipping beyond full scale.
5. Build the new files for the device on deltron (compile them with the JieLi toolchain the same way `build.sh` compiles X0X's dsp files, even though they aren't linked yet) and report code size and `.data`/`.bss` per file.

## Done
- `make unit-engines` passes on the Mac. Device compile of the new files succeeds.
- Small commits with the attribution lines from `CLAUDE.md`'s instructions.
- `reports/T5c.md`: files ported with Felucca SHA, adapter API, sizes, test results, anything T5i needs to know.
- `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T5c.md`.
