# ADR-0004: Remove Classic patterns and shorten the delay line to 0.75 s

- **Status:** Accepted, 2026-10-11 (owner).
- **Context:** T7a moved X0X's own pattern editors and sequencer behind **GLOBAL → Classic patterns**, so the original X0X workflow stayed reachable. That left a third context next to STUDIO and LIVE, with its own header (CLASSIC), its own exits and its own audio owner. The owner found it confusing. Separately, after takes (T9i) and MIDI input (T11i) the RAM pool sits only a few KB above X0X's required 8 KB spare ([open questions](../open-questions.md)).

## Decision
- **Classic patterns are removed from the product.** There is no GLOBAL → Classic row, no CLASSIC header, and no path into X0X's pattern editors or its pattern sequencer. The firmware has two modes, STUDIO and LIVE, and nothing else that owns the audio.
- **X0X's project patterns no longer live in RAM.** That frees about 17 KB of pool. A first boot or an empty song slot builds the song from the song templates instead of from the X0X project. **First boot is the Starter template** (owner, 2026-10-11: a new device should play something at once); the New-song screen still offers Empty, Starter or Copy.
- **The delay line shrinks from 1.0 s to 0.75 s** (`DLY_LEN` 44,100 → 33,075 samples), freeing about 22 KB. Delay times are clamped to the shorter line; at 80 bpm a synced quarter note still fits.
- **The X0X sound pages stay** (909/808/303 pages, FM6, NOISE), as [ADR-0003](0003-sound-design-scope.md) says.

## How this squares with "fork, don't rebuild"
Inherited files are not deleted or renamed, so upstream fixes still merge. Classic is cut at the UI and at the build: the UI paths are removed, and the pattern storage and sequencer stop being allocated or run. The X0X project region on flash is left untouched (never erased), so returning to stock X0X still finds it.

## Consequences
- About 39 KB of pool comes back for later features.
- X0X's own pattern workflow is gone for owners who wanted it; stock X0X firmware remains the way to get it.
- Scenarios, goldens and help cards that reference Classic are removed or regenerated.
