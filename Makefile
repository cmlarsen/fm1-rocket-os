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

.PHONY: all help fw release test test-local check push pull clean-remote

all: help

help:
	@echo "fm1-rocket-os make targets:"
	@echo "  fw           build firmware on $(FW_HOST) (build/x0x.fwsc, x0x.bin, loader/ota.bin)"
	@echo "  release      fw with ./build.sh --release \$$VERSION (e.g. make release VERSION=1.0.5)"
	@echo "  test         run tests/run_tests.sh on $(FW_HOST) (brings back build/scenarios/)"
	@echo "  test-local   run tests/run_tests.sh here (the subset that works on this machine)"
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
