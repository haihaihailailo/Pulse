'use strict';
// Conservative retirement review. No deletion primitive and no age-based adoption.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const hash=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
const check=(value,message)=>{if(!value)throw Error('Artifact review: '+message);};
const relative=(root,file)=>path.relative(root,file).split(path.sep).join('/');
function snapshot(root,storage,{leased=false}={}) {
  const home=path.join(root,'.local-artifact-storage');
  check(!fs.existsSync(path.join(home,'state.pending.json')),'pending ledger; reconcile first');
  check(leased||!fs.existsSync(path.join(home,'lock.json')),'active/unknown lock; retry after completion');
  function read(file,maximum) {
    storage.safe(root,file);const stat=fs.lstatSync(file);
    check(stat.isFile()&&stat.nlink===1&&stat.size<=maximum,'unsafe or oversized metadata');
    return fs.readFileSync(file);
  }
  const raw=read(path.join(home,'state.json'),8*1024*1024);
  const policyRaw=read(path.join(root,'local-artifact-policy.json'),65536);
  const state=JSON.parse(raw),policy=JSON.parse(policyRaw);
  check(state.schema_version===1&&state.objects&&Object.keys(state.objects).length<=8192,'unsupported ledger');
  check(state.last?.status===(leased?'active':'complete'),'unfinished ledger; reconcile first');
  return {state,policy,ledgerSha256:hash(raw),policySha256:hash(policyRaw)};
}
function capacity(root,snap) {
  return Object.entries(snap.state.managedDirectories||{}).map(([file,record])=>{
    const name=relative(root,file),rule=snap.policy.managed_directories?.[name];
    check(rule&&rule.maximum_bytes===record.maximumBytes,'registered budget drift');
    const objects=Object.values(snap.state.objects).filter(x=>x.managedDirectory===file);
    const bytes=objects.reduce((sum,x)=>sum+x.identity.bytes,0);
    return {path:name,bytes,files:objects.length,maximumBytes:record.maximumBytes,
      availableBytes:record.maximumBytes-bytes,percentUsed:Math.round(bytes/record.maximumBytes*1000)/10,
      maximumEntries:record.maximumEntries||512,
      // Guard entry counts also include directories; do not mislabel files as entries.
      entryHeadroomVerified:false,warning:bytes>=record.maximumBytes*0.85};
  });
}
function status(root,storage) {
  const snap=snapshot(root,storage);
  return {status:'RECORDED_CAPACITY',liveContentVerified:false,automaticCleanup:snap.policy.automatic_cleanup===true,
    directories:capacity(root,snap),nextAction:'Artifact-Review',
    note:'Ledger totals only; review checks content and references. No files removed.'};
}
function review(root,storage,snap) {
  const keepBuilds=snap.policy.retention?.keep_recent_builds_per_kind??2;
  check(keepBuilds===2,'unsupported candidate/recovery retention floor');
  const rows=Object.values(snap.state.objects),byPath=new Map(rows.map(x=>[x.path.toLowerCase(),x]));
  const metadata=new Map(),protectedGroups=new Map(),queue=[],visited=new Set();
  function metadataAt(file) {
    file=storage.safe(root,path.resolve(root,file));
    if(metadata.has(file))return metadata.get(file);
    const record=byPath.get(file.toLowerCase());
    check(record,'unregistered reference: '+relative(root,file));
    check(record.identity.bytes<=2*1024*1024,'oversized metadata');
    const actual=storage.fileIdentity(root,file);
    check(JSON.stringify(actual)===JSON.stringify(record.identity),'metadata identity drift: '+relative(root,file));
    const result=JSON.parse(fs.readFileSync(file,'utf8'));metadata.set(file,result);return result;
  }
  function group(file) {
    const match=/^dev-work\/(builds|vm-sessions)\/([^/]+)(?:\/|$)/.exec(relative(root,file));
    return match?'dev-work/'+match[1]+'/'+match[2]:null;
  }
  function pin(file,reason) {
    const key=group(file);
    if(key) {if(!protectedGroups.has(key))protectedGroups.set(key,new Set());protectedGroups.get(key).add(reason);}
  }
  function reference(file,reason) {
    file=storage.safe(root,path.resolve(root,file));
    check(byPath.has(file.toLowerCase()),'unregistered reference: '+relative(root,file));
    pin(file,reason);
    if(file.endsWith('-result.json')) {
      const plan=file.slice(0,-12)+'-plan.json';
      check(byPath.has(plan.toLowerCase()),'missing paired plan');queue.push({file:plan,reason});
    }
    if(file.endsWith('.json')&&!visited.has(file))queue.push({file,reason});
  }
  function walk(value,reason) {
    if(Array.isArray(value)){value.forEach(x=>walk(x,reason));return;}
    if(!value||typeof value!=='object')return;
    // Only explicit reference fields, not command output, source inputs or configs.
    for(const key of ['path','trialReceipt','receipt','originalReceipt']) {
      const file=value[key];
      if(typeof file!=='string')continue;
      const full=path.resolve(root,file);
      if(relative(root,full).startsWith('dev-work/'))reference(full,reason);
    }
    if(typeof value.buildKey==='string'&&/^(?:release-)?[a-f0-9]{24}$/.test(value.buildKey)) {
      pin(path.join(root,'dev-work/builds',value.buildKey),reason);
    }
    if(typeof value.sessionDir==='string')pin(storage.safe(root,value.sessionDir),reason);
    for(const key of ['artifact','build','outputs','files','reusedEvidence','continuation','trialReceipt','rollback','assets','verification','packageReceipt'])
      if(value[key]&&typeof value[key]==='object')walk(value[key],reason);
  }
  const builds=rows.filter(x=>/^dev-work\/builds\/[^/]+\/build\.json$/.test(relative(root,x.path)))
    .map(row=>({row,value:metadataAt(row.path)}));
  for(const kind of ['development','release']) {
    // Keep two newest complete sets as a conservative floor, then add all pinned dependencies.
    builds.filter(x=>x.value.status==='PASS'&&x.value.kind===kind)
      .sort((a,b)=>b.row.identity.mtimeMs-a.row.identity.mtimeMs||a.row.path.localeCompare(b.row.path))
      .slice(0,keepBuilds).forEach(x=>{pin(x.row.path,'recent '+kind+' candidate/recovery floor');walk(x.value,'candidate output');});
  }
  const sessions=new Map(),latestAccepted=new Map(),latestFailure=new Map();
  for(const row of rows.filter(x=>/^dev-work\/evidence\/[^/]+-result\.json$/.test(relative(root,x.path)))) {
    const receipt=metadataAt(row.path),planFile=row.path.slice(0,-12)+'-plan.json';
    if(!byPath.has(planFile.toLowerCase()))continue;
    const plan=metadataAt(planFile);
    // Build plans reserve a sessionDir string but never create that session.
    // Their lack of desktop cleanup is not a failed GUI acceptance dependency.
    if(['build','release'].includes(plan.action))continue;
    if(!/^[a-f0-9-]{36}$/.test(plan.id||'')||!plan.sessionDir)continue;
    const key='dev-work/vm-sessions/'+plan.id;
    check(relative(root,plan.sessionDir)===key,'session/plan mismatch');
    const item={receipt,plan,row};sessions.set(key,item);
    const family=['test','test-continue'].includes(plan.action)?'test':plan.action;
    const clean=receipt.productionUnchanged===true&&receipt.desktopClosed===true&&receipt.slots?.released===true&&
      Array.isArray(receipt.remainingDevelopmentProcesses)&&!receipt.remainingDevelopmentProcesses.length;
    const eligible=(receipt.status==='PASS'?['test','test-continue','trial-test']:
      ['test','test-continue','debug','run','trial','trial-test']).includes(plan.action);
    const latest=receipt.status==='PASS'?latestAccepted:latestFailure;
    if(eligible&&(!latest.has(family)||latest.get(family).row.identity.mtimeMs<row.identity.mtimeMs))latest.set(family,item);
    if(!clean || (receipt.status!=='PASS' && snap.policy.retention?.keep_latest_failure_per_action!==true)) {
      pin(plan.sessionDir,'failed/incomplete or cleanup not proven');walk(plan,'failed/incomplete dependency');
    }
  }
  for(const item of [...latestAccepted.values(),...latestFailure.values()]) {
    pin(item.plan.sessionDir,item.receipt.status==='PASS'?'latest accepted action':'latest failure checkpoint');
    queue.push({file:item.row.path,reason:'latest acceptance/checkpoint dependency'});
  }
  // Published, rollback and prepared packages retain their complete dependency chains.
  for(const row of rows.filter(x=>/^dev-work\/releases\/[^/]+\/(publication-plan|package-receipt)\.json$/.test(relative(root,x.path))))
    queue.push({file:row.path,reason:'release/package/rollback dependency'});
  let changed=true;
  while(changed||queue.length) {
    changed=false;
    for(const [key,item] of sessions) {
      if(protectedGroups.has('dev-work/builds/'+item.plan.buildKey)&&!protectedGroups.has(key)) {
        pin(item.plan.sessionDir,'retained build acceptance');queue.push({file:item.row.path,reason:'retained acceptance dependency'});changed=true;
      }
    }
    while(queue.length) {
      const {file,reason}=queue.shift();if(visited.has(file))continue;
      check(visited.size<2048,'reference graph bound');visited.add(file);walk(metadataAt(file),reason);
    }
  }
  const candidates=[],totals={protectedBytes:0,reviewBytes:0,criticalOrUnknownBytes:0};
  for(const row of rows) {
    const name=relative(root,row.path),key=group(row.path);let reason=null;
    if(key&&protectedGroups.has(key)){totals.protectedBytes+=row.identity.bytes;continue;}
    if(key?.startsWith('dev-work/builds/')&&/\.(exe|o)$/.test(name)) {
      const accepted=builds.find(x=>group(x.row.path)===key);
      if(accepted?.value.status==='PASS')reason='older complete build payload; keep manifest/recipe/receipts';
    }
    if(key?.startsWith('dev-work/vm-sessions/')&&sessions.has(key)&&/\.(bmp|png|exe|o)$/.test(name))
      reason='superseded session payload with proven cleanup; keep configuration/markers/receipts';
    if(reason&&row.managedDirectory) {
      candidates.push({path:name,owner:row.owner,managedDirectory:relative(root,row.managedDirectory),
        identity:row.identity,reason});totals.reviewBytes+=row.identity.bytes;
    } else totals.criticalOrUnknownBytes+=row.identity.bytes;
  }
  candidates.sort((a,b)=>a.path.localeCompare(b.path));
  return {schemaVersion:1,status:'REVIEW_ONLY',deletionAuthorized:false,deletedFiles:0,
    policySha256:snap.policySha256,ledgerSnapshotSha256:snap.ledgerSha256,
    directories:capacity(root,snap),totals,candidates,
    protectedGroups:[...protectedGroups].map(([path,reasons])=>({path,reasons:[...reasons]})),
    rules:{keepLatestFailure:true,keepIncomplete:true,keepCriticalEvidence:true,keepUnknown:true,keepAllReleaseReferences:true,
      historicalPreserveInstruction:snap.policy.automatic_cleanup!==true,automaticCleanup:snap.policy.automatic_cleanup===true},
    note:'Candidates are review proposals, not deletion authorization. Recheck identities, references, occupancy and ledger under lock before any retirement.'};
}
async function run({root,storage,action,quiescent}) {
  if(action==='artifact-status')return status(root,storage);
  check(action==='artifact-review','unknown action');
  const before=snapshot(root,storage);quiescent();
  // Validate the reference graph before starting a generation transaction. A
  // missing historical reference must not leave an otherwise sound ledger failed.
  const report=review(root,storage,before);
  const target=path.join(root,'dev-work/evidence',crypto.randomUUID()+'-artifact-review.json');
  await storage.withLease({root,operation:'development',owner:'pulse-artifact-review',
    purpose:'Exact retirement proposal; preserve all existing materials',verifyAllContent:true,
    targets:[{path:target,purpose:'Capacity, protected dependencies and exact review candidates',estimatedBytes:1048576}]},async()=>{
    const snap=snapshot(root,storage,{leased:true});
    check(snap.policySha256===before.policySha256&&JSON.stringify(snap.state.objects)===JSON.stringify(before.state.objects)&&
      JSON.stringify(snap.state.managedDirectories)===JSON.stringify(before.state.managedDirectories),'review snapshot changed; preserve and reconcile');
    quiescent();
    fs.writeFileSync(target,JSON.stringify(report,null,2)+'\n',{flag:'wx'});
  });
  return {status:report.status,report:target,sha256:storage.fileIdentity(root,target).sha256,
    candidates:report.candidates.length,reviewBytes:report.totals.reviewBytes,
    deletedFiles:0,deletionAuthorized:false,directories:report.directories};
}
module.exports={snapshot,capacity,status,review,run};
