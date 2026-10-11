#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# fm1-rocket-os T2b: forbidden-symbol check.
#
# Device code has no heap, no 64-bit division and no double (CLAUDE.md, ADR-0002). This reads the
# link map / nm output of a build and exits non-zero if any forbidden symbol is present.
#
#   tools/check_symbols.sh build        # looks for build/x0x.elf (nm) or build/x0x.dis
#
# malloc / calloc / realloc / free, the 64-bit division helpers (__divdi3 __udivdi3 __moddi3
# __umoddi3) and any soft-double helper (__*df2 / __*df3) must not be in the image.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
D=${1:-build}
cd "$ROOT" || exit 2

BAD=" malloc calloc realloc free __divdi3 __udivdi3 __moddi3 __umoddi3 __udivmoddi4 __divmoddi4
      __floatsidf __floatunsidf __floatdidf __floatundidf __fixdfsi __fixdfdi __truncdfsf2 __extendsfdf2 "

syms=""
if [ -f "$D/x0x.elf" ]; then
    # the toolchain nm if it is around, else the host nm (it reads the ELF symbol table either way)
    NM=${NM:-}
    [ -z "$NM" ] && [ -n "${JIELI_TOOLCHAIN:-}" ] && [ -x "$JIELI_TOOLCHAIN/common/bin/nm" ] && NM="$JIELI_TOOLCHAIN/common/bin/nm"
    [ -z "$NM" ] && command -v nm >/dev/null 2>&1 && NM=nm
    if [ -n "$NM" ]; then
        syms=$("$NM" "$D/x0x.elf" 2>/dev/null | awk 'NF>=3 {print $NF}')
    fi
fi
if [ -z "$syms" ] && [ -f "$D/x0x.dis" ]; then
    # fall back to the disassembly: label and call-target lines carry the symbol names
    syms=$(grep -oE '<[^>]+>' "$D/x0x.dis" | tr -d '<>' | sort -u)
fi

if [ -z "$syms" ]; then
    echo "check_symbols: no $D/x0x.elf (nm) or $D/x0x.dis to read" >&2
    exit 2
fi

bad=""
for s in $BAD; do
    echo "$syms" | grep -qx -- "$s" && bad="$bad $s"
done
# soft-double helpers: __adddf3, __muldf3, __fixdfsi, __floatsidf, __truncdfsf2, __extendsfdf2, ...
dbl=$(echo "$syms" | grep -E '^__.*(df2|df3|dfsi|dfdi|sf2|sfdf|dftf|tfdf)' || true)
[ -n "$dbl" ] && bad="$bad $dbl"

if [ -n "$bad" ]; then
    echo "check_symbols: FORBIDDEN symbols in $D:"
    for s in $(echo "$bad" | tr ' ' '\n' | sort -u); do echo "   $s"; done
    exit 1
fi
echo "check_symbols: no forbidden symbols (heap, 64-bit division, soft-double)"
