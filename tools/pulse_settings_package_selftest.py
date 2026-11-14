"""Exercise actual package license inclusion with a synthetic binary; never publish."""
import importlib.util,json,re,sys,zipfile
from pathlib import Path
root=Path(__file__).resolve().parent.parent
target=Path(sys.argv[1]).resolve()
assert target.parent==root/'dev-work/vm-sessions' and re.fullmatch('[a-f0-9-]{36}',target.name)
assert json.loads((root/'.local-artifact-storage/state.json').read_text(encoding='utf-8'))['last']['status']=='active'
spec=importlib.util.spec_from_file_location('pulse_package',root/'scripts/publish_custom_release.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
module.gh=lambda *args: (_ for _ in ()).throw(AssertionError('No remote calls in package checks'))
binary=b'MZ synthetic package contract fixture; not executable'
out=target/'settings-package-fixture'
tag,files=module.package(out,binary,None,None,'a'*40)
with zipfile.ZipFile(files[0]) as archive:
    assert set(archive.namelist())=={'Pulse.exe','LICENSE','README.txt','build-info.json'}
    assert archive.read('Pulse.exe')==binary
    notice=(root/'THIRD_PARTY_NOTICES.md').read_text(encoding='utf-8')
    assert notice in archive.read('LICENSE').decode('utf-8') and 'Diorser/LiteMonitor' in notice
    assert '.NET Framework' in archive.read('README.txt').decode('utf-8')
    assert b'\\Users\\' not in archive.read('build-info.json')
print('settings package contract: PASS (synthetic, no network, source notice included)')
