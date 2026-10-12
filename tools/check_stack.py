#!/usr/bin/env python3
"""Conservative pi32v2 product-stack gate over the linked device disassembly.

Every linked function is a root (a superset of reachable product entry points).
Frames include saved 32-bit registers (braced "{...}" and single unbraced
pushes alike) and every split sp allocation in the prologue. Direct calls and
reviewed callback sets add frames; tail transfers reuse the caller's frame.
New indirect-call owners, a callback target that is no longer linked, and
recursion all fail closed (a stale allowlist must not silently drop an edge).
The old objdump splits FPU instructions at 3f e5 / 3f f5; their second half is
not an instruction, even when it prints as a call or stack adjustment.
"""
import argparse
from dataclasses import dataclass
from pathlib import Path
import re

# Device callback seams, audited against their initializers, not host callbacks.
FLASH = ('fb_read', 'fb_erase', 'fb_program')
SONGS = ('song_io_songs_list', 'song_io_songs_current_slot', 'songs_op_open',
         'song_io_songs_create', 'song_io_songs_copy', 'song_io_songs_rename',
         'songs_op_delete', 'song_io_songs_undelete', 'song_io_songs_first_free',
         'song_io_songs_can_undelete', 'song_io_songs_clip_count', 'song_io_songs_discard')
MIDI = ('ml_note_on', 'ml_note_off', 'ml_all_off', 'ml_launch', 'ml_start',
        'ml_cont', 'ml_stop', 'ml_lost')
CALLBACKS = {
    'st_read': ('fl_read_ram',), 'fl_erase4k': ('fl_erase4k_ram',),
    'fl_write': ('fl_prog_ram',), 'fm1_cstart': ('fl_jedec_ram',),
    'ui_songs_refresh': ('song_io_songs_list', 'song_io_songs_current_slot'),
    'sr_current_slot': ('song_io_songs_current_slot',), 'ui_songs_input': SONGS,
    'motion_release': ('mot_base', 'mot_apply'),
    'do_step': ('bridge_ev',), 'clip_engine_render': ('bridge_ev',),
    'bridge_ev': ('live_sink',), 'take_arm': ('bridge_bar_fn', 'bridge_prelude'),
    'take_stop': ('bridge_bar_fn',), 'take_render': ('take_sink_ev',),
    'lz_compress': ('sst_count_put', 'sst_wr_put'),
    'sst_wr_put': ('fb_program',),
    'midi_in_byte': MIDI, 'midi_in_tick': MIDI,
    'fm1_c1_main': ('core2_job',),  # plat_cpu2_run(core2_job, ...) in engine.c
    # engine_tap is zero-initialized and never assigned in the device app.
    'engine_render': (), 'live_sink': (), 'take_sink_ev': (),
}
# Reads are narrowed separately from commit/write callbacks (the compiler
# inlines sst_read / sst_erase / sst_program into these owners).
for _name in ('song_store_init', 'sst_try_song', 'sst_load_song', 'sst_pick_free_ext',
              'song_store_list', 'sst_try_take', 'song_store_take_load'):
    CALLBACKS[_name] = ('fb_read',)
for _name in ('sst_commit', 'sst_write_song', 'sst_write_take'):
    CALLBACKS[_name] = FLASH

LABEL = re.compile(r'^([A-Za-z_][\w.]*):$')
INSN = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)(.*)$')
TARGET = re.compile(r'<([^ :+>]+)(?:\+[^ :>]+)?\s*:\s*([0-9a-f]+)\s*>')


@dataclass
class Result:
    frames: dict
    deepest: tuple
    errors: list
    warnings: list
    interrupts: tuple


def registers(spec):
    n = 0
    for part in spec.split(','):
        part = part.strip()
        m = re.fullmatch(r'r(\d+)-r(\d+)', part)
        n += abs(int(m[1]) - int(m[2])) + 1 if m else 1
    return n * 4


def analyze(dis, stack=24320, margin=2048, callbacks=None, product=None):
    callbacks = CALLBACKS if callbacks is None else callbacks
    bodies, owner, skip = {}, None, -1
    for line in dis.splitlines():
        label = LABEL.fullmatch(line)
        if label:
            owner = label[1]
            if owner in bodies:
                n = 2
                while f'{owner}#{n}' in bodies:
                    n += 1
                owner = f'{owner}#{n}'
            bodies[owner] = []
            continue
        m = INSN.match(line)
        if not m or owner is None:
            continue
        addr, raw, op = int(m[1], 16), m[2].split(), m[3]
        if addr == skip:
            skip = -1
            continue
        if raw in (['3f', 'e5'], ['3f', 'f5']):
            skip = addr + 2
            continue
        bodies[owner].append((addr, op))
    errors, warnings, frames, edges = [], [], {}, {}
    if not bodies:
        errors.append('no functions parsed')
    starts = {body[0][0]: name for name, body in bodies.items() if body}
    # Resolve internal jump-table labels by address: labels beginning '.' do not
    # split a function, and only an exact function entry creates a graph edge.
    for name, body in bodies.items():
        frame = 0
        prologue = True
        for _, op in body:
            alloc = re.search(r'sp \+= (-\d+)\b', op)
            push = re.search(r'\[--sp\] = \{([^}]+)\}', op)
            if not push:
                # a single unbraced register push: "[--sp] = rets" (4 B), which the
                # braced form alone misses
                push = re.search(r'\[--sp\] = (\S+)\s*$', op)
            if not prologue and (alloc or push) and name != 'fm1_fatal_stubs':
                errors.append(f'{name}: stack allocation after prologue requires review: {op}')
            if prologue:
                if alloc:
                    frame += -int(alloc[1])
                if push:
                    frame += registers(push[1])
            if re.search(r'\b(call|goto|rts|rti|tbb|tbh)\b', op):
                prologue = False
        frames[name] = frame
        if frame > 8192:
            errors.append(f'{name}: single frame {frame} B > 8192 B')
        edges[name] = []
        for _, op in body:
            direct = TARGET.search(op)
            if direct and re.search(r'\b(call|goto)\b', op):
                target = starts.get(int(direct[2], 16))
                call = bool(re.search(r'\bcall\b', op))
                if target and (call or target != name):
                    edges[name].append((target, frame if call else 0))
            elif re.search(r'\b(?:call|goto) r\d+\b', op):
                base = name.split('#')[0]
                if base not in callbacks:
                    messages = errors if product is None or base in product else warnings
                    messages.append(f'{name}: unreviewed indirect transfer: {op}')
                else:
                    for target in callbacks[base]:
                        if target in bodies:
                            edges[name].append((target, frame))
                        else:
                            # A listed target that is not linked means the allowlist is stale
                            # (the function was renamed, inlined away or the list is wrong):
                            # fail closed rather than silently dropping the edge.
                            errors.append(f'{name}: callback target {target} is not linked')
    # Bound every linked root; all callbacks are part of the same graph. A cycle
    # with a retained frame has no finite bound, and is rejected, not skipped.
    memo, visiting = {}, []

    def depth(name, incoming=0):
        if name in memo:
            return memo[name]
        active = [entry[0] for entry in visiting]
        if name in active:
            index = active.index(name)
            cycle = active[index:] + [name]
            retained = incoming + sum(entry[1] for entry in visiting[index + 1:])
            if retained:
                errors.append('recursive call chain: ' + ' -> '.join(cycle))
                return (stack, cycle)
            return (max(frames[node] for node in cycle), cycle)
        visiting.append((name, incoming))
        best = (frames[name], [name])
        for target, retained in edges[name]:
            size, path = depth(target, retained)
            candidate = (max(frames[name], retained + size), [name] + path)
            if candidate[0] > best[0]:
                best = candidate
        visiting.pop()
        memo[name] = best
        return best

    deepest = max((depth(name) for name in bodies), default=(0, []), key=lambda p: p[0])
    if deepest[0] > stack - margin:
        errors.append(f'chain {deepest[0]} B > {stack - margin} B ({stack} B stack - {margin} B reserve)')
    audio = depth('isr_alnk0') if 'isr_alnk0' in bodies else (0, [])
    timer = depth('isr_timer5') if 'isr_timer5' in bodies else (0, [])
    # TIMER5 may preempt ALNK0 (main_fm1.c, usb.c). Both run on the
    # supervisor stack: app.ld _sstack_top - _sstack_lo = 7936 bytes.
    interrupts = (audio[0] + timer[0], audio[1] + ['[nested TIMER5]'] + timer[1])
    if interrupts[0] > 7936 - margin:
        errors.append(f'nested IRQ chain {interrupts[0]} B > {7936 - margin} B supervisor budget')
    return Result(frames, deepest, sorted(set(errors)), warnings, interrupts)


def product_functions(root):
    app = root / 'firmware/src/app'
    paths = [*app.glob('ui*.c'), *(app / name for name in
              ('live_bridge.c', 'midi_live.c', 'song_io.c', 'song_templates.c')),
             *(root / 'core').rglob('*.c')]
    names = set()
    for path in paths:
        source = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(), flags=re.S)
        names.update(re.findall(r'(?m)^[^\n;{}]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{', source))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('disassembly', type=Path)
    ap.add_argument('--margin', type=int, required=True)
    args = ap.parse_args()
    if args.margin < 0 or args.margin >= 24320:
        ap.error('margin must be between 0 and 24319')
    result = analyze(args.disassembly.read_text(), margin=args.margin,
                     product=product_functions(Path(__file__).resolve().parents[1]))
    for name in sorted(result.frames, key=result.frames.get, reverse=True)[:10]:
        print(f'stack frame: {result.frames[name]:5d} B  {name}')
    print(f'stack chain: {result.deepest[0]} B / 24320 B; reserve {args.margin} B; ' + ' -> '.join(result.deepest[1]))
    print(f'stack IRQ chain: {result.interrupts[0]} B / 7936 B; reserve {args.margin} B; '
          + ' -> '.join(result.interrupts[1]))
    print(f'stack warnings: {len(result.warnings)} unlisted inherited indirect transfers')
    for warning in result.warnings:
        print('stack WARN:', warning)
    for error in result.errors:
        print('stack FAIL:', error)
    return bool(result.errors)


if __name__ == '__main__':
    raise SystemExit(main())
