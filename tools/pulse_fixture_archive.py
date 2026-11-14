"""Archive completed synthetic tool fixtures, then retire via the proven native helper."""
import hashlib,json,os,sys,zipfile
from pathlib import Path
import pulse_artifact_delete as native

def run(plan_file,expected):
    raw=Path(plan_file).read_bytes();assert hashlib.sha256(raw).hexdigest()==expected
    plan=json.loads(raw);root=Path(plan['root']);session=Path(plan.get('fixtureSession',plan['targetDirectory']))
    assert session.parent==root/'dev-work/vm-sessions'
    active=json.loads((root/'.local-artifact-storage/state.json').read_text('utf-8'))['last']
    assert active['status']=='active' and active['owner']==plan['owner'] and (root/'.local-artifact-storage/lock.json').is_file()
    in_job=native.W.BOOL();assert native.K.IsProcessInJob(native.K.GetCurrentProcess(),None,native.C.byref(in_job)) and in_job.value
    paths=[]
    for folder in ('workflow-fixtures','settings-package-fixture'):
        base=session/folder
        if base.exists():
            for file in base.rglob('*'):
                native.safe(root,file)
                if file.is_file():paths.append(file)
    assert 0<len(paths)<=512
    items=[]
    for file in sorted(paths):
        stat=file.stat();assert stat.st_nlink==1 and stat.st_size<=2*1024*1024
        with native.opened(file,0x80000000,1,0x08200000) as handle: info=native.information(handle)
        identity=dict(path=str(file),deviceId=str(stat.st_dev),fileId=str(stat.st_ino),bytes=stat.st_size,
            mtimeMs=stat.st_mtime_ns/1000000,sha256=hashlib.sha256(file.read_bytes()).hexdigest())
        items.append(dict(path=str(file),identity=identity,nativeIdentity=dict(deviceId=str(info.volume),fileId=str((info.high_id<<32)|info.low_id))))
    if plan['action']=='artifact-retire':
        declared={x['path']:x for x in plan['retirements']};assert set(declared)==set(map(str,paths))
        for item in items:
            expected_item=declared[item['path']]
            assert item['identity']['sha256']==expected_item['identity']['sha256'] and item['nativeIdentity']==expected_item['nativeIdentity']
        items=plan['retirements']
    else:assert plan['action']=='workflow-test' and plan['owner']=='pulse-development-sessions'
    archive=Path(plan['fixtureArchive']);assert archive.parent==root/'dev-work/evidence' and not archive.exists()
    with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED) as zipped:
        for item in items:
            file=Path(item['path']);data=file.read_bytes();assert hashlib.sha256(data).hexdigest()==item['identity']['sha256']
            zipped.writestr(file.relative_to(root).as_posix(),data)
        zipped.writestr('fixture-manifest.json',json.dumps(items,ensure_ascii=False))
    with zipfile.ZipFile(archive) as zipped:
        for item in items:assert hashlib.sha256(zipped.read(Path(item['path']).relative_to(root).as_posix())).hexdigest()==item['identity']['sha256']
    result=dict(status='PASS',archive=str(archive),archiveSha256=hashlib.sha256(archive.read_bytes()).hexdigest(),files=len(items))
    if plan['action']=='workflow-test':
        journal=Path(plan['fixtureJournal']);assert journal.parent==archive.parent
        result.update(native.retire(root,items,journal));result['journal']=str(journal)
    return result
if __name__=='__main__':print(json.dumps(run(sys.argv[1],sys.argv[2])))
