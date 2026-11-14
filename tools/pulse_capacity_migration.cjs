'use strict';
// One authorized migration, dispatched only by Pulse-Dev -Action Capacity.
// Artifact snapshots use the shared lease; control-file mutation uses its same
// exclusive lock and atomic-state protocol, without changing the shared module.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const OLD = 256 * 1024 * 1024, NEXT = 320 * 1024 * 1024;
const owner = 'pulse-build-capacity-20261023';
const authorization = '2026-10-23: user approved build budget 320 MiB, keep all old artifacts, project 1 GiB, no installation';
const same = (a,b) => JSON.stringify(a) === JSON.stringify(b);
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
function check(ok, message) { if (!ok) throw new Error('Pulse capacity migration: ' + message); }
function nextLimits(root, policy, state) {
  const managed = path.join(root,'dev-work','builds');
  const rule = policy.managed_directories?.['dev-work/builds'];
  const record = state.managedDirectories?.[managed];
  check(policy.schema_version === 1 && policy.automatic_cleanup === false &&
    policy.project_budget_bytes === 1024 * 1024 * 1024 && policy.minimum_free_bytes === 40 * 1024**3 &&
    policy.managed_directories?.['dev-work/sessions']?.maximum_bytes === 64 * 1024 * 1024,
    'global budget, free-space reserve or session policy differs');
  check(state.schema_version === 1 && state.last?.status === 'complete' && state.objects &&
    record?.owner === 'pulse-development-builds' && rule?.owner === record.owner &&
    record.path === managed && record.resourceId === managed, 'unfinished state or owner/resource mismatch');
  check(rule.maximum_bytes === OLD && record.maximumBytes === OLD, 'expected exact original 256 MiB limits');
  const newPolicy = structuredClone(policy), newState = structuredClone(state);
  newPolicy.managed_directories['dev-work/builds'].maximum_bytes = NEXT;
  newState.managedDirectories[managed].maximumBytes = NEXT;
  return {policy:newPolicy, state:newState};
}
function selfTest() {
  const assert = require('node:assert/strict'), root = path.resolve('synthetic-pulse');
  const managed = path.join(root,'dev-work','builds');
  const policy = {schema_version:1,automatic_cleanup:false,project_budget_bytes:1024**3,minimum_free_bytes:40*1024**3,
    managed_directories:{'dev-work/builds':{owner:'pulse-development-builds',maximum_bytes:OLD},
      'dev-work/sessions':{maximum_bytes:64*1024*1024}}};
  const state = {schema_version:1,objects:{preserved:{identity:'unchanged'}},last:{status:'complete'},
    managedDirectories:{[managed]:{path:managed,resourceId:managed,owner:'pulse-development-builds',maximumBytes:OLD}}};
  const next = nextLimits(root,policy,state);
  assert.equal(next.policy.managed_directories['dev-work/builds'].maximum_bytes,NEXT);
  assert.equal(next.state.managedDirectories[managed].maximumBytes,NEXT);
  next.policy.managed_directories['dev-work/builds'].maximum_bytes = OLD;
  next.state.managedDirectories[managed].maximumBytes = OLD;
  assert.deepEqual(next,{policy,state}); // Only the two authorized limits may change.
  for (const modify of [p=>p.project_budget_bytes++, p=>p.minimum_free_bytes--,
    p=>p.managed_directories['dev-work/builds'].maximum_bytes=NEXT,
    p=>p.managed_directories['dev-work/sessions'].maximum_bytes++]) {
    const bad = structuredClone(policy); modify(bad);
    assert.throws(()=>nextLimits(root,bad,state));
  }
  for (const modify of [s=>s.last.status='active',s=>s.last.status='failed',
    s=>s.managedDirectories[managed].owner='unknown',s=>s.managedDirectories[managed].maximumBytes=NEXT]) {
    const bad = structuredClone(state); modify(bad);
    assert.throws(()=>nextLimits(root,policy,bad));
  }
  return {pass:true,changedFields:2,rejectedCases:8,oldArtifactsPreserved:true};
}
async function migrate({root,storage,quiescent,testReceipt}) {
  const control = path.join(root,'.local-artifact-storage'), stateFile = path.join(control,'state.json');
  const policyFile = path.join(root,'local-artifact-policy.json'), managed = path.join(root,'dev-work','builds');
  const read = file => { storage.fileIdentity(root,file); return fs.readFileSync(file); };
  const policyRaw = read(policyFile), stateRaw = read(stateFile);
  const policy = JSON.parse(policyRaw), original = JSON.parse(stateRaw);
  nextLimits(root,policy,original);
  check(JSON.parse(read(testReceipt)).capacityChecks?.pass === true,'protected migration checks missing');
  const evidence = path.join(root,'dev-work','evidence','capacity-320-' + crypto.randomUUID());
  const policyBackup = path.join(evidence,'policy-before.json'), stateBackup = path.join(evidence,'state-before.json');
  const planFile = path.join(evidence,'migration.json');
  const targets = [policyBackup,stateBackup,planFile];
  await storage.withLease({root,operation:'development',owner,verifyAllContent:true,
    purpose:authorization,targets:targets.map(file=>({path:file,purpose:'Exact capacity rollback or migration evidence',estimatedBytes:1024*1024}))},
    async admission => {
      check(same(read(policyFile),policyRaw),'policy changed before backup');
      const active = JSON.parse(read(stateFile));
      check(same(active.objects,original.objects) && same(active.managedDirectories,original.managedDirectories),
        'registration changed before backup');
      fs.mkdirSync(evidence);
      fs.writeFileSync(policyBackup,policyRaw,{flag:'wx'});
      fs.writeFileSync(stateBackup,stateRaw,{flag:'wx'});
      fs.writeFileSync(planFile,JSON.stringify({authorization,oldBytes:OLD,newBytes:NEXT,managed,testReceipt,
        policySha256:hash(policyRaw),stateSha256:hash(stateRaw),admission,
        rollback:'Preserve all registrations and evidence; restore only the old policy bytes and registered build maximum under lock after verifying usage <= 256 MiB. Never overwrite current state with the older snapshot.'},null,2)+'\n',{flag:'wx'});
    });
  const lock = storage.safe(root,path.join(control,'lock.json'));
  const durableNew = (file,bytes) => { const fd=fs.openSync(storage.safe(root,file),'wx');
    try { fs.writeFileSync(fd,bytes); fs.fsyncSync(fd); } finally { fs.closeSync(fd); } };
  durableNew(lock,JSON.stringify({owner,id:crypto.randomUUID()}));
  const lockIdentity = storage.fileIdentity(root,lock);
  let state, active = false;
  const atomic = (file,pending,bytes) => { check(Buffer.byteLength(bytes)<=8*1024*1024,'control payload too large');
    durableNew(pending,bytes); fs.renameSync(pending,file); };
  const saveState = () => atomic(stateFile,path.join(control,'state.pending.json'),JSON.stringify(state)+'\n');
  const audit = event => { const file=storage.safe(root,path.join(control,'critical-audit.jsonl'));
    storage.fileIdentity(root,file); const fd=fs.openSync(file,'a');
    try { fs.writeSync(fd,JSON.stringify({utc:new Date().toISOString(),event,...state.last.capacityMigration})+'\n'); fs.fsyncSync(fd); }
    finally { fs.closeSync(fd); } };
  try {
    check(!fs.existsSync(path.join(control,'state.pending.json')) && !fs.existsSync(policyFile+'.pending'),
      'interrupted atomic write; preserve and reconcile');
    check(same(read(policyFile),policyRaw),'policy drift after backup');
    state = JSON.parse(read(stateFile));
    const next = nextLimits(root,policy,state);
    check(state.last.owner === owner && state.last.targets.length === targets.length &&
      targets.every(file=>state.last.targets.some(item=>item.path===file)), 'another lease ran after backup');
    check(same(state.managedDirectories,original.managedDirectories) &&
      Object.keys(state.objects).length === Object.keys(original.objects).length + targets.length &&
      Object.entries(original.objects).every(([file,item])=>same(state.objects[file],item)), 'old registration changed');
    storage.verifyRegistered(root,state,new Set(),true);
    for (const record of Object.values(state.managedDirectories)) {
      const stat=fs.lstatSync(storage.safe(root,record.path));
      check(stat.isDirectory() && String(stat.dev)===record.identity.deviceId && String(stat.ino)===record.identity.fileId,
        'managed directory identity drift');
      const files=[], pending=[record.path];
      while(pending.length) {
        const directory=pending.pop();
        for(const entry of fs.readdirSync(directory,{withFileTypes:true})) {
          const file=storage.safe(root,path.join(directory,entry.name));
          if(entry.isDirectory()) pending.push(file); else files.push(file);
          check(files.length+pending.length<=512,'managed inventory bound exceeded');
        }
      }
      const known=Object.values(state.objects).filter(item=>item.managedDirectory===record.path).map(item=>item.path);
      check(same(files.sort(),known.sort()),'unknown or missing managed file');
    }
    quiescent(managed);
    storage.checkCapacity(root,policy,NEXT-OLD+4*1024*1024);
    state.last.capacityMigration = {authorization,oldBytes:OLD,newBytes:NEXT,policyBackup,stateBackup,planFile,
      oldObjects:Object.keys(original.objects).length,oldBuildBytes:storage.usage(managed).bytes,
      policyBeforeSha256:hash(policyRaw),startedUtc:new Date().toISOString()};
    state.last.status='active'; saveState(); active=true;
    audit('capacity-migration-started');
    const newPolicyRaw=JSON.stringify(next.policy,null,2)+'\n';
    atomic(policyFile,policyFile+'.pending',newPolicyRaw);
    state.managedDirectories[managed].maximumBytes=NEXT;
    check(same(read(policyFile),Buffer.from(newPolicyRaw)),'policy readback differs');
    storage.verifyRegistered(root,state,new Set(),true);
    quiescent(managed);
    state.last.capacityMigration.policyAfterSha256=hash(newPolicyRaw);
    state.last.capacityMigration.completedUtc=new Date().toISOString();
    state.last.status='complete'; state.last.completedUtc=new Date().toISOString();
    audit('capacity-migration-complete'); saveState(); active=false;
    check(same(JSON.parse(read(stateFile)),state),'state readback differs');
    return {status:'PASS',evidence,oldBytes:OLD,newBytes:NEXT,oldObjectsPreserved:state.last.capacityMigration.oldObjects};
  } catch(error) {
    if(active) { state.last.status='failed'; state.last.error=String(error.message).slice(0,300);
      try { saveState(); } catch {} }
    throw error;
  } finally {
    check(same(storage.fileIdentity(root,lock),lockIdentity),'lock identity changed; preserve');
    fs.unlinkSync(lock);
  }
}
module.exports={migrate};
if(require.main===module) {
  check(process.argv.length===3 && process.argv[2]==='--self-test','use Pulse-Dev -Action Capacity');
  console.log(JSON.stringify(selfTest()));
}
