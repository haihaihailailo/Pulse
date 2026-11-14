'use strict';
// Finish a successful tool check whose synthetic fixtures exceeded the file cap.
// Archive every exact fixture before same-handle retirement; never raise limits.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const hash=data=>crypto.createHash('sha256').update(data).digest('hex');
const check=(ok,message)=>{if(!ok)throw Error('Fixture recovery: '+message);};
const encode=value=>JSON.stringify(value)+'\n';
module.exports=async function recover({root,storage,config,child,quiescent,policy,input}){
  const home=path.join(root,'.local-artifact-storage'),stateFile=path.join(home,'state.json'),lock=path.join(home,'lock.json');
  const managed=path.join(root,'dev-work/vm-sessions'),evidence=path.join(root,'dev-work/evidence');
  check(/^[a-f0-9-]{36}$/.test(input.sessionId)&&/^[a-f0-9]{64}$/.test(input.stateSha256),'exact identity required');
  const durable=(file,raw)=>{const fd=fs.openSync(storage.safe(root,file),'wx');try{fs.writeFileSync(fd,raw);fs.fsyncSync(fd);}finally{fs.closeSync(fd);}};
  const json=file=>JSON.parse(fs.readFileSync(storage.safe(root,file),'utf8'));
  const id=crypto.randomUUID();durable(lock,encode({owner:'pulse-fixture-reconciliation',id}));
  const lockIdentity=storage.fileIdentity(root,lock);let state,started=false;
  const save=()=>{const pending=path.join(home,'state.pending.json');durable(pending,encode(state));fs.renameSync(pending,stateFile);};
  try{
    const original=fs.readFileSync(stateFile);check(hash(original)===input.stateSha256,'state drift');state=JSON.parse(original);
    const originalLast=state.last,session=path.join(managed,input.sessionId);
    const oldPlanFile=path.join(evidence,input.sessionId+'-plan.json'),oldResultFile=path.join(evidence,input.sessionId+'-result.json');
    const oldPlan=json(oldPlanFile),accepted=json(oldResultFile),capacity=json(path.join(root,'local-artifact-policy.json'));
    check(capacity.automatic_cleanup===true&&capacity.retention?.keep_unknown_and_incomplete===true,'automatic cleanup authorization required');
    check(originalLast.id===input.runId&&hash(fs.readFileSync(oldPlanFile))===input.planSha256&&
      hash(fs.readFileSync(oldResultFile))===input.resultSha256,'original receipt identity drift');
    if(input.mode==='fixture-refusal') {
      const archive=path.join(evidence,input.sessionId+'-fixtures.zip'),journal=path.join(evidence,input.sessionId+'-fixtures-deletions.jsonl');
      check(originalLast.status==='failed'&&originalLast.owner==='pulse-development-sessions'&&
        originalLast.error===`ENOENT: no such file or directory, lstat '${archive}'`&&oldPlan.action==='workflow-test'&&
        oldPlan.fixtureArchive===archive&&oldPlan.fixtureJournal===journal&&accepted.status==='FAILED'&&
        accepted.commands?.length===1&&accepted.commands[0].exitCode===1&&accepted.commands[0].status==='EXITED'&&
        accepted.productionUnchanged===true&&accepted.remainingDevelopmentProcesses?.length===0&&accepted.slots?.released===true&&
        originalLast.targets.length===4&&originalLast.targets.every(x=>[oldPlanFile,oldResultFile,archive,journal].includes(x.path))&&
        !fs.existsSync(archive)&&!fs.existsSync(journal),'only pre-archive tool refusal with absent optional outputs is eligible');
      for(const item of config.identities)check(hash(fs.readFileSync(item.path))===item.sha256,'tool drift');
      quiescent(managed);storage.verifyRegistered(root,state,new Set(),true);
      for(const record of Object.values(state.managedDirectories)) {
        const members=[],pending=[record.path];while(pending.length)for(const entry of fs.readdirSync(pending.pop(),{withFileTypes:true})) {
          const file=storage.safe(root,path.join(entry.parentPath,entry.name));if(entry.isDirectory())pending.push(file);else members.push(file);
        }
        const known=Object.values(state.objects).filter(x=>x.managedDirectory===record.path).map(x=>x.path).sort();
        check(JSON.stringify(members.sort())===JSON.stringify(known),'unknown or missing managed files');
        const used=storage.usage(record.path);storage.checkManagedBound(used.files,used.bytes,record.maximumBytes,record.maximumEntries);
      }
      storage.checkCapacity(root,capacity,16*1024*1024);
      const snapshot=path.join(evidence,id+'-state-before.json'),reportFile=path.join(evidence,id+'-reconciliation.json');
      state.last={id,owner:'pulse-fixture-reconciliation',status:'active',originalGeneration:originalLast,
        purpose:'Preserve failed tool checkpoint and close absent optional archive outputs',targets:[snapshot,archive,journal,reportFile].map(path=>({path,before:null}))};
      save();started=true;durable(snapshot,original);
      durable(archive,Buffer.from('504b0506000000000000000000000000000000000000','hex'));
      durable(journal,encode({status:'NOT_RUN',reason:'Tool self-check failed before fixture archive; all failure fixtures retained',originalResult:oldResultFile}));
      const report={status:'PASS',originalStatus:'FAILED',originalResult:oldResultFile,deletedFiles:0,limitsChanged:false,emptyArchive:archive,journal,snapshot};
      durable(reportFile,encode(report));
      for(const file of [snapshot,archive,journal,reportFile])state.objects[file]={path:file,owner:originalLast.owner,purpose:'Preserved tool refusal and optional output reconciliation',identity:storage.fileIdentity(root,file)};
      state.last.status='complete';state.last.reconciliation=reportFile;save();check(json(stateFile).last.status==='complete','readback failed');
      console.log(JSON.stringify(report));return report;
    }
    check(originalLast.status==='failed'&&originalLast.error==='Local artifact storage: managed directory bound exceeded'&&
      originalLast.owner==='pulse-development-sessions'&&oldPlan.id===input.sessionId&&oldPlan.action==='workflow-test'&&oldPlan.sessionDir===session&&
      accepted.status==='PASS'&&accepted.workflowChecks?.status==='PASS'&&accepted.workflowChecks.remoteCalls===0&&
      accepted.workflowChecks.productLaunches===0&&accepted.productionUnchanged===true&&accepted.remainingDevelopmentProcesses?.length===0&&accepted.slots?.released===true,
      'only a successful, stopped, tool-only check with capacity-only finalization failure is eligible');
    check(originalLast.targets.length===2&&originalLast.targets.every(x=>[oldPlanFile,oldResultFile].includes(x.path)&&x.before===null),'unexpected generation targets');
    for(const item of config.identities)check(hash(fs.readFileSync(item.path))===item.sha256,'tool drift');
    for(const item of oldPlan.inputs){
      const current=hash(fs.readFileSync(item.path));if(current===item.sha256)continue;
      const changed=input.revisedInputs?.find(x=>x.path===item.path);
      check(['pulse_dev.cjs','pulse_dev_worker.py','pulse_build_identity.cjs'].includes(path.basename(item.path))&&
        changed?.beforeSha256===item.sha256&&changed.afterSha256===current,'accepted input drift: '+item.path);
    }
    quiescent(managed);storage.verifyRegistered(root,state,new Set(),true);
    const all=[],stack=[managed];let totalBytes=0;
    while(stack.length){for(const entry of fs.readdirSync(stack.pop(),{withFileTypes:true})){
      const file=storage.safe(root,path.join(entry.parentPath,entry.name));
      if(entry.isDirectory())stack.push(file);else{const identity=storage.fileIdentity(root,file);all.push(identity);totalBytes+=identity.bytes;}
      check(all.length<=4096,'inventory bound');}}
    const added=all.filter(x=>!state.objects[x.path]);
    check(added.length===input.expectedFiles&&added.reduce((s,x)=>s+x.bytes,0)===input.expectedBytes&&added.every(x=>
      ['workflow-fixtures','settings-package-fixture'].some(name=>x.path.startsWith(path.join(session,name)+path.sep))),'unknown file outside exact completed fixtures');
    const record=state.managedDirectories[managed],limits=capacity.managed_directories['dev-work/vm-sessions'];
    check(record.maximumEntries===1024&&limits.maximum_entries===1024&&record.maximumBytes===limits.maximum_bytes&&
      totalBytes<=record.maximumBytes&&all.length>1024&&all.length-added.length<=1024,'not a file-count-only overflow');
    const directory=fs.lstatSync(managed);check(String(directory.dev)===record.identity.deviceId&&String(directory.ino)===record.identity.fileId,'directory drift');
    const present=new Set(all.map(x=>x.path));check(Object.values(state.objects).filter(x=>x.managedDirectory===managed).every(x=>present.has(x.path)),'registered file missing');
    const target=path.join(evidence,id+'-fixture-recovery'),planFile=path.join(evidence,id+'-plan.json'),receiptFile=path.join(evidence,id+'-result.json');
    const archive=path.join(evidence,id+'-fixtures.zip'),snapshot=path.join(evidence,id+'-state-before.json');
    const journal=path.join(target,'deletions.jsonl'),summary=path.join(evidence,id+'-reconciliation.json');
    const retirements=added.map(identity=>({path:identity.path,owner:record.owner,identity,nativeIdentity:storage.nativeIdentity(root,identity)}));
    const names=['tools/pulse_dev.cjs','tools/pulse_dev_worker.py','tools/pulse_fixture_recovery.cjs','tools/pulse_fixture_archive.py',
      'tools/pulse_artifact_delete.py','tools/dev-environment.local.json','local-artifact-policy.json'];
    const inputs=names.map(name=>{const file=storage.safe(root,path.join(root,name));return {path:file,sha256:hash(fs.readFileSync(file))};});
    const plan={schema:1,root,action:'artifact-retire',id,owner:'pulse-artifact-retirement',parentPid:process.pid,config,inputs,policy,
      buildKey:null,targetDirectory:target,receiptFile,retirementJournal:journal,retirements,fixtureArchive:archive,fixtureSession:session,
      wallSeconds:90,jobMiB:512,outputLimitBytes:262144};
    const admission=storage.checkCapacity(root,capacity,16*1024*1024);
    state.last={id,owner:plan.owner,status:'active',operation:'development',purpose:'Archive completed synthetic fixtures and finish capacity-only failure without changing limits',
      originalGeneration:originalLast,retirements,targets:[snapshot,planFile,receiptFile,archive,journal,summary].map(p=>({path:p,before:null})),admission,
      startedUtc:new Date().toISOString(),authorization:'Existing automatic retirement authorization; exact completed synthetic fixtures only; archive retained'};
    for(const identity of added)state.objects[identity.path]={path:identity.path,owner:record.owner,purpose:originalLast.purpose,managedDirectory:managed,identity};
    for(const file of [oldPlanFile,oldResultFile])state.objects[file]={path:file,owner:record.owner,purpose:'Original accepted tool check and capacity-only finalization failure',identity:storage.fileIdentity(root,file)};
    save();started=true;durable(snapshot,original);const raw=encode(plan);durable(planFile,raw);
    const outcome=await child(planFile,hash(raw),1020);check(!outcome.interrupted,'recovery worker interrupted');
    const result=json(receiptFile);check(result.status==='PASS'&&result.retirement?.deletedFiles===added.length&&result.fixtureArchive?.status==='PASS'&&
      result.productionUnchanged===true&&result.remainingDevelopmentProcesses?.length===0&&result.slots?.released===true,'guarded archive/retirement failed; preserve partial state');
    for(const item of retirements){check(!fs.existsSync(item.path),'retired fixture remains');delete state.objects[item.path];state.retiredObjects[item.path]={
      identity:item.identity,nativeIdentity:item.nativeIdentity,owner:item.owner,runId:id,evidence:[archive,journal,receiptFile],retiredUtc:new Date().toISOString()};}
    const report={status:'PASS',originalSession:input.sessionId,originalResult:oldResultFile,archive,archiveSha256:hash(fs.readFileSync(archive)),
      deletedFiles:added.length,deletedBytes:input.expectedBytes,remainingSessionFiles:all.length-added.length,limitsChanged:false,productRerun:false,
      snapshot,workerReceipt:receiptFile,journal,restoration:'Exact fixture files and identities retained in the archive; do not overwrite existing files or restore an obsolete ledger.'};
    durable(summary,encode(report));
    for(const file of [snapshot,planFile,receiptFile,archive,journal,summary])state.objects[file]={path:file,owner:record.owner,purpose:'Fixture recovery and exact rollback evidence',identity:storage.fileIdentity(root,file)};
    storage.verifyRegistered(root,state,new Set(),true);quiescent(managed);storage.checkCapacity(root,capacity,4*1024*1024);
    const remaining=storage.usage(managed);
    check(remaining.files===all.length-added.length&&remaining.bytes===totalBytes-input.expectedBytes,'remaining fixture inventory drift');
    state.last.status='complete';state.last.reconciliation=summary;state.last.completedUtc=new Date().toISOString();save();
    check(json(stateFile).last.status==='complete','state readback failed');console.log(JSON.stringify(report));return report;
  }catch(error){if(started){state.last.status='failed';state.last.error=String(error.message).slice(0,300);save();}throw error;}
  finally{check(JSON.stringify(storage.fileIdentity(root,lock))===JSON.stringify(lockIdentity),'own lock changed');fs.unlinkSync(lock);}
};
