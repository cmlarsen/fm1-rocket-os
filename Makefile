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

.PHONY: all help fw release test test-local check push pull clean-remote unit scenarios screens parity host

all: help

help:
	@echo "fm1-rocket-os make targets:"
	@echo "  fw           build firmware on $(FW_HOST) (build/x0x.fwsc, x0x.bin, loader/ota.bin)"
	@echo "  release      fw with ./build.sh --release \$$VERSION (e.g. make release VERSION=1.0.5)"
	@echo "  test         run tests/run_tests.sh on $(FW_HOST) (brings back build/scenarios/)"
	@echo "  test-local   run tests/run_tests.sh here (the subset that works on this machine)"
	@echo "  unit         the host unit suites that run on macOS"
	@echo "  scenarios    every tests/scenarios/*.x0x, goldens checked where they exist"
	@echo "  screens      the screens_*.x0x tour and build/screens/contact.png at 1:1 (T2b)"
	@echo "  parity       WASM output vs native, sample for sample (needs emcc and node)"
	@echo "  check        fw + the build's own checks (soft-double, size)"
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

check: fw
	@echo "check: make fw passed the build's own checks (soft-double routines, image/RAM/pool size)"

clean-remote:
	@$(SSH) 'rm -rf $(REMOTE_DIR)/build'

HOST_CC ?= cc
CORE_CFLAGS = -std=c99 -O2 -ffp-contract=off -Wall -Wextra -Wdouble-promotion
SAN_CFLAGS = -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra

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
