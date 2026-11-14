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
