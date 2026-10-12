#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Conservative source guard: 64-bit operands must never share a statement with / or %.

Check named 64-bit values as well as casts. Deliberately also reject mixed-width
statements containing an unrelated 64-bit operand: split those statements so
reviewers can verify the division width without relying on target helper symbols.
"""
import re
import sys
from pathlib import Path

WIDE = (r'\b(?:u?int64_t|u?int_fast64_t|u?int_least64_t|__u?int64_t'
        r'|(?:unsigned\s+|signed\s+)?long\s+long)\b')


def violations(source):
    source = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: '\n' * m[0].count('\n') or ' ', source, flags=re.S)
    # Resolve typedef aliases, then collect named wide operands.
    types = WIDE
    while True:
        aliases = re.findall(r'\btypedef\s+(?:' + types + r')\s+(\w+)\s*;', source)
        extra = [a for a in aliases if not re.search(types, a)]
        if not extra:
            break
        types += r'|\b(?:' + '|'.join(map(re.escape, extra)) + r')\b'
    names = re.findall(r'(?:' + types + r')\s+(?:const\s+|volatile\s+|\*\s*)*([A-Za-z_]\w*)', source)
    for declaration in re.findall(r'(?:' + types + r')\s+([^;(){}]+);', source):
        for part in declaration.split(','):
            name = re.match(r'\s*(?:const\s+|volatile\s+|\*\s*)*([A-Za-z_]\w*)', part)
            if name:
                names.append(name[1])
    wide = types + (r'|\b(?:' + '|'.join(map(re.escape, names)) + r')\b' if names else '')
    for statement in re.finditer(r'[^;{}]+', source):
        if re.search(r'[/%]', statement[0]):
            literals = re.findall(r'\b(0[xX][0-9a-fA-F]+|[0-9]+)([uUlL]*)\b', statement[0])
            literal_wide = any('ll' in suffix.lower() or
                               int(number, 16 if number.lower().startswith('0x') else 10) >
                               (0xFFFFFFFF if 'u' in suffix.lower() or number.lower().startswith('0x') else 0x7FFFFFFF)
                               for number, suffix in literals)
            if re.search(wide, statement[0]) or literal_wide:
                yield source.count('\n', 0, statement.start()) + 1, statement[0].strip()


def main(paths):
    failures = 0
    for path in paths:
        for line, statement in violations(path.read_text()):
            print(f'{path}:{line}: possible 64-bit division/modulo: {statement}')
            failures += 1
    return bool(failures)


if __name__ == '__main__':
    paths = list(map(Path, sys.argv[1:])) if len(sys.argv) > 1 else (
        sorted(Path('core').rglob('*.c')) + sorted(Path('core').rglob('*.h')) +
        sorted(Path('firmware/src/app').glob('*.c')) + sorted(Path('firmware/src/app').glob('*.h')))
    sys.exit(main(paths))
