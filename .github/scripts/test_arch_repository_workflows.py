import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]


def load_workflow(name: str) -> dict:
    with (ROOT / ".github" / "workflows" / name).open(encoding="utf-8") as stream:
        return yaml.load(stream, Loader=yaml.BaseLoader)


class ArchRepositoryWorkflowTest(unittest.TestCase):
    def test_cuda_shared_cache_contains_only_versioned_installer_without_fallback(self) -> None:
        job = load_workflow("ci-archlinux.yml")["jobs"]["build_archlinux"]
        steps = {step.get("name"): step for step in job["steps"]}
        restore = steps["Restore checksum-verified CUDA installer"]
        save = steps["Save checksum-verified CUDA installer"]
        self.assertEqual(restore["with"]["path"], "pkg/cuda-x86_64.run")
        self.assertEqual(save["with"]["path"], "pkg/cuda-x86_64.run")
        self.assertIn("-cuda-installer-v2-", restore["with"]["key"])
        self.assertNotIn("restore-keys", restore["with"])
        self.assertEqual(save["with"]["key"], "${{ steps.cuda-cache.outputs.cache-primary-key }}")
        # The shared archive never restores the SDK or its receipt into scratch.
        for step in job["steps"]:
            if "actions/cache/" in step.get("uses", ""):
                self.assertNotIn(".linux-cache/cuda", step["with"]["path"])
        self.assertEqual(job["env"]["CUDA_CACHE_DIR"], "${{ github.workspace }}/.linux-cache/cuda")
        # Both successful publication and trusted-failure receipts retain the
        # original save restrictions; cache size work does not relax eligibility.
        self.assertIn("steps.cuda-cache.outputs.cache-hit != 'true'", save["if"])
        self.assertIn("success()", save["if"])
        self.assertIn("github.event.repository.default_branch", save["if"])
        self.assertIn("github.event.repository.full_name == github.repository", save["if"])
        self.assertIn("steps.partial-cache.outputs.cuda == 'true'", save["if"])
        partial = steps["Verify failed-validation partial cache eligibility"]
        for required in ("failure()", "inputs.validation_only", "github.event_name == 'workflow_dispatch'",
                         "github.event.repository.full_name == github.repository", "steps.build.outcome == 'failure'",
                         "steps.toolchain.outputs.abi != ''"):
            self.assertIn(required, partial["if"])
        build = steps["Build PKGBUILD"]["run"]
        self.assertIn('CUDA_CACHE_DIR="$CUDA_CACHE_DIR"', build)
        self.assertIn('makepkg "${package_options[@]}"', build)
        self.assertIn("bash scripts/check-linux-partial-cache.sh", partial["run"])

    def test_pages_is_native_and_deploys_the_arch_branch(self) -> None:
        workflow = load_workflow("update-pages.yml")
        text = (ROOT / ".github" / "workflows" / "update-pages.yml").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("LizardByte", text)
        self.assertEqual(workflow["jobs"]["build"]["permissions"]["contents"], "read")
        self.assertEqual(workflow["jobs"]["deploy"]["permissions"]["pages"], "write")
        self.assertEqual(workflow["jobs"]["deploy"]["permissions"]["id-token"], "write")
        self.assertIn("refs/heads/arch-repo", text)
        self.assertIn("actions/configure-pages@45bfe0192ca1faeb007ade9deae92b16b8254a0d", text)
        self.assertIn("actions/upload-pages-artifact@fc324d3547104276b827a68afc52ff2a11cc49c9", text)
        self.assertIn("actions/deploy-pages@cd2ce8fcbc39b97be8ca5fce6e763baed58fa128", text)

    def test_arch_repository_requires_and_verifies_signing_identity(self) -> None:
        workflow = load_workflow("publish-arch-repository.yml")
        job = workflow["jobs"]["publish"]
        text = (
            ROOT / ".github" / "workflows" / "publish-arch-repository.yml"
        ).read_text(encoding="utf-8")

        self.assertEqual(job["permissions"]["contents"], "write")
        self.assertEqual(job["permissions"]["actions"], "write")
        self.assertEqual(job["environment"], "arch-repository")
        self.assertIn("ARCH_REPO_GPG_PRIVATE_KEY", text)
        self.assertIn("ARCH_REPO_GPG_PASSPHRASE", text)
        self.assertIn("ARCH_REPO_GPG_FINGERPRINT", text)
        self.assertIn("Imported signing key fingerprint does not match", text)
        self.assertIn("schedule", workflow["on"])
        self.assertNotIn("allow_draft", workflow["on"]["workflow_dispatch"]["inputs"])
        self.assertIn("/releases/latest", text)
        self.assertIn('vercmp "$package_version" "$previous_version"', text)
        self.assertIn("--detach-sign \"incoming/${PACKAGE_NAME}\"", text)
        self.assertIn("--detach-sign vibepollo.db.tar.gz", text)
        self.assertIn("gpg --batch --verify", text)
        self.assertIn("git -C \"${publication_dir}\" push origin HEAD:arch-repo", text)
        self.assertIn("gh workflow run update-pages.yml --ref master", text)

    def test_public_site_uses_nonary_vibepollo_identity(self) -> None:
        site = (ROOT / "gh-pages-template" / "index.html").read_text(encoding="utf-8")
        self.assertIn("Vibepollo by Nonary", site)
        self.assertIn("https://github.com/Nonary/Vibepollo", site)
        self.assertIn("https://nonary.github.io/Vibepollo/arch/x86_64", site)
        self.assertNotIn("LizardByte", site)


@unittest.skipUnless(
    all(shutil.which(tool) for tool in ("repo-add", "bsdtar", "gpg", "gpgconf", "gpgv", "vercmp", "git")),
    "Arch database publication fixtures require repo-add, bsdtar and gpg",
)
class ArchRepositoryPublicationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.key_dir = tempfile.TemporaryDirectory(prefix="arch-publication-key-")
        cls.addClassCleanup(cls.key_dir.cleanup)
        cls.signing_env = os.environ | {"GNUPGHOME": cls.key_dir.name}
        cls.addClassCleanup(subprocess.run, ["gpgconf", "--kill", "gpg-agent"], env=cls.signing_env)
        cls.passphrase = "publication-fixture"
        generation = subprocess.run(
            [
                "gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase",
                cls.passphrase, "--quick-generate-key",
                "Arch publication fixture <arch-publication@example.invalid>",
                "ed25519", "sign", "0",
            ],
            env=cls.signing_env, text=True, capture_output=True,
        )
        if generation.returncode:
            raise RuntimeError(generation.stderr)
        keys = subprocess.check_output(
            ["gpg", "--batch", "--with-colons", "--list-secret-keys"],
            env=cls.signing_env, text=True,
        )
        cls.fingerprint = next(
            line.split(":")[9] for line in keys.splitlines() if line.startswith("fpr:")
        )

    def setUp(self) -> None:
        temporary_dir = tempfile.TemporaryDirectory(prefix="arch-publication-")
        self.addCleanup(temporary_dir.cleanup)
        self.root = Path(temporary_dir.name)
        self.repository = self.root / "arch-repo" / "x86_64"
        self.repository.mkdir(parents=True)
        self.incoming = self.root / "incoming"
        self.incoming.mkdir()
        self.product = "vibepollo"
        self.tools_dir = self.root / "bin"
        self.tools_dir.mkdir()
        self.env = self.signing_env | {
            "FINGERPRINT": self.fingerprint,
            "SIGNING_PASSPHRASE": self.passphrase,
            "GITHUB_WORKSPACE": str(self.root),
            "RUNNER_TEMP": str(self.root),
            "GITHUB_REPOSITORY": f"Nonary/{self.product}",
            "GITHUB_EVENT_NAME": "schedule",
            "GITHUB_OUTPUT": str(self.root / "outputs"),
            "GIT_CONFIG_GLOBAL": str(self.root / "gitconfig"),
            "PATH": f"{self.tools_dir}:{os.environ['PATH']}",
        }
        helper = Path(".github/scripts/arch_release_assets.py")
        (self.root / helper).parent.mkdir(parents=True)
        shutil.copyfile(ROOT / helper, self.root / helper)
        self.helper = self.root / helper
        steps = load_workflow("publish-arch-repository.yml")["jobs"]["publish"]["steps"]
        self.publication_script = next(step["run"] for step in steps if step["name"] == "Update repository database")
        self.signing_script = next(step["run"] for step in steps if step["name"] == "Sign package and export public key")
        self.validation_script = next(step["run"] for step in steps if step.get("id") == "release")
        self.deploy_script = next(step["run"] for step in steps if step["name"] == "Deploy updated repository through Pages")
        self.package_count = 0

        self.write_tool("gh", """#!/bin/bash
case "$1" in
  api)
    if [[ "$2" == "-H" ]]; then cat "$FIXTURE_ASSET"; else cat "$FIXTURE_RELEASE"; fi ;;
  workflow) touch "$RUNNER_TEMP/deployment-requested" ;;
  *) exit 1 ;;
esac
""")
        self.write_tool("curl", """#!/bin/bash
while [[ "$#" -gt 1 ]]; do
  if [[ "$1" == "--output" ]]; then output="$2"; shift; fi
  shift
done
if [[ -n "$output" ]]; then
  cp "$RUNNER_TEMP/assets/${1##*/}" "$output"
else
  exit 22  # Simulate a push that succeeded while the previous Pages run failed.
fi
""")

    def write_tool(self, name, text):
        tool = self.tools_dir / name
        tool.write_text(text, encoding="utf-8")
        tool.chmod(0o755)

    def run_command(self, command, *, check=True):
        result = subprocess.run(
            command, cwd=self.root, env=self.env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        if check:
            self.assertEqual(result.returncode, 0, result.stdout)
        return result

    def make_package(self, version, *, name=None, arch="x86_64"):
        name = name or self.product
        self.package_count += 1
        contents = self.root / f"package-{self.package_count}"
        (contents / "usr" / "bin").mkdir(parents=True)
        (contents / "usr" / "bin" / name).write_text("fixture\n", encoding="utf-8")
        (contents / ".PKGINFO").write_text(
            f"pkgname = {name}\npkgver = {version}\narch = {arch}\n"
            "pkgdesc = Publication fixture\nsize = 8\nbuilddate = 1788846403\ndepend = glibc>=2\n",
            encoding="utf-8",
        )
        assets = self.root / "assets"
        assets.mkdir(exist_ok=True)
        package = assets / f"{name}-{version.split(':')[-1]}-x86_64.pkg.tar.zst"
        self.run_command(["bsdtar", "--zstd", "-cf", str(package), "-C", str(contents), ".PKGINFO", "usr"])
        return package

    def validate(self, package, tag, *, check=True, **overrides):
        data = {
            "tag_name": tag, "draft": False, "prerelease": False,
            "published_at": "2026-11-03T04:07:29Z",
            "assets": [{"id": 1, "name": package.name, "state": "uploaded",
                        "digest": f"sha256:{hashlib.sha256(package.read_bytes()).hexdigest()}"}],
        }
        release = self.root / "release.json"
        release.write_text(json.dumps(data | overrides), encoding="utf-8")
        self.env.update(FIXTURE_ASSET=str(package), FIXTURE_RELEASE=str(release))
        Path(self.env["GITHUB_OUTPUT"]).write_text("")
        result = self.run_command(["bash", "-c", self.validation_script], check=check)
        outputs = dict(line.split("=", 1) for line in Path(self.env["GITHUB_OUTPUT"]).read_text().splitlines())
        return result, outputs

    def publish(self, package, tag):
        _, outputs = self.validate(package, tag)
        self.assertEqual(outputs["changed"], "true")
        self.env.update(PACKAGE_NAME=outputs["package_name"], PREVIOUS_PACKAGE=outputs["previous_package"],
                        ASSET_DIGEST=outputs["digest"])
        self.run_command(["bash", "-c", self.signing_script])
        self.run_command(["bash", "-c", self.publication_script])
        self.assertFalse((self.repository / package.name).exists())
        self.assertTrue((self.repository / f"{package.name}.sig").is_file())

    def restore(self, directory=None, *, check=True):
        target = directory or self.repository
        return self.run_command(["python", str(self.helper), "restore", str(target)], check=check)

    def test_rebuild_signed_indexes_restore_payloads_and_retry_pages(self):
        old = self.make_package("2.0.0-1")
        self.publish(old, "2.0.0")
        latest = self.make_package("2.0.6-1")
        self.publish(latest, "2.0.6")
        self.assertEqual((self.repository / "release-assets.tsv").read_text().count("\n"), 2)

        snapshot = self.root / "pages"
        shutil.copytree(self.repository, snapshot)
        self.restore(snapshot)
        for package in (old, latest):
            self.assertEqual((snapshot / package.name).read_bytes(), package.read_bytes())

        expected = hashlib.sha256(latest.read_bytes()).hexdigest()
        for kind in ("db", "files"):
            archive = snapshot / f"{self.product}.{kind}.tar.gz"
            entries = self.run_command(["bsdtar", "-tf", str(archive)]).stdout.splitlines()
            self.assertEqual({entry.split("/")[0] for entry in entries}, {f"{self.product}-2.0.6-1"})
            desc = self.run_command(["bsdtar", "-xOf", str(archive), f"{self.product}-2.0.6-1/desc"]).stdout
            self.assertIn(f"%FILENAME%\n{latest.name}\n", desc)
            self.assertIn("%DEPENDS%\nglibc>=2\n", desc)
            self.assertIn(f"%SHA256SUM%\n{expected}\n", desc)

        _, outputs = self.validate(latest, "2.0.6")
        self.assertEqual(outputs["changed"], "false")
        self.env.update(CHANGED="false", PACKAGE_NAME=latest.name)
        self.run_command(["bash", "-c", self.deploy_script])
        self.assertTrue((self.root / "deployment-requested").exists())

    def test_stale_publication_cannot_replace_remote_index(self):
        old = self.make_package("2.0.0-1")
        self.publish(old, "2.0.0")
        remote = self.root / "remote.git"
        self.run_command(["git", "init", "--bare", str(remote)])
        publication = self.repository.parent
        self.run_command(["git", "-C", str(publication), "init", "-b", "arch-repo"])
        self.run_command(["git", "-C", str(publication), "remote", "add", "origin", str(remote)])
        steps = load_workflow("publish-arch-repository.yml")["jobs"]["publish"]["steps"]
        commit = next(step["run"] for step in steps if step["name"] == "Commit published repository")
        self.env["RELEASE_VERSION"] = "2.0.0"
        self.run_command(["bash", "-c", commit])

        stale = self.root / "stale"
        self.run_command(["git", "clone", "--branch", "arch-repo", str(remote), str(stale / "arch-repo")])
        self.publish(self.make_package("2.0.6-1"), "2.0.6")
        self.env["RELEASE_VERSION"] = "2.0.6"
        self.run_command(["bash", "-c", commit])
        successful_head = self.run_command(["git", "--git-dir", str(remote), "rev-parse", "arch-repo"]).stdout

        self.env["RUNNER_TEMP"] = str(stale)
        self.repository = stale / "arch-repo" / "x86_64"
        self.publish(self.make_package("2.0.4-1"), "2.0.4")
        self.env["RELEASE_VERSION"] = "2.0.4"
        rejected = self.run_command(["bash", "-c", commit], check=False)
        self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
        self.assertIn("[rejected]", rejected.stdout)
        self.assertEqual(self.run_command(["git", "--git-dir", str(remote), "rev-parse", "arch-repo"]).stdout,
                         successful_head)

    def test_version_order_uses_epoch_and_pkgrel(self):
        first = self.make_package("1:2.0.6-2")
        self.publish(first, "2.0.6")
        for tag, version in (("2.18.9", "2.18.9-1"), ("2.0.6", "1:2.0.6-1")):
            with self.subTest(version=version):
                result, _ = self.validate(self.make_package(version), tag, check=False)
                self.assertIn("Refusing to replace indexed package", result.stdout)
        self.publish(self.make_package("1:2.0.6-2.1"), "2.0.6")

    def test_wrong_channel_architecture_and_bytes_fail_closed(self):
        package = self.make_package("2.0.6-1")
        for flag in ("draft", "prerelease"):
            result, _ = self.validate(package, "2.0.6", check=False, **{flag: True})
            self.assertNotEqual(result.returncode, 0, result.stdout)
        result, _ = self.validate(package, "2.0.6daily.d20261118", check=False)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        wrong_arch = self.make_package("2.0.7-1", arch="aarch64")
        result, _ = self.validate(wrong_arch, "2.0.7", check=False)
        self.assertIn("Package metadata does not match", result.stdout)
        self.publish(package, "2.0.6")
        package.write_bytes(b"mismatched payload")
        result, _ = self.validate(package, "2.0.6", check=False)
        self.assertIn("changed bytes without a new package version", result.stdout)
        result = self.restore(check=False)
        self.assertIn("Release asset SHA-256 mismatch", result.stdout)

    def test_pages_rejects_a_damaged_index_or_package_signature(self):
        package = self.make_package("2.0.6-1")
        self.publish(package, "2.0.6")
        for filename in (f"{self.product}.db", f"{self.product}.files.sig", f"{package.name}.sig"):
            with self.subTest(filename=filename):
                original = self.repository / filename
                content = original.read_bytes()
                original.write_bytes(b"bad")
                self.assertNotEqual(self.restore(check=False).returncode, 0)
                original.write_bytes(content)


if __name__ == "__main__":
    unittest.main()
