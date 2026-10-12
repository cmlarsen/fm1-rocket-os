"""pi32v2 gate regressions: register pushes, split allocation and call chains."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from check_stack import analyze


def dis(body):
    return '\n'.join(f'{name}:\n' + '\n'.join(f' {0x2000000 + k * 256 + j * 2:x}:    00 00             \t{x}' for j, x in enumerate(lines))
                     for k, (name, lines) in enumerate(body.items()))


class Gate(unittest.TestCase):
    def test_push_and_split(self):
        r = analyze(dis({'root': ['[--sp] = {r8-r11, rets}', 'sp += -100', 'sp += -200', 'rts']}))
        self.assertEqual(r.frames['root'], 320)

    def test_single_frame(self):
        self.assertTrue(analyze(dis({'root': ['sp += -8196', 'rts']})).errors)

    def test_chain(self):
        body = {'a': ['sp += -8000', 'call 2 <b : 2000100 >', 'rts'],
                'b': ['sp += -8000', 'call 2 <c : 2000200 >', 'rts'],
                'c': ['sp += -8000', 'rts']}
        self.assertTrue(analyze(dis(body)).errors)

    def test_recursion(self):
        self.assertTrue(analyze(dis({'root': ['sp += -4', 'call 0 <root : 2000000 >', 'rts']})).errors)

    def test_tail(self):
        r = analyze(dis({'a': ['sp += -100', 'sp += 100', 'goto 2 <b : 2000100 >'],
                         'b': ['sp += -200', 'rts']}))
        self.assertEqual(r.deepest[0], 200)

    def test_old_device_code(self):
        fixture = Path(__file__).resolve().parents[1] / 'fixtures/tfixs-old-new.dis'
        r = analyze(fixture.read_text())
        self.assertEqual(r.frames['song_io_songs_create'], 14388)
        self.assertEqual(r.frames['song_template_starter'], 14420)
        self.assertEqual(r.deepest[0], 28808)
        self.assertTrue(r.errors)

    def test_indirect_deepest_candidate(self):
        r = analyze(dis({'root': ['sp += -100', 'call r3', 'rts'],
                         'small': ['sp += -20', 'rts'],
                         'large': ['sp += -300', 'rts']}),
                    callbacks={'root': ('small', 'large')})
        self.assertEqual(r.deepest[0], 400)
        self.assertFalse(r.errors)

    def test_unlisted_product_and_inherited(self):
        body = dis({'root': ['call r3', 'rts']})
        self.assertTrue(analyze(body, callbacks={}, product={'root'}).errors)
        r = analyze(body, callbacks={}, product=set())
        self.assertFalse(r.errors)
        self.assertEqual(len(r.warnings), 1)

    def test_fpu_half_is_not_call(self):
        body = "root:\n 2000000:    3f e5              <unknown instruction>\n 2000002:    c0 00             \tcall r0\n"
        self.assertFalse(analyze(body, callbacks={}).errors)

    def test_internal_jump_label(self):
        body = dis({'root': ['sp += -100', 'goto 2 <.GJTIE0_0+0x2 : 2000004 >', 'call r3', 'rts']})
        r = analyze(body, callbacks={'root': ()})
        self.assertEqual(r.frames['root'], 100)
        self.assertFalse(r.errors)

    def test_tail_cycle_bounded(self):
        r = analyze(dis({'a': ['sp += -100', 'sp += 100', 'goto 0 <b : 2000100 >'],
                         'b': ['sp += -200', 'sp += 200', 'goto 0 <a : 2000000 >']}))
        self.assertEqual(r.deepest[0], 200)
        self.assertFalse(r.errors)

    def test_missing_disassembly_fails(self):
        self.assertTrue(analyze('').errors)

    def test_nonprologue_allocation_fails(self):
        self.assertTrue(analyze(dis({'root': ['call r3', 'sp += -9000', 'rts']}),
                                callbacks={'root': ()}).errors)


if __name__ == '__main__':
    unittest.main()
