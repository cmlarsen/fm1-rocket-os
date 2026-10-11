# T11c review (Opus): merge after one fix

The reviewer's scratch test is `/private/tmp/claude-502/-Users-caleb-CalebApps-fm1-rocket-os/10296272-f272-4eb6-9e65-5ca02d389a5d/scratchpad/t.c`. Turn it into a real test.

1. **Dropped clock ticks corrupt the tempo** (`midi_in.c:55`).
   - Make the gap test relative to the current estimate: an interval of more than 1.5 times the expected tick interval is a missed-tick gap. Neither it nor the next interval may enter the tempo window. If the gap is a near-integer multiple of the expected interval, you may divide it by that multiple.
   - Keep `clock_lost` for "no ticks for longer than 1 beat at the last tempo", which is also relative to the estimate. It must work above 240 bpm.
   - Tests: at 120 bpm with 2 and 4 dropped F8s, and at 250 bpm with 5 dropped, the estimate stays within ±1 bpm. A real stop still fires `clock_lost`.
2. Minor fixes:
   - `clock_lost` must also fire after a single F8 when no tempo was ever established. Use a fixed timeout of 1 second for that case.
   - A stray F7 clears running status.
   - Document the 20 bpm floor.
   - Write in reports/T11c.md that X0X's TRS and USB bytes arrive in one shared ring (`midi_uart.c:6`). T11i therefore uses one instance per source if the ring can tell the sources apart, otherwise one instance. It must call `midi_in_tick` in the same context as the byte feed.

Then run `make unit-midi` (plain and sanitizer) and the device compile, commit, update the report, and send worker_done.
