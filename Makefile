# fm1-rocket-os top-level Makefile.
#
# T2a owns check / unit / scenarios / screens / parity. T4c adds unit-clip only.
# The host tiers run on the Mac; `fw`, `check` (-m32) and `emu-binary` run on FW_HOST.

CC ?= cc
CFLAGS ?= -std=c99 -O2 -ffp-contract=off -Wall -Wextra -Wdouble-promotion

.PHONY: unit-clip
unit-clip:
	@mkdir -p build/host
	$(CC) $(CFLAGS) -Werror -I. -o build/host/clip_engine_test \
	    tests/host/clip_engine_test.c core/seq/clip_engine.c
	build/host/clip_engine_test
	@echo "clip_engine: ASan/UBSan"
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -Wall -Wextra -I. -o build/host/clip_engine_test_san \
	    tests/host/clip_engine_test.c core/seq/clip_engine.c
	build/host/clip_engine_test_san
