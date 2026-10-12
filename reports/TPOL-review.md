# TPOL review (Opus, 2026-10-11)

No blockers. `make unit scenarios screens-lint` passed (0 lint findings). Fixed in TPOL r1. The reviewer's four repros became `studio_home_knob`, `studio_clear_abort`, `studio_tap_home_first` and an extended `tlset_lock`.

## Should-fix (STUDIO HOME gestures, `ui_studio.c`)
1. Every HOME + pad press started the clear ring, and every release launched, so a clear abandoned mid-ring fired the clip. Now a release under 250 ms (the SEL clear's threshold) is a tap and launches; past it the ring shows, and an early release keeps the clip without launching.
2. Stage lock: every HOME + tap showed LOCKED, and a refused hold launched on release. The lock is now checked only past the threshold, and a refused hold never launches.
3. On the song home, HOME + K1–K4 edited the clip (LENGTH 16 → 36), while the cheat sheet said it clears a lane. It now does nothing there, and the sheet row shows only where lanes exist.
4. Releasing HOME before the pad lost the launch. The pending tap now resolves on whichever release comes first.

On r1, the reviewer's `r4_lock_tap` repro failed because of its own expectations: `liveclipN` is per role, and key 1 is drums clip B (role 0). `tlset_lock` pins the corrected behaviour.

## Nits (done in r1)
- ux-principles: SEL's two jobs in LIVE.
- A stale help comment (six rows → five).
- The Felucca "SCL" note restored in `plat.h`.
- Cited spelling kept in fm1-hardware.md.
- The stale Classic wording in the build-plan TPOL bullet.
- Glossary wording.

## Verified OK
- **Take offset:** `drain` tapped at the block end while replay tapped the block base. The fix is host-only (`engine_tap`), so device audio is unchanged, and `eventscmp` is a real compare.
- **Shift sheet:** dropping the FX row is correct, because HOME+FX reset exists only in LIVE.
- **Footer:** the footer fix holds at 1:1, and screens-lint catches a regression.
- **SCL rename:** clean.
- **Goldens:** all 72 changed PNGs are explained.
