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
#   make emu-binary   build fm1-emulator on $(FW_HOST) and boot build/x0x.fwsc headless
#   make push/pull    rsync the tree to / from $(FW_HOST)
#   make help         this list
#
# FW_HOST may be overridden: make fw FW_HOST=other-host

FW_HOST      ?= deltron.local
REMOTE_ROOT  ?= fm1-build
DIRNAME      := $(notdir $(CURDIR))
REMOTE_DIR   := $(REMOTE_ROOT)/$(DIRNAME)
SSH          := ssh $(FW_HOST)
RSYNC_EXCL   := --exclude=.git --exclude=.venv --exclude=build --exclude=__pycache__

.PHONY: all help fw release test test-local check emu-binary push pull clean-remote

all: help

help:
	@echo "fm1-rocket-os make targets:"
	@echo "  fw           build firmware on $(FW_HOST) (build/x0x.fwsc, x0x.bin, loader/ota.bin)"
	@echo "  release      fw with ./build.sh --release \$$VERSION (e.g. make release VERSION=1.0.5)"
	@echo "  test         run tests/run_tests.sh on $(FW_HOST) (brings back build/scenarios/)"
	@echo "  test-local   run tests/run_tests.sh here (the subset that works on this machine)"
	@echo "  check        fw + the build's own checks (soft-double, size)"
	@echo "  emu-binary   build fm1-emulator on $(FW_HOST) and boot build/x0x.fwsc headless"
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

# fm1-emulator (simonjohansson/fm1-emulator, QEMU for pi32v2) on $(FW_HOST): build it there
# (once, into $(REMOTE_ROOT)/fm1-emulator), then boot build/x0x.fwsc headless and keep a screenshot.
emu-binary: fw
	@$(SSH) 'set -e; cd $(REMOTE_ROOT); [ -d fm1-emulator ] || git clone --depth 1 https://github.com/simonjohansson/fm1-emulator.git; \
	  cd fm1-emulator; ./build.sh 2>/dev/null || make 2>/dev/null || true'
	@$(SSH) 'cd $(REMOTE_DIR) && mkdir -p build/emu && $(REMOTE_ROOT)/fm1-emulator/run.sh build/x0x.fwsc build/emu 2>/dev/null || true'
	@rsync -az $(FW_HOST):$(REMOTE_DIR)/build/emu/ ./build/emu/ || true
	@echo "emu-binary: see build/emu/ (home.png when the emulator booted)"

clean-remote:
	@$(SSH) 'rm -rf $(REMOTE_DIR)/build'
