"""TFIXS: template/CRUD frames must not put a song_t on the stack."""
import pathlib
import subprocess
import tempfile

with tempfile.TemporaryDirectory() as tmp:
    for source in ('song_templates', 'song_io'):
        subprocess.run(['cc', '-O2', '-fstack-usage', '-DX0X_HOST', '-I.', '-c',
                        f'firmware/src/app/{source}.c', '-o', f'{tmp}/{source}.o'], check=True)
    wanted = {'song_template_empty', 'song_template_starter', 'song_io_songs_create', 'song_io_songs_copy', 'song_io_songs_rename'}
    seen = set()
    for path in pathlib.Path(tmp).glob('*.su'):
        for line in path.read_text().splitlines():
            location, size, *_ = line.split('\t')
            name = location.rsplit(':', 1)[-1]
            if name in wanted:
                seen.add(name)
                print(name, size)
                assert int(size) <= 8192, f'{name}: {size} B > 8 KB'
    assert seen == wanted
