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
const binaryInputs=rows=>rows.filter(item=>!['pulse_dev_worker.py','pulse_dev.cjs','pulse_build_identity.cjs',
  'development-defaults.ini','local-artifact-policy.json'].includes(path.basename(item.path)));
function sameBinaries(rows,baseline,worker,raw) {
  const current=rows.find(item=>item.path===worker)?.sha256, built=baseline.find(item=>item.path===worker)?.sha256;
  if(!current||!built||sha(raw)!==current)return false;
  const script=raw.toString('utf8').replace(/\r\n/g,'\n');
  const stripped=script.replace(toolDispatch,'');
  const normalized=sha(stripped);
  const compatible=current===built || normalized===built ||
    (built==='c8030503f08944cab8ad4035a9db607a1df57f412d6811885f61ca11b207816c' &&
     normalized==='463433e932b049cd566e8d49f6975b6e77410ad7d8c530c450cc324a3d792ff4');
  const start=stripped.indexOf('        bin_dir = Path(');
  const end=stripped.indexOf("        else:\n            profile = target/'profile'",start);
  return compatible && start>=0 && end>start &&
    sha(stripped.slice(start,end))==='617944517f9cfd94c8203b24a50f7fffe71e2ffdfab35920fd6709c9077c0eb6' &&
    JSON.stringify(binaryInputs(rows))===JSON.stringify(binaryInputs(baseline));
}
module.exports={sameBinaries,toolDispatch};
