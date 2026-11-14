"""Exact same-handle retirement, invoked only inside the protected Pulse Job."""
import ctypes as C
from ctypes import wintypes as W
from contextlib import ExitStack, contextmanager
import hashlib
import json
import os
from pathlib import Path
import sys

K = C.WinDLL('kernel32', use_last_error=True)
class Info(C.Structure):
    _fields_ = [('attrs', W.DWORD), ('created', W.FILETIME), ('accessed', W.FILETIME),
                ('written', W.FILETIME), ('volume', W.DWORD), ('high_size', W.DWORD),
                ('low_size', W.DWORD), ('links', W.DWORD), ('high_id', W.DWORD), ('low_id', W.DWORD)]
class Disposition(C.Structure):
    _fields_ = [('delete', W.BOOLEAN)]
def api(name, result, arguments):
    method = getattr(K, name); method.restype = result; method.argtypes = arguments; return method
api('CreateFileW', W.HANDLE, [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE])
api('CloseHandle', W.BOOL, [W.HANDLE])
api('GetFileInformationByHandle', W.BOOL, [W.HANDLE, C.POINTER(Info)])
api('SetFileInformationByHandle', W.BOOL, [W.HANDLE, C.c_int, C.c_void_p, W.DWORD])
api('ReadFile', W.BOOL, [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p])
api('IsProcessInJob', W.BOOL, [W.HANDLE, W.HANDLE, C.POINTER(W.BOOL)])
api('GetCurrentProcess', W.HANDLE, [])

def require(value, message):
    if not value: raise RuntimeError(message)

def safe(root, file):
    file = Path(os.path.abspath(file))
    require(file != root and file.is_relative_to(root), 'Path outside exact root')
    for parent in [file, *file.parents]:
        info = parent.lstat()
        require(not info.st_file_attributes & 0x400 and not parent.is_symlink(), 'Reparse point refused')
    require(file.resolve(strict=True) == file, 'Resolved path changed')
    return file

def information(handle):
    info = Info()
    if not K.GetFileInformationByHandle(handle, C.byref(info)): raise C.WinError(C.get_last_error())
    return info

@contextmanager
def opened(file, access, sharing, flags):
    handle = K.CreateFileW('\\\\?\\' + str(file), access, sharing, None, 3, flags, None)
    if handle == W.HANDLE(-1).value: raise C.WinError(C.get_last_error())
    try: yield handle
    finally:
        if not K.CloseHandle(handle): raise C.WinError(C.get_last_error())

def retire(root, items, journal, fail_after=None):
    """All handles and hashes validated before the first durable deletion intent."""
    require(0 < len(items) <= 512, 'Bounded exact files required')
    paths = [safe(root, x['identity']['path']) for x in items]
    require(len(set(str(x).lower() for x in paths)) == len(paths), 'Duplicate target')
    require(all(x.is_relative_to(root/'dev-work/builds') or x.is_relative_to(root/'dev-work/vm-sessions') for x in paths),
            'Only registered generated payload roots allowed')
    directories = sorted({p for file in paths for p in file.parents}, key=lambda x: (len(x.parts), str(x)))
    deleted = []
    with ExitStack() as stack:
        for directory in directories:
            h = stack.enter_context(opened(directory, 0, 3, 0x02200000))
            info = information(h)
            require(info.attrs & 16 and not info.attrs & 0x400, 'Directory handle changed/reparse')
        handles = []
        for item, file in zip(items, paths):
            expected = item['identity']
            native = item.get('nativeIdentity', expected)
            h = stack.enter_context(opened(file, 0x80010000, 0, 0x08200000))
            info = information(h)
            written = ((info.written.dwHighDateTime << 32) | info.written.dwLowDateTime)
            mtime_ms = (written - 116444736000000000) / 10000
            require(not info.attrs & (0x400 | 16) and info.links == 1 and
                    str(info.volume) == native['deviceId'] and
                    str((info.high_id << 32) | info.low_id) == native['fileId'] and
                    ((info.high_size << 32) | info.low_size) == expected['bytes'] and
                    abs(mtime_ms - expected['mtimeMs']) < 0.002, 'Exclusive file identity drift: '+json.dumps({
                        'actual': {'deviceId': str(info.volume), 'fileId': str((info.high_id << 32) | info.low_id),
                                   'bytes': (info.high_size << 32) | info.low_size, 'mtimeMs': mtime_ms, 'links': info.links, 'attrs': info.attrs},
                        'expected': expected}, ensure_ascii=False))
            digest = hashlib.sha256(); buffer = C.create_string_buffer(65536); count = W.DWORD()
            while True:
                if not K.ReadFile(h, buffer, len(buffer), C.byref(count), None): raise C.WinError(C.get_last_error())
                if not count.value: break
                digest.update(buffer.raw[:count.value])
            require(digest.hexdigest() == expected['sha256'], 'SHA256 drift')
            handles.append((item, h))
        # The exclusive handles prevent content changes, replacement and competing
        # opens. Ancestor handles prevent rename/delete throughout the transaction.
        with Path(journal).open('x', encoding='utf-8', newline='\n') as stream:
            def log(value):
                stream.write(json.dumps(value, ensure_ascii=False) + '\n'); stream.flush(); os.fsync(stream.fileno())
            log({'event': 'validated', 'files': len(handles)})
            for item, handle in handles:
                if fail_after is not None and len(deleted) == fail_after: raise RuntimeError('Synthetic interruption')
                log({'event': 'delete-intent', 'identity': item['identity']})
                disposition = Disposition(True)
                if not K.SetFileInformationByHandle(handle, 4, C.byref(disposition), C.sizeof(disposition)):
                    raise C.WinError(C.get_last_error())
                deleted.append(item['identity']['path'])
                log({'event': 'delete-marked', 'path': deleted[-1]})
    require(all(not os.path.lexists(file) for file in paths), 'Marked file remains after handle close')
    return {'status': 'PASS', 'deletedFiles': len(deleted), 'deletedBytes': sum(x['identity']['bytes'] for x in items)}

def main(plan_file, expected_hash):
    raw = Path(plan_file).read_bytes()
    require(len(raw) <= 524288 and hashlib.sha256(raw).hexdigest() == expected_hash, 'Retirement plan identity drift')
    plan = json.loads(raw); root = Path(plan['root'])
    safe(root, plan_file)
    require(plan['action'] == 'artifact-retire', 'Use protected retirement entry')
    in_job = W.BOOL()
    require(K.IsProcessInJob(K.GetCurrentProcess(), None, C.byref(in_job)) and in_job.value, 'Protected Windows Job required')
    state = json.loads((root/'.local-artifact-storage/state.json').read_text('utf-8'))
    require((root/'.local-artifact-storage/lock.json').is_file() and state['last']['status'] == 'active' and
            state['last']['owner'] == 'pulse-artifact-retirement', 'Active retirement lease required')
    require(state['last']['retirements'] == plan['retirements'], 'Active exact retirement set differs')
    for item in plan['retirements']:
        require(item.get('nativeIdentity') and all(isinstance(item['nativeIdentity'].get(k), str) and
                item['nativeIdentity'][k].isdigit() for k in ('deviceId', 'fileId')), 'Exact native identity required')
        require(state['objects'].get(item['path'], {}).get('identity') == item['identity'], 'Unregistered target')
    journal = Path(plan['retirementJournal'])
    require(journal.parent == Path(plan['targetDirectory']) and journal.name == 'deletions.jsonl' and
            not os.path.lexists(journal), 'Journal target differs')
    try:
        return retire(root, plan['retirements'], journal)
    finally:
        # Preserve an explicit empty evidence target on pre-delete refusal.
        if not journal.exists():
            with journal.open('x', encoding='utf-8') as stream:
                stream.write('{"event":"refused-before-deletion"}\n'); stream.flush(); os.fsync(stream.fileno())

if __name__ == '__main__':
    require(len(sys.argv) == 3, 'Use Pulse-Dev.ps1')
    print(json.dumps(main(sys.argv[1], sys.argv[2])))
