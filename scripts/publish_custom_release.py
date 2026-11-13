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
                    architecture='Windows x64', compiler='MinGW',
                    executable=dict(name='Pulse.exe', bytes=len(exe), sha256=sha(exe)),
                    binaryUnmodified=True, license='MIT')
    if artifact is not None:
        manifest.update(ciRun=run, ciArtifact=artifact['id'], ciZipSha256=artifact['digest'].removeprefix('sha256:'))
    else:
        manifest.update(compiler='LLVM-MinGW', build='Protected local release, independently trial-tested')
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
    notes = ('稳定性与低占用优化，保留个性化设置和软件内下载更新。\n\n'
             '- CPU、内存采集按显示需求启停，保留频率、内存详情和任务栏组件所需数据。\n'
             '- 暂停时释放 CPU 查询，恢复时重新建立采样区间；未知读数显示 --，避免误报为零。\n'
             '- 修正更新下载超时检查，已完成的异步读取也受整次下载期限约束。\n'
             '- 保留上游 1.3.0、显卡选择、自动缩放、吸附、鼠标穿透及配置兼容；没有新增依赖。\n\n'
             '已有 custom-2026.11.10.2 可右键「检查更新」后下载替换；更早无更新入口的版本请手动下载。\n'
             '下载 ZIP 解压运行，附来源清单和 SHA256。普通设置和便携 data 不包含在包内。\n'
             '功能改动已通过本地受保护回归；本版本通过 Windows MinGW/MSVC 构建、定向回归和准确产物的独立配置重开验证。'
             '尚未量化长期占用改善；真实在线升级安装及全部硬件场景未在本轮执行。\n\n'
             f'源码：https://github.com/{REPO}/commit/{commit}\n'
             f'CI：https://github.com/{REPO}/actions/runs/{run}\n\n'
             '旧 Release 保留供回退；回退前正常退出并备份个人设置。\n')
    if tag == 'custom-2026.11.13.1':
        notes = ('新增外设电量卡片，保留软件内下载更新和现有配置。\n\n'
                 '- 显示设备名称、百分比与低电量颜色；未上报显示 --，真实 0% 保留。\n'
                 '- 后台每 60 秒刷新；关闭卡片、暂停或进入不显示卡片的模式时停止查询。\n'
                 '- 支持 Windows 上报电量的蓝牙鼠标、键盘和耳机，以及部分 VGN/ATK/VXE 接收器。\n'
                 '- VGN：3554:F503/F505/F50B；VXE：3554:F58A/F58D/F58E、373B:104D/1040；ATK：373B:1031/11D9。其他型号不保证支持。\n'
                 '- 酷雨键盘接收器尚无专用协议适配；OPPO 耳机依赖 Windows 上报。用户具体设备尚未实测。\n\n'
                 '源码编译、异常数据处理、卡片开关保存和原生界面验证通过；下载包使用同一验收 EXE，附来源清单和 SHA256。\n'
                 '已有带更新功能的定制版可右键「检查更新」下载替换；也可下载 ZIP 解压使用。\n'
                 '普通设置和便携 data 不包含在包内。旧版本保留供回退，回退前正常退出并备份个人设置。\n\n'
                 f'源码：https://github.com/{REPO}/commit/{commit}\n')
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
       '--draft', '--title', 'Pulse 1.3.0 定制版 — 稳定与低占用优化',
       '--notes-file', str(ROOT/'dist-custom/release-notes.md'))
    # A draft's tag is not created until publication; resolve its release ID.
    drafts = [r for r in api('releases?per_page=100') if r['tag_name'] == tag]
    require(len(drafts) == 1, 'Expected one uploaded draft; inspect releases')
    draft = api('releases/' + str(drafts[0]['id']))
    expected = {p.name: sha(p.read_bytes()) for p in files}
    require(draft['draft'] and draft['target_commitish'] == commit and len(draft['assets']) == 3 and
            {a['name']: a['digest'].removeprefix('sha256:') for a in draft['assets']} == expected,
            'Uploaded asset identity mismatch; draft retained')
    gh('release', 'edit', tag, '--repo', REPO, '--draft=false', '--latest')
    print(json.dumps(dict(tag=tag, commit=commit, assets=expected)))

if __name__ == '__main__':
    main()
