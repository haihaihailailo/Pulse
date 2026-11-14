"""Small synthetic Windows handle tests, only under Workflow-Test's Job/lease."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import sys

def main(target):
    project = Path(__file__).resolve().parent.parent
    target = Path(target).absolute()
    if not target.is_relative_to(project/'dev-work/vm-sessions') or target.exists():
        raise RuntimeError('Fresh synthetic session fixture required')
    target.mkdir()
    module = runpy.run_path(str(project/'tools/pulse_artifact_delete.py'))
    checks = []
    def fixture(name):
        root = target/name; (root/'dev-work/builds').mkdir(parents=True)
        items = []
        for name in ('中文 first.exe', 'second.bmp'):
            file = root/'dev-work/builds'/name; file.write_bytes(b'synthetic payload '+name.encode())
            info = file.stat()
            items.append({'path': str(file), 'owner': 'synthetic', 'identity': {
                # Match the existing Node/libuv ledger's DWORD volume identity.
                # Python's stat uses the wider volume ID on this runtime.
                'path': str(file), 'bytes': info.st_size, 'deviceId': str(info.st_dev & 0xffffffff), 'fileId': str(info.st_ino),
                'mtimeMs': info.st_mtime_ns/1e6, 'sha256': hashlib.sha256(file.read_bytes()).hexdigest()}})
        return root, items, root/'journal.jsonl'
    def refuses(root, items, journal):
        try: module['retire'](root, items, journal)
        except (RuntimeError, OSError): pass
        else: raise AssertionError('Expected refusal')
        assert all(Path(x['path']).exists() for x in items)
        assert not journal.exists()
    root, items, journal = fixture('success')
    result = module['retire'](root, items, journal)
    assert result['deletedFiles'] == 2 and all(not Path(x['path']).exists() for x in items)
    assert len(journal.read_text('utf-8').splitlines()) == 5
    checks.append('Unicode paths, exact native identity and durable same-handle deletion')
    root, items, journal = fixture('drift')
    Path(items[1]['path']).write_bytes(b'changed payload'); refuses(root, items, journal)
    checks.append('content drift refuses entire batch before first deletion')
    root, items, journal = fixture('occupied')
    with module['opened'](Path(items[1]['path']), 0x80000000, 1, 0x08200000):
        refuses(root, items, journal)
    checks.append('occupied second file leaves first file and journal untouched')
    root, items, journal = fixture('hardlink')
    alias = root/'alias'; os.link(items[1]['path'], alias)
    try: refuses(root, items, journal)
    finally: alias.unlink()  # Only the synthetic alias created immediately above.
    checks.append('hardlinks rejected')
    root, items, journal = fixture('replace')
    file = Path(items[1]['path']); replacement = root/'replacement'
    replacement.write_bytes(file.read_bytes()); os.replace(replacement, file)
    refuses(root, items, journal); checks.append('same bytes with a new file identity rejected')
    root, items, journal = fixture('scope')
    try: module['safe'](root, project/'tools/pulse_dev.cjs')
    except RuntimeError: pass
    else: raise AssertionError('Scope escaped')
    checks.append('outside-root refusal')
    root, items, journal = fixture('interruption')
    try: module['retire'](root, items, journal, fail_after=1)
    except RuntimeError as error: assert str(error) == 'Synthetic interruption'
    else: raise AssertionError('Expected interruption')
    assert not Path(items[0]['path']).exists() and Path(items[1]['path']).exists()
    assert len(journal.read_text('utf-8').splitlines()) == 3
    checks.append('partial interruption preserves exact per-file intentions and remaining payload')
    print(json.dumps({'status': 'PASS', 'checks': checks}))

if __name__ == '__main__': main(sys.argv[1])
