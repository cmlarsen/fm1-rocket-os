# fm1-rocket-os top-level Makefile (T1).
#
# X0X's own scripts stay where they are: this Makefile only wraps them. The firmware
# toolchain (JieLi clang for pi32v2) is Linux x86-64 only, so `make fw` and `make check`
# build on $(FW_HOST) (default deltron.local): the tree is rsynced there, the script runs
# over ssh, and the build artefacts are rsynced back. `make test-local` runs the host tests
# on this machine (a subset: see reports/T1.md).
#
#   make fw           build on $(FW_HOST); brings back build/x0x.fwsc, x0x.bin, loader/ota.bin
#   make release      like fw but ./build.sh --release $(VERSION) (reproduces an X0X release)
#   make test         run tests/run_tests.sh on $(FW_HOST); brings back build/scenarios/
#   make test-local   run tests/run_tests.sh here (the subset that works off-Linux)
#   make unit         the host unit suites that run on macOS (tests/run_tests.sh --unit)
#   make scenarios    every tests/scenarios/*.x0x, into build/scenarios/<name>/, goldens checked
#   make screens      the screens_*.x0x tour (T2b) + build/screens/contact.png at 1:1
#   make parity       WASM output vs native, byte for byte (needs emcc and node)
#   make check        make fw plus the build's own checks (soft-double, size)
#   make push/pull    rsync the tree to / from $(FW_HOST)
#   make help         this list
#
# (make emu-binary — fm1-emulator/QEMU — is not here: T1's step 8 was skipped on the
# coordinator's call, and the current emulator rejects X0X's LCD SPI baud; see reports/T1.md.)
#
# FW_HOST may be overridden: make fw FW_HOST=other-host

FW_HOST      ?= deltron.local
REMOTE_ROOT  ?= fm1-build
DIRNAME      := $(notdir $(CURDIR))
# a short hash of the checkout path: two worktrees must not share a remote build dir
DIRHASH      := $(shell printf '%s' '$(CURDIR)' | cksum | cut -d' ' -f1)
REMOTE_DIR   := $(REMOTE_ROOT)/$(DIRNAME)-$(DIRHASH)
SSH          := ssh $(FW_HOST)
RSYNC_EXCL   := --exclude=/.git --exclude=/.venv --exclude=/build --exclude=/__pycache__

.PHONY: all help fw release fw-debug fw-debug-check test test-local check push pull clean-remote unit scenarios screens parity host screens-lint fuzz-ui

all: help

help:
	@echo "fm1-rocket-os make targets:"
	@echo "  fw           build firmware on $(FW_HOST) (build/x0x.fwsc, x0x.bin, loader/ota.bin)"
	@echo "  fw-debug     build X0X_DEBUG=1 on $(FW_HOST) into build/debug/ (RC1p: the probe)"
	@echo "  fw-debug-check  fw-debug + forbidden-symbol and loader checks on it"
	@echo "  release      fw with ./build.sh --release \$$VERSION (e.g. make release VERSION=1.0.5)"
	@echo "  test         run tests/run_tests.sh on $(FW_HOST) (brings back build/scenarios/)"
	@echo "  test-local   run tests/run_tests.sh here (the subset that works on this machine)"
	@echo "  unit         the host unit suites that run on macOS"
	@echo "  scenarios    every tests/scenarios/*.x0x, goldens checked where they exist"
	@echo "  screens      the screens_*.x0x tour and build/screens/contact.png at 1:1 (T2b)"
	@echo "  parity       WASM output vs native, sample for sample (needs emcc and node)"
	@echo "  check        fw + the build's own checks (soft-double, size)"
	@echo "  screens-lint render the screen tour and lint the layout (Mac; needs no deltron)"
	@echo "  fuzz-ui      the UI fuzz: 200 seeds x 2000 events (T2c)"
	@echo "  push         rsync this tree to $(FW_HOST):$(REMOTE_DIR)/"
	@echo "  pull         rsync $(FW_HOST):$(REMOTE_DIR)/build/ back into ./build/"
	@echo "  help         this list"
	@echo ""
	@echo "  FW_HOST=$(FW_HOST)  (override on the command line)"

# the tree, minus .git/.venv/build, at $(FW_HOST):$(REMOTE_DIR)/ (a path relative to ~)
push:
	@$(SSH) 'mkdir -p $(REMOTE_DIR)'
	@rsync -az --delete $(RSYNC_EXCL) ./ $(FW_HOST):$(REMOTE_DIR)/

# everything the build wrote back (a bare `make pull` for the whole build dir)
pull:
	@mkdir -p build
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/ ./build/

fw: push
	@$(SSH) 'cd $(REMOTE_DIR) && ./build.sh'
	@mkdir -p build/loader
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/x0x.fwsc ./build/x0x.fwsc
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/x0x.bin ./build/x0x.bin
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/loader/ota.bin ./build/loader/ota.bin
	@echo "fw: brought back build/x0x.fwsc, build/x0x.bin, build/loader/ota.bin"

# RC1p: the development build the CPU probe talks to (X0X_DEBUG=1: ED_STRESS / ED_STATS over USB).
# tools/build.py adds -DENG_FM6_VOICES=8 so the stress can raise the FM6 voice cap; a release build
# keeps eng_fm6.h's 4. The loader is unchanged, so tools/check_loader.sh must still pass.
fw-debug: push
	@$(SSH) 'cd $(REMOTE_DIR) && X0X_DEBUG=1 ./build.sh'
	@mkdir -p build/debug
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/x0x.fwsc ./build/debug/x0x.fwsc
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/x0x.bin ./build/debug/x0x.bin
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/x0x.elf ./build/debug/x0x.elf
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/x0x.dis ./build/debug/x0x.dis
	@echo "fw-debug: brought back build/debug/x0x.fwsc (X0X_DEBUG=1, ENG_FM6_VOICES=8)"

# the debug package must pass the same limits a release does, and keep the X0X 1.0.5 loader
fw-debug-check: fw-debug
	@echo "fw-debug-check: forbidden symbols (heap, 64-bit division, soft-double)"
	@sh tools/check_symbols.sh build/debug
	@echo "fw-debug-check: the loader is X0X 1.0.5's"
	@sh tools/check_loader.sh build/debug/x0x.fwsc

# reproduce an X0X release recipe: the identity and version string are the only difference
# from `make fw` (X0X 1.0.5 builds byte-for-byte from `./build.sh --release 1.0.5`)
release: push
	@test -n "$(VERSION)" || { echo "usage: make release VERSION=1.0.5"; exit 2; }
	@$(SSH) 'cd $(REMOTE_DIR) && ./build.sh --release $(VERSION)'
	@mkdir -p build/loader
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/x0x-$(VERSION).fwsc ./build/x0x-$(VERSION).fwsc
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/loader/ota.bin ./build/loader/ota.bin
	@echo "release: brought back build/x0x-$(VERSION).fwsc, build/loader/ota.bin"

test: push
	@$(SSH) 'cd $(REMOTE_DIR) && tests/run_tests.sh'; rc=$$?; \
	rsync -az $(FW_HOST):$(REMOTE_DIR)/build/scenarios/ ./build/scenarios/ || true; \
	exit $$rc

# the host tests off-Linux. Some need the reference checkouts next to the tree (see
# tests/host/run_*.sh) or the built package; what does not run here is noted in reports/T1.md.
test-local:
	@tests/run_tests.sh

# the host simulator, for the targets below (the same script tests/run_tests.sh uses)
host:
	@sh host/build_host.sh

# the host unit suites that work on a Mac (no reference checkouts, no Linux, no toolchain)
unit:
	@tests/run_tests.sh --unit

# every scenario, each into its own directory, compared with its golden where one exists
scenarios: host
	@fail=0; for s in tests/scenarios/*.x0x; do n=$$(basename $$s .x0x); \
		mkdir -p "build/scenarios/$$n"; \
		if build/host/x0x_host "$$s" "build/scenarios/$$n" > "build/scenarios/$$n.log" 2>&1; then \
			echo "  ok   scenario $$n"; \
		else \
			echo "  FAIL scenario $$n (see build/scenarios/$$n.log)"; tail -15 "build/scenarios/$$n.log" | sed 's/^/       /'; fail=1; \
		fi; \
		if [ -d "tests/golden/$$n" ]; then \
			tests/golden.sh "build/scenarios/$$n" "tests/golden/$$n" || fail=1; \
		fi; \
	done; \
	[ $$fail -eq 0 ] && echo "scenarios: all passed" || { echo "scenarios: FAILED"; exit 1; }

# the screen catalogue (T2b writes tests/scenarios/screens_*.x0x): run it and build one contact
# sheet of every shot at 1:1 with its file name under it. Until T2b lands, a no-op that says so.
screens: host
	@found=0; for s in tests/scenarios/screens_*.x0x; do [ -e "$$s" ] && found=1; done; \
	if [ $$found -eq 0 ]; then \
		echo "make screens: no tests/scenarios/screens_*.x0x yet (T2b owns the screen tour); nothing to do"; \
		exit 0; \
	fi; \
	mkdir -p build/screens; fail=0; shots=""; \
	for s in tests/scenarios/screens_*.x0x; do n=$$(basename $$s .x0x); mkdir -p "build/screens/$$n"; \
		if build/host/x0x_host "$$s" "build/screens/$$n" > "build/screens/$$n.log" 2>&1; then \
			echo "  ok   screen $$n"; \
		else \
			echo "  FAIL screen $$n (see build/screens/$$n.log)"; tail -15 "build/screens/$$n.log" | sed 's/^/       /'; fail=1; \
		fi; \
		if [ -d "tests/golden/$$n" ]; then tests/golden.sh "build/screens/$$n" "tests/golden/$$n" || fail=1; fi; \
		for p in build/screens/$$n/*.png; do [ -e "$$p" ] && shots="$$shots $$p"; done; \
	done; \
	if [ -z "$$shots" ]; then echo "make screens: no shots were produced"; exit $$fail; fi; \
	python3 tests/contact_sheet.py build/screens/contact.png $$shots || fail=1; \
	exit $$fail

# WASM output must match native byte for byte (tests/parity.sh); fails clearly without node
parity:
	@sh tests/parity.sh

check: fw host
	@echo "check: forbidden symbols (heap, 64-bit division, soft-double)"
	@$(SSH) 'cd $(REMOTE_DIR) && sh tools/check_symbols.sh build'
	@echo "check: the flash write path refuses the protected regions"
	@sh tests/lint/flash_guard_test.sh
	@echo "check: the user-sample upload (a second writer in the song region) is gone"
	@sh tests/lint/no_sample_writers_test.sh
	@echo "check: every gesture-table button has a help card"
	@python3 tools/check_help.py
	@echo "check: make fw passed the build's own checks (soft-double routines, image/RAM/pool size)"
	@echo "check: the UI fuzz (short: 20 seeds x 500 events)"
	@mkdir -p build/fuzz
	@sh tests/fuzz/run_fuzz.sh 20 500

# T2b: render the screen tour with a host build that records every text draw (-DX0X_LINT), then
# lint the layout (tests/lint/check_layout.py). Runs on the Mac; exits non-zero on any finding or
# on a shot that failed to write.
screens-lint:
	@mkdir -p build/screens/tour
	@X0X_HOST_DEFS="-DX0X_LINT" X0X_HOST_OUT=build/host/x0x_host_lint sh host/build_host.sh
	@X0X_LINT_LOG=build/screens/lint.log build/host/x0x_host_lint tests/scenarios/screens_tour.x0x build/screens/tour > build/screens/screens_tour.log 2>&1 || { cat build/screens/screens_tour.log; exit 1; }
	@if grep -q "cannot write" build/screens/screens_tour.log; then echo "screens-lint: a shot failed to write"; exit 1; fi
	@n=$$(grep -c '^shot ' tests/scenarios/screens_tour.x0x); m=$$(ls build/screens/tour/*.png 2>/dev/null | wc -l | tr -d ' '); \
	[ "$$n" = "$$m" ] || { echo "screens-lint: $$m of $$n shots written"; exit 1; }
	@python3 tests/lint/check_layout.py build/screens/lint.log

# T2c: the UI fuzz. Random key, button and encoder events across every view and mode, then
# every input released, the transport stopped and `expect voices 0` after a 3 s tail (the
# architecture's "no hung notes" invariant). A failure prints the seed and writes a full and a
# minimised, ready-to-run .x0x into build/fuzz/. 200 seeds x 2000 events is the long run;
# `make check` runs the short one.
fuzz-ui: host
	@mkdir -p build/fuzz
	@sh tests/fuzz/run_fuzz.sh 200 2000

clean-remote:
	@$(SSH) 'rm -rf $(REMOTE_DIR)/build'

HOST_CC ?= cc
CORE_CFLAGS = -std=c99 -O2 -ffp-contract=off -Wall -Wextra -Wdouble-promotion
SAN_CFLAGS = -std=c99 -O1 -g -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra

.PHONY: unit-clip
unit-clip:
	@mkdir -p build/host
	$(HOST_CC) $(CORE_CFLAGS) -Werror -I. -o build/host/clip_engine_test \
	    tests/host/clip_engine_test.c core/seq/clip_engine.c
	build/host/clip_engine_test
	$(HOST_CC) $(SAN_CFLAGS) -I. -o build/host/clip_engine_test_san \
	    tests/host/clip_engine_test.c core/seq/clip_engine.c
	build/host/clip_engine_test_san

.PHONY: unit-engines
unit-engines:
	@sh tests/host/run_fm6.sh
	@sh tests/host/run_noise.sh

# T5i: the FM6 voice bank's flash import and the ROM fallback (fm6_vbank.c, eng_fm6.c). Its own test
# file is at -Werror; the ported DSP keeps T5c's lighter flags (the parity scripts').
.PHONY: unit-fm6bank
unit-fm6bank:
	@mkdir -p build/host build/gen
	python3 tools/gen_fm6_patches.py build/gen/fm6_patches.h >/dev/null
	$(HOST_CC) -std=c99 -O2 -ffp-contract=off -Wall -Wno-unused-function -Wno-unused-const-variable \
	    -Ibuild/gen -Ifirmware/src/dsp -c firmware/src/dsp/fm6_core.c -o build/host/fm6_core_bank.o
	$(HOST_CC) -std=c99 -O2 -ffp-contract=off -Wall -Wno-unused-function -Wno-unused-const-variable \
	    -Ibuild/gen -Ifirmware/src/dsp -c firmware/src/dsp/fm6_vbank.c -o build/host/fm6_vbank_bank.o
	$(HOST_CC) -std=c99 -O2 -ffp-contract=off -Wall -Wno-unused-function -Wno-unused-const-variable \
	    -Ibuild/gen -Ifirmware/src/dsp -c firmware/src/dsp/eng_fm6.c -o build/host/eng_fm6_bank.o
	$(HOST_CC) $(CORE_CFLAGS) -Werror -Ibuild/gen -Ifirmware/src/dsp -c tests/host/fm6_bank_test.c \
	    -o build/host/fm6_bank_test.o
	$(HOST_CC) -o build/host/fm6_bank_test build/host/fm6_bank_test.o build/host/fm6_core_bank.o \
	    build/host/fm6_vbank_bank.o build/host/eng_fm6_bank.o
	build/host/fm6_bank_test
	$(HOST_CC) $(SAN_CFLAGS) -Ibuild/gen -Ifirmware/src/dsp -c tests/host/fm6_bank_test.c \
	    -o build/host/fm6_bank_test_san.o
	$(HOST_CC) -fsanitize=address,undefined -fno-sanitize-recover=all -o build/host/fm6_bank_test_san \
	    build/host/fm6_bank_test_san.o firmware/src/dsp/fm6_core.c firmware/src/dsp/fm6_vbank.c \
	    firmware/src/dsp/eng_fm6.c -Ibuild/gen -Ifirmware/src/dsp
	build/host/fm6_bank_test_san

# T6c: the song/take store and the undo stack. The store includes X0X's lz.c, so
# it is its own translation unit. -DX0X_HOST (T6i): the host has no .pool section, so the
# store's lz_head and its scratch stay ordinary statics (the device build omits the flag).
.PHONY: unit-store
unit-store:
	@mkdir -p build/host
	$(HOST_CC) $(CORE_CFLAGS) -Werror -I. -DX0X_HOST -o build/host/song_store_test \
	    tests/host/song_store_test.c core/model/song_store.c
	build/host/song_store_test
	$(HOST_CC) $(CORE_CFLAGS) -Werror -I. -DX0X_HOST -o build/host/undo_stack_test \
	    tests/host/undo_stack_test.c core/model/undo_stack.c core/model/song_store.c
	build/host/undo_stack_test
	$(HOST_CC) $(CORE_CFLAGS) -Werror -I. -DX0X_HOST -o build/host/song_fuzz \
	    tests/host/song_fuzz.c core/model/song_store.c
	build/host/song_fuzz
	@echo "store: ASan/UBSan"
	$(HOST_CC) $(SAN_CFLAGS) -I. -DX0X_HOST -o build/host/song_store_test_san \
	    tests/host/song_store_test.c core/model/song_store.c
	build/host/song_store_test_san
	$(HOST_CC) $(SAN_CFLAGS) -I. -DX0X_HOST -o build/host/undo_stack_test_san \
	    tests/host/undo_stack_test.c core/model/undo_stack.c core/model/song_store.c
	build/host/undo_stack_test_san
	$(HOST_CC) $(SAN_CFLAGS) -I. -DX0X_HOST -o build/host/song_fuzz_san \
	    tests/host/song_fuzz.c core/model/song_store.c
	build/host/song_fuzz_san

# T6i: song_io glues the store, the LIVE song and undo; its own test stubs live_bridge and drives
# a RAM song flash (the power cut included). Song_store.c is its own unit (it includes lz.c).
.PHONY: unit-songio
unit-songio:
	@mkdir -p build/host
	$(HOST_CC) $(CORE_CFLAGS) -Werror -I. -DX0X_HOST -o build/host/song_io_test \
	    tests/host/song_io_test.c firmware/src/app/song_io.c firmware/src/app/song_templates.c core/model/song_store.c core/model/undo_stack.c
	build/host/song_io_test
	$(HOST_CC) $(SAN_CFLAGS) -I. -DX0X_HOST -o build/host/song_io_test_san \
	    tests/host/song_io_test.c firmware/src/app/song_io.c firmware/src/app/song_templates.c core/model/song_store.c core/model/undo_stack.c
	build/host/song_io_test_san

.PHONY: unit-take
unit-take:
	@mkdir -p build/host
	$(HOST_CC) $(CORE_CFLAGS) -Werror -I. -o build/host/take_test \
	    tests/host/take_test.c core/seq/take.c core/seq/clip_engine.c
	build/host/take_test
	$(HOST_CC) $(SAN_CFLAGS) -I. -o build/host/take_test_san \
	    tests/host/take_test.c core/seq/take.c core/seq/clip_engine.c
	build/host/take_test_san

.PHONY: unit-core
unit-core: unit-clip unit-live unit-engines unit-fm6bank unit-store unit-songio unit-take unit-midi unit-fx unit-name unit-songs

# T3: the LIVE bridge's pure pattern -> song mapping. The app builds add clip_engine.c at
# FS = 44100 (the device sample counter); here it is passed explicitly so the T4c goldens
# (which keep the 48 kHz default) are untouched.
.PHONY: unit-live
unit-live:
	@mkdir -p build/host
	$(HOST_CC) $(CORE_CFLAGS) -Werror -I. -Ifirmware/src -DX0X_HOST -DCLIP_ENGINE_FS=44100 \
	    -o build/host/live_bridge_test \
	    tests/host/live_bridge_test.c firmware/src/app/live_bridge.c core/seq/clip_engine.c
	build/host/live_bridge_test
	$(HOST_CC) $(SAN_CFLAGS) -I. -Ifirmware/src -DX0X_HOST -DCLIP_ENGINE_FS=44100 \
	    -o build/host/live_bridge_test_san \
	    tests/host/live_bridge_test.c firmware/src/app/live_bridge.c core/seq/clip_engine.c
	build/host/live_bridge_test_san
.PHONY: unit-midi
unit-midi:
	@mkdir -p build/host
	$(HOST_CC) $(CORE_CFLAGS) -Werror -I. -o build/host/midi_in_test \
	    tests/host/midi_in_test.c core/midi/midi_in.c
	build/host/midi_in_test
	$(HOST_CC) $(SAN_CFLAGS) -I. -o build/host/midi_in_test_san \
	    tests/host/midi_in_test.c core/midi/midi_in.c
	build/host/midi_in_test_san

.PHONY: unit-fx
unit-fx:
	@mkdir -p build/host
	$(HOST_CC) $(CORE_CFLAGS) -Wno-double-promotion -Werror -I. -Ifirmware/src/dsp \
	    -o build/host/perform_test tests/host/perform_test.c firmware/src/dsp/perform.c
	build/host/perform_test
	$(HOST_CC) $(SAN_CFLAGS) -I. -Ifirmware/src/dsp \
	    -o build/host/perform_test_san tests/host/perform_test.c firmware/src/dsp/perform.c
	build/host/perform_test_san

# T8c: the NAME state machine and its render. ui_name.c is an include-style view that
# draws through gfx.c's statics, so the test is one translation unit holding gfx.c +
# ui_name.c (as X0X's x0x.c does); -Wno-unused-function is what host/build_host.sh uses
# for gfx.c. The test writes build/host/ui_name/*.png (listed in reports/T8c.md).
.PHONY: unit-name
unit-name:
	@mkdir -p build/host/ui_name
	$(HOST_CC) $(CORE_CFLAGS) -Wno-unused-function -Wno-unused-parameter -Werror -I. -Ifirmware/src -Ifirmware/src/app \
	    -o build/host/ui_name_test tests/host/ui_name_test.c
	build/host/ui_name_test
	$(HOST_CC) $(SAN_CFLAGS) -Wno-unused-function -Wno-unused-parameter -I. -Ifirmware/src -Ifirmware/src/app \
	    -o build/host/ui_name_test_san tests/host/ui_name_test.c
	build/host/ui_name_test_san

# T8: the Empty/Starter song templates, and the SONGS view (STUDIO 7, 8, 11, 12 and 13)
# against a RAM ops fake. Both link song_store.c for song_store_empty; -DX0X_HOST keeps
# the store's buffers as plain statics on the host. ui_songs.c draws through gfx.c's
# statics, so its test is one translation unit holding gfx.c + ui_name.c + ui_songs.c.
.PHONY: unit-songs
unit-songs:
	@mkdir -p build/host/ui_songs
	$(HOST_CC) $(CORE_CFLAGS) -Werror -I. -DX0X_HOST -o build/host/song_templates_test \
	    tests/host/song_templates_test.c firmware/src/app/song_templates.c core/model/song_store.c
	build/host/song_templates_test
	$(HOST_CC) $(SAN_CFLAGS) -I. -DX0X_HOST -o build/host/song_templates_test_san \
	    tests/host/song_templates_test.c firmware/src/app/song_templates.c core/model/song_store.c
	build/host/song_templates_test_san
	$(HOST_CC) $(CORE_CFLAGS) -Wno-unused-function -Wno-unused-parameter -Werror -I. -DX0X_HOST \
	    -Ifirmware/src -Ifirmware/src/app -o build/host/ui_songs_test \
	    tests/host/ui_songs_test.c firmware/src/app/song_templates.c core/model/song_store.c
	build/host/ui_songs_test
	$(HOST_CC) $(SAN_CFLAGS) -Wno-unused-function -Wno-unused-parameter -I. -DX0X_HOST \
	    -Ifirmware/src -Ifirmware/src/app -o build/host/ui_songs_test_san \
	    tests/host/ui_songs_test.c firmware/src/app/song_templates.c core/model/song_store.c
	build/host/ui_songs_test_san


