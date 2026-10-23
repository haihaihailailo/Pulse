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
const storage = require(config.storage);
const worker = path.join(__dirname, 'pulse_dev_worker.py');
const fail = message => { throw new Error(message); };
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
      policy.profiles?.daily?.hostReserveMiB !== 4096 || policy.profiles?.idle?.hostReserveMiB !== 2048)
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
async function main() {
  const [action = 'test', profile = 'daily', consent] = process.argv.slice(2);
  if (!['build','test','debug','run'].includes(action)) fail('Use build, test, debug or run');
  if (action === 'run' && consent !== '--interactive') fail('Visible development use requires -Action Run -Interactive');
  if (consent && !(action === 'run' && consent === '--interactive')) fail('Unexpected interactive flag');
  for (const item of config.identities) {
    if (fileHash(item.path) !== item.sha256) fail('Shared tool identity changed: ' + item.path);
  }
  const policy = memoryPolicy(profile);
  const sources = fs.readdirSync(path.join(root,'src')).filter(x => /\.(cpp|h)$/.test(x)).sort().map(x => ownFile(path.join(root,'src',x)));
  const inputs = [...sources, ...['app.rc','app.manifest','pulse.ico','tools/pulse_dev_worker.py','tools/pulse_dev.cjs',
    'local-artifact-policy.json','tools/dev-environment.local.json','tools/development-defaults.ini'].map(x => ownFile(path.join(root,x)))];
  const executionKey = sha(JSON.stringify(inputs)).slice(0,24);
  let buildKey = executionKey;
  let buildDir = path.join(root,'dev-work','builds',buildKey);
  let buildManifest = path.join(buildDir,'build.json');
  // Launch defaults do not change C++ binaries. Reuse this accepted baseline only
  // when every compiler/source/resource input AND the exact compile recipe match.
  if (!fs.existsSync(buildManifest)) {
    const baselinePath = path.join(root,'dev-work/evidence/3aa07fd3-4a34-4850-981f-cea6265b6beb-plan.json');
    if (fs.existsSync(baselinePath)) {
    const baseline = json(baselinePath);
    const binaryInputs = rows => rows.filter(item => !['pulse_dev_worker.py','pulse_dev.cjs','development-defaults.ini'].includes(path.basename(item.path)));
    const script = read(worker).toString('utf8').replace(/\r\n/g,'\n');
    const start = script.indexOf('        bin_dir = Path(');
    const end = script.indexOf("        else:\n            profile = target/'profile'",start);
    if (start >= 0 && end > start &&
        sha(script.slice(start,end)) === 'bb9277aa207580474af6ae4cd09c522d5a3400791e4d84589262b8dbd2fce5ba' &&
        JSON.stringify(binaryInputs(inputs)) === JSON.stringify(binaryInputs(baseline.inputs))) {
      buildKey = baseline.buildKey;
      buildDir = path.join(root,'dev-work','builds',buildKey);
      buildManifest = path.join(buildDir,'build.json');
    }
    }
  }
  let build = null;
  if (fs.existsSync(buildManifest)) {
    build = json(buildManifest);
    if (build.status !== 'PASS' || build.buildKey !== buildKey) fail('Existing build is incomplete; preserve it for diagnosis');
    for (const item of build.outputs) {
      if (fileHash(storage.safe(root,item.path)) !== item.sha256) fail('Build output drift: ' + item.path);
    }
  }
  if (action !== 'build' && !build) fail('No verified build for these inputs. Run -Action Build first');
  const id = crypto.randomUUID();
  const evidence = path.join(root,'dev-work','evidence');
  const planFile = path.join(evidence, id + '-plan.json');
  const receiptFile = path.join(evidence, id + '-result.json');
  const managed = path.join(root,'dev-work',action === 'build' ? 'builds' : 'sessions');
  const owner = action === 'build' ? 'pulse-development-builds' : 'pulse-development-sessions';
  const sessionDir = path.join(root,'dev-work','sessions',id);
  const wallSeconds = action === 'build' ? 240 : action === 'run' ? 1800 : action === 'debug' ? 120 : 90;
  const plan = {schema:1, root, action, id, owner, parentPid:process.pid, executionKey, buildKey, buildDir, build,
    sessionDir, receiptFile, config, inputs, policy, wallSeconds,
    jobMiB:action === 'build' ? 2048 : 512, outputLimitBytes:262144,
    targetDirectory:action === 'build' ? buildDir : sessionDir,
    purpose:'Isolated development only; no installation, production writes or publication'};
  if (action === 'build' && !build) {
    const existing = storage.usage(managed);
    if (existing.bytes + 24 * 1024 * 1024 > 268435456) fail('Insufficient registered build headroom for a new compilation; preserve existing builds');
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
    if (action === 'build' && build) {
      const result = {status:'PASS',action,buildKey,buildDir,reused:true,policy};
      fs.writeFileSync(receiptFile,JSON.stringify(result,null,2)+'\n',{flag:'wx'});
      return result;
    }
    const outcome = await child(planFile,sha(Buffer.from(serialized)),wallSeconds + 30);
    if (outcome.interrupted) fail('Worker interrupted; retain lease evidence and reconcile before retrying');
    if (!fs.existsSync(receiptFile)) {
      fs.writeFileSync(receiptFile,JSON.stringify({status:'FAILED',error:outcome.output.slice(-16000),code:outcome.code})+'\n',{flag:'wx'});
    }
    const receipt = json(receiptFile);
    return {status:receipt.status,action,buildKey,buildDir,sessionDir:action === 'build'?null:sessionDir,
      receipt:receiptFile,error:receipt.error || null,policy,slots:receipt.slots};
  });
  // The storage transaction has finished before a PASS is exposed to the caller.
  console.log(JSON.stringify(result));
  if (result.status !== 'PASS') process.exitCode = 1;
}
main().catch(error => {console.error(error.message);process.exitCode=1;});
