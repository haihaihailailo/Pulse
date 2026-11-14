'use strict';
// Run only through Pulse-Dev.ps1 -Action Workflow-Test (Job/slots/capacity).
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const flow=require('./pulse_release_flow.cjs'),release=require('./publish_upstream_20261110.cjs');
const build=require('./pulse_build_identity.cjs');
const project=path.resolve(__dirname,'..'),target=path.resolve(process.argv[2]||'.');
assert.equal(path.dirname(target),path.join(project,'dev-work','vm-sessions'));
assert.match(path.basename(target),/^[a-f0-9-]{36}$/);
const root=path.join(target,'workflow-fixtures');fs.mkdirSync(root);
const checks=[];
function test(name,fn){fn();checks.push(name);}
const put=(name,value)=>{const file=flow.local(root,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,value);return file;};
const putJson=(name,value)=>put(name,flow.encode(value));
function fixture(tag,commit,published=false) {
  const dir=flow.directory(tag),exe=Buffer.from('synthetic accepted executable '+tag);
  const artifact={name:'Pulse.exe',bytes:exe.length,sha256:flow.hash(exe)};
  const manifest={repository:flow.REPO,tag,commit,fileVersion:'1.3.0.5',executable:artifact,binaryUnmodified:true};
  putJson(path.join(dir,'release-manifest.json'),manifest);
  put(path.join(dir,'Pulse-'+tag+'-win-x64.zip'),exe);
  put(path.join(dir,'release-notes.md'),'合成发布说明：保留配置。\n');
  const names=['Pulse-'+tag+'-win-x64.zip','release-manifest.json'];
  put(path.join(dir,'SHA256SUMS.txt'),names.map(name=>flow.identity(root,path.join(dir,name)).sha256+'  '+name+'\n').join(''));
  names.push('SHA256SUMS.txt');
  const trial=putJson('dev-work/evidence/trial-'+tag+'.json',{status:'PASS',action:'trial-test',artifact,
    productionUnchanged:true,desktopClosed:true,slots:{released:true},remainingDevelopmentProcesses:[]});
  const assets=names.map(name=>({name,bytes:fs.statSync(flow.local(root,path.join(dir,name))).size,sha256:flow.identity(root,path.join(dir,name)).sha256}));
  putJson(path.join(dir,'package-receipt.json'),{status:'PASS',commit,trialReceipt:trial,trialReceiptSha256:flow.identity(root,trial).sha256,assets});
  if(published)putJson(path.join(dir,'public-verification.json'),{status:'PUBLISHED_AND_DOWNLOAD_VERIFIED',tag,commit,
    latestVerified:true,releaseId:10,anonymousDownloads:assets});
}
const tag='custom-2026.11.14.2', previous='custom-2026.11.14.1',commit='6bd690998893ee21e8df5fca2afcfa381cc499ab';
fixture(previous,'b'.repeat(40),true);fixture(tag,commit);
let plan;
test('derive all assets, rollback and immutable preparation',()=>{
  assert.equal(flow.previousTag(root,tag),previous);
  plan=flow.prepare(root,tag,previous);
  assert.equal(plan.assets.length,3);assert.equal(plan.rollback.commit,'b'.repeat(40));
  const first=flow.savePrepared(root,plan),again=flow.savePrepared(root,plan);
  assert.equal(first.reused,false);assert.equal(again.reused,true);assert.equal(first.sha256,again.sha256);
  assert.throws(()=>flow.savePrepared(root,{...plan,title:'different'}),/changed/);
});
test('status does not hash artifacts or claim live verification',()=>{
  const file=flow.local(root,plan.assets[0].path),raw=fs.readFileSync(file);fs.writeFileSync(file,'drift');
  try {
    assert.equal(flow.status(root,tag).liveInputsVerified,false);
    assert.throws(()=>flow.prepare(root,tag,previous),/Asset drift/);
  } finally {fs.writeFileSync(file,raw);}
});
test('unsafe paths, missing assets, checksum and trial failures are rejected',()=>{
  assert.throws(()=>flow.directory('../escape'),/Invalid/);
  assert.throws(()=>flow.local(root,'../escape'),/outside/);
  const file=flow.local(root,path.join(flow.directory(tag),'package-receipt.json')),raw=fs.readFileSync(file);
  try {
    const value=JSON.parse(raw);value.assets[0].name='../escape';fs.writeFileSync(file,flow.encode(value));
    assert.throws(()=>flow.prepare(root,tag,previous),/Unsafe/);
    value.assets[0].name=plan.assets[0].name;value.assets.pop();fs.writeFileSync(file,flow.encode(value));
    assert.throws(()=>flow.prepare(root,tag,previous),/assets/);
  } finally {fs.writeFileSync(file,raw);}
  const sums=flow.local(root,plan.assets[2].path),sumsRaw=fs.readFileSync(sums);
  try {fs.writeFileSync(sums,'0'.repeat(64)+'  missing.zip\n');assert.throws(()=>flow.prepare(root,tag,previous),/Asset drift/);}
  finally {fs.writeFileSync(sums,sumsRaw);}
  const trial=flow.local(root,plan.trialReceipt.path),trialRaw=fs.readFileSync(trial);
  try {const value=JSON.parse(trialRaw);value.slots.released=false;fs.writeFileSync(trial,flow.encode(value));assert.throws(()=>flow.prepare(root,tag,previous),/Trial/);}
  finally {fs.writeFileSync(trial,trialRaw);}
});
test('authorization must bind repository, tag, commit and preparation hash',()=>{
  const digest=flow.hash(flow.encode(plan)),authorization={repository:flow.REPO,tag,commit,publish:true};
  flow.authorize(plan,digest,digest,authorization);
  assert.throws(()=>flow.authorize(plan,'0'.repeat(64),digest,authorization),/authorization/);
  assert.throws(()=>flow.authorize(plan,digest,digest,{...authorization,commit:'c'.repeat(40)}),/authorization/);
  assert.throws(()=>flow.authorize(plan,digest,digest,{...authorization,publish:false}),/authorization/);
});
function service(options={}) {
  const state={release:null,mutations:[],lost:new Set(),fail:new Set(options.fail||[]),lose:new Set(options.lose||[]),events:[]};
  const copy=x=>JSON.parse(JSON.stringify(x));
  const mutate=(key,fn)=>{
    assert.ok(state.events.some(event=>event.kind==='intent'&&event.key===key),'intent precedes mutation');
    state.mutations.push(key);
    if(state.fail.has(key))throw Error('unknown network result');
    const value=fn();
    if(state.lose.has(key)&&!state.lost.has(key)){state.lost.add(key);throw Error('lost response after success');}
    return copy(value);
  };
  const transport={notes:()=>flow.read(root,plan.notes.path).toString('utf8'),
    releases:()=>state.release?[copy(state.release)]:[],
    tag:()=>state.release&&!state.release.draft?{object:{type:'commit',sha:commit}}:null,
    upload:(_,asset)=>mutate('upload:'+asset.name,()=>{state.release.assets.push({name:asset.name,size:asset.bytes,digest:'sha256:'+asset.sha256,state:'uploaded'});return {}; }),
    api:(endpoint,payload,method)=>{
      if(endpoint==='user')return {login:'test-account'};
      if(endpoint==='repos/'+flow.REPO)return {full_name:flow.REPO,permissions:{push:!options.deny}};
      if(method==='POST')return mutate('create',()=>state.release={...payload,id:11,assets:[]});
      if(method==='PATCH')return mutate('publish',()=>{state.release.draft=false;return state.release;});
      assert.ok(state.release,'release exists');
      assert.ok(endpoint.endsWith('/11')||endpoint.endsWith('/latest'),'draft queried by ID');
      return copy(state.release);
    }};
  const attempt=history=>{const events=[];state.events=events;let result,error;try{result=flow.publish(plan,transport,history,e=>events.push(e));}catch(e){error=e;}return {events,result,error};};
  return {state,transport,attempt};
}
test('complete publication and repeated invocation do not repeat mutations',()=>{
  const mock=service(),first=mock.attempt([]);assert.equal(first.result.status,'PUBLISHED_METADATA_VERIFIED');
  const count=mock.state.mutations.length;assert.equal(count,5);
  const again=mock.attempt([first]);assert.equal(again.result.status,'PUBLISHED_METADATA_VERIFIED');assert.equal(mock.state.mutations.length,count);
});
test('lost create, upload and publish responses reconcile without duplicate writes',()=>{
  for(const key of ['create','upload:'+plan.assets[0].name,'publish']) {
    const mock=service({lose:[key]}),first=mock.attempt([]);assert.match(first.error.message,/lost response/);
    const resumed=mock.attempt([first]);assert.equal(resumed.result.status,'PUBLISHED_METADATA_VERIFIED');
    assert.equal(mock.state.mutations.filter(value=>value===key).length,1);
  }
});
test('unknown operation with absent result refuses blind retry',()=>{
  for(const key of ['create','upload:'+plan.assets[0].name,'publish']) {
    const mock=service({fail:[key]}),first=mock.attempt([]);assert.ok(first.error);
    const count=mock.state.mutations.length;mock.state.fail.clear();
    const resumed=mock.attempt([first]);assert.match(resumed.error.message,/Unresolved/);assert.equal(mock.state.mutations.length,count);
  }
});
test('remote drift and missing permissions refuse publication',()=>{
  const denied=service({deny:true});assert.match(denied.attempt([]).error.message,/permission/);assert.equal(denied.state.mutations.length,0);
  const mock=service();mock.attempt([]);mock.state.release.assets[0].digest='sha256:'+'0'.repeat(64);
  const count=mock.state.mutations.length;assert.match(mock.attempt([]).error.message,/Remote asset differs/);assert.equal(mock.state.mutations.length,count);
});
test('tag conflict is rejected before draft creation and before publication',()=>{
  const before=service();before.transport.tag=()=>({object:{type:'commit',sha:'f'.repeat(40)}});
  assert.match(before.attempt([]).error.message,/Tag conflict/);assert.equal(before.state.mutations.length,0);
  const during=service();let reads=0;
  during.transport.tag=()=>++reads===1?null:{object:{type:'commit',sha:'f'.repeat(40)}};
  assert.match(during.attempt([]).error.message,/Tag conflict/);
  assert.ok(!during.state.mutations.includes('publish'));assert.equal(during.state.release.draft,true);
});
test('gh adapter preserves argument boundaries and UTF-8 JSON without clobber',()=>{
  const calls=[],transport=release.ghTransport(root,plan,(exe,args,options)=>{
    calls.push({exe,args,options});return {status:0,stdout:'{"id":1}',stderr:''};
  });
  transport.api('repos/'+flow.REPO+'/releases',{body:'中文 "$()" ` text'},'POST');
  assert.deepEqual(JSON.parse(calls[0].options.input),{body:'中文 "$()" ` text'});
  assert.deepEqual(calls[0].args.slice(-4),['--method','POST','--input','-']);
  transport.upload(tag,plan.assets[0]);
  assert.equal(calls[1].args[3],flow.local(root,plan.assets[0].path));assert.ok(!calls[1].args.includes('--clobber'));
  const file=flow.local(root,plan.assets[0].path),raw=fs.readFileSync(file);
  try {fs.writeFileSync(file,'late drift');assert.throws(()=>transport.upload(tag,plan.assets[0]),/drift/);assert.equal(calls.length,2);}
  finally {fs.writeFileSync(file,raw);}
});
test('build identity helper accepts tool-only dispatch and rejects product/recipe drift',()=>{
  const worker=path.join(project,'tools/pulse_dev_worker.py'),raw=fs.readFileSync(worker);
  // Synthetic build evidence: future product edits or retirement of a historical
  // build must not break a tool-only check or force a product rebuild.
  const baseline=[{path:path.join(project,'src/main.cpp'),sha256:flow.hash('synthetic source')},
    {path:worker,sha256:flow.hash(raw.toString('utf8').replace(build.toolDispatch,''))},
    {path:path.join(project,'tools/dev-environment.local.json'),sha256:flow.hash('synthetic toolchain binding')}];
  const rows=baseline.map(item=>item.path===worker?{...item,sha256:flow.hash(raw)}:{...item});
  assert.equal(build.sameBinaries(rows,baseline,worker,raw),true);
  const changed=structuredClone(rows);changed[0].sha256='0'.repeat(64);
  assert.equal(build.sameBinaries(changed,baseline,worker,raw),false);
  const recipe=Buffer.from(raw.toString('utf8').replace("'-O2' if release","'-O3' if release"));
  const recipeRows=rows.map(item=>item.path===worker?{...item,sha256:flow.hash(recipe)}:item);
  assert.equal(build.sameBinaries(recipeRows,baseline,worker,recipe),false);
  const outside=Buffer.from(raw.toString('utf8')+'\n# unreviewed harness change\n');
  assert.equal(build.sameBinaries(rows.map(item=>item.path===worker?{...item,sha256:flow.hash(outside)}:item),baseline,worker,outside),false);
  assert.equal(build.sameBinaries(rows.slice(1),baseline,worker,raw),false);
});
test('versioned storage preserves default and byte guards; only explicit entry bound changes',()=>{
  const config=JSON.parse(fs.readFileSync(path.join(project,'tools/dev-environment.local.json')));
  const guard=require(path.join(path.dirname(config.storage),'local_artifact_storage_v2.cjs'));
  assert.equal(guard.entryLimit(),512);assert.equal(guard.entryLimit(1024),1024);
  assert.throws(()=>guard.entryLimit(2048),/explicit/);
  assert.throws(()=>guard.checkManagedBound(513,1,100),/bound exceeded/);
  guard.checkManagedBound(1024,100,100,1024);
  assert.throws(()=>guard.checkManagedBound(1025,1,100,1024),/bound exceeded/);
  assert.throws(()=>guard.checkManagedBound(1,101,100,1024),/bound exceeded/);
});
(async()=>{
  const configNative=JSON.parse(fs.readFileSync(path.join(project,'tools/dev-environment.local.json')));
  const native=require('node:child_process').spawnSync(configNative.python,['-B',path.join(project,'tools/pulse_artifact_retirement_selftest.py'),
    path.join(root,'native-retirement')],{windowsHide:true,encoding:'utf8',timeout:15000,maxBuffer:65536});
  assert.equal(native.status,0,native.stderr);const nativeResult=JSON.parse(native.stdout);
  assert.equal(nativeResult.status,'PASS');checks.push(...nativeResult.checks);
  {
    const guard=require('./shared/pulse_artifact_storage_v3.cjs');
    const fixture=async name=>{
      const base=path.join(root,'ledger-'+name),managed=path.join(base,'dev-work/builds');fs.mkdirSync(base);
      fs.writeFileSync(path.join(base,'local-artifact-policy.json'),JSON.stringify({schema_version:1,automatic_cleanup:true,
        retention:{schema_version:1},project_budget_bytes:1073741824,minimum_free_bytes:0,control_reserve_bytes:4096,
        allowed_output_roots:['dev-work'],reservations:{development:4096},managed_directories:{'dev-work/builds':{owner:'fixture-builds',maximum_bytes:1048576}}}));
      const x=path.join(managed,'x.exe'),y=path.join(managed,'y.exe'),proof=path.join(base,'dev-work/initial.json');
      const quiet=()=>({quiescent:true,resourceId:managed});
      await guard.withLease({root:base,operation:'development',owner:'fixture-builds',purpose:'synthetic generated payload',
        targets:[{path:proof,purpose:'initial fixture proof',estimatedBytes:1024}],managedDirectories:[{path:managed}],
        assertManagedQuiescent:quiet},async()=>{fs.mkdirSync(managed,{recursive:true});fs.writeFileSync(x,'old');fs.writeFileSync(y,'keep');fs.writeFileSync(proof,'{}');});
      const stateFile=path.join(base,'.local-artifact-storage/state.json');
      const state=()=>JSON.parse(fs.readFileSync(stateFile));
      const exact=[x].map(file=>({path:file,owner:state().objects[file].owner,identity:state().objects[file].identity,
        nativeIdentity:guard.nativeIdentity(base,state().objects[file].identity)}));
      const result=path.join(base,'dev-work/receipt.json');
      const options={root:base,operation:'development',owner:'pulse-artifact-retirement',purpose:'synthetic exact retirement',
        retirements:exact,retirementAuthorization:'synthetic fixture only',allowManagedRetirements:true,
        validateRetirements:()=>{},assertManagedQuiescent:quiet,targets:[{path:result,purpose:'synthetic final receipt',estimatedBytes:1024}]};
      return {base,x,y,result,state,stateFile,options};
    };
    let f=await fixture('success');
    const intent=path.join(f.base,'dev-work/plan.json'),nativeDir=path.join(f.base,'dev-work/native');
    const journal=path.join(nativeDir,'deletions.jsonl');
    f.options.targets.push({path:intent,purpose:'native exact plan',estimatedBytes:4096},{path:journal,purpose:'native journal',estimatedBytes:4096});
    await guard.withLease(f.options,async()=>{
      fs.mkdirSync(nativeDir);const plan={root:f.base,action:'artifact-retire',retirements:f.options.retirements,targetDirectory:nativeDir,retirementJournal:journal};
      const raw=flow.encode(plan);fs.writeFileSync(intent,raw);
      const result=require('node:child_process').spawnSync(configNative.python,['-B',path.join(project,'tools/pulse_artifact_delete.py'),intent,flow.hash(raw)],
        {windowsHide:true,encoding:'utf8',timeout:10000,maxBuffer:65536});
      assert.equal(result.status,0,result.stderr);fs.writeFileSync(f.result,result.stdout);return JSON.parse(result.stdout);
    });
    assert.equal(fs.existsSync(f.x),false);assert.equal(fs.readFileSync(f.y,'utf8'),'keep');
    assert.equal(f.state().last.status,'complete');assert.ok(f.state().retiredObjects[f.x]);assert.equal(f.state().objects[f.x],undefined);
    assert.equal(build.retiredBuild({outputs:[{path:f.x,sha256:f.options.retirements[0].identity.sha256}]},f.state().retiredObjects),true);
    checks.push('native deletion and managed ledger finalize together; tombstone prevents retired build reuse');
    f=await fixture('abort');
    await guard.withLease(f.options,async()=>{fs.writeFileSync(f.result,'{"status":"REFUSED"}');return {retirementAborted:true};});
    assert.equal(f.state().last.status,'complete');assert.equal(f.state().last.retirementAborted,true);assert.ok(fs.existsSync(f.x));
    checks.push('zero-delete refusal verifies original identities and leaves a complete ledger');
    f=await fixture('references');const before=fs.readFileSync(f.stateFile);
    f.options.validateRetirements=()=>{throw Error('protected reference');};
    await assert.rejects(guard.withLease(f.options,async()=>{throw Error('must not execute');}),/protected reference/);
    assert.deepEqual(fs.readFileSync(f.stateFile),before);assert.equal(fs.existsSync(path.join(f.base,'.local-artifact-storage/lock.json')),false);
    checks.push('protected reference refused under lock before mutation; own lock released');
    f=await fixture('unknown');fs.writeFileSync(path.join(f.base,'dev-work/builds/unknown.exe'),'unknown');
    await assert.rejects(guard.withLease(f.options,async()=>{}),/unknown\/missing managed/);
    assert.equal(f.state().last.status,'complete');assert.ok(fs.existsSync(f.x));
    checks.push('unknown managed members block retirement without adoption');
    f=await fixture('interrupted');
    await assert.rejects(guard.withLease(f.options,async()=>{fs.unlinkSync(f.x);throw Error('synthetic interruption');}),/synthetic interruption/);
    assert.equal(f.state().last.status,'failed');assert.ok(fs.existsSync(f.y));
    await assert.rejects(guard.withLease(f.options,async()=>{}),/unfinished generation/);
    checks.push('partial deletion leaves failed ledger and blocks automatic replay');
  }
  {
  const artifacts=require('./pulse_artifact_review.cjs');
  const configGuard=JSON.parse(fs.readFileSync(path.join(project,'tools/dev-environment.local.json')));
  const guard=require(path.join(path.dirname(configGuard.storage),'local_artifact_storage_v2.cjs'));
  const reviewRoot=path.join(root,'retention');fs.mkdirSync(reviewRoot);
  const state={schema_version:1,objects:{},managedDirectories:{},last:{status:'complete'}};
  const policy={managed_directories:{'dev-work/builds':{maximum_bytes:1048576},'dev-work/vm-sessions':{maximum_bytes:1048576}}};
  const add=(name,value)=>{
    const file=path.join(reviewRoot,name);fs.mkdirSync(path.dirname(file),{recursive:true});
    fs.writeFileSync(file,typeof value==='string'?value:JSON.stringify(value));
    const managed=path.join(reviewRoot,'dev-work',name.includes('/builds/')?'builds':'vm-sessions');
    state.objects[file]={path:file,owner:'synthetic',identity:guard.fileIdentity(reviewRoot,file),
      ...(name.includes('/builds/')||name.includes('/vm-sessions/')?{managedDirectory:managed}:{})};
    return file;
  };
  const snap={state,policy,ledgerSha256:'1'.repeat(64),policySha256:'2'.repeat(64)};
  for(const kind of ['builds','vm-sessions']) {
    const file=path.join(reviewRoot,'dev-work',kind);
    state.managedDirectories[file]={maximumBytes:1048576,maximumEntries:1024};
  }
  const keys=['1','2','3'].map(x=>x.repeat(24));
  for(const [i,key] of keys.entries()) {
    const exe=add('dev-work/builds/'+key+'/Pulse.exe','synthetic '+key);
    const manifest=add('dev-work/builds/'+key+'/build.json',{status:'PASS',kind:'development',buildKey:key,
      outputs:[{path:exe,sha256:state.objects[exe].identity.sha256}]});
    // Selection uses stable real metadata, with deterministic ordering below.
    fs.utimesSync(manifest,new Date(1000+i*1000),new Date(1000+i*1000));
    state.objects[manifest].identity=guard.fileIdentity(reviewRoot,manifest);
  }
  const id='11111111-1111-1111-1111-111111111111';
  const session=path.join(reviewRoot,'dev-work/vm-sessions',id);
  const capture=add('dev-work/vm-sessions/'+id+'/capture.bmp','pixels');
  add('dev-work/vm-sessions/'+id+'/smoke-first.json',{pass:true});
  const result=add('dev-work/evidence/'+id+'-result.json',{status:'PASS',productionUnchanged:true,
    desktopClosed:true,slots:{released:true},remainingDevelopmentProcesses:[]});
  add('dev-work/evidence/'+id+'-plan.json',{id,sessionDir:session,buildKey:keys[0]});
  const buildId='22222222-2222-2222-2222-222222222222';
  add('dev-work/evidence/'+buildId+'-plan.json',{id:buildId,action:'build',buildKey:keys[0],
    sessionDir:path.join(reviewRoot,'dev-work/vm-sessions',buildId)});
  add('dev-work/evidence/'+buildId+'-result.json',{status:'PASS',action:'build'});
  add('dev-work/builds/unknown/stray.exe','unclassified');
  test('retirement review preserves critical/unknown files; a build-only plan is not a failed GUI session',()=>{
    const report=artifacts.review(reviewRoot,guard,snap);
    assert.equal(report.candidates.length,2);assert.equal(report.deletionAuthorized,false);
    assert.ok(report.candidates.some(x=>x.path.endsWith('capture.bmp')));
    assert.ok(report.candidates.every(x=>!x.path.includes('unknown')&&!x.path.endsWith('.json')));
    assert.ok(fs.existsSync(capture));assert.ok(fs.existsSync(result));
    assert.equal(artifacts.capacity(reviewRoot,snap)[0].entryHeadroomVerified,false);
  });
  const retirement=require('./pulse_artifact_retire.cjs');
  const approvedReview=artifacts.review(reviewRoot,guard,snap);
  const approvedFile=add('dev-work/evidence/aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa-artifact-review.json',approvedReview);
  const authorization={reviewPath:path.relative(reviewRoot,approvedFile),reviewSha256:state.objects[approvedFile].identity.sha256,
    expectedFiles:approvedReview.candidates.length,expectedBytes:approvedReview.totals.reviewBytes,authorization:'synthetic exact approval'};
  test('retirement request binds review hash, exact count/bytes and current policy authorization',()=>{
    assert.equal(retirement.choose(reviewRoot,guard,snap,authorization,false).candidates.length,2);
    assert.throws(()=>retirement.choose(reviewRoot,guard,snap,{...authorization,reviewSha256:'0'.repeat(64)},false),/identity differs/);
    assert.throws(()=>retirement.choose(reviewRoot,guard,snap,{...authorization,expectedFiles:1},false),/count\/bytes/);
    assert.throws(()=>retirement.choose(reviewRoot,guard,snap,null,true),/not authorized/);
    const enabled={...snap,policy:{...policy,automatic_cleanup:true,retention:{authorization:'synthetic standing approval'}}};
    assert.equal(retirement.choose(reviewRoot,guard,enabled,null,true).candidates.length,2);
  });
  const pub=add('dev-work/releases/synthetic/package-receipt.json',{trialReceipt:result});
  test('release reference closure retains session, paired plan and exact older build',()=>{
    assert.equal(artifacts.review(reviewRoot,guard,snap).candidates.length,0);
    assert.throws(()=>retirement.choose(reviewRoot,guard,snap,authorization,false),/became protected/);
  });
  test('metadata drift refuses review instead of suggesting deletion',()=>{
    const bytes=fs.readFileSync(pub);fs.writeFileSync(pub,'{}');
    assert.throws(()=>artifacts.review(reviewRoot,guard,snap),/identity drift/);
    fs.writeFileSync(pub,bytes);state.objects[pub].identity=guard.fileIdentity(reviewRoot,pub);
  });
  test('failed outer cleanup protects session and source build',()=>{
    delete state.objects[pub]; // Synthetic ledger scenario; the fixture file remains retained.
    const receipt=JSON.parse(fs.readFileSync(result));receipt.slots.released=false;
    fs.writeFileSync(result,JSON.stringify(receipt));state.objects[result].identity=guard.fileIdentity(reviewRoot,result);
    assert.equal(artifacts.review(reviewRoot,guard,snap).candidates.length,0);
  });
  test('missing reference and traversal refuse review',()=>{
    state.objects[pub]={path:pub,owner:'synthetic',identity:guard.fileIdentity(reviewRoot,pub)};
    fs.writeFileSync(pub,JSON.stringify({trialReceipt:'dev-work/evidence/missing-result.json'}));
    state.objects[pub].identity=guard.fileIdentity(reviewRoot,pub);
    assert.throws(()=>artifacts.review(reviewRoot,guard,snap),/unregistered reference/);
    assert.throws(()=>guard.safe(reviewRoot,path.join(reviewRoot,'../escape')),/outside/);
  });
  fs.mkdirSync(path.join(reviewRoot,'.local-artifact-storage'));
  fs.writeFileSync(path.join(reviewRoot,'.local-artifact-storage/state.json'),JSON.stringify(state));
  fs.writeFileSync(path.join(reviewRoot,'local-artifact-policy.json'),JSON.stringify(policy));
  let reviewLeases=0;
  await assert.rejects(artifacts.run({root:reviewRoot,storage:{...guard,withLease:async()=>{reviewLeases++;}},
    action:'artifact-review',quiescent:()=>{}}),/unregistered reference/);
  assert.equal(reviewLeases,0);checks.push('missing historical reference refuses before opening a generation transaction');
  test('cheap status is read-only, explicit about ledger evidence, and refuses active locks',()=>{
    const before=fs.readFileSync(path.join(reviewRoot,'.local-artifact-storage/state.json'));
    const current=artifacts.status(reviewRoot,guard);assert.equal(current.liveContentVerified,false);
    assert.deepEqual(fs.readFileSync(path.join(reviewRoot,'.local-artifact-storage/state.json')),before);
    fs.writeFileSync(path.join(reviewRoot,'.local-artifact-storage/lock.json'),'synthetic active lock');
    assert.throws(()=>artifacts.status(reviewRoot,guard),/lock/);
  });
  }
  let leases=0;
  const storage={withLease:async(options,callback)=>{
    leases++;
    if(options.owner==='pulse-release-flow'){assert.ok(options.verifyPaths.length>=12);assert.equal(options.targets.length,1);}
    (options.verifyPaths||[]).forEach(file=>assert.ok(fs.existsSync(file)));
    return callback();
  }};
  const result=await release.preparedStage({root,storage,action:'release-prepare',args:{tag},emit:false});
  assert.equal(result.status,'PREPARED');assert.equal(result.reused,true);assert.equal(leases,1);
  checks.push('actual preparation entry reuses the same registered plan under a lease');
  const config={identities:[]};
  await assert.rejects(release.run({root,storage,config,action:'release-source',preparedArgs:{
    base:'a'.repeat(40),branch:'codex/synthetic',tag,paths:['tools/dev-environment.local.json']},emit:false}),/Private or unsupported/);
  await assert.rejects(release.run({root,storage,config,action:'release-package',preparedArgs:{
    tag,commit:'not-a-commit',artifactExe:'../outside.exe'},emit:false}),/Exact package identity/);
  putJson('dev-work/evidence/release-source-'+tag+'/source-receipt.json',{status:'PASS',tag,commit});
  const artifactExe=put('dev-work/builds/release-'+'a'.repeat(24)+'/Pulse.exe','synthetic accepted executable '+tag);
  const dynamicPackage=await release.run({root,storage,config,action:'release-package',preparedArgs:{tag,commit,artifactExe},emit:false});
  assert.equal(dynamicPackage.status,'PASS');assert.equal(dynamicPackage.reused,true);
  await assert.rejects(release.run({root,storage,config,action:'release-package',preparedArgs:{tag,commit:'c'.repeat(40),artifactExe},emit:false}),/Accepted source/);
  checks.push('scoped source refuses private files; generic package binds accepted source commit and exact local artifact');
  const packaged=await release.run({root,storage,config,action:'release-package',preparedArgs:{commit},emit:false});
  assert.equal(packaged.reused,true);assert.equal(packaged.preparation.reused,true);
  putJson(path.join(flow.directory(tag),'public-verification.json'),{status:'PUBLISHED_AND_DOWNLOAD_VERIFIED',tag,commit,
    latestVerified:true,releaseId:11,anonymousDownloads:plan.assets});
  const verified=await release.run({root,storage,config,action:'release-verify',preparedArgs:{tag},emit:false});
  assert.equal(verified.reused,true);assert.equal(verified.remoteChecked,false);
  assert.equal(flow.status(root,tag).nextAction,'COMPLETE_RECORDED');
  checks.push('package automatically prepares; verify consumes prepared assets and reuses accepted evidence');
  console.log(JSON.stringify({status:'PASS',checks,productLaunches:0,compilerInvocations:0,remoteCalls:0}));
})().catch(error=>{console.error(error.stack);process.exitCode=1;});
