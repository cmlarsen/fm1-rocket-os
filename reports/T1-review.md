# T1 review (Opus): merge after these fixes

1. **tools/check_loader.sh must select the loader the way the device does.** `firmware/src/ota.c:249` takes entries of type 100, and the last one wins; today the script takes the first entry *named* `ota.bin`. Mirror `ota_ufw` exactly:
   - select type-100 entries and take the last;
   - reject a package that has more than one type-100 entry;
   - verify the header CRC, the entry-list CRC, the `AC791N` chip tag, and that `nent` is 1–11;
   - require `len(data) == ln`;
   - catch `struct.error` and every other parse failure, then exit 2 (fail closed).
   Add tests: a crafted package with an `ota.bin`-named decoy entry plus a different type-100 entry must FAIL, two type-100 entries must FAIL, a bad CRC must FAIL, and the real 1.0.5 package and our build must PASS.
2. **The flashing gate checks the package, not `build/loader/ota.bin`.** Make `tools/check_loader.sh` take only a `.fwsc` path. Remove directory mode, or make it check every `*.fwsc` in the directory and fail if any fails.
3. **Delete the dead BREAK footer branch** at `firmware/src/app/ui.c:3536-3538` ("PIN A SLICE" / "9 REV 10 HALF 11 STUT").
4. **Remove the stale "BB Gen's break generator" credit** at `web/x0x_installer.html:64`, and grep the whole tree once more, case-insensitively, for `bb gen`, `slice` and `stut` leftovers from BREAK.
5. **Makefile rsync excludes need leading slashes:** use `--exclude=/build` and `--exclude=/.venv`, so nested `build` paths are not dropped.
6. **Makefile `REMOTE_DIR` must be unique per checkout:** append a short hash of `$(CURDIR)`.
7. Add compile-time size asserts for `pattern_t`, `song_bar_t`, `sound_t` and `settings_t` at their current sizes, so storage layout changes fail the build.

Then re-run `make test-local`, `make fw`, and `tools/check_loader.sh build/x0x.fwsc`; commit; update reports/T1.md; send worker_done.
