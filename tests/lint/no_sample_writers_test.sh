#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# T6i review item 1: the user-sample upload was a second writer in the song region
# (0xA0000..0xDBFFF). It is gone: its commands, its slot code and its tool, and every other raw flash
# writer that could reach the region. The flash guard test covers the address rules; this covers the
# tree: no sample-upload symbol and no raw writer besides the guarded driver.
set -u
cd "$(dirname "$0")/../.."
fail=0

# 1. no trace of the user-sample feature (commands, slot code, tool) remains. Skip this script itself.
for p in ED_SMP SMP_USER smp_user plat_slot PLAT_NSLOTS PLAT_SLOT_ZONES; do
    hits=$(grep -rn "$p" firmware host core tools web tests 2>/dev/null |
           grep -v '^Binary' | grep -v 'no_sample_writers_test.sh')
    if [ -n "$hits" ]; then
        echo "FAIL: $p is still present:"
        echo "$hits"
        fail=1
    fi
done
if [ -e tools/fm1_sample_upload.py ]; then
    echo "FAIL: tools/fm1_sample_upload.py still exists"
    fail=1
fi

# 2. the only raw erase/program in firmware/src is the guarded driver (x0x.c: st_erase/st_prog and the
#    OTA wrappers), which check FL_STORE_OK and reach the song region only through plat_song_*. The
#    update loader has its own driver, firmware/loader/loader.c.
calls=$(grep -rlE '\bfl_(erase4k|write)\(' firmware/src 2>/dev/null | sort | tr '\n' ' ')
if [ "$calls" != "firmware/src/x0x.c " ]; then
    echo "FAIL: raw flash writers in firmware/src: '$calls' (expected only 'firmware/src/x0x.c ')"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "no-sample-writers: the sample upload is gone; only the guarded driver writes flash"
fi
exit $fail
