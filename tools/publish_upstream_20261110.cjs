'use strict';
const fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const flow=require('./pulse_release_flow.cjs');
// Resume only an interrupted blob-upload checkpoint. Existing commits/refs
// remain a separate readback decision; never replay an uncertain publication.
function resumeSource(root,storage,args,dir,execute) {
  const home=path.join(root,'.local-artifact-storage'),file=path.join(home,'state.json'),lock=path.join(home,'lock.json');
  const original=fs.readFileSync(file),state=JSON.parse(original),last=state.last;
  const progress=path.join(dir,'source-progress.jsonl'),receipt=path.join(dir,'source-receipt.json');
  if(last?.status==='complete')return null;
  flow.check(last?.status==='failed'&&last.owner==='pulse-upstream-release-20261110'&&last.targets.length>=2&&
    last.targets.slice(0,2).every((row,i)=>row.path===[receipt,progress][i]&&row.before===null)&&
    !state.objects[receipt]&&!state.objects[progress]&&!fs.existsSync(receipt),'Not an unfinished first source upload');
  const records=flow.read(root,progress,1048576).toString('utf8').trim().split('\n').map(JSON.parse);
  flow.check(records[0]?.operation==='prepared'&&['base','branch','tag'].every(key=>records[0].response[key]===args[key])&&
    records.every(row=>row.operation==='prepared'||row.operation==='git/blobs'||(row.operation==='intent'&&row.response.endpoint==='git/blobs')),
    'Source checkpoint advanced beyond blobs or changed target; inspect before continuation');
  const durable=(name,raw)=>{const fd=fs.openSync(storage.safe(root,name),'wx');try{fs.writeFileSync(fd,raw);fs.fsyncSync(fd);}finally{fs.closeSync(fd);}};
  const id=require('node:crypto').randomUUID(),snapshot=path.join(dir,'state-before-resume-'+id+'.json');
  durable(lock,JSON.stringify({owner:last.owner,id}));const lockIdentity=storage.fileIdentity(root,lock);let active=false;
  const save=()=>{const raw=JSON.stringify(state)+'\n';flow.check(Buffer.byteLength(raw)<=8*1024*1024,'State size bound');
    const pending=path.join(home,'state.pending.json');durable(pending,raw);fs.renameSync(pending,file);};
  try {
    flow.check(fs.readFileSync(file).equals(original),'State changed before source continuation');
    storage.verifyRegistered(root,state,new Set(),true);
    const policy=JSON.parse(fs.readFileSync(path.join(root,'local-artifact-policy.json')));
    for(const record of Object.values(state.managedDirectories)) {
      const found=[],pending=[record.path];while(pending.length)for(const entry of fs.readdirSync(pending.pop(),{withFileTypes:true})) {
        const name=storage.safe(root,path.join(entry.parentPath,entry.name));if(entry.isDirectory())pending.push(name);else found.push(name);
      }
      const known=Object.values(state.objects).filter(row=>row.managedDirectory===record.path).map(row=>row.path);
      flow.check(JSON.stringify(found.sort())===JSON.stringify(known.sort()),'Managed members changed');
      const used=storage.usage(record.path);storage.checkManagedBound(used.files,used.bytes,record.maximumBytes,record.maximumEntries);
      const rule=policy.managed_directories[path.relative(root,record.path).split(path.sep).join('/')];
      flow.check(rule.maximum_bytes===record.maximumBytes&&storage.entryLimit(rule.maximum_entries)===record.maximumEntries,'Capacity policy changed');
    }
    storage.checkCapacity(root,policy,16*1024*1024);
    last.status='active';last.resumedFrom=snapshot;last.targets.push({path:snapshot,before:null,purpose:'Immutable failed source checkpoint'});save();active=true;
    durable(snapshot,original);state.objects[snapshot]={path:snapshot,owner:last.owner,purpose:'Original interrupted source state',identity:storage.fileIdentity(root,snapshot)};save();
    const result=execute();flow.check(result.status==='PASS','Source continuation failed');
    storage.verifyRegistered(root,state,new Set(),true);
    for(const name of [receipt,progress])state.objects[name]={path:name,owner:last.owner,purpose:'Exact resumed source publication',identity:storage.fileIdentity(root,name)};
    last.status='complete';last.completedUtc=new Date().toISOString();delete last.error;save();
    flow.check(JSON.parse(fs.readFileSync(file)).last.status==='complete','Continuation readback failed');return result;
  }catch(error){if(active){last.status='failed';last.error=String(error.message).slice(0,300);save();}throw error;}
  finally{flow.check(JSON.stringify(storage.fileIdentity(root,lock))===JSON.stringify(lockIdentity),'Source continuation lock changed');fs.unlinkSync(lock);}
}
function ghTransport(root, plan, spawnSync=cp.spawnSync) {
  const gh='C:\\Program Files\\GitHub CLI\\gh.exe';
  if(!fs.existsSync(gh))throw Error('Official gh unavailable; inspect before choosing a fallback');
  const started=Date.now(); let calls=0;
  function invoke(args, payload) {
    if(++calls>64||Date.now()-started>180000)throw Error('Release operation budget reached; inspect recorded progress');
    const r=spawnSync(gh,args,{input:payload===undefined?undefined:JSON.stringify(payload),encoding:'utf8',
      windowsHide:true,timeout:Math.min(60000,Math.max(1,180000-(Date.now()-started))),maxBuffer:4*1024*1024,
      env:{...process.env,GH_PROMPT_DISABLED:'1',GH_HOST:'github.com'}});
    if(r.error||r.status!==0)throw Error((r.stderr||r.error?.message||'gh operation failed').slice(-1500));
    return r.stdout;
  }
  const api=(endpoint,payload,method)=>JSON.parse(invoke(['api',endpoint,...(payload===undefined?[]:['--method',method,'--input','-'])],payload));
  function releases() {
    const rows=[];
    for(let page=1;page<=10;page++) {
      const batch=api('repos/'+flow.REPO+'/releases?per_page=100&page='+page); rows.push(...batch);
      if(batch.length<100)return rows;
    }
    throw Error('Release listing bound reached');
  }
  return {api,releases,notes:()=>{
      const raw=flow.read(root,plan.notes.path);flow.check(raw.length===plan.notes.bytes&&flow.hash(raw)===plan.notes.sha256,'Release notes drift');
      return raw.toString('utf8');},
    tag:tag=>{
      const rows=api('repos/'+flow.REPO+'/git/matching-refs/tags/'+tag);
      const matches=rows.filter(row=>row.ref==='refs/tags/'+tag);
      flow.check(matches.length<=1,'Ambiguous tag');return matches[0]||null;
    },
    upload:(tag,asset)=>{
      const actual=flow.identity(root,asset.path);
      flow.check(actual.bytes===asset.bytes&&actual.sha256===asset.sha256,'Asset drift before upload');
      invoke(['release','upload',tag,flow.local(root,asset.path),'--repo',flow.REPO]);return null;}};
}
async function preparedStage({root,storage,action,args,emit=true}) {
  const tag=args.tag, dir=flow.directory(tag), planPath=path.join(dir,'publication-plan.json');
  if(action==='release-status') {
    flow.check(Object.keys(args).every(key=>key==='tag'),'Status takes only a tag');
    const result=flow.status(root,tag);if(emit)console.log(JSON.stringify(result));return result;
  }
  const publishing=action==='release-publish';
  flow.check(Object.keys(args).every(key=>(publishing?['tag','expectedPlanSha256','authorization']:['tag','rollbackTag']).includes(key)),
    'Unexpected preparation/publication arguments');
  const saved=publishing?flow.json(root,planPath):null;
  const rollbackTag=publishing?saved.rollback.tag:args.rollbackTag||flow.previousTag(root,tag);
  const deps=[...flow.dependencies(root,tag),...flow.dependencies(root,rollbackTag),
    flow.local(root,path.join(flow.directory(rollbackTag),'public-verification.json'))];
  const history=[];
  if(publishing) {
    flow.authorize(saved,args.expectedPlanSha256,flow.identity(root,planPath).sha256,args.authorization);
    deps.push(flow.local(root,planPath));
    const names=fs.readdirSync(flow.local(root,dir)).filter(name=>/^publication-attempt-[a-f0-9-]+\.json$/.test(name));
    flow.check(names.length<32,'Publication attempt history bound');
    for(const name of names)deps.push(flow.local(root,path.join(dir,name)));
  }
  const target=publishing?path.join(dir,'publication-attempt-'+require('node:crypto').randomUUID()+'.json'):planPath;
  const result=await storage.withLease({root,operation:'development',owner:'pulse-release-flow',
    purpose:publishing?'Publish exact prepared assets with durable intentions; retain prior releases':'Prepare immutable release parameters from accepted assets; no remote writes',
    verifyPaths:[...new Set(deps)],targets:[{path:flow.local(root,target),purpose:action,estimatedBytes:65536}]},async()=>{
    const plan=flow.prepare(root,tag,rollbackTag);
    if(!publishing)return {status:'PREPARED',...flow.savePrepared(root,plan),remoteWrites:0};
    flow.check(flow.encode(plan)===flow.encode(saved),'Preparation inputs drifted');
    flow.authorize(plan,args.expectedPlanSha256,flow.identity(root,planPath).sha256,args.authorization);
    for(const file of deps.filter(name=>path.basename(name).startsWith('publication-attempt-'))) {
      const attempt=flow.json(root,file);
      flow.check(attempt.planSha256===args.expectedPlanSha256&&Array.isArray(attempt.events),'Publication history identity differs');
      history.push(attempt);
    }
    const attempt={schemaVersion:1,planSha256:args.expectedPlanSha256,tag,commit:plan.commit,status:'RUNNING',events:[]};
    const file=flow.local(root,target), fd=fs.openSync(file,'wx');
    function persist() {
      const raw=Buffer.from(flow.encode(attempt));flow.check(raw.length<=65536,'Publication journal bound');
      fs.writeSync(fd,raw,0,raw.length,0);fs.ftruncateSync(fd,raw.length);fs.fsyncSync(fd);
    }
    try {
      persist();
      attempt.result=flow.publish(plan,ghTransport(root,plan),history,event=>{attempt.events.push({...event,utc:new Date().toISOString()});persist();});
      attempt.status='PASS';
    } catch(error) {attempt.status='FAILED';attempt.error=error.message;}
    finally {persist();fs.closeSync(fd);}
    // Operational failure is registered completely, without misreporting success or replaying it.
    return {status:attempt.status,receipt:file,result:attempt.result,error:attempt.error};
  });
  if(emit)console.log(JSON.stringify(result));if(result.status==='FAILED')process.exitCode=1;
  return result;
}
exports.run=async({root,storage,config,action,preparedArgs,emit=true})=>{
  const input=preparedArgs===undefined?fs.readFileSync(0,'utf8'):JSON.stringify(preparedArgs);
  if(Buffer.byteLength(input)>65536)throw Error('Bounded release input required');
  const args=JSON.parse(input);
  if(action!=='release-status')for(const item of config.identities.filter(item=>[config.node,config.python,config.storage].includes(item.path))) {
    if(flow.hash(fs.readFileSync(item.path))!==item.sha256)throw Error('Release tool identity changed: '+item.path);
  }
  if(['release-prepare','release-status','release-publish'].includes(action)) {
    const result=await preparedStage({root,storage,action,args,emit:false});
    if(action==='release-publish'&&result.status==='PASS')
      result.verification=await exports.run({root,storage,config,action:'release-verify',preparedArgs:{tag:args.tag},emit:false});
    if(emit)console.log(JSON.stringify(result));return result;
  }
  const originalCommit='c76c8f5da525e81c6d7d52dbd741d76a1c00336e';
  const releaseCommit='3087bb8c41cd58618809f44258a65333ab099321';
  const peripheralCommit='f8001b2c69d3e03d602c76f2f8dedcdad27fc02f';
  const settingsCommit='2a118b31fbabbe99c45e55fc1810e02aefe3d45f';
  const optimizationCommit='6bd690998893ee21e8df5fca2afcfa381cc499ab';
  const preparedVerification=action==='release-verify' && args.tag!==undefined;
  const prepared=preparedVerification?flow.json(root,path.join(flow.directory(args.tag),'publication-plan.json')):null;
  if(preparedVerification)flow.check(args.commit===undefined||args.commit===prepared.commit,'Verification commit differs');
  const importCommit=preparedVerification?prepared.commit:args.commit||originalCommit;
  const sourcing=action==='release-source';
  const currentPackage=action==='release-package'&&args.tag!==undefined;
  if(sourcing&&args.paths!==undefined) {
    if(args.message!==undefined)flow.check(typeof args.message==='string'&&args.message.length>=1&&args.message.length<=200&&!/[\x00-\x1f]/.test(args.message),'Invalid commit message');
    flow.check(/^[a-f0-9]{40}$/.test(args.base)&&/^codex\/[a-z0-9-]+$/.test(args.branch)&&
      /^custom-\d{4}\.\d{2}\.\d{2}\.\d+$/.test(args.tag)&&Array.isArray(args.paths)&&args.paths.length>0&&args.paths.length<=128,
      'Bounded exact source scope required');
    for(const name of args.paths) {
      flow.check(/^(?:src\/[^/]+\.(?:h|cpp|ps1)|src\/settings_ui\/(?:LiteUI|PulseSettings)\.cs|THIRD_PARTY_NOTICES\.md|tests\/[^/]+\.(?:cpp|ps1)|tools\/(?:shared\/)?[a-zA-Z0-9_.-]+\.(?:cjs|py|ps1)|scripts\/publish_custom_release\.py|docs\/development-guide\.md|app\.rc|build(?:_msvc)?\.bat|local-artifact-policy\.json)$/.test(name),
        'Private or unsupported source path');
      flow.read(root,name,524288);
    }
  }
  if(currentPackage) {
    flow.check(/^[a-f0-9]{40}$/.test(args.commit)&&/^custom-\d{4}\.\d{2}\.\d{2}\.\d+$/.test(args.tag),'Exact package identity required');
    const source=flow.json(root,path.join('dev-work/evidence','release-source-'+args.tag,'source-receipt.json'));
    flow.check(source.status==='PASS'&&source.commit===args.commit&&source.tag===args.tag,'Accepted source publication required');
    const exe=flow.local(root,args.artifactExe);
    flow.check(/^dev-work\/builds\/release-[a-f0-9]{24}\/Pulse\.exe$/.test(path.relative(root,exe).split(path.sep).join('/')),'Exact local release artifact required');
  }
  const approved=action==='release-import'?[originalCommit,'503f60eee59a2bf31de11648bb13d4fb3ad77bc8',releaseCommit]:[originalCommit,releaseCommit,peripheralCommit,settingsCommit,optimizationCommit];
  if(!sourcing&&!preparedVerification&&!currentPackage&&!approved.includes(importCommit))throw Error('Unapproved artifact commit');
  const tag=(preparedVerification||currentPackage||(sourcing&&args.paths))?args.tag:sourcing||importCommit===optimizationCommit?'custom-2026.11.14.2':importCommit===settingsCommit?'custom-2026.11.14.1':importCommit===peripheralCommit?'custom-2026.11.13.1':importCommit===releaseCommit?'custom-2026.11.11.1':'custom-2026.11.10.2';
  const ci=path.join(root,'dev-work/ci',importCommit.slice(0,7)),out=path.join(root,'dev-work/releases',tag);
  const names=sourcing?['source-receipt.json','source-progress.jsonl']:action==='release-import'?['artifact.zip','Pulse.exe','metrics_selftest.exe','artifact.json']:
    action==='release-package'?['Pulse-'+tag+'-win-x64.zip','release-manifest.json','SHA256SUMS.txt','release-notes.md','package-receipt.json']:['public-verification.json'];
  const dir=sourcing?path.join(root,'dev-work/evidence/release-source-'+tag):action==='release-import'?ci:out;
  const targets=names.map(n=>({path:path.join(dir,n),purpose:action+' exact authorized Pulse artifact',estimatedBytes:n.endsWith('.zip')||n.endsWith('.exe')?16*1024*1024:n.endsWith('.jsonl')?1024*1024:65536}));
  const verifyPaths=preparedVerification?[...flow.dependencies(root,tag),flow.local(root,path.join(out,'publication-plan.json'))]:[];
  const execute=()=>{const r=cp.spawnSync(config.python,['-B',path.join(__dirname,'publish_upstream_20261110.py'),action],{input,encoding:'utf8',windowsHide:true,timeout:90000,maxBuffer:65536,env:{...process.env,PYTHONUTF8:'1',PYTHONIOENCODING:'utf-8'}});
    if(r.error||r.status!==0)throw Error((r.stderr||r.error?.message||'Release stage failed').slice(-2000));return JSON.parse(r.stdout);};
  if(sourcing&&fs.existsSync(path.join(dir,'source-progress.jsonl'))){const resumed=resumeSource(root,storage,args,dir,execute);
    if(resumed){if(emit)console.log(JSON.stringify(resumed));return resumed;}}
  const result=await storage.withLease({root,operation:'development',owner:'pulse-upstream-release-20261110',purpose:action==='release-import'?'Import exact authorized CI artifact; retain previous downloads and installations':'Prepare authorized release; retain previous downloads and installations',targets,verifyPaths},async()=>{
    fs.mkdirSync(dir,{recursive:true});
    if(preparedVerification)flow.check(flow.encode(flow.prepare(root,tag,prepared.rollback.tag))===flow.encode(prepared),'Preparation drift before verification');
    // Complete package and public-download checkpoints are immutable. Reuse evidence,
    // not the packager/network operation. Status explicitly does not claim a fresh remote check.
    if(['release-package','release-verify'].includes(action)&&fs.existsSync(path.join(out,'package-receipt.json'))) {
      const current=flow.acceptedPackage(root,tag);
      flow.check(current.commit===importCommit,'Package commit differs');
      if(action==='release-package') {
        if(args.trialReceipt)flow.check(path.resolve(args.trialReceipt)===flow.local(root,current.trialReceipt.path),'Trial receipt differs');
        return {status:'PASS',stage:action,reused:true,receipt:path.join(out,'package-receipt.json')};
      }
      const prior=flow.publicReceiptMatches(root,tag,current);
      if(prior)return {status:'PASS',stage:action,reused:true,remoteChecked:false,verification:'RECORDED_PUBLIC_DOWNLOADS',receipt:prior.path};
    }
    return execute();
  });
  if(action==='release-package')result.preparation=await preparedStage({root,storage,action:'release-prepare',args:{tag,rollbackTag:args.rollbackTag},emit:false});
  if(emit)console.log(JSON.stringify(result));return result;
};
exports.preparedStage=preparedStage;
exports.ghTransport=ghTransport;
