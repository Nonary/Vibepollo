import hashlib
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
        self.assertIn("Release ${source_tag} is still a draft", text)
        self.assertIn("Expected exactly one non-debug Vibepollo Arch package", text)
        self.assertIn("arch_package_version=${release_version//-/}", text)
        self.assertIn("pkgver = ${arch_package_version}-1", text)
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
    all(shutil.which(tool) for tool in ("repo-add", "bsdtar", "gpg", "gpgconf")),
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
        self.package_name = "vibepollo-2.0.0-1-x86_64.pkg.tar.zst"
        self.env = self.signing_env | {
            "FINGERPRINT": self.fingerprint,
            "PACKAGE_NAME": self.package_name,
            "SIGNING_PASSPHRASE": self.passphrase,
            "publication_dir": str(self.repository.parent),
        }
        steps = load_workflow("publish-arch-repository.yml")["jobs"]["publish"]["steps"]
        update = next(step["run"] for step in steps if step["name"] == "Update repository database")
        # Execute the real database/signature/payload operations without a remote push.
        self.publication_script = update[update.index('cd "${publication_dir}/x86_64"'):]
        download = next(step["run"] for step in steps if step.get("id") == "release")
        self.validation_script = download[
            download.index("package_info="):download.index('echo "package_name=')
        ]
        self.package_count = 0

    def run_command(self, command: list[str], *, check: bool = True) -> subprocess.CompletedProcess:
        result = subprocess.run(
            command, cwd=self.root, env=self.env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        if check:
            self.assertEqual(result.returncode, 0, result.stdout)
        return result

    def make_package(self, version: str, *, name: str = "vibepollo") -> Path:
        self.package_count += 1
        contents = self.root / f"package-{self.package_count}"
        (contents / "usr" / "bin").mkdir(parents=True)
        (contents / "usr" / "bin" / name).write_text("fixture\n", encoding="utf-8")
        (contents / ".PKGINFO").write_text(
            f"pkgname = {name}\npkgver = {version}\narch = x86_64\n"
            "pkgdesc = Publication fixture\nsize = 8\nbuilddate = 1788846403\n",
            encoding="utf-8",
        )
        package = self.repository / f"{name}-{version}-x86_64.pkg.tar.zst"
        self.run_command(["bsdtar", "--zstd", "-cf", str(package), "-C", str(contents), ".PKGINFO", "usr"])
        self.run_command([
            "gpg", "--batch", "--yes", "--pinentry-mode", "loopback", "--passphrase",
            self.passphrase, "--local-user", self.fingerprint, "--detach-sign", str(package),
        ])
        return package

    def publish(self) -> None:
        self.run_command(["bash", "-euo", "pipefail", "-c", self.publication_script])

    def assert_current_repository(self) -> None:
        package = self.repository / self.package_name
        digest = hashlib.sha256(package.read_bytes()).hexdigest()
        for kind in ("db", "files"):
            archive = self.repository / f"vibepollo.{kind}.tar.gz"
            entries = self.run_command(["bsdtar", "-tf", str(archive)]).stdout.splitlines()
            self.assertEqual({entry.split("/")[0] for entry in entries}, {"vibepollo-2.0.0-1"})
            description = self.run_command([
                "bsdtar", "-xOf", str(archive), "vibepollo-2.0.0-1/desc",
            ]).stdout
            self.assertIn(f"%FILENAME%\n{self.package_name}\n", description)
            self.assertIn("%VERSION%\n2.0.0-1\n", description)
            self.assertIn(f"%SHA256SUM%\n{digest}\n", description)
            self.run_command(["gpg", "--batch", "--verify", f"{archive}.sig", str(archive)])
            for suffix in ("", ".sig"):
                alias = self.repository / f"vibepollo.{kind}{suffix}"
                self.assertFalse(alias.is_symlink())
                self.assertEqual(alias.read_bytes(), Path(f"{archive}{suffix}").read_bytes())
        self.run_command(["gpg", "--batch", "--verify", f"{package}.sig", str(package)])
        self.assertEqual(
            {path.name for path in self.repository.glob("vibepollo-*.pkg.tar.zst*")},
            {self.package_name, f"{self.package_name}.sig"},
        )
        self.assertFalse(list(self.repository.glob("*.old*")))

    def test_first_publication_and_republication(self) -> None:
        self.make_package("2.0.0-1")
        self.publish()
        self.assert_current_repository()
        self.publish()
        self.assert_current_repository()

    def test_old_payloads_and_ghost_entries_are_replaced(self) -> None:
        old = self.make_package("2.0.0beta.1-1")
        ghost = self.make_package("1.0.0-1", name="removed-addon")
        self.run_command(["repo-add", str(self.repository / "vibepollo.db.tar.gz"), str(old), str(ghost)])
        ghost.unlink()
        Path(f"{ghost}.sig").unlink()
        # A stray broken package must never enter the new database's input list.
        (self.repository / "vibepollo-9.9.9-1-x86_64.pkg.tar.zst").write_bytes(b"broken")
        (self.repository / "vibepollo-orphan.pkg.tar.zst.sig").write_bytes(b"orphan")
        for kind in ("db", "files"):
            (self.repository / f"vibepollo.{kind}.tar.gz.old").write_bytes(b"old archive")
            (self.repository / f"vibepollo.{kind}.tar.gz.old.sig").write_bytes(b"old signature")
        self.make_package("2.0.0-1")
        self.publish()
        self.assert_current_repository()

    def test_missing_previous_payload_does_not_block_publication(self) -> None:
        old = self.make_package("2.0.0beta.1-1")
        self.run_command(["repo-add", str(self.repository / "vibepollo.db.tar.gz"), str(old)])
        old.unlink()
        Path(f"{old}.sig").unlink()
        self.make_package("2.0.0-1")
        self.publish()
        self.assert_current_repository()

    def test_release_version_is_compared_literally(self) -> None:
        incoming = self.root / "incoming"
        incoming.mkdir()
        for release, version, accepted in (
            ("2.0.0", "2.0.0-1", True),
            ("2.0.0-beta.2", "2.0.0beta.2-1", True),
            ("2.0.0", "2x0x0-1", False),
            ("2.0.0-beta.2", "2.0.0betaX2-1", False),
        ):
            with self.subTest(release=release, version=version):
                package = self.make_package(version)
                shutil.copyfile(package, incoming / self.package_name)
                self.env.update(release_version=release, package_name=self.package_name)
                result = self.run_command(
                    ["bash", "-euo", "pipefail", "-c", self.validation_script], check=False,
                )
                self.assertEqual(result.returncode == 0, accepted, result.stdout)


if __name__ == "__main__":
    unittest.main()
