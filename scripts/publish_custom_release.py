"""Publish an already-tested CI artifact, never rebuild or overwrite a release."""
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent.parent
REPO = 'haihaihailailo/Pulse'

def require(ok, message):
    if not ok:
        raise RuntimeError(message)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def gh(*args):
    return subprocess.check_output(['gh', *args], timeout=120)

def api(tail):
    return json.loads(gh('api', 'repos/' + REPO + '/' + tail))

def package(out, exe, artifact, run, commit):
    header = (ROOT / 'src/updater.h').read_text(encoding='utf-8')
    tag = re.search(r'Version\[\] = L"(custom-\d{4}\.\d{2}\.\d{2}\.\d+)"', header).group(1)
    version = re.search(r'VALUE "FileVersion", "([\d.]+)', (ROOT / 'app.rc').read_text(encoding='utf-8')).group(1)
    require(re.fullmatch('[0-9a-f]{40}', commit), 'Invalid commit')
    out.mkdir(exist_ok=True, parents=True)
    require(not any(out.iterdir()), 'Package directory must be empty')
    manifest = dict(repository=REPO, tag=tag, commit=commit, fileVersion=version,
                    architecture='Windows x64', compiler='MinGW', ciRun=run,
                    ciArtifact=artifact['id'], ciZipSha256=artifact['digest'].removeprefix('sha256:'),
                    executable=dict(name='Pulse.exe', bytes=len(exe), sha256=sha(exe)),
                    binaryUnmodified=True, license='MIT')
    data = (json.dumps(manifest, ensure_ascii=False, indent=2)+'\n').encode('utf-8')
    (out / 'release-manifest.json').write_bytes(data)
    archive = out / ('Pulse-' + tag + '-win-x64.zip')
    readme = ('Pulse '+tag+'\nUnzip into a separate folder and run Pulse.exe.\n'
              'Create an empty pulse-portable.ini beside the EXE BEFORE launch to use portable data.\n'
              'Right-click > Check for updates: download, verify, replace and restart; settings are preserved.\n'
              'Windows PowerShell 5.1 and write access to the executable directory are required.\n'
              'Old executables are retained as .rollback-* files for recovery.\n')
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, content in [('Pulse.exe', exe), ('LICENSE', (ROOT/'LICENSE').read_text(encoding='utf-8').encode()),
                              ('README.txt', readme.encode()), ('build-info.json', data)]:
            entry = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(entry, content, compresslevel=9)
    files = [archive, out/'release-manifest.json']
    (out/'SHA256SUMS.txt').write_bytes(''.join(sha(p.read_bytes())+'  '+p.name+'\n' for p in files).encode())
    files.append(out/'SHA256SUMS.txt')
    notes = ('新增软件内更新：右键检查新版，确认后直接下载、校验、正常退出、替换并重启；保留个性化设置。\n\n'
             '- 下载及 EXE SHA256、版本、大小和来源校验；失败回退旧程序，不自动提权。\n'
             '- 更新器嵌入 EXE，无需附加脚本；默认不在启动时或定时联网检查。\n'
             '- 合并上游 1.3.0，保留显卡选择、自动缩放、吸附、鼠标穿透和配置兼容。\n'
             '- 旧版没有更新入口，首次请下载此 ZIP；后续正式版本可在软件内更新。\n\n'
             '下载 ZIP 解压运行，附来源清单和 SHA256。普通设置和便携 data 不包含在包内。\n'
             '本地双分支构建、合成下载/替换/回退、原生菜单与独立配置重开验证通过。'
             '真实在线升级安装及全部硬件场景未在本轮执行。\n\n'
             f'源码：https://github.com/{REPO}/commit/{commit}\n'
             f'CI：https://github.com/{REPO}/actions/runs/{run}\n\n'
             '旧 Release 保留供回退；回退前正常退出并备份个人设置。\n')
    (out/'release-notes.md').write_bytes(notes.encode())
    return tag, files

def main():
    require(os.environ.get('GITHUB_ACTIONS') == 'true' and os.environ.get('GITHUB_REPOSITORY') == REPO,
            'Publishing is restricted to the repository workflow')
    commit = os.environ['GITHUB_SHA']
    run_id = os.environ['PULSE_BUILD_RUN']
    require(run_id.isdigit(), 'Invalid build run ID')
    run = api('actions/runs/' + run_id)
    require(run['head_sha'] == commit and run['conclusion'] == 'success' and
            run['path'] == '.github/workflows/build.yml', 'Exact Build workflow must pass')
    jobs = api('actions/runs/' + run_id + '/jobs')['jobs']
    require(len(jobs) == 2 and all(j['conclusion'] == 'success' for j in jobs), 'Both compilers must pass')
    artifacts = api('actions/runs/' + run_id + '/artifacts')['artifacts']
    selected = [a for a in artifacts if a['name'] == 'Pulse-mingw-win-x64' and not a['expired']]
    require(len(selected) == 1 and selected[0]['size_in_bytes'] < 16*1024*1024, 'Unexpected artifact')
    artifact = selected[0]
    raw = gh('api', f'repos/{REPO}/actions/artifacts/{artifact["id"]}/zip')
    require(artifact['digest'] == 'sha256:' + sha(raw), 'Artifact digest mismatch')
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        require(sorted(z.namelist()) == ['Pulse.exe', 'metrics_selftest.exe'] and
                sum(i.file_size for i in z.infolist()) < 24*1024*1024, 'Unexpected artifact members')
        exe = z.read('Pulse.exe')
    tag, files = package(ROOT/'dist-custom', exe, artifact, int(run_id), commit)
    # No clobber, deletion or retry of an uncertain release mutation.
    existing = api('releases?per_page=100')
    require(all(r['tag_name'] != tag for r in existing), 'Release exists; inspect it before retrying')
    gh('release', 'create', tag, *map(str, files), '--repo', REPO, '--target', commit,
       '--draft', '--title', 'Pulse 1.3.0 定制版 — 软件内下载更新',
       '--notes-file', str(ROOT/'dist-custom/release-notes.md'))
    draft = api('releases/tags/'+tag)
    expected = {p.name: sha(p.read_bytes()) for p in files}
    require(draft['draft'] and len(draft['assets']) == 3 and
            {a['name']: a['digest'].removeprefix('sha256:') for a in draft['assets']} == expected,
            'Uploaded asset identity mismatch; draft retained')
    gh('release', 'edit', tag, '--repo', REPO, '--draft=false', '--latest')
    print(json.dumps(dict(tag=tag, commit=commit, assets=expected)))

if __name__ == '__main__':
    main()
