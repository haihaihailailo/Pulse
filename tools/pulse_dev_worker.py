"""Private worker for Pulse-Dev.ps1. Reuses the installed Windows Job/slot guards."""
import argparse
import configparser
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import sys
import time
import winreg

sys.dont_write_bytecode = True
K = C.WinDLL('kernel32', use_last_error=True)
U = C.WinDLL('user32', use_last_error=True)
MIB = 1024**2


def api(dll, name, result, arguments):
    method = getattr(dll, name)
    method.restype, method.argtypes = result, arguments
    return method


class ProcessEntry(C.Structure):
    _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD), ('heap', C.c_size_t),
                ('module', W.DWORD), ('threads', W.DWORD), ('parent', W.DWORD),
                ('priority', W.LONG), ('flags', W.DWORD), ('name', W.WCHAR * 260)]


api(K, 'CreateToolhelp32Snapshot', W.HANDLE, [W.DWORD, W.DWORD])
api(K, 'Process32FirstW', W.BOOL, [W.HANDLE, C.POINTER(ProcessEntry)])
api(K, 'Process32NextW', W.BOOL, [W.HANDLE, C.POINTER(ProcessEntry)])
api(K, 'OpenProcess', W.HANDLE, [W.DWORD, W.BOOL, W.DWORD])
api(K, 'QueryFullProcessImageNameW', W.BOOL, [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)])
api(K, 'GetProcessTimes', W.BOOL, [W.HANDLE, C.POINTER(W.FILETIME), C.POINTER(W.FILETIME), C.POINTER(W.FILETIME), C.POINTER(W.FILETIME)])
api(K, 'CloseHandle', W.BOOL, [W.HANDLE])
api(U, 'CreateDesktopW', W.HANDLE, [W.LPCWSTR, W.LPCWSTR, C.c_void_p, W.DWORD, W.DWORD, C.c_void_p])
api(U, 'OpenInputDesktop', W.HANDLE, [W.DWORD, W.BOOL, W.DWORD])
api(U, 'CloseDesktop', W.BOOL, [W.HANDLE])
api(U, 'GetUserObjectInformationW', W.BOOL, [W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.POINTER(W.DWORD)])


def sha(file):
    h = hashlib.sha256()
    with Path(file).open('rb') as stream:
        while data := stream.read(65536):
            h.update(data)
    return h.hexdigest()


def processes():
    names = {'pulse.exe', 'pulse-dev.exe', 'metrics-selftest-dev.exe', 'pulse-releasecheck.exe'}
    snapshot = K.CreateToolhelp32Snapshot(2, 0)
    if snapshot == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    rows = []
    try:
        entry = ProcessEntry(); entry.size = C.sizeof(entry)
        valid = K.Process32FirstW(snapshot, C.byref(entry))
        while valid:
            if entry.name.lower() in names:
                handle = K.OpenProcess(0x1000, False, entry.pid)
                if not handle:
                    raise RuntimeError('Cannot identify an existing Pulse process')
                try:
                    text = C.create_unicode_buffer(32768); size = W.DWORD(len(text))
                    if not K.QueryFullProcessImageNameW(handle, 0, text, C.byref(size)):
                        raise C.WinError(C.get_last_error())
                    created, exited, kernel, user = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
                    if not K.GetProcessTimes(handle, C.byref(created), C.byref(exited), C.byref(kernel), C.byref(user)):
                        raise C.WinError(C.get_last_error())
                    rows.append(dict(pid=entry.pid, path=text.value, created=(created.dwHighDateTime << 32) | created.dwLowDateTime))
                finally:
                    K.CloseHandle(handle)
            valid = K.Process32NextW(snapshot, C.byref(entry))
    finally:
        K.CloseHandle(snapshot)
    return sorted(rows, key=lambda row: row['pid'])


def within(root, file):
    return os.path.commonpath([os.path.abspath(root), os.path.abspath(file)]).lower() == os.path.abspath(root).lower()


def safe(file):
    file = Path(file).absolute()
    for part in (file, *file.parents):
        if part.exists() and part.lstat().st_file_attributes & 0x400:
            raise RuntimeError('Reparse path refused: ' + str(part))
    if file.exists() and file.is_file() and file.stat().st_nlink != 1:
        raise RuntimeError('Multiple-link file refused')
    return file


def write(file, data):
    file = safe(file)
    payload = (json.dumps(data, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
    if len(payload) > 2 * MIB:
        raise RuntimeError('Receipt bound exceeded')
    with file.open('xb') as stream:
        stream.write(payload); stream.flush(); os.fsync(stream.fileno())


def input_desktop():
    desktop = U.OpenInputDesktop(0, False, 1)
    if not desktop:
        raise RuntimeError('Input desktop cannot be inspected; no GUI checks started')
    try:
        text = C.create_unicode_buffer(256); needed = W.DWORD()
        if not U.GetUserObjectInformationW(desktop, 2, text, C.sizeof(text), C.byref(needed)):
            raise C.WinError(C.get_last_error())
        return text.value
    finally:
        U.CloseDesktop(desktop)


def production(config):
    exe = Path(config['installedExe'])
    home = Path(os.environ['USERPROFILE'])
    paths = [exe, home/'AppData/Local/Pulse/settings.ini',
             home/'AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Pulse.lnk', home/'Desktop/Pulse.lnk']
    files = {str(file): sha(safe(file)) if file.exists() else None for file in paths}
    registry = {}
    for key in ('Software\\Microsoft\\Windows\\CurrentVersion\\Run',
                'Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run'):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key, 0, winreg.KEY_READ) as opened:
                value, kind = winreg.QueryValueEx(opened, 'Pulse')
                registry[key] = [value.hex() if isinstance(value, bytes) else value, kind]
        except FileNotFoundError:
            registry[key] = None
    current = [row for row in processes() if os.path.normcase(row['path']) == os.path.normcase(str(exe))]
    return dict(files=files, registry=registry, processes=current)


def apply_interactive_defaults(pid, profile, defaults_hash):
    """Use the application's existing commands after its tray and visible window exist."""
    callback = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    api(U, 'EnumWindows', W.BOOL, [callback, W.LPARAM])
    api(U, 'GetWindowThreadProcessId', W.DWORD, [W.HWND, C.POINTER(W.DWORD)])
    api(U, 'GetClassNameW', C.c_int, [W.HWND, W.LPWSTR, C.c_int])
    api(U, 'IsWindowVisible', W.BOOL, [W.HWND])
    api(U, 'GetWindowLongPtrW', C.c_ssize_t, [W.HWND, C.c_int])
    api(U, 'GetWindowRect', W.BOOL, [W.HWND, C.POINTER(W.RECT)])
    api(U, 'SetWindowPos', W.BOOL, [W.HWND, W.HWND, C.c_int, C.c_int, C.c_int, C.c_int, W.UINT])
    api(U, 'SendMessageTimeoutW', C.c_ssize_t,
        [W.HWND, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT, C.POINTER(C.c_size_t)])
    api(U, 'SetThreadDpiAwarenessContext', C.c_void_p, [C.c_void_p])
    api(U, 'MonitorFromWindow', W.HANDLE, [W.HWND, W.DWORD])
    class MonitorInfo(C.Structure):
        _fields_ = [('size', W.DWORD), ('monitor', W.RECT), ('work', W.RECT), ('flags', W.DWORD)]
    api(U, 'GetMonitorInfoW', W.BOOL, [W.HANDLE, C.POINTER(MonitorInfo)])
    windows = []
    @callback
    def visit(window, unused):
        owner = W.DWORD(); U.GetWindowThreadProcessId(window, C.byref(owner))
        if owner.value == pid:
            name = C.create_unicode_buffer(128); U.GetClassNameW(window, name, len(name))
            if name.value == 'Pulse.Development.Main.v1' and U.IsWindowVisible(window):
                windows.append(window)
        return True
    U.EnumWindows(visit, 0)
    if not windows:
        return None
    if len(windows) != 1:
        raise RuntimeError('Ambiguous development startup window')
    window = windows[0]
    def send(message, command=0):
        result = C.c_size_t()
        if not U.SendMessageTimeoutW(window, message, command, 0, 2, 2000, C.byref(result)):
            raise RuntimeError('Development startup command timed out')
    previous_dpi = U.SetThreadDpiAwarenessContext(C.c_void_p(-4))
    try:
        info = MonitorInfo(); info.size = C.sizeof(info)
        rect = W.RECT()
        if not U.GetMonitorInfoW(U.MonitorFromWindow(window, 2), C.byref(info)) or not U.GetWindowRect(window, C.byref(rect)):
            raise RuntimeError('Cannot locate development startup window')
        work = info.work
        if not U.SetWindowPos(window, None, work.right-(rect.right-rect.left), work.top, 0, 0, 0x15):
            raise RuntimeError('Cannot dock development startup window')
        send(0x232)  # WM_EXITSIZEMOVE: native snap, fit and save handlers.
        if not U.GetWindowLongPtrW(window, -20) & 0x20:
            send(0x111, 115)  # Existing click-through command, requires a registered tray.
        U.GetWindowRect(window, C.byref(rect))
        style = U.GetWindowLongPtrW(window, -20)
        saved = configparser.ConfigParser(); saved.read(profile/'settings.ini', encoding='utf-8-sig')
        keys = ['hardware','battery','power_chart','network','processes','disk_io','sys_disk',
                'system_info','gpu_memory','wifi','battery_health']
        if (saved.get('Window','Mode') != 'full' or saved.getint('Window','zoom') != 70 or
                saved.getint('Window','OnTop') != 0 or saved.getint('Window','snap') != 1 or
                not all(saved.getint('Full', key) == 1 for key in keys) or
                style & 8 or not style & 0x20 or not style & 0x80000 or
                rect.right != work.right or rect.top != work.top or rect.bottom > work.bottom):
            raise RuntimeError('Development startup defaults did not take effect')
        return dict(pid=pid,defaultsSha256=defaults_hash,mode='full',zoom=70,allAvailableItems=True,
                    fpsEnabled=False,topmost=False,clickThrough=True,dock='right',
                    bounds=[rect.left,rect.top,rect.right,rect.bottom],
                    workArea=[work.left,work.top,work.right,work.bottom])
    finally:
        if previous_dpi:
            U.SetThreadDpiAwarenessContext(previous_dpi)


def main(plan_file, expected):
    plan_file = safe(plan_file)
    if plan_file.stat().st_size > 65536 or sha(plan_file) != expected:
        raise RuntimeError('Plan identity mismatch')
    plan = json.loads(plan_file.read_text(encoding='utf-8'))
    root, config = Path(plan['root']), plan['config']
    if plan['parentPid'] != os.getppid() or not within(root/'dev-work/evidence', plan_file):
        raise RuntimeError('Use the protected Node entry; direct worker invocation refused')
    state = json.loads((root/'.local-artifact-storage/state.json').read_text(encoding='utf-8'))
    lock = root/'.local-artifact-storage/lock.json'
    if not lock.is_file() or state['last']['status'] != 'active' or state['last']['owner'] != plan['owner']:
        raise RuntimeError('An active registered capacity lease is required')
    for item in config['identities'] + plan['inputs']:
        if sha(safe(item['path'])) != item['sha256']:
            raise RuntimeError('Input identity drift: ' + item['path'])
    os.environ['CODEX_TEST_MEMORY_PROFILE'] = plan['policy']['profile']
    P = runpy.run_path(config['memoryPolicy'])
    S = runpy.run_path(config['resourceSlots'])
    H = runpy.run_path(config['jobGuard'])
    policy = P['read_policy']()
    if policy != plan['policy']:
        raise RuntimeError('Child memory policy differs from the outer receipt')
    memory = H['MEMORY'](); memory.dwLength = C.sizeof(memory)
    if not H['K'].GlobalMemoryStatusEx(C.byref(memory)):
        raise C.WinError(C.get_last_error())
    slots = S['SlotLease'](S['plan_slots'](memory.ullTotalPhys, memory.ullAvailPhys, policy['hostReserveMiB'], plan['jobMiB']))
    started = time.monotonic()
    receipt = dict(status='FAILED', action=plan['action'], policy=policy, buildKey=plan['buildKey'], commands=[])
    desktop = None
    before = None
    original_job = H['guarded'].__globals__['job']
    original_decode = H['guarded'].__globals__['native_output']
    H['guarded'].__globals__['ROOT'] = root
    last_tick = 0
    startup_state = None

    def bounded_job():
        handle = original_job()
        limits = H['LIMITS']()
        if not H['K'].QueryInformationJobObject(handle, 9, C.byref(limits), C.sizeof(limits), None):
            H['K'].CloseHandle(handle); raise C.WinError(C.get_last_error())
        limits.ProcessMemoryLimit = limits.JobMemoryLimit = plan['jobMiB'] * MIB
        limits.BasicLimitInformation.LimitFlags |= 0x4 | 0x8
        limits.BasicLimitInformation.PerJobUserTimeLimit = (600 if plan['action'] == 'run' else 120) * 10000000
        limits.BasicLimitInformation.ActiveProcessLimit = 16
        class Cpu(C.Structure):
            _fields_ = [('flags', W.DWORD), ('rate', W.DWORD)]
        cpu = Cpu(5, 2500)
        if (not H['K'].SetInformationJobObject(handle, 9, C.byref(limits), C.sizeof(limits)) or
                not H['K'].SetInformationJobObject(handle, 15, C.byref(cpu), C.sizeof(cpu))):
            H['K'].CloseHandle(handle); raise C.WinError(C.get_last_error())
        return handle

    def tick():
        nonlocal last_tick
        now = time.monotonic()
        if now - last_tick < 1:
            return
        last_tick = now
        if now - started > plan['wallSeconds']:
            raise RuntimeError('Whole action deadline reached')
        if P['read_policy']() != policy or H['N']['available']() < policy['hostReserveMiB'] * MIB:
            raise RuntimeError('Memory policy/reserve drift')
        if shutil.disk_usage(root).free < 40 * 1024**3:
            raise RuntimeError('Physical disk headroom exhausted')
        used = 0; count = 0
        for file in root.rglob('*'):
            safe(file)
            if file.is_file():
                used += file.stat().st_size; count += 1
        if used > 1024 * MIB or count > 10000:
            raise RuntimeError('Project capacity bound exceeded')
        if plan['action'] == 'build':
            build_bytes = sum(file.stat().st_size for file in (root/'dev-work'/'builds').rglob('*') if file.is_file())
            if build_bytes > 256 * MIB:
                raise RuntimeError('Registered build capacity bound exceeded')
        if startup_state and startup_state.get('pid') and not startup_state.get('applied'):
            applied = apply_interactive_defaults(startup_state['pid'], startup_state['profile'], startup_state['sha256'])
            if applied:
                write(startup_state['profile']/'startup-defaults.json', applied)
                receipt['interactiveDefaults'] = applied
                startup_state['applied'] = True
            elif now - startup_state['started'] > 10:
                raise RuntimeError('Development window did not become ready for startup defaults')

    def decode(raw, ansi_codepage=None):
        if len(raw) > 4 and raw[1::2].count(0) > len(raw[1::2]) // 2:
            return dict(output=raw.decode('utf-16-le'), outputEncoding='utf-16-le')
        return original_decode(raw, ansi_codepage)

    def run(args, label, desktop_name=None, expected_code=0, maximum_seconds=60):
        tick()
        original_create = H['K'].CreateProcessW
        def create(*parameters):
            startup = C.cast(parameters[8], C.POINTER(H['STARTUP_EX'])).contents
            startup.startup.desktop = desktop_name
            if startup_state:
                startup.startup.show = 1  # Explicitly authorized visible Run, not SW_HIDE.
            created = original_create(*parameters)
            if created and startup_state:
                startup_state['pid'] = C.cast(parameters[9], C.POINTER(H['PROCESS'])).contents.pid
            return created
        if desktop_name or startup_state:
            H['K'].CreateProcessW = create
        try:
            remaining = max(1, plan['wallSeconds'] - (time.monotonic() - started))
            result = H['guarded']([str(arg) for arg in args], min(maximum_seconds, remaining), tick,
                                  maximum=plan['outputLimitBytes'])
        finally:
            H['K'].CreateProcessW = original_create
        receipt['commands'].append(dict(label=label, **result))
        if result['status'] != 'EXITED' or result['exitCode'] != expected_code:
            raise RuntimeError(label + ' failed: ' + result['output'][-12000:])
        return result['output']

    try:
        H['guarded'].__globals__['job'] = bounded_job
        H['guarded'].__globals__['native_output'] = decode
        before = production(config)
        receipt['productionBefore'] = before
        target = safe(plan['targetDirectory'])
        if target.exists():
            raise RuntimeError('Unknown/previous run directory exists; no overwrite')
        target.mkdir()
        temp = target/'tmp'; temp.mkdir()
        os.environ['TEMP'] = os.environ['TMP'] = str(temp)
        bin_dir = Path(config['toolchain'])/'bin'
        os.environ['PATH'] = str(bin_dir) + os.pathsep + os.environ['PATH']
        compiler = bin_dir/'clang++.exe'
        libs = ['winhttp','powrprof','pdh','iphlpapi','ntdll','winmm','shell32','gdi32','user32','ole32','advapi32','uuid','ws2_32','setupapi']
        if plan['action'] == 'build':
            resource = target/'app.res.o'
            run([bin_dir/'x86_64-w64-mingw32-windres.exe','-F','pe-x86-64','-i',root/'app.rc','-O','coff','-o',resource], 'resources')
            cpp = sorted((root/'src').glob('*.cpp'))
            commands = []
            all_outputs = [resource]
            dev_objects, normal_objects = {}, {}
            intermediates = []
            def release_objects(files):
                identities = [dict(path=str(file),bytes=file.stat().st_size,sha256=sha(file)) for file in files]
                for item in identities:
                    file = safe(item['path'])
                    if file.parent != target or file.stat().st_size != item['bytes'] or sha(file) != item['sha256']:
                        raise RuntimeError('Current build intermediate identity changed; preserve it')
                    file.unlink()
                intermediates.extend(identities)
            for variant, objects in (('dev', dev_objects), ('normal', normal_objects)):
                for source in cpp:
                    if variant == 'normal' and source.stem == 'metrics_selftest':
                        continue
                    output = target/(variant+'-'+source.stem+'.o')
                    args = [compiler,'-std=c++17','-g' if variant == 'dev' else '-g0','-O0','-fno-omit-frame-pointer','-pthread','-c',source,'-o',output]
                    if variant == 'dev':
                        args.append('-DPULSE_DEVELOPMENT=1')
                    commands.append(dict(directory=str(root), file=str(source), arguments=[str(arg) for arg in args]))
                    run(args, variant+' '+source.name)
                    objects[source.stem] = output; all_outputs.append(output)
                programs = [('Pulse-Dev.exe',False), ('metrics-selftest-dev.exe',True)] if variant == 'dev' else [('Pulse-ReleaseCheck.exe',False)]
                for name, console in programs:
                    selected = [objects[key] for key in ('metrics_selftest','metrics','netprobe','fpsmon')] if console else [value for key,value in objects.items() if key != 'metrics_selftest']
                    exe = target/name
                    args = [compiler,'-g' if variant == 'dev' else '-g0','-static','-pthread','-municode', *selected]
                    if not console:
                        args += ['-mwindows',resource,'-ld2d1','-ldwrite','-ldwmapi','-lcomctl32']
                    args += ['-o',exe] + ['-l'+library for library in libs]
                    run(args, 'link '+name); all_outputs.append(exe)
                release_objects(objects.values())
            database = target/'compile_commands.json'
            write(database, [row for row in commands if '-DPULSE_DEVELOPMENT=1' in row['arguments']])
            all_outputs.append(database)
            # Each variant releases only its own objects after all its links succeed.
            release_objects([resource])
            receipt['removedBuildIntermediates'] = intermediates
            all_outputs = [file for file in all_outputs if file.suffix != '.o']
            manifest = dict(status='PASS',buildKey=plan['buildKey'],
                outputs=[dict(path=str(file),bytes=file.stat().st_size,sha256=sha(file)) for file in all_outputs])
            write(target/'build.json', manifest)
            receipt['build'] = manifest
        else:
            profile = target/'profile'; profile.mkdir()
            write(target/'pulse-dev-session.json', dict(id=plan['id'],owner=plan['owner'],action=plan['action']))
            if plan['action'] == 'run':
                defaults = safe(root/'tools/development-defaults.ini')
                if defaults.stat().st_size > 8192:
                    raise RuntimeError('Oversized development defaults')
                with (profile/'settings.ini').open('xb') as destination:
                    destination.write(defaults.read_bytes())
                startup_state = dict(profile=profile,sha256=sha(defaults),started=time.monotonic())
            else:
                (profile/'settings.ini').write_text('[Window]\nMode=compact\nCompact=1\nOnTop=0\nzoom=100\n[Network]\nnotify=0\nnotify_sound=0\n',encoding='utf-8')
            os.environ['PULSE_DEV_DATA_DIR'] = str(profile)
            os.environ['PULSE_DEV_SESSION_KIND'] = 'interactive' if plan['action'] == 'run' else plan['action']
            exe = Path(plan['buildDir'])/'Pulse-Dev.exe'
            if plan['action'] == 'run':
                run([exe], 'visible development session', maximum_seconds=1800)
            else:
                visible = input_desktop()
                name = 'PulseDev-' + plan['id'].replace('-','')
                # No DESKTOP_SWITCHDESKTOP access is requested; the input desktop is never switched.
                desktop = U.CreateDesktopW(name, None, None, 0, 0x00C7, None)
                if not desktop:
                    raise C.WinError(C.get_last_error())
                os.environ['PULSE_DEV_DESKTOP'] = name
                desktop_name = 'winsta0\\' + name
                receipt['desktop'] = dict(name=name,inputBefore=visible,switchRequested=False)
                if plan['action'] == 'debug':
                    for argument, label, restored in (('--dev-smoke','first launch',False),
                                                       ('--dev-smoke-resume','settings restart',True)):
                        output = run([bin_dir/'lldb.exe','--no-lldbinit','--batch','-o','breakpoint set --name wWinMain',
                                      '-o','run','-o','next','-o','bt','-o','continue','--',exe,argument],
                                     'LLDB GUI breakpoint, step and resume: '+label,desktop_name,maximum_seconds=30)
                        smoke_file = safe(profile/('smoke-resume.json' if restored else 'smoke-first.json'))
                        if not smoke_file.is_file() or smoke_file.stat().st_size > 4096:
                            raise RuntimeError('GUI did not write a bounded smoke receipt in this session')
                        smoke_result = json.loads(smoke_file.read_text(encoding='utf-8'))
                        if ('stop reason = breakpoint' not in output or 'stop reason = step' not in output or
                                f"Process {smoke_result.get('pid')} exited with status = 0" not in output):
                            raise RuntimeError('LLDB did not confirm GUI breakpoint, step and this process clean exit')
                        if (smoke_result.get('pass') is not True or smoke_result.get('restored') != restored or
                                smoke_result.get('mode') != 'compact' or smoke_result.get('zoom') != 125 or
                                smoke_result.get('fullPaint') != 0 or smoke_result.get('compactPaint') != 0):
                            raise RuntimeError('GUI smoke output did not confirm rendering and settings')
                        receipt.setdefault('guiSmokeResults', []).append(smoke_result)
                    output = run([bin_dir/'lldb.exe','--no-lldbinit','--batch','-o','breakpoint set --name wmain',
                                  '-o','run','-o','bt','-o','continue','--',Path(plan['buildDir'])/'metrics-selftest-dev.exe',
                                  '--samples','1','--compact','--no-probe'],
                                 'LLDB backend breakpoint and resume',desktop_name,maximum_seconds=30)
                    if 'stop reason = breakpoint' not in output or 'duration boundaries: PASS' not in output or 'exited with status = 0' not in output:
                        raise RuntimeError('LLDB did not confirm the backend breakpoint and clean completion')
                    receipt['debugScope'] = 'GUI first launch and settings restart: source breakpoint, step, rendering and clean exit; backend breakpoint and clean exit.'
                else:
                    output = run([exe,'--dev-smoke'], 'isolated first launch',desktop_name)
                    if 'PULSE_DEV_SMOKE ' not in output:
                        raise RuntimeError('First development launch produced no smoke receipt')
                    output = run([exe,'--dev-smoke-resume'], 'isolated restart restores settings',desktop_name)
                    if '"restored":true' not in output:
                        raise RuntimeError('Restart did not restore development settings')
                    run([exe,'--dev-smoke','unexpected'], 'refuse extra smoke arguments',desktop_name,expected_code=1)
                    run([exe], 'refuse missing smoke argument',desktop_name,expected_code=1)
                    run([Path(plan['buildDir'])/'metrics-selftest-dev.exe','--samples','3','--compact','--no-probe'],
                        'bounded backend sampling without network',desktop_name)
                    run([exe,'--dev-smoke'], 'refuse smoke on current desktop',expected_code=2)
                    original = os.environ.pop('PULSE_DEV_DATA_DIR')
                    try:
                        run([exe], 'refuse unregistered profile',desktop_name,expected_code=2)
                    finally:
                        os.environ['PULSE_DEV_DATA_DIR'] = original
                    from PIL import Image
                    captures = []
                    for bitmap in sorted(profile.glob('*.bmp')):
                        safe(bitmap)
                        if bitmap.stat().st_size > 17 * MIB:
                            raise RuntimeError('Native capture exceeded its bound')
                        png = bitmap.with_suffix('.png')
                        with Image.open(bitmap) as source:
                            rgb = source.convert('RGB')
                            if rgb.getextrema() == ((0, 0), (0, 0), (0, 0)):
                                raise RuntimeError('Native render capture is blank')
                            with safe(png).open('xb') as destination:
                                rgb.save(destination, format='PNG')
                            captures.append(dict(path=str(png),width=rgb.width,height=rgb.height,sha256=sha(png)))
                    if len(captures) != 7:
                        raise RuntimeError('Expected six zoom captures and one click-through capture')
                    receipt['renderCaptures'] = captures
                if input_desktop() != visible:
                    raise RuntimeError('Input desktop changed during verification')
                receipt['desktop']['inputAfter'] = visible
        for item in plan['inputs']:
            if sha(item['path']) != item['sha256']:
                raise RuntimeError('Source changed while executing; result not reusable')
        receipt['status'] = 'PASS'
    except BaseException as error:
        receipt['error'] = str(error)
        receipt['status'] = 'FAILED'
    finally:
        try:
            after = production(config)
            receipt['productionAfter'] = after
            receipt['productionUnchanged'] = before == after
            if before != after:
                receipt['status'] = 'FAILED'
                receipt['error'] = 'Installed Pulse state changed during this action; preserve and inspect, do not revert user edits'
            remaining = [row for row in processes() if within(root/'dev-work',row['path'])]
            receipt['remainingDevelopmentProcesses'] = remaining
            if remaining:
                receipt['status'] = 'FAILED'; receipt['error'] = 'Development process exit is not confirmed'
            if desktop and not U.CloseDesktop(desktop):
                receipt['status'] = 'FAILED'; receipt['error'] = 'Private desktop cleanup failed'
            receipt['desktopClosed'] = bool(desktop)
        finally:
            slots.close()
            receipt['slots'] = slots.record()
            receipt['elapsedSeconds'] = time.monotonic() - started
            write(plan['receiptFile'], receipt)
    print(json.dumps(dict(status=receipt['status'],receipt=plan['receiptFile'],error=receipt.get('error'))))
    return 0 if receipt['status'] == 'PASS' else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--inventory', type=Path)
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--sha256')
    args = parser.parse_args()
    if args.inventory:
        print(json.dumps({'processes':[row for row in processes() if within(args.inventory/'dev-work',row['path'])]}))
    elif args.plan and args.sha256:
        sys.exit(main(args.plan,args.sha256))
    else:
        parser.error('Use tools/Pulse-Dev.ps1')
