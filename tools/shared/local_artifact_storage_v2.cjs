'use strict';
// Local admission/registration only. No device access or file-deletion primitive.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const CONTROL = '.local-artifact-storage';
// Keep schema 1 readable by existing evidence/recovery tools. The state-specific
// bound is separate from small policy JSON; its actual peak is reserved below.
const MAX_STATE_BYTES = 8 * 1024 * 1024;
const equalPath = (a, b) => process.platform === 'win32' ? a.toLowerCase() === b.toLowerCase() : a === b;
function fail(message) { throw new Error('Local artifact storage: ' + message); }
function safe(root, target, allowRoot = false) {
  root = path.resolve(root); target = path.resolve(target);
  const rel = path.relative(root, target);
  if ((!rel && !allowRoot) || rel === '..' || rel.startsWith('..' + path.sep) || path.isAbsolute(rel)) fail('path outside exact project root');
  let current = root;
  for (const part of ['', ...rel.split(path.sep).filter(Boolean)]) {
    if (part) current = path.join(current, part);
    try {
      const st = fs.lstatSync(current);
      if (st.isSymbolicLink() || !equalPath(fs.realpathSync(current), current)) fail('link/reparse path rejected');
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  return target;
}
function usage(root) {
  root = safe(root, root, true);
  const stack = [root]; let bytes = 0, files = 0;
  while (stack.length) {
    const directory = stack.pop();
    for (const name of fs.readdirSync(directory)) {
      const target = path.join(directory, name), st = fs.lstatSync(target);
      if (st.isSymbolicLink()) fail('link/reparse point prevents complete accounting');
      if (st.isDirectory()) stack.push(safe(root, target));
      else if (st.isFile()) { bytes += st.size; files++; }
      else fail('unknown filesystem object prevents complete accounting');
      if (!Number.isSafeInteger(bytes)) fail('accounting overflow');
    }
  }
  return { bytes, files };
}
function fileIdentity(root, target) {
  target = safe(root, target);
  const before = fs.lstatSync(target);
  if (!before.isFile() || before.nlink !== 1) fail('target must be a single-link regular file');
  const fd = fs.openSync(target, 'r'), hash = crypto.createHash('sha256'), chunk = Buffer.alloc(65536);
  const unchanged = (a, b) => b.isFile() && b.nlink === 1 && a.dev === b.dev && a.ino === b.ino && a.size === b.size && a.mtimeMs === b.mtimeMs;
  try {
    const opened = fs.fstatSync(fd);
    if (!unchanged(before, opened)) fail('opened file identity differs from checked path');
    for (;;) { const n = fs.readSync(fd, chunk, 0, chunk.length, null); if (!n) break; hash.update(chunk.subarray(0, n)); }
    const after = fs.fstatSync(fd), pathAfter = fs.lstatSync(target);
    if (!unchanged(opened, after) || !unchanged(after, pathAfter)) fail('file changed while verifying identity');
    return { path: target, bytes: after.size, deviceId: String(after.dev), fileId: String(after.ino), mtimeMs: after.mtimeMs, sha256: hash.digest('hex') };
  } finally { fs.closeSync(fd); }
}
function same(a, b) { return JSON.stringify(a) === JSON.stringify(b); }
function readJson(root, file, fallback, maximum = 1024 * 1024) {
  safe(root, file);
  if (!fs.existsSync(file)) return fallback;
  const stat = fs.lstatSync(file);
  if (!stat.isFile() || stat.nlink !== 1 || stat.size > maximum) fail('invalid/control JSON exceeds bounded size; preserve and reconcile');
  return JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
}
function policyFor(root) {
  const policy = readJson(root, path.join(root, 'local-artifact-policy.json'));
  if (policy?.schema_version !== 1 || policy.automatic_cleanup !== false) fail('unsupported policy');
  for (const key of ['project_budget_bytes', 'minimum_free_bytes', 'control_reserve_bytes']) {
    if (!Number.isSafeInteger(policy[key]) || policy[key] < (key === 'minimum_free_bytes' ? 0 : 1)) fail('invalid capacity policy');
  }
  if (!Array.isArray(policy.allowed_output_roots) || policy.allowed_output_roots.some(x => !/^[a-zA-Z0-9_-]+$/.test(x))) fail('invalid output roots');
  return policy;
}
function durableNew(file, value) {
  const fd = fs.openSync(file, 'wx');
  try { fs.writeFileSync(fd, value); fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
}
function atomicState(root, home, state) {
  const target = safe(root, path.join(home, 'state.json'));
  const pending = safe(root, path.join(home, 'state.pending.json'));
  const text = JSON.stringify(state) + '\n';
  if (Buffer.byteLength(text) > MAX_STATE_BYTES) fail('state exceeds bounded atomic-write size');
  durableNew(pending, text);
  fs.renameSync(pending, target);
}
function audit(root, home, event) {
  const file = safe(root, path.join(home, 'critical-audit.jsonl'));
  if (fs.existsSync(file) && fs.lstatSync(file).nlink !== 1) fail('audit hardlink rejected');
  const text = JSON.stringify({ utc: new Date().toISOString(), ...event }) + '\n';
  if (Buffer.byteLength(text) > 256 * 1024) fail('audit event exceeds bounded size');
  const fd = fs.openSync(file, 'a');
  try { fs.writeFileSync(fd, text); fs.fsyncSync(fd); }
  finally { fs.closeSync(fd); }
}
function checkCapacity(root, policy, reservation, statfs = fs.statfsSync) {
  const used = usage(root);
  if (used.bytes + reservation > policy.project_budget_bytes) fail('project budget exceeded before generation');
  const disk = statfs(root), free = Number(disk.bavail) * Number(disk.bsize);
  if (!Number.isFinite(free) || free < reservation + policy.minimum_free_bytes) fail('physical free-space reserve insufficient');
  return { ...used, reservedBytes: reservation, budgetBytes: policy.project_budget_bytes, freeBytes: free };
}
function directoryIdentity(root, target) {
  target = safe(root, target);
  const st = fs.lstatSync(target);
  if (!st.isDirectory()) fail('managed target must be a directory');
  return { path: target, deviceId: String(st.dev), fileId: String(st.ino) };
}
function verifyRegistered(root, state, fullPaths, fullAll = false) {
  const result = { metadataFiles: 0, hashedFiles: 0, hashedBytes: 0,
    mode: fullAll ? 'all-content' : 'all-metadata-and-task-content' };
  for (const [key, record] of Object.entries(state.objects)) {
    if (key !== record.path || record.identity?.path !== record.path || typeof record.owner !== 'string') fail('invalid registered identity');
    const file = safe(root, record.path), stat = fs.lstatSync(file), expected = record.identity;
    if (!stat.isFile() || stat.nlink !== 1 || String(stat.dev) !== expected.deviceId || String(stat.ino) !== expected.fileId
        || stat.size !== expected.bytes || stat.mtimeMs !== expected.mtimeMs) fail('registered output drift; preserve and reconcile');
    result.metadataFiles++;
    if (fullAll || fullPaths.has(file.toLowerCase()) || record.managedDirectory) {
      if (!same(fileIdentity(root, file), expected)) fail('registered output drift; preserve and reconcile');
      result.hashedFiles++; result.hashedBytes += expected.bytes;
    }
  }
  return result;
}
function entryLimit(value = 512) {
  if (value !== 512 && value !== 1024) fail('explicit managed entry limit must be 512 or 1024');
  return value;
}
function checkManagedBound(entries, bytes, maximum, maximumEntries = 512) {
  if (entries > entryLimit(maximumEntries) || bytes > maximum) fail('managed directory bound exceeded');
}
function treeFiles(root, target, maximum, maximumEntries = 512) {
  const files = [], stack = [target]; let bytes = 0;
  while (stack.length) {
    const directory = stack.pop(); directoryIdentity(root, directory);
    for (const name of fs.readdirSync(directory)) {
      const file = safe(root, path.join(directory, name)), st = fs.lstatSync(file);
      if (st.isDirectory()) stack.push(file);
      else { const identity = fileIdentity(root, file); files.push(identity); bytes += identity.bytes; }
      checkManagedBound(files.length + stack.length, bytes, maximum, maximumEntries);
    }
  }
  return { files, bytes };
}
async function withLease(options, produce) {
  const root = safe(options.root, options.root, true), policy = policyFor(root);
  if (typeof produce !== 'function' || typeof options.owner !== 'string' || !options.owner || options.owner.length > 128
      || typeof options.purpose !== 'string' || !options.purpose || options.purpose.length > 1024) fail('bounded owner, purpose and producer are required');
  const configured = policy.reservations?.[options.operation];
  if (!Number.isSafeInteger(configured) || configured < 1) fail('operation reservation is missing');
  if (!Array.isArray(options.targets) || !options.targets.length || options.targets.length > 64) fail('explicit bounded targets required');
  const seen = new Set();
  const targets = options.targets.map(item => {
    const target = safe(root, item.path), rel = path.relative(root, target);
    if (!policy.allowed_output_roots.includes(rel.split(path.sep)[0]) || typeof item.purpose !== 'string' || !item.purpose || item.purpose.length > 1024
        || !Number.isSafeInteger(item.estimatedBytes) || item.estimatedBytes < 1 || seen.has(target.toLowerCase())) fail('invalid target registration');
    seen.add(target.toLowerCase());
    return { path: target, purpose: item.purpose, owner: options.owner, estimatedBytes: item.estimatedBytes };
  });
  const retireInput = options.retirements || [];
  if (!Array.isArray(retireInput) || retireInput.length > 64) fail('bounded exact retirements required');
  if (retireInput.length && (typeof options.retirementAuthorization !== 'string'
      || !options.retirementAuthorization.trim() || options.retirementAuthorization.length > 2048))
    fail('explicit current retirement authorization required');
  const retirementPaths = new Set();
  const retirements = retireInput.map(item => {
    const target = safe(root, item.path);
    if (!policy.allowed_output_roots.includes(path.relative(root, target).split(path.sep)[0])
        || seen.has(target.toLowerCase()) || retirementPaths.has(target.toLowerCase())
        || typeof item.owner !== 'string' || !item.owner || !item.identity || item.identity.path !== target)
      fail('invalid exact retirement registration');
    retirementPaths.add(target.toLowerCase());
    return {path: target, owner: item.owner, identity: item.identity};
  });
  const managed = (options.managedDirectories || []).map(item => {
    const target = safe(root, item.path), rel = path.relative(root, target).split(path.sep).join('/');
    const rule = policy.managed_directories?.[rel];
    if (!rule || rule.owner !== options.owner || !Number.isSafeInteger(rule.maximum_bytes) || rule.maximum_bytes < 1
        || typeof options.assertManagedQuiescent !== 'function' || targets.some(t => !path.relative(target,t.path).startsWith('..')))
      fail('managed directory explicit policy/owner/quiescence required');
    return { path: target, owner: options.owner, maximumBytes: rule.maximum_bytes, maximumEntries: entryLimit(rule.maximum_entries) };
  });
  if (managed.length > 1) fail('one managed directory per lease');
  const home = safe(root, path.join(root, CONTROL));
  try { fs.mkdirSync(home, { recursive: false }); }
  catch (error) { if (error.code !== 'EEXIST' || !fs.lstatSync(home).isDirectory()) throw error; safe(root, home); }
  return runLocked();

  async function runLocked() {
    const lock = safe(root, path.join(home, 'lock.json'));
    const lockValue = JSON.stringify({ owner: options.owner, id: crypto.randomUUID() });
    // wx never steals an old lock by PID/age. A crash leaves it for reconciliation.
    durableNew(lock, lockValue);
    const lockIdentity = fileIdentity(root, lock);
    let state, run, active = false;
    try {
      if (fs.existsSync(path.join(home, 'state.pending.json'))) fail('interrupted atomic state requires reconciliation');
      state = readJson(root, path.join(home, 'state.json'), { schema_version: 1, objects: {}, last: null }, MAX_STATE_BYTES);
      if (state.schema_version !== 1 || !state.objects || (state.last && state.last.status !== 'complete')) fail('unfinished generation requires reconciliation');
      const dependencies = options.verifyPaths || [];
      if (!Array.isArray(dependencies) || dependencies.length > 256) fail('bounded explicit integrity dependencies required');
      if (options.verifyAllContent !== undefined && typeof options.verifyAllContent !== 'boolean') fail('explicit full-content flag must be boolean');
      const registeredPaths = new Set(Object.keys(state.objects).map(x => x.toLowerCase()));
      const dependencyPaths = dependencies.map(x => safe(root, x));
      if (dependencyPaths.some(x => !registeredPaths.has(x.toLowerCase()))) fail('integrity dependency must have an existing registration');
      const fullPaths = new Set([...targets.map(x => x.path), ...dependencyPaths,
        ...Object.values(state.objects).filter(x => x.owner === options.owner).map(x => x.path)].map(x => x.toLowerCase()));
      const integrity = verifyRegistered(root, state, fullPaths, retirements.length > 0 || options.verifyAllContent === true);
      for (const item of retirements) {
        const record = state.objects[item.path];
        if (!record || record.owner !== item.owner || record.managedDirectory || !same(record.identity, item.identity))
          fail('retirement must match an existing immutable registration and owner');
      }
      state.managedDirectories ||= {};
      for (const record of Object.values(state.managedDirectories)) {
        if (!same(directoryIdentity(root, record.path), record.identity)) fail('managed directory identity drift');
        const rule = policy.managed_directories[path.relative(root, record.path).split(path.sep).join('/')];
        if (entryLimit(rule?.maximum_entries) !== entryLimit(record.maximumEntries)) fail('managed entry policy drift');
        const actual = treeFiles(root, record.path, record.maximumBytes, record.maximumEntries);
        const known = Object.values(state.objects).filter(x => x.managedDirectory === record.path).map(x => x.path).sort();
        if (!same(actual.files.map(x => x.path).sort(), known)) fail('unknown/missing managed file; preserve and reconcile');
      }
      let growthReserve = 0;
      for (const item of managed) {
        const previous = state.managedDirectories[item.path];
        if (previous && (previous.owner !== options.owner || previous.maximumBytes !== item.maximumBytes ||
          entryLimit(previous.maximumEntries) !== item.maximumEntries)) fail('managed ownership/policy drift');
        if (!previous && fs.existsSync(item.path)) fail('unknown existing managed directory cannot be adopted');
        const check = await options.assertManagedQuiescent('before', previous || null);
        if (check?.quiescent !== true || (previous && check.resourceId !== previous.resourceId)) fail('managed resource not quiescent or identity mismatch');
        growthReserve += item.maximumBytes - (previous ? treeFiles(root,item.path,item.maximumBytes,item.maximumEntries).bytes : 0);
      }
      const observed = targets.map(item => ({ ...item, before: fs.existsSync(item.path) ? fileIdentity(root, item.path) : null }));
      const missing = observed.filter(item => !item.before);
      if (observed.some(item => item.before) && options.allowExistingReadOnly !== true) fail('existing files are protected; explicit read-only reuse required');
      const payloadReserve = missing.length ? Math.max(configured, missing.reduce((sum, item) => sum + item.estimatedBytes, 0)) : 0;
      // Budget the future index before producer side effects. Preserve the schema
      // and all identities rather than pruning history or hiding a size failure.
      const projected = { ...state, objects: { ...state.objects }, last: { targets: observed, managedDirectories: managed, retirements,
        owner: options.owner, purpose: options.purpose, retirementAuthorization: options.retirementAuthorization } };
      for (const item of missing) projected.objects[item.path] = { path: item.path, owner: options.owner, purpose: item.purpose,
        identity: { path: item.path, bytes: item.estimatedBytes, deviceId: '9'.repeat(24), fileId: '9'.repeat(24), mtimeMs: Number.MAX_SAFE_INTEGER, sha256: 'f'.repeat(64) } };
      const projectedStateBytes = Buffer.byteLength(JSON.stringify(projected)) + 16384
        + managed.reduce((sum, item) => sum + 512 * (item.path.length * 6 + options.purpose.length * 3 + 2048), 0);
      if (projectedStateBytes > MAX_STATE_BYTES) fail('projected state exceeds bounded size before generation; preserve and reconcile');
      const controlReserve = Math.max(policy.control_reserve_bytes, projectedStateBytes * 2 + 512 * 1024);
      const admission = checkCapacity(root, policy, payloadReserve + growthReserve + controlReserve);
      admission.controlReserveBytes = controlReserve;
      admission.projectedStateBytes = projectedStateBytes;
      run = { id: crypto.randomUUID(), owner: options.owner, purpose: options.purpose, operation: options.operation,
        status: 'active', admission, integrity, targets: observed, managedDirectories: managed, startedUtc: new Date().toISOString() };
      if (retirements.length) {
        run.retirements = retirements;
        run.retirementAuthorization = options.retirementAuthorization;
      }
      state.last = run;
      atomicState(root, home, state); active = true;
      audit(root, home, { event: 'registered-before-generation', runId: run.id, targets: observed.map(x => ({ path: x.path, mode: x.before ? 'existing-readonly' : 'new', estimatedBytes: x.estimatedBytes })), retirements });
      const value = await produce(admission);
      // The caller performs only its separately authorized, same-handle deletion.
      // A partial operation never becomes a complete run or an automatic retry.
      for (const item of retirements) {
        safe(root, item.path);
        try { fs.lstatSync(item.path); fail('retired target still exists; preserve and reconcile'); }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
      }
      if (retirements.length) {
        for (const record of Object.values(state.objects)) {
          if (!retirementPaths.has(record.path.toLowerCase()) && !same(fileIdentity(root, record.path), record.identity))
            fail('non-retired registered output changed; preserve and reconcile');
        }
        for (const item of retirements) delete state.objects[item.path];
      }
      for (const item of managed) {
        const previous = state.managedDirectories[item.path];
        const check = await options.assertManagedQuiescent('after', previous || null);
        if (check?.quiescent !== true || typeof check.resourceId !== 'string' || !check.resourceId
            || (previous && check.resourceId !== previous.resourceId)) fail('managed resource must be stopped with stable identity');
        const actual = treeFiles(root,item.path,item.maximumBytes,item.maximumEntries), present = new Set(actual.files.map(x => x.path));
        for (const record of Object.values(state.objects)) if (record.managedDirectory === item.path && !present.has(record.path))
          fail('managed deletion not authorized; preserve and reconcile');
        for (const identity of actual.files) state.objects[identity.path] = {path:identity.path,owner:options.owner,
          purpose:options.purpose,managedDirectory:item.path,identity};
        state.managedDirectories[item.path] = {...item,identity:directoryIdentity(root,item.path),resourceId:check.resourceId};
        const again = await options.assertManagedQuiescent('after-hash', state.managedDirectories[item.path]);
        if (again?.quiescent !== true || again.resourceId !== check.resourceId) fail('managed state changed during hashing');
      }
      for (const item of observed) {
        const actual = fileIdentity(root, item.path);
        if (item.before && !same(actual, item.before)) fail('existing read-only target changed');
        if (!item.before) {
          if (actual.bytes > item.estimatedBytes) fail('new output exceeds registered estimate');
          state.objects[item.path] = { path: item.path, owner: options.owner, purpose: item.purpose, identity: actual };
        }
      }
      checkCapacity(root, policy, controlReserve);
      run.status = 'complete'; run.completedUtc = new Date().toISOString();
      audit(root, home, { event: 'generation-complete', runId: run.id, createdFiles: missing.length, reusedFiles: observed.length - missing.length, retiredFiles: retirements.length });
      atomicState(root, home, state); active = false;
      return value;
    } catch (error) {
      if (active) {
        run.status = 'failed'; run.error = String(error.message).slice(0, 300);
        try { audit(root, home, { event: 'generation-failed', runId: run.id, error: run.error }); atomicState(root, home, state); }
        catch { /* Preserve pending/active state; never replace failure with success. */ }
      }
      throw error;
    } finally {
      if (!same(fileIdentity(root, lock), lockIdentity)) fail('own lock changed; do not unlink it');
      fs.unlinkSync(lock); // Only this invocation's identity-verified lock, never artifacts.
    }
  }
}

module.exports = { withLease, usage, safe, fileIdentity, checkCapacity, verifyRegistered, checkManagedBound, entryLimit };
