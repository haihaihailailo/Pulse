'use strict';
// Helpers shared by the protected release stages and their synthetic checks.
const fs = require('node:fs'), path = require('node:path'), crypto = require('node:crypto');
const REPO = 'haihaihailailo/Pulse';
const hash = raw => crypto.createHash('sha256').update(raw).digest('hex');
const encode = value => JSON.stringify(value, null, 2) + '\n';
const check = (ok, message) => { if (!ok) throw Error(message); };
const validTag = tag => typeof tag === 'string' && /^custom-\d{4}\.\d{2}\.\d{2}\.\d+$/.test(tag);
const validHash = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
function local(root, name) {
  const file = path.resolve(root, name), relative = path.relative(root, file);
  check(relative && !relative.startsWith('..') && !path.isAbsolute(relative), 'Path outside project');
  for (let part = file; part !== root; part = path.dirname(part)) {
    if (fs.existsSync(part)) check(!fs.lstatSync(part).isSymbolicLink(), 'Linked release path');
  }
  return file;
}
function read(root, name, maximum = 65536) {
  const file = local(root, name), stat = fs.lstatSync(file);
  check(stat.isFile() && stat.nlink === 1 && stat.size <= maximum, 'Invalid or oversized release input');
  return fs.readFileSync(file);
}
const json = (root, name) => JSON.parse(read(root, name).toString('utf8'));
function identity(root, name, maximum = 16 * 1024 * 1024) {
  const raw = read(root, name, maximum);
  return {path:path.relative(root, local(root, name)), bytes:raw.length, sha256:hash(raw)};
}
function directory(tag) {
  check(validTag(tag), 'Invalid release tag');
  return path.join('dev-work', 'releases', tag);
}
function assetNames(receipt) {
  check(receipt.status === 'PASS' && /^[a-f0-9]{40}$/.test(receipt.commit), 'Package is not accepted');
  check(Array.isArray(receipt.assets) && receipt.assets.length >= 3 && receipt.assets.length <= 16, 'Bounded assets required');
  const names = receipt.assets.map(row => row.name);
  check(names.every(name => typeof name === 'string' && /^[a-zA-Z0-9][a-zA-Z0-9._-]*$/.test(name)) &&
    new Set(names.map(name => name.toLowerCase())).size === names.length, 'Unsafe or duplicate asset name');
  check(names.includes('release-manifest.json') && names.includes('SHA256SUMS.txt'), 'Missing manifest/checksums');
  return names;
}
// Only bounded receipt discovery here. All content is checked inside the lease.
function dependencies(root, tag) {
  const dir = directory(tag), receipt = json(root, path.join(dir, 'package-receipt.json'));
  return [path.join(dir, 'package-receipt.json'), path.join(dir, 'release-notes.md'),
    ...assetNames(receipt).map(name => path.join(dir, name)), receipt.trialReceipt].map(name => local(root, name));
}
function acceptedPackage(root, tag) {
  const dir = directory(tag), receiptPath = path.join(dir, 'package-receipt.json');
  const receipt = json(root, receiptPath), names = assetNames(receipt);
  const manifest = json(root, path.join(dir, 'release-manifest.json'));
  check(manifest.repository === REPO && manifest.tag === tag && manifest.commit === receipt.commit &&
    /^\d+\.\d+\.\d+\.\d+$/.test(manifest.fileVersion) &&
    manifest.binaryUnmodified === true && manifest.executable?.name === 'Pulse.exe' &&
    Number.isSafeInteger(manifest.executable.bytes) && manifest.executable.bytes > 0 &&
    validHash(manifest.executable.sha256), 'Manifest identity differs');
  const zip = 'Pulse-' + tag + '-win-x64.zip';
  check(names.includes(zip), 'Missing full package');
  // Future extra assets must be explicitly bound by the package receipt AND checksums.
  const assets = receipt.assets.map(row => {
    const actual = identity(root, path.join(dir, row.name));
    check(validHash(row.sha256) && actual.sha256 === row.sha256 && actual.bytes === row.bytes, 'Asset drift: ' + row.name);
    return {name:row.name, ...actual};
  });
  const sums = read(root, path.join(dir, 'SHA256SUMS.txt')).toString('utf8').trim().split(/\r?\n/);
  const expected = assets.filter(row => row.name !== 'SHA256SUMS.txt').map(row => row.sha256 + '  ' + row.name);
  check(sums.length === expected.length && sums.every(line => expected.includes(line)) &&
    new Set(sums).size === sums.length, 'Checksum set differs');
  const trialIdentity = identity(root, receipt.trialReceipt), trial = json(root, receipt.trialReceipt);
  check(trialIdentity.sha256 === receipt.trialReceiptSha256 && trial.status === 'PASS' && trial.action === 'trial-test' &&
    trial.productionUnchanged === true && trial.desktopClosed === true && trial.slots?.released === true &&
    Array.isArray(trial.remainingDevelopmentProcesses) && trial.remainingDevelopmentProcesses.length === 0 &&
    trial.artifact?.sha256 === manifest.executable.sha256, 'Trial identity/cleanup differs');
  const notes = identity(root, path.join(dir, 'release-notes.md'), 65536);
  return {repository:REPO, tag, commit:receipt.commit, fileVersion:manifest.fileVersion,
    assets, notes, packageReceipt:identity(root, receiptPath), trialReceipt:trialIdentity};
}
function publicReceiptMatches(root, tag, current) {
  const name = path.join(directory(tag), 'public-verification.json');
  if (!fs.existsSync(local(root, name))) return null;
  const receipt = json(root, name);
  check(receipt.status === 'PUBLISHED_AND_DOWNLOAD_VERIFIED' && receipt.tag === tag && receipt.commit === current.commit &&
    receipt.latestVerified === true && Array.isArray(receipt.anonymousDownloads) &&
    receipt.anonymousDownloads.length === current.assets.length &&
    new Set(receipt.anonymousDownloads.map(row => row.name)).size === current.assets.length &&
    current.assets.every(asset => receipt.anonymousDownloads.some(row => row.name === asset.name &&
      row.bytes === asset.bytes && row.sha256 === asset.sha256)), 'Public receipt identity differs');
  return identity(root, name);
}
function prepare(root, tag, rollbackTag) {
  const current = acceptedPackage(root, tag);
  check(validTag(rollbackTag) && rollbackTag !== tag, 'Explicit retained rollback tag required');
  const rollback = acceptedPackage(root, rollbackTag);
  const verification = publicReceiptMatches(root, rollbackTag, rollback);
  check(verification, 'Rollback must have accepted public downloads');
  return {schemaVersion:1, ...current, title:'Pulse ' + current.fileVersion + ' (' + tag + ')',
    rollback:{tag:rollbackTag, commit:rollback.commit, assets:rollback.assets, verification},
    policy:{retainPreviousReleases:true, replaceAssets:false, installation:false}};
}
function previousTag(root, tag) {
  directory(tag);
  const parts=value=>value.slice(7).split('.').map(Number);
  const compare=(a,b)=>{const x=parts(a),y=parts(b);for(let i=0;i<x.length;i++)if(x[i]!==y[i])return x[i]-y[i];return 0;};
  const names=fs.readdirSync(path.join(root,'dev-work/releases')).filter(validTag);
  check(names.length<=256,'Release inventory bound');
  const retained=names.filter(name=>compare(name,tag)<0 &&
    fs.existsSync(local(root,path.join(directory(name),'public-verification.json')))).sort(compare);
  check(retained.length,'No retained published rollback; supply an accepted rollback tag');
  return retained[retained.length-1];
}
function savePrepared(root, plan) {
  const file = local(root, path.join(directory(plan.tag), 'publication-plan.json')), raw = encode(plan);
  if (fs.existsSync(file)) {
    check(read(root, file).toString('utf8') === raw, 'Prepared release changed; inspect instead of replacing');
    return {path:file, sha256:hash(raw), reused:true};
  }
  const fd = fs.openSync(file, 'wx');
  try { fs.writeFileSync(fd, raw); fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
  check(read(root, file).toString('utf8') === raw, 'Prepared release readback differs');
  return {path:file, sha256:hash(raw), reused:false};
}
function status(root, tag) {
  const dir = directory(tag), readIf = name => fs.existsSync(local(root, path.join(dir, name))) ? json(root, path.join(dir, name)) : null;
  const pkg = readIf('package-receipt.json'), prepared = readIf('publication-plan.json'), published = readIf('public-verification.json');
  const attempts=fs.existsSync(local(root,dir))?fs.readdirSync(local(root,dir)).filter(name=>/^publication-attempt-[a-f0-9-]+\.json$/.test(name)):[];
  check(attempts.length<=32,'Publication history bound');
  const outcomes=attempts.map(name=>readIf(name));
  const metadata=outcomes.some(value=>value.status==='PASS'&&value.result?.status==='PUBLISHED_METADATA_VERIFIED');
  const failed=outcomes.some(value=>value.status!=='PASS');
  const complete=published?.status==='PUBLISHED_AND_DOWNLOAD_VERIFIED';
  return {status:'RECORDED_STATE_ONLY', tag, package:pkg?.status || 'MISSING', prepared:Boolean(prepared),
    publicVerification:published?.status || 'MISSING', liveInputsVerified:false, remoteChecked:false,
    nextAction:complete ? 'COMPLETE_RECORDED' : published || (pkg && pkg.status!=='PASS') ? 'INSPECT_RECORDED_FAILURE' :
      metadata ? 'Release-Verify' : failed ? 'INSPECT_REMOTE_STATE' : prepared ? 'Release-Publish' : pkg ? 'Release-Prepare' : 'Release-Package'};
}
function authorize(plan, expectedHash, actualHash, authorization) {
  check(validHash(expectedHash) && expectedHash === actualHash && authorization?.repository === plan.repository &&
    authorization?.tag === plan.tag && authorization?.commit === plan.commit && authorization?.publish === true,
    'Exact current publication authorization and preparation hash required');
}
function remoteAssets(plan, release, complete) {
  check(Array.isArray(release.assets) && release.assets.length <= plan.assets.length, 'Unexpected remote assets');
  check(new Set(release.assets.map(row => row.name)).size === release.assets.length, 'Duplicate remote asset');
  for (const row of release.assets) {
    const expected = plan.assets.find(asset => asset.name === row.name);
    check(expected && row.state === 'uploaded' && row.size === expected.bytes && row.digest === 'sha256:' + expected.sha256,
      'Remote asset differs; never overwrite: ' + row.name);
  }
  if (complete) check(release.assets.length === plan.assets.length, 'Remote assets incomplete');
}
// transport is the official gh adapter in production and an in-memory service in tests.
// Intent is durable BEFORE each mutation. Missing/uncertain results are never replayed.
function publish(plan, transport, history, record) {
  const prior = history.flatMap(attempt => attempt.events);
  const seen = key => prior.some(event => event.key === key && event.kind === 'intent');
  const change = (key, operation) => {
    check(!seen(key), 'Unresolved previous operation: ' + key + '; inspect remote state before retrying');
    record({kind:'intent', key});
    const result = operation();
    record({kind:'result', key, id:result?.id || null});
    return result;
  };
  const account = transport.api('user');
  const repo = transport.api('repos/' + REPO);
  check(typeof account.login === 'string' && repo.full_name === REPO && repo.permissions?.push === true,
    'Authenticated repository write permission required');
  record({kind:'identity', account:account.login, repository:repo.full_name});
  const matches = transport.releases().filter(row => row.tag_name === plan.tag);
  check(matches.length <= 1, 'Ambiguous release identity');
  let release = matches[0];
  const notes = transport.notes();
  const verifyTag = () => {
    const tag = transport.tag(plan.tag);
    check(!tag || (tag.object.type === 'commit' && tag.object.sha === plan.commit), 'Tag conflict');
  };
  verifyTag();
  if (!release) {
    release = change('create', () => transport.api('repos/' + REPO + '/releases',
      {tag_name:plan.tag,target_commitish:plan.commit,name:plan.title,body:notes,draft:true,prerelease:false}, 'POST'));
  }
  check(Number.isSafeInteger(release.id) && release.id > 0, 'Invalid release ID');
  const endpoint = 'repos/' + REPO + '/releases/' + release.id;
  release = transport.api(endpoint); // Drafts are resolved by ID, never by /tags.
  check(release.tag_name === plan.tag && !release.prerelease, 'Release identity differs');
  if (release.draft) {
    check(release.target_commitish === plan.commit && release.body === notes && release.name === plan.title,
      'Draft metadata differs; inspect before continuing');
    remoteAssets(plan, release, false);
    for (const asset of plan.assets) {
      if (!release.assets.some(row => row.name === asset.name)) {
        change('upload:' + asset.name, () => transport.upload(plan.tag, asset));
        release = transport.api(endpoint);
        remoteAssets(plan, release, false);
        check(release.assets.some(row => row.name === asset.name), 'Upload was not confirmed');
      }
    }
    remoteAssets(plan, release, true);
    verifyTag();
    change('publish', () => transport.api(endpoint, {draft:false,make_latest:'true'}, 'PATCH'));
    release = transport.api(endpoint);
  }
  check(release.draft === false && !release.prerelease, 'Release is not published');
  remoteAssets(plan, release, true);
  const tag = transport.tag(plan.tag);
  check(tag?.object.type === 'commit' && tag.object.sha === plan.commit, 'Published tag differs');
  check(transport.api('repos/' + REPO + '/releases/latest').id === release.id, 'Latest differs; inspect instead of editing again');
  return {status:'PUBLISHED_METADATA_VERIFIED', releaseId:release.id, tag:plan.tag, commit:plan.commit,
    nextAction:'Release-Verify', anonymousDownloadVerified:false};
}
module.exports = {REPO, hash, encode, check, local, read, json, identity, directory, dependencies,
  acceptedPackage, publicReceiptMatches, prepare, previousTag, savePrepared, status, authorize, remoteAssets, publish};
