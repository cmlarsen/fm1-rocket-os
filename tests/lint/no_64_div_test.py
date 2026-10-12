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
):
    assert list(lint.violations(source)), source
for source in ('uint32_t x = p * n / d;', 'uint64_t n = a * b; x = p / d;',
               '// (uint64_t)p / d\nx = p / d;'):
    assert not list(lint.violations(source)), source
assert not lint.main([Path('core/seq/clip_engine.c')])
print('no_64_div: fixtures and production tempo arithmetic passed')
