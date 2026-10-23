"""Cross-check API identity, CI evidence and exact staged public downloads."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class PublicContractTest(unittest.TestCase):
    def test_identity_and_public_evidence(self):
        n = load('nightly_notes')
        host = (SCRIPTS / 'daily_release.py').exists()
        workflow = (SCRIPTS.parent / '.github/workflows/daily-prerelease.yml').read_text(encoding='utf-8') if host else ''
        product = 'Vibepollo' if '--product Vibepollo' in workflow else 'Vibeshine'
        if host:
            self.assertIn('--destination public-assets --product ' + product, workflow)
        repo = 'Nonary/' + (product if product == 'Vibepollo' else 'vibeshine') if host else 'Nonary/moonlight-qt'
        source = 'a' * 40
        tag = '2.0.1-nightly.d20261012.' + source[:12] if host else 'v6.2.0-daily.183'
        version = tag if host else tag[1:]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'assets'
            root.mkdir()
            notes = Path(temp) / 'notes.md'
            if host:
                (root / (product + 'Setup.exe')).write_bytes(b'installer')
                (root / (product.lower() + '-fixture-x86_64.pkg.tar.zst')).write_bytes(b'linux')
                (root / 'daily-manifest.json').write_text(json.dumps({'source_commit': source}))
                (root / 'release-provenance.json').write_text('{}')
            else:
                (root / ('MoonlightSetup-' + version + '.exe')).write_bytes(b'installer')
            def fake_run(*args):
                return '[[]]' if args[0] == 'gh' else source
            with patch.object(n, 'run', fake_run):
                n.generate(repo, tag, version, source, root, notes)
            metadata = json.loads((root / 'nightly-metadata.json').read_text())
            line = notes.read_text().strip().splitlines()[0]
            self.assertEqual(json.loads(line.removeprefix('<!-- nightly-identity: ').removesuffix(' -->')), metadata)
            self.assertEqual(metadata['source_commit'], source)
            self.assertIn('nightly-metadata.json', (root / 'SHA256SUMS').read_text())
            if host:
                self.assertNotIn('daily-manifest.json', (root / 'SHA256SUMS').read_text())
                self.assertNotIn('release-provenance.json', (root / 'SHA256SUMS').read_text())
                self.assertEqual(metadata['qualification']['source_commit'], source)
                d = load('daily_release')
                destination = Path(temp) / 'public'
                d.stage_public_assets(root, destination, product)
                self.assertEqual({p.name for p in destination.iterdir()}, {product + 'Setup.exe', product.lower() + '-fixture-x86_64.pkg.tar.zst', 'nightly-metadata.json', 'SHA256SUMS'})
                self.assertTrue((root / 'release-provenance.json').exists())
                (root / 'garbage.txt').write_text('garbage')
                with self.assertRaises(ValueError):
                    d.stage_public_assets(root, Path(temp) / 'bad-public', product)

    def test_user_initiated_checksums_fail_closed(self):
        import hashlib
        import os
        import shutil
        import subprocess
        bash = shutil.which('bash')
        if not bash and Path('C:/Program Files/Git/bin/bash.exe').exists():
            bash = 'C:/Program Files/Git/bin/bash.exe'
        if not bash:
            self.skipTest('Bash is required for the isolated installer integrity fixture')
        source = (SCRIPTS / 'linux_install.sh').read_text(encoding='utf-8')
        block = source[source.index('  # This is an explicit installation'):source.index('  # Verify against the release provenance manifest')]
        script = """set -euo pipefail
curl() { if [[ "$FIXTURE_AVAILABLE" == 0 ]]; then return 1; fi; printf '%s' "$FIXTURE_SUMS"; }
die() { echo "$*" >&2; exit 1; }
ok() { :; }
REPO_URL=https://example.invalid/repository
tag=2.0.1-daily.d20261012.1
asset_name=fixture.pkg.tar.zst
check() {
""" + block + "\n}\ncheck\n"
        with tempfile.TemporaryDirectory() as temp:
            package = Path(temp) / 'fixture.pkg.tar.zst'
            package.write_bytes(b'qualified-package')
            digest = hashlib.sha256(package.read_bytes()).hexdigest()
            env = dict(os.environ, local_package=str(package).replace(chr(92), '/'))
            for label, sums, available, expected in [
                ('valid', digest + '  fixture.pkg.tar.zst\n', '1', 0),
                ('wrong digest', '0' * 64 + '  fixture.pkg.tar.zst\n', '1', 1),
                ('absent package', digest + '  other.pkg.tar.zst\n', '1', 1),
                ('duplicate', (digest + '  fixture.pkg.tar.zst\n') * 2, '1', 1),
                ('unavailable', '', '0', 1),
            ]:
                with self.subTest(case=label):
                    env.update(FIXTURE_SUMS=sums, FIXTURE_AVAILABLE=available)
                    result = subprocess.run([bash, '-c', script], env=env, capture_output=True, text=True)
                    self.assertEqual(result.returncode, expected, result.stderr)

if __name__ == '__main__':
    unittest.main()
