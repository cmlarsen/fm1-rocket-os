# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('lint64', Path('tools/check_no_64_div.py'))
lint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lint)
for source in (
    'x = (uint32_t)((uint64_t)p * n / d);',
    'uint64_t n; x = n % d;',
    'typedef uint64_t wide_t; wide_t n; x = n / d;',
    'unsigned long long n; x = n / 2;',
    'uint64_t *p; x = *p / d;',
    'uint64_t a, b; x = b % d;',
    'x = 1ULL / d;',
    'x = 2147483648 / d;',
    # r1/N2: the C99 fast/least families and the compiler's __uint64_t must be caught too.
    'uint_fast64_t a; uint32_t f(uint32_t b){ return a / b; }',
    'uint_least64_t a; uint32_t f(uint32_t b){ return a % b; }',
    'uint32_t f(uint32_t b){ return (uint32_t)(__uint64_t)b / 3u; }',
    'int_fast64_t a; int g(int b){ return a / b; }',
    'int_least64_t a; int g(int b){ return a % b; }',
    'int h(int a){ return a / (int)(__int64_t)9; }',
    # r1/N2: probes the reviewer used — a cast and a struct member are still wide.
    'uint32_t f(uint32_t b, uint32_t c){ return ((unsigned long long)b * c) % 7u; }',
    'struct s { uint64_t big; };\n'
    'uint32_t g(struct s *p){ return (uint32_t)(p->big / 10u); }',
):
    assert list(lint.violations(source)), source
for source in ('uint32_t x = p * n / d;', 'uint64_t n = a * b; x = p / d;',
               '// (uint64_t)p / d\nx = p / d;',
               # r1/N2: likewise a false-positive probe: a wide return type on its own line must
               # not be mistaken for the next function's name.
               'int64_t\nscale(int x) { return x; }\nint h(int a){ return a/2; }',
               'uint32_t f(uint32_t b, uint32_t c){ return (b * c) % 7u; }'):
    assert not list(lint.violations(source)), source
assert not lint.main([Path('core/seq/clip_engine.c')])
print('no_64_div: fixtures and production tempo arithmetic passed')
