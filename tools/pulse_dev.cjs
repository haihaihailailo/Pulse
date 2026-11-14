'use strict';
// Single local development entry: admission, registered outputs, guarded child, final receipt.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {spawn, spawnSync} = require('node:child_process');
const root = path.resolve(__dirname, '..');
const sha = data => crypto.createHash('sha256').update(data).digest('hex');
const read = file => fs.readFileSync(file);
function fileHash(file) {
  const stream = fs.openSync(file,'r'), digest = crypto.createHash('sha256'), buffer = Buffer.alloc(65536);
  try { for (;;) { const count = fs.readSync(stream,buffer,0,buffer.length,null); if (!count) break; digest.update(buffer.subarray(0,count)); } }
  finally { fs.closeSync(stream); }
  return digest.digest('hex');
}
const configPath = path.join(__dirname, 'dev-environment.local.json');
const config = JSON.parse(read(configPath));
// Pulse opts into the separately versioned shared guard. Keep the old configured
// module and other projects untouched; bind the effective module in every plan.
config.storage = path.join(path.dirname(config.storage),'local_artifact_storage_v2.cjs');
const storageIdentity = {path:config.storage,sha256:'9939d2ab774b7071ae18ca0c3a65373cab80d77d678055692123293760b37fef'};
if(fileHash(storageIdentity.path)!==storageIdentity.sha256)throw Error('Versioned storage guard identity changed');
config.identities.push(storageIdentity);
const storage = require(config.storage);
const worker = path.join(__dirname, 'pulse_dev_worker.py');
const fail = message => { throw new Error(message); };
// Product, toolchain, worker and compiler-recipe identities are checked together.
function sameBinaries(rows, baseline) {
  return require('./pulse_build_identity.cjs').sameBinaries(rows,baseline,worker,read(worker));
}
function json(file, maximum = 2 * 1024 * 1024) {
  storage.safe(root, file);
  if (fs.statSync(file).size > maximum) fail('Oversized development receipt');
  return JSON.parse(read(file));
}
function ownFile(file) {
  storage.safe(root, file);
  const stat = fs.lstatSync(file);
  if (!stat.isFile() || stat.nlink !== 1) fail('Linked/nonregular development input');
  return {path: file, sha256: fileHash(file)};
}
function memoryPolicy(profile) {
  if (!['daily','idle'].includes(profile)) fail('Unknown memory profile');
  const file = path.join(process.env.CODEX_HOME || path.join(process.env.USERPROFILE, '.codex'), 'test-memory-policy.json');
  const raw = read(file), policy = JSON.parse(raw.toString('utf8').replace(/^\uFEFF/, ''));
  if (raw.length > 8192 || policy.schemaVersion !== 1 || policy.automaticSwitch !== false ||
      !((policy.profiles?.daily?.hostReserveMiB === 4096 && policy.profiles?.idle?.hostReserveMiB === 2048) || (policy.profiles?.daily?.hostReserveMiB === 2048 && policy.profiles?.idle?.hostReserveMiB === 1024)))
    fail('Unexpected shared memory policy');
  return {profile, policyPath: file, policySha256: sha(raw), hostReserveMiB: policy.profiles[profile].hostReserveMiB};
}
function quiescent(managed) {
  const result = spawnSync(config.python, ['-B', worker, '--inventory', root], {
    cwd: root, windowsHide: true, timeout: 10000, maxBuffer: 65536, encoding: 'utf8'
  });
  if (result.error || result.status !== 0) fail('Development process inventory failed: ' + (result.stderr || result.error));
  const state = JSON.parse(result.stdout);
  if (state.processes.length) fail('A development instance is still running; close it normally before proceeding');
  return {quiescent: true, resourceId: managed};
}
function child(planFile, expectedHash, timeout) {
  return new Promise((resolve, reject) => {
    const proc = spawn(config.python, ['-B', worker, '--plan', planFile, '--sha256', expectedHash], {
      cwd: root, windowsHide: true, stdio: ['ignore','pipe','pipe'],
      env: {...process.env, PYTHONDONTWRITEBYTECODE:'1'}
    });
    let output = '', size = 0, expired = false;
    const timer = setTimeout(() => { expired = true; proc.kill(); }, timeout * 1000);
    const collect = bytes => {
      size += bytes.length;
      if (size > 2 * 1024 * 1024) { expired = true; proc.kill(); }
      else output += bytes.toString('utf8');
    };
    proc.stdout.on('data', collect); proc.stderr.on('data', collect);
    proc.on('error', error => { clearTimeout(timer); reject(error); });
    proc.on('close', code => { clearTimeout(timer); resolve({code, output, interrupted: expired}); });
  });
}
// Exact recovery of the pre-compilation queue timeout; never accepts another run.
function reconcileWait() {
  const id = '64299a81-fd38-4673-be4a-2571da370ca1';
  const home = path.join(root,'.local-artifact-storage');
  const stateFile = path.join(home,'state.json'), pending = path.join(home,'state.pending.json');
  const lock = path.join(home,'lock.json');
  const planFile = path.join(root,'dev-work/evidence',id+'-plan.json');
  const resultFile = path.join(root,'dev-work/evidence',id+'-result.json');
  const backup = path.join(root,'dev-work/evidence',id+'-storage-before.json');
  const durable = (file, bytes) => {
    const fd = fs.openSync(storage.safe(root,file),'wx');
    try { fs.writeFileSync(fd,bytes); fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
  };
  for (const item of config.identities) if (fileHash(item.path) !== item.sha256) fail('Shared tool identity changed');
  durable(lock,JSON.stringify({owner:'pulse-exact-wait-reconciliation',id:crypto.randomUUID()}));
  const lockIdentity = storage.fileIdentity(root,lock);
  try {
    if (fs.existsSync(pending)) fail('Pending state requires separate inspection');
    const raw = read(stateFile), state = JSON.parse(raw), plan = json(planFile);
    if (state.last?.id !== 'a8132773-370e-48c1-95d2-d89e7a93f0e5' || state.last.status !== 'failed' ||
        state.last.error !== 'Worker interrupted; retain lease evidence and reconcile before retrying' ||
        fileHash(planFile) !== '385e99f3f4924a0f81664a11f64f85b94087e9772d3f42f49a2cb8b430518355' ||
        plan.id !== id || plan.action !== 'build' ||
        plan.buildDir !== path.join(root,'dev-work/builds/3bf31be5132ba22f2c3a1796') ||
        fs.existsSync(plan.buildDir) || fs.existsSync(resultFile) || fs.existsSync(backup)) fail('Not the inspected pre-build interruption');
    if (JSON.stringify(state.last.targets.map(t=>t.path)) !== JSON.stringify([planFile,resultFile]) ||
        state.last.targets.some(t=>t.before !== null)) fail('Unexpected recovery targets');
    quiescent(path.join(root,'dev-work/builds'));
    storage.verifyRegistered(root,state,new Set(),true);
    const admission = storage.checkCapacity(root,json(path.join(root,'local-artifact-policy.json')),2*1024*1024);
    const save = () => { durable(pending,JSON.stringify(state)+'\n'); fs.renameSync(pending,stateFile); };
    state.last.reconciliation = {phase:'registered-before-generation',targets:[{path:backup,estimatedBytes:1048576},
      {path:resultFile,estimatedBytes:65536}],admission};
    save();
    if (raw.length > 1048576) fail('Recovery backup exceeds reservation');
    durable(backup,raw);
    durable(resultFile,JSON.stringify({status:'FAILED',action:'build',buildStarted:false,
      reason:state.last.error,workerExitVerified:true,buildDirectoryAbsent:true,
      recovery:'Preserved failed queue wait; no build or test acceptance claimed.'},null,2)+'\n');
    for (const file of [backup,planFile,resultFile]) {
      state.objects[file] = {path:file,owner:'pulse-development-builds',purpose:'Preserved exact interrupted wait',
        identity:storage.fileIdentity(root,file)};
    }
    quiescent(path.join(root,'dev-work/builds'));
    storage.verifyRegistered(root,state,new Set(),true);
    state.last.reconciliation.phase = 'complete';
    state.last.reconciliation.functionalStatus = 'FAILED';
    state.last.status = 'complete'; state.last.completedUtc = new Date().toISOString();
    save();
    if (json(stateFile).last.reconciliation.phase !== 'complete') fail('Recovery readback failed');
    console.log(JSON.stringify({status:'RECONCILED_FAILED_WAIT',receipt:resultFile,deletedFiles:0}));
  } finally {
    if (JSON.stringify(storage.fileIdentity(root,lock)) !== JSON.stringify(lockIdentity)) fail('Own recovery lock changed');
    fs.unlinkSync(lock);
  }
}
// Finish a successful test whose outer registration alone exceeded its session
// bound. Explicit input binds the approved capacity change to the failed run;
// the original test, backups, and every existing artifact remain immutable.
function reconcileCapacity() {
  const rawInput=fs.readFileSync(0,'utf8').replace(/^\uFEFF/,'');
  if(Buffer.byteLength(rawInput)>8192)fail('Oversized reconciliation input');
  const input=JSON.parse(rawInput);
  const preflight=input.mode==='preflight';
  const entries=input.mode==='entries';
  if(input.mode && !preflight && !entries)fail('Unknown capacity migration mode');
  const uuid=/^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/;
  if(!uuid.test(input.runId)||!uuid.test(input.sessionId)||
     !/^[a-f0-9]{64}$/.test(input.previousRunnerSha256)||
     !Number.isSafeInteger(input.oldMaximumBytes)||!Number.isSafeInteger(input.newMaximumBytes)||
     input.oldMaximumBytes<=0||(entries ? input.newMaximumBytes!==input.oldMaximumBytes ||
       input.oldMaximumBytes!==268435456 || input.oldMaximumEntries!==512 || input.newMaximumEntries!==1024 :
       input.newMaximumBytes<=input.oldMaximumBytes)||
     typeof input.authorization!=='string'||!input.authorization.trim()||input.authorization.length>1000)
    fail('Exact run, bounds, previous runner identity and explicit authorization required');
  const home=path.join(root,'.local-artifact-storage'),stateFile=path.join(home,'state.json');
  const pending=path.join(home,'state.pending.json'),lock=path.join(home,'lock.json');
  const policyFile=path.join(root,'local-artifact-policy.json'),policyPending=policyFile+'.pending';
  const managed=path.join(root,'dev-work/vm-sessions'),session=path.join(managed,input.sessionId);
  const planFile=path.join(root,'dev-work/evidence',input.sessionId+'-plan.json');
  const resultFile=path.join(root,'dev-work/evidence',input.sessionId+'-result.json');
  const backup=path.join(root,'dev-work/evidence',input.sessionId+'-capacity');
  const stateBackup=path.join(backup,'state.before.json'),policyBackup=path.join(backup,'policy.before.json');
  const receiptFile=path.join(backup,'reconciliation.json');
  const same=(a,b)=>JSON.stringify(a)===JSON.stringify(b);
  const durable=(file,data)=>{const fd=fs.openSync(storage.safe(root,file),'wx');
    try{fs.writeFileSync(fd,data);fs.fsyncSync(fd);}finally{fs.closeSync(fd);}};
  for(const item of config.identities)if(fileHash(item.path)!==item.sha256)fail('Shared tool identity changed');
  durable(lock,JSON.stringify({owner:'pulse-capacity-reconciliation',id:crypto.randomUUID()}));
  const lockIdentity=storage.fileIdentity(root,lock);
  try {
    if(fs.existsSync(pending)||fs.existsSync(policyPending)||fs.existsSync(backup))fail('Pending or existing reconciliation requires inspection');
    const stateRaw=read(stateFile),policyRaw=read(policyFile);
    const state=json(stateFile,8*1024*1024),policy=json(policyFile),plan=preflight?null:json(planFile),result=preflight?null:json(resultFile);
    if(preflight) {
      if(state.last?.id!==input.runId||state.last.status!=='complete'||
         input.oldMaximumBytes!==134217728||input.newMaximumBytes!==268435456||
         policy.project_budget_bytes!==1073741824||policy.minimum_free_bytes!==21474836480||
         policy.managed_directories['dev-work/builds'].maximum_bytes!==335544320||
         fs.existsSync(session)||fs.existsSync(planFile)||fs.existsSync(resultFile))
        fail('Preflight migration requires the exact authorized completed state and unchanged budgets');
    } else {
    if(state.last?.id!==input.runId||state.last.status!=='failed'||state.last.reconciliation||
       state.last.owner!=='pulse-development-sessions'||
       state.last.error!=='Local artifact storage: managed directory bound exceeded'||
       plan.id!==input.sessionId||plan.action!==(entries?'workflow-test':'test')||plan.sessionDir!==session||
       result.status!=='PASS'||result.buildKey!==plan.buildKey||result.productionUnchanged!==true||
       result.desktopClosed!==!entries||result.remainingDevelopmentProcesses?.length!==0||result.slots?.released!==true)
      fail('Not the approved successful test with capacity-only finalization failure');
    if(entries && (result.workflowChecks?.status!=='PASS'||result.workflowChecks.productLaunches!==0||
       result.workflowChecks.compilerInvocations!==0||result.workflowChecks.remoteCalls!==0||
       result.commands.some(command=>command.status!=='EXITED'||command.exitCode!==0)||
       policy.project_budget_bytes!==1073741824||policy.minimum_free_bytes!==21474836480||
       policy.managed_directories['dev-work/builds'].maximum_bytes!==335544320))
      fail('Exact successful tool-only test and unchanged byte budgets required');
    if(!same(state.last.targets.map(x=>x.path).sort(),[planFile,resultFile].sort())||
       state.last.targets.some(x=>x.before!==null)||state.last.managedDirectories.length!==1||
       state.last.managedDirectories[0].path!==managed)fail('Unexpected failed transaction targets');
    }
    const record=state.managedDirectories[managed],directoryStat=fs.lstatSync(storage.safe(root,managed));
    if(record?.owner!=='pulse-development-sessions'||record.resourceId!==managed||
       record.maximumBytes!==input.oldMaximumBytes||
       policy.managed_directories['dev-work/vm-sessions'].maximum_bytes!==input.oldMaximumBytes||
       !directoryStat.isDirectory()||String(directoryStat.dev)!==record.identity.deviceId||
       String(directoryStat.ino)!==record.identity.fileId||input.newMaximumBytes>=policy.project_budget_bytes)
      fail('Directory identity or approved old capacity differs');
    if(entries && ((record.maximumEntries??512)!==input.oldMaximumEntries ||
       (policy.managed_directories['dev-work/vm-sessions'].maximum_entries??512)!==input.oldMaximumEntries))
      fail('Registered old entry limit differs');
    // Only the reviewed local reconciliation entry changed since the test.
    for(const item of plan?.inputs||[]) {
      if(item.path===__filename) {
        if(item.sha256!==input.previousRunnerSha256)fail('Previous entry identity differs');
      } else if(fileHash(storage.safe(root,item.path))!==item.sha256) {
        const revised=entries && input.revisedToolInputs?.find(row=>row.path===item.path);
        if(!revised||!['publish_upstream_20261110.cjs','pulse_release_flow.cjs','pulse_workflow_selftest.cjs'].includes(path.basename(item.path))||
           revised.beforeSha256!==item.sha256||revised.afterSha256!==fileHash(item.path))fail('Test input drift: '+item.path);
      }
    }
    if(!preflight&&!entries) {
    const build=json(path.join(plan.buildDir,'build.json'));
    if(build.status!=='PASS'||build.buildKey!==plan.buildKey)fail('Build receipt differs');
    for(const item of build.outputs)if(fileHash(storage.safe(root,item.path))!==item.sha256)fail('Build artifact drift');
    }
    quiescent(managed);
    const verification=storage.verifyRegistered(root,state,new Set(),true);
    const files=[],stack=[managed];let bytes=0;
    while(stack.length) {
      const directory=stack.pop();
      for(const entry of fs.readdirSync(directory,{withFileTypes:true})) {
        const file=storage.safe(root,path.join(directory,entry.name));
        if(entry.isDirectory())stack.push(file);
        else {const identity=storage.fileIdentity(root,file);files.push(identity);bytes+=identity.bytes;}
        if(files.length>(entries?input.newMaximumEntries:4096)||bytes>input.newMaximumBytes)fail('Reconciliation inventory exceeds approved bound');
      }
    }
    if(!preflight&&(entries?files.length<=input.oldMaximumEntries:bytes<=input.oldMaximumBytes))fail('Expected actual over-capacity evidence is absent');
    const newFiles=files.filter(x=>!state.objects[x.path]);
    if(preflight ? newFiles.length!==0 : (!newFiles.length||newFiles.some(x=>!x.path.startsWith(session+path.sep))))fail('Unknown file outside the exact failed session');
    const present=new Set(files.map(x=>x.path));
    if(Object.values(state.objects).some(x=>x.managedDirectory===managed&&!present.has(x.path)))fail('Registered session file missing');
    for(const capture of result?.renderCaptures||[]) {
      if(!capture.path.startsWith(session+path.sep)||fileHash(storage.safe(root,capture.path))!==capture.sha256)
        fail('Accepted rendering evidence drift');
    }
    const next=structuredClone(policy);next.managed_directories['dev-work/vm-sessions'].maximum_bytes=input.newMaximumBytes;
    if(entries)next.managed_directories['dev-work/vm-sessions'].maximum_entries=input.newMaximumEntries;
    const admission=storage.checkCapacity(root,next,8*1024*1024+(input.newMaximumBytes-bytes));
    const targets=[{path:stateBackup,estimatedBytes:8*1024*1024},{path:policyBackup,estimatedBytes:16384},
      {path:receiptFile,estimatedBytes:256*1024}];
    const save=()=>{const value=JSON.stringify(state)+'\n';if(Buffer.byteLength(value)>8*1024*1024)fail('State bound');
      durable(pending,value);fs.renameSync(pending,stateFile);};
    state.last.reconciliation={phase:'registered-before-generation',originalStatus:preflight?'complete':'failed',
      authorization:input.authorization,oldMaximumBytes:input.oldMaximumBytes,newMaximumBytes:input.newMaximumBytes,
      ...(entries?{oldMaximumEntries:input.oldMaximumEntries,newMaximumEntries:input.newMaximumEntries,revisedToolInputs:input.revisedToolInputs}:{}),
      targets,admission};save();
    fs.mkdirSync(storage.safe(root,backup));durable(stateBackup,stateRaw);durable(policyBackup,policyRaw);
    if(!read(policyFile).equals(policyRaw))fail('Policy drift before atomic replacement');
    durable(policyPending,JSON.stringify(next,null,2)+'\n');fs.renameSync(policyPending,policyFile);
    record.maximumBytes=input.newMaximumBytes;
    if(entries)record.maximumEntries=input.newMaximumEntries;
    for(const identity of newFiles) {
      if(!same(storage.fileIdentity(root,identity.path),identity))fail('New session file drift');
      state.objects[identity.path]={path:identity.path,owner:record.owner,purpose:state.last.purpose,managedDirectory:managed,identity};
    }
    for(const file of preflight?[]:[planFile,resultFile]) {
      if(state.objects[file])fail('Failed receipt already registered');
      state.objects[file]={path:file,owner:record.owner,purpose:'Preserved accepted test and capacity failure',identity:storage.fileIdentity(root,file)};
    }
    const receipt={status:'PASS',kind:preflight?'CAPACITY_PREFLIGHT_MIGRATED':'CAPACITY_RECONCILED',runId:input.runId,sessionId:input.sessionId,
      authorization:input.authorization,oldMaximumBytes:input.oldMaximumBytes,newMaximumBytes:input.newMaximumBytes,
      ...(entries?{oldMaximumEntries:input.oldMaximumEntries,newMaximumEntries:input.newMaximumEntries,revisedToolInputs:input.revisedToolInputs,
        toolTestEvidence:'Original inputs only; later tool changes require their own directed check',effectiveStorage:storageIdentity}:{}),
      sessionBytes:bytes,deletedFiles:0,testReceipt:preflight?null:resultFile,buildKey:plan?.buildKey,workerStatus:result?.status,
      stateBeforeSha256:sha(stateRaw),policyBeforeSha256:sha(policyRaw),policyAfterSha256:sha(read(policyFile)),
      previousRunnerSha256:input.previousRunnerSha256,reconciliationRunnerSha256:fileHash(__filename),
      preservedNewFiles:newFiles,verification,productionUnchanged:true,desktopClosed:!entries,slotsReleased:true,
      rollback:'Keep every artifact and registration. Reducing the bound requires a new authorized reconciliation with usage below that bound; never restore the old state over newer registrations.'};
    durable(receiptFile,JSON.stringify(receipt,null,2)+'\n');
    for(const target of targets) {
      const identity=storage.fileIdentity(root,target.path);
      if(identity.bytes>target.estimatedBytes)fail('Reconciliation evidence bound');
      state.objects[target.path]={path:target.path,owner:record.owner,purpose:'Capacity reconciliation and rollback evidence',identity};
    }
    storage.verifyRegistered(root,state,new Set(),true);quiescent(managed);storage.checkCapacity(root,next,4*1024*1024);
    state.last.reconciliation.phase='complete';state.last.reconciliation.receipt=receiptFile;
    if(!preflight)state.last.reconciliation.functionalTestStatus='PASS';state.last.status='complete';state.last.completedUtc=new Date().toISOString();
    const auditFile=storage.safe(root,path.join(home,'critical-audit.jsonl'));
    if(fs.existsSync(auditFile)&&fs.lstatSync(auditFile).nlink!==1)fail('Audit identity unsafe');
    const fd=fs.openSync(auditFile,'a');
    try{fs.writeSync(fd,JSON.stringify({utc:new Date().toISOString(),event:'session-capacity-reconciled',runId:input.runId,receiptFile,deletedFiles:0})+'\n');fs.fsyncSync(fd);}finally{fs.closeSync(fd);}
    save();
    if(!same(json(stateFile,8*1024*1024),state)||!same(json(policyFile),next))fail('Reconciliation readback mismatch');
    console.log(JSON.stringify({status:'PASS',stage:'capacity-reconciled',receipt:receiptFile,sessionBytes:bytes,newMaximumBytes:input.newMaximumBytes,deletedFiles:0}));
  } finally {
    if(!same(storage.fileIdentity(root,lock),lockIdentity))fail('Own reconciliation lock changed');
    fs.unlinkSync(lock);
  }
}
async function main() {
  const [action = 'test', profile = 'daily', consent, artifactExe] = process.argv.slice(2);
  if(action==='reconcile-capacity') {
    if(profile!=='daily'||consent||artifactExe)fail('Unexpected capacity reconciliation arguments');
    return reconcileCapacity();
  }
  if (action === 'reconcile-wait') {
    if (profile !== 'daily' || consent || artifactExe) fail('Unexpected recovery arguments');
    return reconcileWait();
  }
  if (['release-source','release-import','release-package','release-prepare','release-status','release-publish','release-verify'].includes(action)) {
    if (profile !== 'daily' || consent || artifactExe) fail('Unexpected release arguments');
    return require('./publish_upstream_20261110.cjs').run({root,storage,config,action});
  }
  if (action === 'merge-upstream') {
    if (consent || artifactExe || profile !== 'daily') fail('Unexpected upstream preparation arguments');
    return require('./merge_upstream_20261110.cjs').prepare({root, storage});
  }
  const trial = ['trial','trial-test'].includes(action);
  const building = ['build','release'].includes(action);
  const workflowTest = action === 'workflow-test';
  if (!['build','release','test','debug','run','trial','trial-test','capacity','script-test','workflow-test'].includes(action)) fail('Unknown action');
  if (['run','trial'].includes(action) && consent !== '--interactive') fail('Visible use requires -Interactive');
  if (consent && !(['run','trial'].includes(action) && consent === '--interactive') &&
      !(action === 'trial-test' && consent === '--isolated')) fail('Unexpected interactive flag');
  if (Boolean(artifactExe) !== trial) fail('Trial requires an exact downloaded ArtifactExe');
  for (const item of config.identities) {
    if (fileHash(item.path) !== item.sha256) fail('Shared tool identity changed: ' + item.path);
  }
  const policy = memoryPolicy(profile);
  const sources = workflowTest ? [] : fs.readdirSync(path.join(root,'src')).filter(x => /\.(cpp|h|ps1)$/.test(x)).sort().map(x => ownFile(path.join(root,'src',x)));
  const upstreamTests = workflowTest ? [] : fs.readdirSync(path.join(root,'tests')).filter(x => /\.(cpp|ps1)$/.test(x)).sort().map(x => ownFile(path.join(root,'tests',x)));
  const inputs = workflowTest ? ['tools/Pulse-Dev.ps1','tools/pulse_dev.cjs','tools/pulse_dev_worker.py',
    'tools/publish_upstream_20261110.cjs','tools/pulse_release_flow.cjs','tools/pulse_workflow_selftest.cjs','tools/pulse_build_identity.cjs',
    'tools/dev-environment.local.json','local-artifact-policy.json'].map(x=>ownFile(path.join(root,x))) :
    [...sources, ...upstreamTests, ...['app.rc','app.manifest','pulse.ico','tools/pulse_dev_worker.py','tools/pulse_dev.cjs','tools/pulse_build_identity.cjs',
    'local-artifact-policy.json','tools/dev-environment.local.json','tools/development-defaults.ini',
    'tools/pulse_capacity_migration.cjs'].map(x => ownFile(path.join(root,x)))];
  let artifact = null;
  if (trial) {
    const exact = path.resolve(artifactExe);
    const local = exact.startsWith(path.join(root,'dev-work','builds') + path.sep);
    if ((!local && !exact.startsWith(path.join(root,'dev-work','ci') + path.sep)) || path.basename(exact) !== 'Pulse.exe')
      fail('Trial must use an admitted CI or local release Pulse.exe');
    artifact = ownFile(exact);
    const manifestPath = path.join(path.dirname(exact),local ? 'build.json' : 'artifact.json');
    const manifest = json(manifestPath);
    if (manifest.status !== 'PASS' || (local ?
      manifest.kind !== 'release' || !sameBinaries(inputs,manifest.inputs) ||
        !manifest.outputs.some(item => item.path === exact && item.sha256 === artifact.sha256) :
      manifest.executableSha256 !== artifact.sha256 || manifest.repository !== 'haihaihailailo/Pulse'))
      fail('Artifact identity or local release inputs are not verified');
    inputs.push(artifact, ownFile(manifestPath));
  }
  const executionKey = sha(JSON.stringify(inputs)).slice(0,24);
  let buildKey = action === 'release' ? 'release-' + executionKey : executionKey;
  let buildDir = path.join(root,'dev-work','builds',buildKey);
  let buildManifest = path.join(buildDir,'build.json');
  // Test-harness/evidence-policy edits do not require another C++ build.
  // Select a retained verified build by binary inputs, never by age alone.
  if (!workflowTest && !fs.existsSync(buildManifest)) {
    const builds=path.join(root,'dev-work/builds');
    const candidates=fs.readdirSync(builds).filter(name=>(action==='release'?/^release-[a-f0-9]{24}$/:/^[a-f0-9]{24}$/).test(name));
    if(candidates.length>256)fail('Build receipt inventory bound');
    for(const name of candidates) {
      const baselinePath=path.join(builds,name,'build.json');
      if(!fs.existsSync(baselinePath))continue;
      const baseline=json(baselinePath);
      if (baseline.status==='PASS' && baseline.buildKey===name &&
          baseline.kind===(action==='release'?'release':'development') && sameBinaries(inputs,baseline.inputs)) {
      buildKey = baseline.buildKey;
      buildDir = path.join(root,'dev-work','builds',buildKey);
      buildManifest = path.join(buildDir,'build.json');
      break;
      }
    }
  }
  let build = null;
  if (!workflowTest && fs.existsSync(buildManifest)) {
    build = json(buildManifest);
    if (build.status !== 'PASS' || build.buildKey !== buildKey) fail('Existing build is incomplete; preserve it for diagnosis');
    for (const item of build.outputs) {
      if (fileHash(storage.safe(root,item.path)) !== item.sha256) fail('Build output drift: ' + item.path);
    }
  }
  if (!building && !['capacity','script-test','workflow-test'].includes(action) && !build && !trial) fail('No verified build for these inputs. Run -Action Build first');
  const id = crypto.randomUUID();
  const evidence = path.join(root,'dev-work','evidence');
  const planFile = path.join(evidence, id + '-plan.json');
  const receiptFile = path.join(evidence, id + '-result.json');
  const managed = path.join(root,'dev-work',building ? 'builds' : 'vm-sessions');
  const owner = building ? 'pulse-development-builds' : 'pulse-development-sessions';
  const sessionDir = path.join(root,'dev-work','vm-sessions',id);
  const wallSeconds = building ? 240 : ['run','trial'].includes(action) ? 1800 : action === 'debug' ? 120 : 90;
  const plan = {schema:1, root, action, id, owner, parentPid:process.pid, executionKey, buildKey, buildDir, build,
    sessionDir, receiptFile, config, inputs, policy, wallSeconds, artifact,
    jobMiB:512, outputLimitBytes:262144,
    targetDirectory:building ? buildDir : sessionDir,
    purpose:'Isolated development only; no installation, production writes or publication'};
  if (building && !build) {
    const existing = fs.existsSync(managed) ? storage.usage(managed) : {bytes:0,files:0};
    if (existing.bytes + 24 * 1024 * 1024 > 335544320) fail('Insufficient registered build headroom for a new compilation; preserve existing builds');
  }
  const result = await storage.withLease({root, operation:'development', owner,
    purpose:plan.purpose, managedDirectories:[{path:managed}],
    assertManagedQuiescent:() => quiescent(managed),
    targets:[{path:planFile,purpose:'Exact inputs, paths and resource reservation before execution',estimatedBytes:65536},
      {path:receiptFile,purpose:'Bounded critical verification receipt',estimatedBytes:2097152}]}, async () => {
    fs.mkdirSync(evidence,{recursive:true});
    fs.mkdirSync(managed,{recursive:true});
    const serialized = JSON.stringify(plan,null,2) + '\n';
    fs.writeFileSync(planFile,serialized,{flag:'wx'});
    if (building && build) {
      const result = {status:'PASS',action,buildKey,buildDir,reused:true,policy};
      fs.writeFileSync(receiptFile,JSON.stringify(result,null,2)+'\n',{flag:'wx'});
      return result;
    }
    // The shared admission queue may wait 900 s before the worker starts its
    // original action deadline. Keep the Job/CPU/wall budget itself unchanged.
    const outcome = await child(planFile,sha(Buffer.from(serialized)),wallSeconds + 930);
    if (outcome.interrupted) fail('Worker interrupted; retain lease evidence and reconcile before retrying');
    if (!fs.existsSync(receiptFile)) {
      fs.writeFileSync(receiptFile,JSON.stringify({status:'FAILED',error:outcome.output.slice(-16000),code:outcome.code})+'\n',{flag:'wx'});
    }
    const receipt = json(receiptFile);
    return {status:receipt.status,action,buildKey,buildDir,sessionDir:building?null:sessionDir,
      receipt:receiptFile,error:receipt.error || null,policy,slots:receipt.slots};
  });
  // The storage transaction has finished before a PASS is exposed to the caller.
  // A restored VM may verify capacity without replaying a historical migration.
  if (action === 'capacity' && result.status === 'PASS' && config.legacyCapacityMigration === true)
    result.migration = await require('./pulse_capacity_migration.cjs').migrate({root,storage,quiescent,testReceipt:receiptFile});
  console.log(JSON.stringify(result));
  if (result.status !== 'PASS') process.exitCode = 1;
}
main().catch(error => {console.error(error.message);process.exitCode=1;});
