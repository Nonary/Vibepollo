"""Add only qualified exact-source Linux packages; preserve existing binaries."""
import argparse, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
from nightly_linux_qualification import final_ctest_result

parser = argparse.ArgumentParser(); parser.add_argument('--product', choices=['Vibeshine','Vibepollo'], required=True)
product = parser.parse_args().product
repo, sha, linux_run, windows_run = {
    'Vibeshine': ('Nonary/vibeshine', 'aaa39188cc78defbf63d0351c63c9e1c1014b6e1', '38153590870', '38148740943'),
    'Vibepollo': ('Nonary/Vibepollo', '0d62c962e081469eb0c21674e7ef079002af734a', '38153592930', '38148743256'),
}[product]
tag, version = 'daily-2026-10-11', '2.0.0-daily.d20261011+'+sha[:12]
base = Path(os.environ.get('NIGHTLY_VALIDATION_ROOT', r'D:\sources\builds\nightly-release-validation-20261011')); base.mkdir(parents=True,exist_ok=True)
if os.environ.get('GITHUB_ACTIONS') == 'true':
    assert os.environ['GITHUB_REPOSITORY'] == repo
scratch = base / 'linux-publication-temp'; scratch.mkdir(exist_ok=True)
os.environ['TEMP'] = os.environ['TMP'] = str(scratch)
def run(*args): return subprocess.check_output(args, text=True, encoding='utf-8')
def api(path): return json.loads(run('gh','api',path))
qualification = json.loads(run('gh','run','view',linux_run,'--repo',repo,'--json','status,conclusion,jobs,headSha'))
job = next(j for j in qualification['jobs'] if j['name'] == 'linux / Archlinux')
assert job['conclusion'] == 'success', 'Linux native qualification has not passed'
for name in ('Build PKGBUILD','Verify Arch package contract','Upload Artifacts'):
    assert next(s for s in job['steps'] if s['name']==name)['conclusion'] == 'success'
logs = run('gh','api','--allow-escape-sequences',f'repos/{repo}/actions/jobs/{job["databaseId"]}/logs')
assert sha in logs and version in logs, 'Exact native source/version missing from qualification evidence'
test_result = final_ctest_result(logs)
log_path = base / (product+'-nightly-linux-qualification.log'); log_path.write_text(logs,encoding='utf-8')
artifact = base / ('qualified-linux-'+product); artifact.mkdir(exist_ok=True)
if not list(artifact.glob('*.pkg.tar.zst')):
    subprocess.run(['gh','run','download',linux_run,'--repo',repo,'--name','build-Archlinux','--dir',str(artifact)],check=True)
packages = list(artifact.glob(product.lower()+'-*.pkg.tar.zst'))
assert len(packages)==1 and packages[0].stat().st_size>0
package = packages[0]
pkginfo = run('tar','-xOf',str(package),'.PKGINFO')
assert f'pkgname = {product.lower()}\n' in pkginfo and 'arch = x86_64' in pkginfo
assert 'daily.d20261011' in pkginfo and sha in pkginfo, 'Native package version is not the exact nightly identity'
binary_name = product.lower()+'-'+version
listing = run('tar','-tvf',str(package))
assert any(line.startswith('l') and line.endswith('usr/bin/'+product.lower()+' -> '+binary_name) for line in listing.splitlines()), 'The application symlink does not target the exact nightly binary'
binary = subprocess.check_output(['tar','-xOf',str(package),'usr/bin/'+binary_name])
assert version.encode() in binary, 'The actual packaged application does not contain the canonical version'
digest = hashlib.sha256(package.read_bytes()).hexdigest()
release = api(f'repos/{repo}/releases/tags/{tag}')
assert release['prerelease'] and not release['draft']
original_assets = {a['name']: a for a in release['assets']}
assert package.name not in original_assets, 'Linux package is already published; verify rather than overwrite'
ref = api(f'repos/{repo}/git/ref/tags/{tag}')['object']
while ref['type']=='tag': ref=api(f'repos/{repo}/git/tags/{ref["sha"]}')['object']
assert ref['type']=='commit' and ref['sha']==sha
original = base / ('public-download-'+product.lower())
if not original.exists():
    original.mkdir()
    subprocess.run(['gh','release','download',tag,'--repo',repo,'--dir',str(original)],check=True)
stage = base / ('augmented-release-'+product); stage.mkdir(exist_ok=True)
for name in original_assets:
    assert (original/name).is_file()
    shutil.copyfile(original/name,stage/name)
    if original_assets[name].get('digest'):
        assert original_assets[name]['digest']=='sha256:'+hashlib.sha256((stage/name).read_bytes()).hexdigest()
known_windows = {
    'Vibeshine': ('8204f465132d4c89b402469eb132d4b29d477845f0d0de54d7a286aa8ba87c9c', '64bd81a21ab8c0f216f18ea5be9cf0cf78d9e893d42606b59482c16a2463641a'),
    'Vibepollo': ('a0e4f20c118bfc0d9e707608fdd6a8936ea431a7b041a04ff1c4a53e2daee5b8', 'd690931ade47fe1feaa4866f0481b9750f9d44142fc5f14a92d1ec60bfebef8b'),
}[product]
for name, expected_digest in zip((product+'.msi',product+'Setup.exe'), known_windows):
    assert hashlib.sha256((stage/name).read_bytes()).hexdigest()==expected_digest, 'Original qualified Windows payload changed'
shutil.copyfile(package,stage/package.name)
manifest_path = stage/'daily-manifest.json'
manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
assert manifest['source_commit']==sha and manifest['version']==version and str(manifest['workflow_run_id'])==windows_run
manifest['assets'][package.name]=digest
manifest['linux'] = dict(schema_version=1, source_commit=sha, version=version, workflow_run_id=linux_run,
    workflow_source_commit=qualification['headSha'], qualification_job_id=job['databaseId'], qualification_status='success',
    workflow_run_conclusion=qualification['conclusion'], tests_total=test_result['total'], native_tests=test_result,
    assets={package.name:digest})
manifest_path.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
notes_root=base/('updated-notes-'+product)
if not notes_root.exists():
    notes_root.mkdir()
    import importlib.util
    spec=importlib.util.spec_from_file_location('nightly_notes',Path(__file__).with_name('nightly_notes.py'))
    generator=importlib.util.module_from_spec(spec); spec.loader.exec_module(generator)
    generator.generate(repo,tag,version,sha,stage,notes_root/'release-notes.md')
    shutil.copyfile(stage/'nightly-metadata.json',notes_root/'nightly-metadata.json')
metadata=json.loads((notes_root/'nightly-metadata.json').read_text(encoding='utf-8'))
assert metadata['source_commit']==sha and metadata['version']==version and metadata['commits_total']>0
(stage/'nightly-metadata.json').write_text(json.dumps(metadata,indent=2)+'\n',encoding='utf-8')
files=sorted(p for p in stage.iterdir() if p.name!='SHA256SUMS')
(stage/'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in files),encoding='utf-8')
for name in (product+'.msi',product+'Setup.exe','release-provenance.json'):
    assert (stage/name).read_bytes()==(original/name).read_bytes(), 'Published Windows binary/provenance changed'
subprocess.run(['gh','release','upload',tag,str(stage/package.name),'--repo',repo],check=True)
subprocess.run(['gh','release','upload',tag,str(stage/'daily-manifest.json'),str(stage/'nightly-metadata.json'),str(stage/'SHA256SUMS'),'--repo',repo,'--clobber'],check=True)
subprocess.run(['gh','release','edit',tag,'--repo',repo,'--notes-file',str(notes_root/'release-notes.md'),'--draft=false','--prerelease','--latest=false'],check=True)
live=api(f'repos/{repo}/releases/tags/{tag}')
assert live['prerelease'] and not live['draft']
assert live['body'].replace('\r\n','\n').strip()==(notes_root/'release-notes.md').read_text(encoding='utf-8').strip()
assert {a['name'] for a in live['assets']} == set(original_assets)|{package.name}
for asset in live['assets']:
    assert asset['digest']=='sha256:'+hashlib.sha256((stage/asset['name']).read_bytes()).hexdigest()
    if asset['name'] in (product+'.msi',product+'Setup.exe','release-provenance.json'):
        assert asset['id']==original_assets[asset['name']]['id'], 'Immutable payload asset was replaced'
final_ref=api(f'repos/{repo}/git/ref/tags/{tag}')['object']
while final_ref['type']=='tag': final_ref=api(f'repos/{repo}/git/tags/{final_ref["sha"]}')['object']
assert final_ref['type']=='commit' and final_ref['sha']==sha
report=dict(repository=repo,release=live['html_url'],source=sha,version=version,linux_run=linux_run,
    linux_job=job['url'],native_tests=test_result,native_package=package.name,sha256=digest,assets=len(live['assets']),
    preserved_existing_binaries=True,real_installation_tested=False)
(base/(product+'-linux-publication-report.json')).write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,indent=2))
