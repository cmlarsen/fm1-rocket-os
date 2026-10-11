# TRAM: restore pool headroom

Priority task from the coordinator. `integration`'s build fails its own pool check: after T8i's 14 KB
song-open scratch landed beside T10i's 33 KB `perform_t`, the release pool had only 4,516 B spare and the
debug 2,724 B, both under the required 8,192 B. TRAM shares buffers back to a **≥ 10,240 B** target on
both. Nothing was flashed; only the build check was run.

- Branch `cmlarsen/tram`, from `integration`.
- `make unit-core scenarios screens-lint parity check fw fw-debug` and `tools/check_loader.sh` on both
  packages all pass (see below). Every scenario golden is byte-identical.

## The problem

`grep` of the `.pool` users showed the pool carried **two** 14,352 B `song_t` scratch buffers:

- `firmware/src/app/song_io.c` — `static song_t scratch` (T8i: one song off flash, for the SONGS
  open/load and `clip_count` paths);
- `core/model/song_store.c` — `static song_t sst_recommit_scratch` (T6i: the delete/undelete recommit
  path, so it is not a 14 KB stack frame).

Both are main-loop only. `unit-core`'s guard (below) proves they never nest.

## Step 1 (done): one shared `song_t` scratch

The scratch moved to `song_store.c` as `sst_scratch`, exposed through two functions in `song_store.h`:

```c
song_t *song_store_scratch_acquire(void);   /* returns the buffer, takes the guard */
void song_store_scratch_release(void);
```

- `sst_recommit_song` (delete/undelete) acquires and releases around its load/write.
- `song_io_songs_open` and `song_io_songs_clip_count` acquire, load into the shared buffer, then release;
  `song_io`'s second static is gone.

Freeing the duplicate is **+14,352 B** of pool.

### Call-graph proof and the nested-use guard

The two paths never call each other while the buffer is held: `song_io`'s open path calls
`song_store_load` (which never recommits), and the recommit path calls only the store's own internals.
`undo_stack_undo`/`_redo` reach `song_store_delete`/`undelete` from `song_io` with no buffer held. The
guard makes that a checked invariant rather than a claim:

```c
song_t *song_store_scratch_acquire(void)
{
#if defined(X0X_HOST) || (defined(X0X_DEBUG) && X0X_DEBUG)
    if (sst_scratch_held)
        __builtin_trap();                 /* TRAM: the scratch is not reentrant: a nested use */
    sst_scratch_held = 1;
#endif
    return &sst_scratch;
}
```

It is active in the host unit suites (`-DX0X_HOST`) and in the device debug build (`X0X_DEBUG=1`); a
release build compiles the flag out. `make unit-store unit-songio` (both plain and ASan/UBSan) pass with
the trap armed, which is the proof.

Steps 2 (a separate lz work buffer in `song_store` vs `project.c`) and 3 (the ~12 KB undo ring) were
**not needed**: step 1 already clears the target, so per the spec the task stops there. `project.c`'s
`lz_head` stays in `.bss` and `song_store`'s in `.pool`; the undo ring is unchanged, so
`docs/engineering/model.md`'s undo depth is unchanged.

## Memory (deltron)

| Build | Pool | spare | `.data+.bss` |
|---|---:|---:|---:|
| release (`make fw`) | **325,196 B of 344,064** | **18,868 B** | 96,128 B of 98,304 |
| debug (`make fw-debug`) | **326,988 B of 344,064** | **17,076 B** | 96,228 B of 98,304 |

Both are above the 8,192 B requirement and the TRAM 10,240 B target. The release pool returns exactly to
its post-T10i value (325,196 B); T8i's second scratch was the whole regression. `architecture.md`'s RAM
plan table and a "Measured after TRAM" paragraph carry these numbers.

## No behaviour change

- `make scenarios`: every scenario passes and **every golden is byte-identical** (`golden ok` for all).
- `make screens-lint`: 1026 frames, 2100 canvases, 0 findings.
- `make parity`: WASM matches native (audio, screen, lights); max audio diff 3.05e-5 (the standing 1 LSB
  tolerance).
- The change is RAM placement plus a debug-only guard; no audio, UI or storage path changes.

## Ladder

```
make unit-core        ok
make scenarios        ok (all goldens byte-identical)
make screens-lint     ok (0 findings)
make parity           ok (WASM == native)
make check            ok (fw + forbidden symbols + flash/no-sample lint)
make fw               ok (pool 325,196 / 344,064)
make fw-debug         ok (pool 326,988 / 344,064)
tools/check_loader.sh build/x0x.fwsc        ok (X0X 1.0.5 loader)
tools/check_loader.sh build/debug/x0x.fwsc  ok (X0X 1.0.5 loader)
```

## Ownership note

Changes stay in the listed paths: `core/model/song_store.c` and `firmware/src/app/song_io.c` (RAM
placement / buffer sharing), with the two accessor declarations added to `core/model/song_store.h` and
the RAM table in `docs/engineering/architecture.md`. `perform_t` and `dly_buf` were not touched. Nothing
was flashed.
