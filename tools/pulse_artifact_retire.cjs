'use strict';
// Coordinator called only by Pulse-Dev.ps1. One ledger, immutable intent, same Job.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const reviews=require('./pulse_artifact_review.cjs');
const hash=x=>crypto.createHash('sha256').update(x).digest('hex');
const check=(x,message)=>{if(!x)throw Error('Artifact retirement: '+message);};
const encode=x=>JSON.stringify(x,null,2)+'\n';
function choose(root,storage,snap,request,automatic) {
  const fresh=reviews.review(root,storage,snap);
  let candidates=fresh.candidates,authorization;
  if(automatic) {
    check(snap.policy.automatic_cleanup===true&&snap.policy.retention?.authorization,'automatic retirement not authorized');
    authorization=snap.policy.retention.authorization;
  } else {
    check(request&&typeof request.authorization==='string'&&request.authorization.trim()&&request.authorization.length<=2048,
      'exact current authorization required');
    const file=storage.safe(root,path.resolve(root,request.reviewPath||''));
    check(/^dev-work\/evidence\/[a-f0-9-]+-artifact-review\.json$/.test(path.relative(root,file).split(path.sep).join('/')),
      'review must be an exact registered evidence file');
    check(fs.statSync(file).size<=1048576&&/^[a-f0-9]{64}$/.test(request.reviewSha256),'bounded review hash required');
    const raw=fs.readFileSync(file),registered=snap.state.objects[file];
    check(registered&&hash(raw)===request.reviewSha256&&registered.identity.sha256===request.reviewSha256,'review identity differs');
    const approved=JSON.parse(raw);check(approved.status==='REVIEW_ONLY'&&approved.deletedFiles===0,'not a retirement proposal');
    candidates=approved.candidates;authorization=request.authorization;
    check(Array.isArray(candidates)&&candidates.length===request.expectedFiles&&
      candidates.reduce((n,x)=>n+x.identity.bytes,0)===request.expectedBytes,'authorized count/bytes differ');
  }
  check(candidates.length<=512,'retirement batch exceeds 512; preserve instead of widening scope');
  const permitted=new Map(fresh.candidates.map(x=>[x.path,x]));
  for(const row of candidates)check(permitted.has(row.path)&&
    JSON.stringify(permitted.get(row.path).identity)===JSON.stringify(row.identity)&&permitted.get(row.path).owner===row.owner,
    'candidate became protected or changed: '+row.path);
  check(new Set(candidates.map(x=>x.path.toLowerCase())).size===candidates.length,'duplicate retirement');
  return {fresh,candidates,authorization};
}
async function run({root,storage,config,policy,quiescent,child,request,automatic=false}) {
  const before=reviews.snapshot(root,storage);
  if(automatic&&before.policy.automatic_cleanup!==true)return {status:'DISABLED',deletedFiles:0};
  quiescent();const selected=choose(root,storage,before,request,automatic);
  if(!selected.candidates.length)return {status:'NOTHING_TO_RETIRE',deletedFiles:0};
  const id=crypto.randomUUID(),evidence=path.join(root,'dev-work/evidence');
  const target=path.join(evidence,id+'-retirement'),planFile=path.join(evidence,id+'-plan.json'),
    receiptFile=path.join(evidence,id+'-result.json'),journal=path.join(target,'deletions.jsonl');
  const retirements=selected.candidates.map(x=>({path:path.resolve(root,x.path),owner:x.owner,identity:x.identity,
    nativeIdentity:storage.nativeIdentity(root,x.identity)}));
  const sourceNames=['tools/pulse_dev.cjs','tools/pulse_dev_worker.py','tools/pulse_artifact_delete.py',
    'tools/pulse_artifact_retire.cjs','tools/pulse_artifact_review.cjs','tools/shared/pulse_artifact_storage_v3.cjs',
    'tools/dev-environment.local.json','local-artifact-policy.json'];
  const inputs=sourceNames.map(x=>{const file=storage.safe(root,path.join(root,x));return {path:file,sha256:hash(fs.readFileSync(file))};});
  const plan={schema:1,root,action:'artifact-retire',id,owner:'pulse-artifact-retirement',parentPid:process.pid,
    config,inputs,policy,buildKey:null,targetDirectory:target,receiptFile,retirementJournal:journal,
    retirements,authorization:selected.authorization,automatic,review:request||null,
    protectedGroups:selected.fresh.protectedGroups,wallSeconds:90,jobMiB:512,outputLimitBytes:262144};
  const result=await storage.withLease({root,operation:'development',owner:plan.owner,
    purpose:'Retire exact unreferenced generated payload; retain recovery and acceptance evidence',
    retirements,retirementAuthorization:selected.authorization,allowManagedRetirements:true,
    assertManagedQuiescent:(_phase,record)=>({...quiescent(),resourceId:record.resourceId}),
    validateRetirements:state=>{
      check(JSON.stringify(state.objects)===JSON.stringify(before.state.objects),'ledger changed since reference selection');
      choose(root,storage,{...before,state},request,automatic);
    },
    targets:[{path:planFile,purpose:'Durable exact deletion intent and protected references',estimatedBytes:524288},
      {path:receiptFile,purpose:'Guarded retirement and resource cleanup receipt',estimatedBytes:2097152},
      {path:journal,purpose:'Per-file deletion intentions/results',estimatedBytes:524288}]},async()=>{
    const raw=encode(plan);check(Buffer.byteLength(raw)<=524288,'retirement plan bound');
    const fd=fs.openSync(planFile,'wx');try{fs.writeFileSync(fd,raw);fs.fsyncSync(fd);}finally{fs.closeSync(fd);}
    const outcome=await child(planFile,hash(raw),1020);
    check(!outcome.interrupted,'worker interrupted; preserve and reconcile');
    check(fs.existsSync(receiptFile),'retirement receipt missing; preserve and reconcile');
    const receipt=JSON.parse(fs.readFileSync(receiptFile));
    if(!fs.existsSync(journal))fs.writeFileSync(journal,'{"event":"worker-refused-before-native-deletion"}\n',{flag:'wx'});
    const aborted=receipt.status!=='PASS'&&retirements.every(x=>fs.existsSync(x.path));
    return {status:receipt.status,retirementAborted:aborted,deletedFiles:receipt.retirement?.deletedFiles||0,
      deletedBytes:receipt.retirement?.deletedBytes||0,receipt:receiptFile,plan:planFile,journal,
      error:receipt.error||null,automatic};
  });
  check(result.status==='PASS','guarded deletion refused; retained receipt: '+receiptFile+'; '+result.error);
  return result;
}
module.exports={choose,run};
