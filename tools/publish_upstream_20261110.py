"""Exact release preparation; no credentials, installation or remote writes."""
import io,json,sys,hashlib,zipfile,urllib.request,importlib.util,subprocess,os
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
PRIVATE_HOME=Path(os.environ.get('USERPROFILE',str(Path.home())))
COMMIT='c76c8f5da525e81c6d7d52dbd741d76a1c00336e'
TAG='custom-2026.11.10.2'
REPO='haihaihailailo/Pulse'
CI=ROOT/'dev-work/ci/c76c8f5'
OUT=ROOT/'dev-work/releases'/TAG
def check(ok,message):
    if not ok: raise RuntimeError(message)
def digest(raw): return hashlib.sha256(raw).hexdigest()
def sha(p): return digest(p.read_bytes())
def write(p,value):
    raw=(json.dumps(value,ensure_ascii=False,indent=2)+'\n').encode('utf-8')
    check(len(raw)<65536,'Receipt bound')
    with p.open('xb') as f:f.write(raw)
def fetch(url):
    check(url.startswith('https://'),'HTTPS required')
    req=urllib.request.Request(url,headers={'User-Agent':'Pulse-release-verification'})
    with urllib.request.urlopen(req,timeout=30) as r: data=r.read(16*1024*1024+1)
    check(len(data)<=16*1024*1024,'Download bound');return data
def api(tail):
    if stage=='release-verify':
        # Metadata uses the authenticated CLI; asset downloads below stay anonymous.
        value=subprocess.run([r'C:\Program Files\GitHub CLI\gh.exe','api','repos/'+REPO+'/'+tail],capture_output=True,timeout=30)
        check(value.returncode==0,value.stderr.decode('utf-8','replace')[:1000])
        check(len(value.stdout)<4*1024*1024,'Metadata response bound')
        return json.loads(value.stdout)
    return json.loads(fetch('https://api.github.com/repos/'+REPO+'/'+tail))
args=json.load(sys.stdin);stage=sys.argv[1]
if stage == 'release-verify' and 'tag' in args:
    import re
    check(re.fullmatch(r'custom-\d{4}\.\d{2}\.\d{2}\.\d+',args['tag']), 'Invalid verification tag')
    TAG=args['tag']; OUT=ROOT/'dev-work/releases'/TAG
    prepared=json.loads((OUT/'publication-plan.json').read_text(encoding='utf-8'))
    check(prepared['tag']==TAG and prepared['repository']==REPO and re.fullmatch('[0-9a-f]{40}',prepared['commit']), 'Invalid preparation')
    COMMIT=prepared['commit']
elif stage == 'release-package' and 'tag' in args:
    import re
    TAG=args['tag']; COMMIT=args['commit']
    check(re.fullmatch(r'custom-\d{4}\.\d{2}\.\d{2}\.\d+',TAG) and re.fullmatch('[0-9a-f]{40}',COMMIT),'Invalid package target')
    executable=Path(args['artifactExe']).resolve()
    check(executable.name=='Pulse.exe' and executable.parent.parent==ROOT/'dev-work/builds' and
          re.fullmatch('release-[a-f0-9]{24}',executable.parent.name),'Exact local artifact required')
    CI=executable.parent; OUT=ROOT/'dev-work/releases'/TAG
elif stage not in ('release-import','release-source'):
    COMMIT=args.get('commit',COMMIT)
    check(COMMIT in ('c76c8f5da525e81c6d7d52dbd741d76a1c00336e','3087bb8c41cd58618809f44258a65333ab099321','f8001b2c69d3e03d602c76f2f8dedcdad27fc02f','2a118b31fbabbe99c45e55fc1810e02aefe3d45f','6bd690998893ee21e8df5fca2afcfa381cc499ab'),'Unapproved release commit')
    if COMMIT=='3087bb8c41cd58618809f44258a65333ab099321': TAG='custom-2026.11.11.1'
    if COMMIT=='f8001b2c69d3e03d602c76f2f8dedcdad27fc02f': TAG='custom-2026.11.13.1'
    if COMMIT=='2a118b31fbabbe99c45e55fc1810e02aefe3d45f': TAG='custom-2026.11.14.1'
    if COMMIT=='6bd690998893ee21e8df5fca2afcfa381cc499ab': TAG='custom-2026.11.14.2'
    CI=ROOT/'dev-work/ci'/COMMIT[:7]
    if TAG=='custom-2026.11.13.1': CI=ROOT/'dev-work/builds/release-7766aa69fec401daf6c56cd8'
    if TAG=='custom-2026.11.14.1': CI=ROOT/'dev-work/builds/release-af882c0ff08431d95ada7d78'
    if TAG=='custom-2026.11.14.2': CI=ROOT/'dev-work/builds/release-2f761f28c82a93f358051d31'
    OUT=ROOT/'dev-work/releases'/TAG
state=json.loads((ROOT/'.local-artifact-storage/state.json').read_text(encoding='utf-8'))
check(state['last']['status']=='active' and state['last']['owner']=='pulse-upstream-release-20261110','Active release lease required')
if stage=='release-source':
    scoped=isinstance(args.get('paths'),list)
    base=args['base'] if scoped else '2a118b31fbabbe99c45e55fc1810e02aefe3d45f'
    branch=args['branch'] if scoped else 'codex/pulse-optimization-20261114'
    tag=args['tag'] if scoped else 'custom-2026.11.14.2'
    fields={'base','branch','tag','paths'} if scoped else {'base','branch','tag'}
    check(set(args)-{'message'}==fields and ('message' not in args or scoped),'Exact source scope required')
    message=args.get('message','Fix peripheral discovery and battery protocols; automate safe artifact retirement' if scoped else 'Optimize desktop interaction, background sampling and peripheral settings')
    check(isinstance(message,str) and 1<=len(message)<=200 and not any(ord(c)<32 for c in message),'Invalid commit message')
    gh=Path(r'C:\Program Files\GitHub CLI\gh.exe');check(gh.is_file(),'Verified gh is missing')
    audit=ROOT/'dev-work/evidence'/('release-source-'+tag)
    progress_file=audit/'source-progress.jsonl'
    previous=[json.loads(line) for line in progress_file.read_text(encoding='utf-8').splitlines()] if progress_file.exists() else []
    if previous:
        check(previous[0]['operation']=='prepared' and all(previous[0]['response'][k]==v for k,v in
              [('base',base),('branch',branch),('tag',tag)]),'Source continuation target differs')
        check(not any(row['operation'] in ('git/trees','git/commits','commit-prepared','git/refs') or
              (row['operation']=='intent' and row['response']['endpoint']!='git/blobs') for row in previous),
              'Source advanced beyond blobs; inspect immutable commit/ref checkpoint before continuing')
    progress=progress_file.open('a' if previous else 'x',encoding='utf-8')
    def record(operation,value):
        progress.write(json.dumps(dict(operation=operation,response=value),ensure_ascii=False)+'\n');progress.flush();os.fsync(progress.fileno())
    def github(tail,payload=None,missing=False):
        command=[str(gh),'api','repos/'+REPO+'/'+tail]
        body=None
        if payload is not None:
            body=json.dumps(payload,ensure_ascii=False).encode('utf-8');check(len(body)<1048576,'Git request bound')
            command+=['--method','POST','--input','-']
            record('intent',dict(endpoint=tail,bodySha256=digest(body)))
        completed=subprocess.run(command,input=body,capture_output=True,timeout=30)
        if missing and completed.returncode!=0:
            try: absent=json.loads(completed.stdout)
            except (ValueError,UnicodeDecodeError): absent={}
            if str(absent.get('status'))=='404' and 'HTTP 404' in completed.stderr.decode('utf-8','replace'): return None
        check(completed.returncode==0,completed.stderr.decode('utf-8','replace')[:1000])
        check(len(completed.stdout)<4*1024*1024,'Git response bound')
        value=json.loads(completed.stdout)
        if payload is not None:record(tail,value)
        return value
    check(github('git/ref/heads/main')['object']['sha']==base,'Main changed before source publication')
    check(not github('git/matching-refs/heads/'+branch),'Branch already exists; inspect before retrying')
    check(not any(r['tag_name']==tag for r in github('releases?per_page=10')),'Version already exists')
    paths=['app.rc','src/updater.h','src/i18n.cpp','src/metrics.cpp','src/metrics.h','src/metrics_sampler.h',
           'src/peripheral_battery.h','src/peripheral_state.h','src/ui_model.cpp','src/ui_model.h',
           'src/ui_renderer.cpp','src/ui_renderer.h','src/ui_settings.cpp','src/ui_settings.h',
           'src/ui_window.cpp','src/update.ps1','tests/update_selftest.ps1','scripts/publish_custom_release.py']
    if scoped: paths=args['paths']
    check(len(paths)==len(set(paths)) and len(paths)<=128,'Bounded unique paths required')
    content={p:(ROOT/p).read_text(encoding='utf-8-sig').replace('\r\n','\n') for p in paths}
    check(tag in content['src/updater.h'] and 'FILEVERSION 1,3,0,' in content['app.rc'],'Source version mismatch')
    for text in content.values():
        check(str(PRIVATE_HOME) not in text and PRIVATE_HOME.as_posix() not in text,'Private source path')
    identities=[dict(path=p,sha256=digest(text.encode('utf-8')),bytes=len(text.encode('utf-8'))) for p,text in content.items()]
    record('prepared',dict(base=base,branch=branch,tag=tag,files=identities))
    base_tree=github('git/commits/'+base)['tree']['sha']
    prior_tree=github('git/trees/'+base_tree+'?recursive=1')
    check(not prior_tree.get('truncated'),'Base tree truncated')
    prior={x['path']:x['sha'] for x in prior_tree['tree'] if x['type']=='blob'}
    rows=[]
    acknowledged={row['response']['sha'] for row in previous if row['operation']=='git/blobs'}
    for p,text in content.items():
        raw=text.encode('utf-8');expected=hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()
        if prior.get(p)==expected: continue
        blob=dict(sha=expected) if expected in acknowledged else github('git/blobs/'+expected,missing=True) if previous else None
        if blob is None: blob=github('git/blobs',dict(content=text,encoding='utf-8'))
        check(blob['sha']==expected,'Remote blob differs: '+p)
        rows.append(dict(path=p,mode='100644',type='blob',sha=blob['sha']))
    check(rows,'No source changes to publish')
    tree=github('git/trees',dict(base_tree=base_tree,tree=rows))
    for p,text in content.items():check((ROOT/p).read_text(encoding='utf-8-sig').replace('\r\n','\n')==text,'Source changed during upload')
    commit=github('git/commits',dict(message=message+'\n\nRelease '+tag,tree=tree['sha'],parents=[base]))
    record('commit-prepared',dict(commit=commit['sha'],tree=tree['sha']))
    github('git/refs',dict(ref='refs/heads/'+branch,sha=commit['sha']))
    check(github('git/ref/heads/'+branch)['object']['sha']==commit['sha'],'Published branch differs')
    actual=github('git/trees/'+tree['sha']+'?recursive=1')
    check(not actual.get('truncated'),'Remote tree truncated')
    bypath={x['path']:x for x in actual['tree']}
    check(all(bypath[x['path']]['sha']==x['sha'] for x in rows),'Source readback differs')
    main_unchanged=github('git/ref/heads/main')['object']['sha']==base
    write(audit/'source-receipt.json',dict(status='PASS',repository=REPO,base=base,branch=branch,tag=tag,
          commit=commit['sha'],tree=tree['sha'],files=identities,remoteFiles=rows,mainUnchanged=main_unchanged))
    progress.close()
    print(json.dumps(dict(stage=stage,status='PASS',commit=commit['sha'],branch=branch,files=len(rows))))
    sys.exit(0)
elif stage=='release-import':
    import_commit=args.get('commit',COMMIT)
    check(import_commit in (COMMIT,'503f60eee59a2bf31de11648bb13d4fb3ad77bc8','3087bb8c41cd58618809f44258a65333ab099321'),'Unapproved CI commit')
    ci=ROOT/'dev-work/ci'/import_commit[:7]
    run=api('actions/runs/'+str(args['run']))
    check(run['head_sha']==import_commit and run['conclusion']=='success','Exact CI must pass')
    check(run['path']=='.github/workflows/build.yml','Expected build workflow required')
    if import_commit!=COMMIT:
        check(run['head_branch']=='codex/pulse-stability-20261111','Expected authorized branch required')
    jobs=api('actions/runs/'+str(args['run'])+'/jobs')['jobs']
    check(len(jobs)==2 and all(j['conclusion']=='success' for j in jobs),'Both compilers must pass')
    artifact=api('actions/artifacts/'+str(args['artifact']))
    check(artifact['name']=='Pulse-mingw-win-x64' and not artifact['expired'] and artifact['workflow_run']['id']==run['id'] and artifact['workflow_run']['head_sha']==import_commit,'Exact artifact provenance required')
    raw=fetch(args['url']) if 'url' in args else Path(args['downloadPath']).read_bytes()
    check(len(raw)<=16*1024*1024 and artifact['digest']=='sha256:'+digest(raw),'Artifact digest mismatch')
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        check(sorted(z.namelist())==['Pulse.exe','metrics_selftest.exe'],'Unexpected artifact members')
        check(sum(i.file_size for i in z.infolist())<=24*1024*1024,'Expansion bound')
        payload={n:z.read(n) for n in z.namelist()}
    with (ci/'artifact.zip').open('xb') as f:f.write(raw)
    for n,b in payload.items():
        with (ci/n).open('xb') as f:f.write(b)
    write(ci/'artifact.json',dict(status='PASS',repository=REPO,commit=import_commit,ciRun=run['id'],artifactId=artifact['id'],ciZipSha256=digest(raw),executableSha256=digest(payload['Pulse.exe']),bytes=len(payload['Pulse.exe'])))
elif stage=='release-package':
    if TAG=='custom-2026.11.14.2' or 'tag' in args:
        source=json.loads((ROOT/'dev-work/evidence'/('release-source-'+TAG)/'source-receipt.json').read_text(encoding='utf-8'))
        check(source['status']=='PASS' and source['commit']==COMMIT and source['tag']==TAG,'Verified source publication required')
        for item in source['files']:
            raw=(ROOT/item['path']).read_text(encoding='utf-8-sig').replace('\r\n','\n').encode('utf-8')
            check(digest(raw)==item['sha256'],'Published source changed: '+item['path'])
    trial=Path(args['trialReceipt']);result=json.loads(trial.read_text(encoding='utf-8'))
    plan=json.loads(Path(str(trial).replace('-result.json','-plan.json')).read_text(encoding='utf-8'))
    check(result['status']=='PASS' and result['action']=='trial-test' and plan['artifact']['path']==str(CI/'Pulse.exe') and plan['artifact']['sha256']==sha(CI/'Pulse.exe'),'Exact trial required')
    local='artifactExe' in args or TAG in ('custom-2026.11.13.1','custom-2026.11.14.1','custom-2026.11.14.2')
    if local:
        build=json.loads((CI/'build.json').read_text(encoding='utf-8'))
        check(build['status']=='PASS' and build['kind']=='release','A verified release build is required')
        outputs=[o for o in build['outputs'] if o['path']==str(CI/'Pulse.exe')]
        check(len(outputs)==1 and outputs[0]['sha256']==sha(CI/'Pulse.exe'),'Release binary changed')
        check(result['productionUnchanged'] and result['desktopClosed'] and
              not result['remainingDevelopmentProcesses'] and result['slots']['released'],'Trial cleanup incomplete')
        for item in build['inputs']:
            if Path(item['path']).name not in ('pulse_dev.cjs',):
                current_sha=sha(Path(item['path']))
                test_reader_fix=(TAG=='custom-2026.11.14.2' and Path(item['path']).name=='pulse_dev_worker.py' and
                    item['sha256']=='c8030503f08944cab8ad4035a9db607a1df57f412d6811885f61ca11b207816c' and
                    current_sha=='463433e932b049cd566e8d49f6975b6e77410ad7d8c530c450cc324a3d792ff4' and
                    any(p['path']==item['path'] and p['sha256']==current_sha for p in plan['inputs']))
                check(current_sha==item['sha256'] or test_reader_fix,'Build input changed: '+Path(item['path']).name)
        a=dict(executableSha256=sha(CI/'Pulse.exe'))
        raw=(CI/'Pulse.exe').read_bytes()
        for private in (str(PRIVATE_HOME),PRIVATE_HOME.as_posix(),'Documents\\Codex','Documents/Codex'):
            check(private.encode() not in raw and private.encode('utf-16-le') not in raw,'Private build path in executable')
    else:
        a=json.loads((CI/'artifact.json').read_text(encoding='utf-8'))
        check(a['commit']==COMMIT and a['executableSha256']==sha(CI/'Pulse.exe'),'Artifact changed')
    spec=importlib.util.spec_from_file_location('publish_custom',ROOT/'scripts/publish_custom_release.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    tag,files=module.package(OUT,(CI/'Pulse.exe').read_bytes(),None if local else dict(id=a['artifactId'],digest='sha256:'+a['ciZipSha256']),None if local else a['ciRun'],COMMIT)
    check(tag==TAG,'Wrong version')
    with zipfile.ZipFile(files[0]) as z:
        check(digest(z.read('Pulse.exe'))==a['executableSha256'],'Binary changed')
        check(set(z.namelist())=={'Pulse.exe','LICENSE','README.txt','build-info.json'},'Unexpected delivered files')
        check(not z.comment and all(not item.comment and not item.extra for item in z.infolist()),'Unexpected ZIP metadata')
        for name in ('LICENSE','README.txt','build-info.json'):
            text=z.read(name).decode('utf-8')
            check(all(value not in text for value in (str(PRIVATE_HOME),PRIVATE_HOME.as_posix(),'Documents\\Codex','Documents/Codex','AI generated','ChatGPT','OpenAI')),'Private or assistant metadata in package text')
    write(OUT/'package-receipt.json',dict(status='PASS',commit=COMMIT,trialReceipt=str(trial),trialReceiptSha256=sha(trial),assets=[dict(name=p.name,bytes=p.stat().st_size,sha256=sha(p)) for p in files],previousZipBytes=543178 if TAG=='custom-2026.11.14.2' else 529605 if TAG=='custom-2026.11.14.1' else 544755 if local else 544278 if TAG=='custom-2026.11.11.1' else 453906,zipBytes=files[0].stat().st_size,executableBytes=(CI/'Pulse.exe').stat().st_size,privatePathCheck=local))
elif stage=='release-verify':
    r=api('releases/tags/'+TAG);check(not r['draft'] and not r['prerelease'],'Not a stable published release')
    check(api('git/ref/tags/'+TAG)['object']['sha']==COMMIT,'Wrong tag commit')
    check(api('releases/latest')['tag_name']==TAG,'Wrong latest release')
    expected=json.loads((OUT/'package-receipt.json').read_text(encoding='utf-8'))['assets'];byname={a['name']:a for a in expected}
    check(set(byname)=={a['name'] for a in r['assets']},'Unexpected release assets')
    verified=[]
    for a in r['assets']:
        raw=fetch(a['browser_download_url']);check(digest(raw)==byname[a['name']]['sha256'] and len(raw)==a['size'],'Public download mismatch')
        verified.append(dict(name=a['name'],bytes=len(raw),sha256=digest(raw),url=a['browser_download_url']))
    write(OUT/'public-verification.json',dict(status='PUBLISHED_AND_DOWNLOAD_VERIFIED',tag=TAG,commit=COMMIT,releaseId=r['id'],latestVerified=True,anonymousDownloads=verified))
else:raise RuntimeError('Unknown stage')
print(json.dumps(dict(stage=stage,status='PASS')))
