import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]


def load_workflow(name: str) -> dict:
    with (ROOT / ".github" / "workflows" / name).open(encoding="utf-8") as stream:
        return yaml.load(stream, Loader=yaml.BaseLoader)


class ArchRepositoryWorkflowTest(unittest.TestCase):
    def test_portable_packages_match_the_linux_product_identity(self) -> None:
        appimage = (ROOT / ".github/workflows/ci-linux.yml").read_text(encoding="utf-8")
        flatpak = (ROOT / ".github/workflows/ci-flatpak.yml").read_text(encoding="utf-8")
        self.assertIn("--executable ./vibepollo", appimage)
        self.assertIn('ICON_FILE="${ICON_FILE:-apollo.svg}"', appimage)
        self.assertTrue((ROOT / "apollo.svg").is_file())
        self.assertIn("mv [Vv]ibepollo*.AppImage ../artifacts/vibepollo.AppImage", appimage)
        self.assertIn("../artifacts/vibepollo_${MATRIX_ARCH}.flatpak", flatpak)
        self.assertIn("APP_ID: io.github.Nonary.vibepollo", flatpak)
        self.assertIn("mkdir -p build\n          cp glad-dependencies.json build/", flatpak)

    def test_native_packages_replace_both_legacy_hosts(self) -> None:
        cpack = (ROOT / "cmake/packaging/linux.cmake").read_text(encoding="utf-8")
        rpm = (ROOT / "packaging/linux/copr/Sunshine.spec").read_text(encoding="utf-8")
        arch = (ROOT / "packaging/linux/Arch/PKGBUILD").read_text(encoding="utf-8")
        self.assertIn('CPACK_DEBIAN_PACKAGE_CONFLICTS "sunshine, vibeshine"', cpack)
        self.assertIn('CPACK_RPM_PACKAGE_CONFLICTS "Sunshine, sunshine, vibeshine"', cpack)
        self.assertIn("Conflicts: Sunshine sunshine vibeshine", rpm)
        self.assertIn("%{_bindir}/vibepollo-mangohud", rpm)
        self.assertIn("conflicts=('sunshine' 'vibeshine')", arch)

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


if __name__ == "__main__":
    unittest.main()
