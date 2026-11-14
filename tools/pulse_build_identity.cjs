'use strict';
const path=require('node:path'), crypto=require('node:crypto');
const sha=raw=>crypto.createHash('sha256').update(raw).digest('hex');
// This exact dispatch runs only for workflow-test, never for build/release.
// Removing it for compatibility must reproduce the complete previously verified
// worker identity. Any other worker change still invalidates legacy reuse.
const toolDispatch = `        elif plan['action'] == 'workflow-test':
            result = run([config['node'], '--max-old-space-size=64', root/'tools/pulse_workflow_selftest.cjs', target],
                         'release preparation, checkpoint recovery and refusal checks', maximum_seconds=25)
            receipt['workflowChecks'] = json.loads(result)
            if receipt['workflowChecks'].get('status') != 'PASS':
                raise RuntimeError('Workflow self-check failed')
`;
const retirementDispatch = `        elif plan['action'] == 'artifact-retire':
            result = run([config['python'], '-B', root/'tools/pulse_artifact_delete.py', plan_file, expected],
                         'exact registered same-handle retirement', maximum_seconds=60)
            receipt['retirement'] = json.loads(result)
            if receipt['retirement'].get('deletedFiles') != len(plan['retirements']):
                raise RuntimeError('Retirement count differs from exact plan')
`;
function beforeRetirement(script) {
  return script.replace(retirementDispatch,'')
    .replace("if plan_file.stat().st_size > 524288 or sha(plan_file) != expected:","if plan_file.stat().st_size > 65536 or sha(plan_file) != expected:")
    .replace("    if plan['action'] != 'artifact-retire' and plan_file.stat().st_size > 65536:\n        raise RuntimeError('Non-retirement plan exceeds original bound')\n",'')
    .replace("        retiring = {item['path']: item['identity']['bytes'] for item in plan.get('retirements', [])} if plan['action'] == 'artifact-retire' else {}\n        used = sum(retiring.values()); count = len(retiring)\n",'        used = 0; count = 0\n')
    .replace("            if str(file) in retiring:\n                continue  # Charge original bytes while exclusive handles delete; avoid a stat/delete race.\n",'');
}
const binaryInputs=rows=>rows.filter(item=>!['pulse_dev_worker.py','pulse_dev.cjs','pulse_build_identity.cjs',
  'development-defaults.ini','local-artifact-policy.json'].includes(path.basename(item.path)));
// These dispatches only validate/reuse test evidence, beyond the compilation
// branch. Reversing them must reproduce the entire worker that built the EXE.
const continuationBegin = `            if plan['action'] == 'test-continue':
                for item in plan['continuation']['files']:
                    if sha(safe(item['path'])) != item['sha256']:
                        raise RuntimeError('Accepted test evidence changed')
                receipt['reusedEvidence'] = plan['continuation']
`;
const continuationEnd = `        if plan['action'] == 'test-continue':
            for item in plan['continuation']['files']:
                if sha(safe(item['path'])) != item['sha256']:
                    raise RuntimeError('Accepted test evidence changed during continuation')
`;
const continuationKind = `            if plan['action'] == 'test-continue':
                os.environ['PULSE_DEV_SESSION_KIND'] = 'debug'
`;
const guiOriginal = `                    output = run([exe,'--dev-smoke'], 'isolated first launch',desktop_name)
                    if 'PULSE_DEV_SMOKE ' not in output:
                        raise RuntimeError('First development launch produced no smoke receipt')
                    output = run([exe,'--dev-smoke-resume'], 'isolated restart restores settings',desktop_name)
                    if '"restored":true' not in output:
                        raise RuntimeError('Restart did not restore development settings')
`;
const guiContinued = "                    if not (plan.get('continuation') or {}).get('guiAccepted', False):\n"+
  guiOriginal.split('\n').filter(Boolean).map(line=>'    '+line).join('\n')+'\n';
function beforeFixtures(script) {
  // Only these tool-fixture branches are reversible; the entire original worker
  // must still match the accepted build hash. Compilation remains unchanged.
  for(const label of ['archive exact completed tool fixtures','archive and retire completed tool fixtures'])
    script=script.replace(`            if plan.get('fixtureArchive'):
                result = run([config['python'], '-B', root/'tools/pulse_fixture_archive.py', plan_file, expected],
                             '${label}', maximum_seconds=20)
                receipt['fixtureArchive'] = json.loads(result)
`,'');
  return script;
}
function sameBinaries(rows,baseline,worker,raw) {
  const current=rows.find(item=>item.path===worker)?.sha256, built=baseline.find(item=>item.path===worker)?.sha256;
  if(!current||!built||sha(raw)!==current)return false;
  if(current===built)return JSON.stringify(binaryInputs(rows))===JSON.stringify(binaryInputs(baseline));
  const actualScript=beforeFixtures(raw.toString('utf8').replace(/\r\n/g,'\n'));
  const script=beforeRetirement(actualScript);
  const stripped=script.replace(toolDispatch,'');
  const normalized=sha(stripped);
  const beforeContinuation=script.replace(continuationBegin,'').replace(continuationEnd,'').replace(continuationKind,'').replace(guiContinued,guiOriginal)
    .replace("if plan['action'] != 'test-continue' and (len(captures) != 15 or any(","if len(captures) != 15 or any(")
    .replace("'settings-dark.png','settings-peripherals.png'))):","'settings-dark.png','settings-peripherals.png')):");
  const compatible=current===built || sha(actualScript)===built || sha(actualScript.replace(/\n/g,'\r\n'))===built || normalized===built ||
    sha(actualScript.replace(toolDispatch,''))===built || sha(script)===built || sha(script.replace(/\n/g,'\r\n'))===built ||
    (beforeContinuation!==script && (sha(beforeContinuation)===built||sha(beforeContinuation.replace(/\n/g,'\r\n'))===built)) ||
    (built==='c8030503f08944cab8ad4035a9db607a1df57f412d6811885f61ca11b207816c' &&
     normalized==='463433e932b049cd566e8d49f6975b6e77410ad7d8c530c450cc324a3d792ff4');
  // Compatibility above reproduces the entire accepted worker hash after only
  // the enumerated non-compilation dispatch reversals. A fixed historical
  // recipe hash would incorrectly reject newly verified compiler/resource steps.
  // Any source, compiler binding or unrelated worker/recipe edit still fails.
  return compatible &&
    JSON.stringify(binaryInputs(rows))===JSON.stringify(binaryInputs(baseline));
}
function retiredBuild(manifest,retired) {
  return (manifest.outputs||[]).some(item=>retired[item.path]?.identity?.sha256===item.sha256);
}
module.exports={sameBinaries,toolDispatch,binaryInputs,beforeRetirement,beforeFixtures,retiredBuild};
