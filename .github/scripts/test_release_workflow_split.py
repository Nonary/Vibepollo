import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
COMPOSE_RELEASE_NOTES = ROOT / ".github" / "scripts" / "compose_release_notes.py"


def load_workflow(name: str) -> dict:
    with (ROOT / ".github" / "workflows" / name).open(encoding="utf-8") as stream:
        return yaml.load(stream, Loader=yaml.BaseLoader)


class ReleaseWorkflowSplitTest(unittest.TestCase):
    def test_tag_ci_builds_without_publishing(self) -> None:
        workflow = load_workflow("ci.yml")
        jobs = workflow["jobs"]
        build_inputs = jobs["build-windows"]["with"]
        awaiting_signing = jobs["awaiting-signing"]
        workflow_text = (ROOT / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("release", jobs)
        self.assertIn("build-archlinux", jobs)
        self.assertIn("awaiting-signing", jobs)
        self.assertIn("build-archlinux", awaiting_signing["needs"])
        self.assertIn(
            "github.ref == 'refs/heads/vibe-test'", jobs["build-archlinux"]["if"]
        )
        self.assertIn("should_release", build_inputs["build_only"])
        self.assertNotIn("require_signpath_signing", build_inputs)
        self.assertEqual(
            build_inputs["build_tests"],
            "${{ needs.release-candidate.outputs.should_release != 'true' }}",
        )
        self.assertEqual(
            build_inputs["release_artifact_retention_days"],
            "${{ needs.release-candidate.outputs.should_release == 'true' && 14 || 1 }}",
        )
        self.assertEqual(
            awaiting_signing["steps"][0]["env"]["BUILD_RUN_ID"],
            "${{ github.run_id }}",
        )
        self.assertIn("Leave \\`build_run_id\\` empty", workflow_text)
        self.assertIn("optional recovery override", workflow_text)
        self.assertIn('if tag.startswith("v"):', workflow_text)
        self.assertIn("release source tags must be v-less", workflow_text)
        self.assertIn(
            "valid_candidates.append((key, tag, notes_file, release_commit))",
            workflow_text,
        )
        self.assertIn('os.environ.get("GITHUB_REF") == "refs/heads/vibe-test"', workflow_text)
        self.assertIn("Linux branch candidate:", workflow_text)
        self.assertIn("Linux branch snapshot:", workflow_text)
        self.assertIn('snapshot_version = f"{release_version}+branch.{head_commit[:12]}"', workflow_text)
        self.assertIn("release_version=release_version", workflow_text)
        self.assertIn("release_version=snapshot_version", workflow_text)
        self.assertIn("should_release=\"false\"", workflow_text)
        self.assertNotIn("def canonical_release_tag", workflow_text)

    def test_arch_package_is_built_and_carried_into_release(self) -> None:
        pkgbuild = (ROOT / "packaging/linux/Arch/PKGBUILD").read_text(encoding="utf-8")
        self.assertIn("pkgname='vibepollo'", pkgbuild)
        self.assertIn("conflicts=('sunshine' 'vibeshine')", pkgbuild)
        ci_workflow = load_workflow("ci.yml")
        arch_workflow = load_workflow("ci-archlinux.yml")
        release_workflow = load_workflow("sign-release.yml")

        arch_call = ci_workflow["jobs"]["build-archlinux"]
        self.assertEqual(arch_call["uses"], "./.github/workflows/ci-archlinux.yml")
        self.assertEqual(
            arch_call["with"]["release_commit"],
            "${{ needs.release-candidate.outputs.release_commit || github.sha }}",
        )
        self.assertEqual(
            arch_call["with"]["artifact_retention_days"],
            "${{ needs.release-candidate.outputs.should_release == 'true' && 14 || 1 }}",
        )

        checkout = next(
            step
            for step in arch_workflow["jobs"]["build_archlinux"]["steps"]
            if step["name"] == "Checkout"
        )
        self.assertEqual(checkout["with"]["submodules"], "recursive")
        self.assertEqual(checkout["with"]["ref"], "${{ inputs.release_commit }}")

        arch_text = (ROOT / ".github" / "workflows" / "ci-archlinux.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn('find "/usr/lib/modules/${kernel_release}"', arch_text)
        self.assertNotIn('modinfo -k "${kernel_release}" -n vibeshine_drm', arch_text)
        self.assertIn(
            'write_drm_version_header "${module_build_dir}" "0.0.0"', arch_text
        )
        self.assertIn(
            'write_drm_version_header "${dkms_source}" "${dkms_ci_version}"',
            arch_text,
        )
        self.assertIn('if [[ "${BRANCH}" == "vibe-test" ]]', arch_text)
        self.assertIn('sub_version=".r${COMMIT}"', arch_text)
        self.assertIn("makedepends = nodejs", arch_text)
        self.assertIn("makedepends = npm", arch_text)
        self.assertIn("makedepends = ninja", arch_text)

        pkgbuild_text = (ROOT / "packaging" / "linux" / "Arch" / "PKGBUILD").read_text(
            encoding="utf-8"
        )
        self.assertIn("-G Ninja", pkgbuild_text)

        glad_text = (ROOT / "cmake" / "dependencies" / "glad.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn('COMMAND "${Python_EXECUTABLE}" -c "import jinja2"', glad_text)
        self.assertNotIn("import pkg_resources", glad_text)

        resolver = release_workflow["jobs"]["resolve_release"]
        self.assertIn("arch_artifact_id", resolver["outputs"])
        release_text = (ROOT / ".github" / "workflows" / "sign-release.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn('"build-Archlinux"', release_text)
        self.assertIn(".assets[$name] = $hash", release_text)
        release_steps = release_workflow["jobs"]["release"]["steps"]
        self.assertIn(
            "Download Arch Linux artifacts",
            {step["name"] for step in release_steps},
        )
        self.assertIn(
            "Include Arch Linux package",
            {step["name"] for step in release_steps},
        )
        self.assertIn(
            r"Download [\`${package_name}\`]("
            r"https://github.com/${GITHUB_REPOSITORY}/releases/download/${TAG_NAME}/${package_name})",
            release_text,
        )
        self.assertIn(
            "sudo pacman -U ./${package_name}",
            release_text,
        )
        self.assertIn(
            "Managed virtual displays require Linux 6.16 or newer.", release_text
        )
        self.assertIn(
            "https://github.com/${GITHUB_REPOSITORY}/blob/${TAG_NAME}/docs/linux/install.md",
            release_text,
        )
        self.assertIn("arch_package_version=${RELEASE_VERSION//-/}", release_text)
        self.assertIn("pkgver = ${arch_package_version}-1", release_text)

        publish_arch_workflow = load_workflow("publish-arch-repository.yml")
        self.assertIn("publish", publish_arch_workflow["jobs"])
        publish_arch_text = (
            ROOT / ".github" / "workflows" / "publish-arch-repository.yml"
        ).read_text(encoding="utf-8")
        self.assertIn("arch_package_version=${release_version//-/}", publish_arch_text)
        self.assertIn("pkgver = ${arch_package_version}-1", publish_arch_text)

        getting_started = (ROOT / "docs" / "getting_started.md").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("vibepollo.pkg.tar.gz", getting_started)
        self.assertIn("vibepollo-*.pkg.tar.zst", getting_started)
        self.assertIn("without guessing a package name", getting_started)

    def test_prerelease_notes_do_not_claim_to_cover_stable_releases(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            notes_dir = Path(temporary_dir) / "notes"
            notes_dir.mkdir()
            (notes_dir / "1.19.0-beta.3.md").write_text(
                "# 1.19.0-beta.3\n\n## Changes\n\n- Test prerelease notes.\n",
                encoding="utf-8",
            )
            output = Path(temporary_dir) / "release-notes.md"
            subprocess.run(
                [
                    sys.executable,
                    str(COMPOSE_RELEASE_NOTES),
                    "--release-version",
                    "1.19.0-beta.3",
                    "--notes-dir",
                    str(notes_dir),
                    "--output",
                    str(output),
                ],
                check=True,
            )
            rendered = output.read_text(encoding="utf-8")

        self.assertIn(
            "These release notes cover the 1.19.0-beta.3 prerelease.", rendered
        )
        self.assertNotIn("stable 1.19.0 releases", rendered)

    def test_manual_workflow_auto_resolves_an_exact_valid_build(self) -> None:
        workflow = load_workflow("sign-release.yml")
        jobs = workflow["jobs"]
        signing_inputs = jobs["build-windows"]["with"]
        dispatch_inputs = workflow["on"]["workflow_dispatch"]["inputs"]
        workflow_text = (ROOT / ".github" / "workflows" / "sign-release.yml").read_text(
            encoding="utf-8"
        )

        self.assertIn("workflow_dispatch", workflow["on"])
        self.assertEqual(dispatch_inputs["build_run_id"]["required"], "false")
        self.assertIn("Optional recovery override", dispatch_inputs["build_run_id"]["description"])
        self.assertIn("Auto-resolved exact CI build", workflow_text)
        self.assertIn("source_run_is_valid", workflow_text)
        self.assertIn(".head_sha == $release_commit", workflow_text)
        self.assertIn(".head_branch == $source_tag", workflow_text)
        self.assertIn('.path == ".github/workflows/ci.yml"', workflow_text)
        self.assertIn(
            'startswith(".github/workflows/ci.yml@")',
            workflow_text,
        )
        self.assertIn(
            '"${source_path}" != ".github/workflows/ci.yml" && "${source_path}" != ".github/workflows/ci.yml@"*',
            workflow_text,
        )
        self.assertIn("Vibepollo.msi", workflow_text)
        self.assertIn("windows-versioninfo-Windows", workflow_text)
        self.assertEqual(dispatch_inputs["signed_run_id"]["required"], "false")
        self.assertIn("finalized signed artifacts", dispatch_inputs["signed_run_id"]["description"])
        self.assertEqual(dispatch_inputs["release_draft"]["type"], "boolean")
        self.assertEqual(dispatch_inputs["release_draft"]["default"], "false")
        self.assertIn('source_tag="${requested_tag#v}"', workflow_text)
        self.assertIn('tag_name="${source_tag}"', workflow_text)
        self.assertNotIn('tag_name="v${release_version}"', workflow_text)
        self.assertEqual(
            signing_inputs["artifact_source_run_id"],
            "${{ needs.resolve_release.outputs.build_run_id }}",
        )
        self.assertEqual(signing_inputs["build_only"], "false")
        self.assertEqual(signing_inputs["require_signpath_signing"], "true")
        self.assertEqual(
            signing_inputs["signpath_wait_for_completion_timeout_in_seconds"],
            "3600",
        )
        self.assertIn("release", jobs)
        self.assertEqual(
            signing_inputs["release_tag"],
            "${{ needs.resolve_release.outputs.tag_name }}",
        )
        self.assertIn('--arg source_tag "${TAG_NAME}"', workflow_text)
        self.assertIn('--arg legacy_tag "${legacy_tag}"', workflow_text)
        self.assertIn('--arg tag_name "${publish_tag}"', workflow_text)
        self.assertIn(".tag_name // $source_tag", workflow_text)
        self.assertNotIn("target_commitish", workflow_text)
        self.assertIn("signed_run_is_valid", workflow_text)
        self.assertIn("release-provenance.json", workflow_text)
        self.assertIn("Reusing explicitly requested finalized signed artifacts", workflow_text)
        self.assertIn("Auto-reusing provenance-matched signed artifacts", workflow_text)
        self.assertEqual(
            jobs["build-windows"]["if"],
            "needs.resolve_release.outputs.signed_run_id == ''",
        )
        self.assertIn("reuse-windows", jobs)
        reuse_steps = {
            step["name"]: step for step in jobs["reuse-windows"]["steps"]
        }
        self.assertEqual(
            reuse_steps["Download finalized signed artifacts"]["with"]["run-id"],
            "${{ needs.resolve_release.outputs.signed_run_id }}",
        )
        reuse_verification = reuse_steps[
            "Verify reused signed artifacts and provenance"
        ]["run"]
        self.assertIn("Get-AuthenticodeSignature", reuse_verification)
        self.assertIn("$signature.Status -ne 'Valid'", reuse_verification)
        self.assertIn("Provenance hash mismatch", reuse_verification)
        self.assertIn("reuse-windows", jobs["release"]["needs"])
        self.assertIn("needs.build-windows.result == 'success'", jobs["release"]["if"])
        self.assertIn("needs.reuse-windows.result == 'success'", jobs["release"]["if"])
        self.assertIn(
            '[[ "${asset_name}" == "release-provenance.json" ]] && continue',
            workflow_text,
        )

        release_steps = jobs["release"]["steps"]
        close_issues = next(
            step for step in release_steps if step["name"] == "Close fixed issues for release"
        )
        self.assertEqual(close_issues["if"], "inputs.release_draft == false")
        self.assertIn('--argjson draft "${RELEASE_DRAFT}"', workflow_text)
        self.assertIn('if [[ "${RELEASE_DRAFT}" == "true" ]]', workflow_text)

    def test_reusable_windows_workflow_supports_deferred_signing(self) -> None:
        workflow_path = ROOT / ".github" / "workflows" / "ci-windows.yml"
        workflow = load_workflow("ci-windows.yml")
        inputs = workflow["on"]["workflow_call"]["inputs"]
        jobs = workflow["jobs"]
        workflow_text = workflow_path.read_text(encoding="utf-8")

        self.assertIn("artifact_source_run_id", inputs)
        self.assertIn("build_only", inputs)
        self.assertEqual(inputs["build_tests"]["type"], "boolean")
        self.assertEqual(inputs["build_tests"]["default"], "true")
        self.assertIn("resolve_source_artifacts", jobs)
        self.assertIn("release_artifacts", jobs)
        self.assertIn("inputs.build_only == false", jobs["sign_windows_msi"]["if"])
        signing_steps = {
            step["name"]: step for step in jobs["sign_windows_msi"]["steps"]
        }
        deferred_download = signing_steps[
            "Download deferred unsigned MSI for SignPath"
        ]
        self.assertEqual(
            deferred_download["with"]["artifact-ids"],
            "${{ needs.release_artifacts.outputs.unsigned_msi_artifact_id }}",
        )
        deferred_upload = signing_steps[
            "Re-upload deferred unsigned MSI for SignPath"
        ]
        self.assertEqual(
            deferred_upload["if"], "inputs.artifact_source_run_id != ''"
        )
        self.assertEqual(deferred_upload["with"]["archive"], "false")
        submit_signing = signing_steps[
            "Submit and wait for SignPath MSI signing request"
        ]
        self.assertEqual(
            submit_signing["with"]["github-artifact-id"],
            "${{ steps.select-signpath-msi-artifact.outputs.artifact_id }}",
        )
        for job_name in (
            "sign_windows_msi",
            "package_windows",
            "sign_windows_installer",
            "finalize_windows",
        ):
            condition = jobs[job_name]["if"]
            self.assertIn("always()", condition)
            self.assertIn("!cancelled()", condition)
        workflow_text = workflow_path.read_text(encoding="utf-8")
        self.assertIn(
            'direct_msi_artifact="${SYMBOL_PRODUCT_NAME}.msi"',
            workflow_text,
        )
        self.assertIn(
            "REQUIRE_SIGNPATH_SIGNING: ${{ inputs.require_signpath_signing }}",
            workflow_text,
        )
        self.assertIn("Deferred signing requires signpath_api_token.", workflow_text)
        self.assertIn(
            "-DBUILD_TESTS=${{ inputs.build_tests && 'ON' || 'OFF' }}",
            workflow_text,
        )
        self.assertIn(
            '"${source_path}" != ".github/workflows/ci.yml" && "${source_path}" != ".github/workflows/ci.yml@"*',
            workflow_text,
        )

    def test_signing_waits_at_five_second_intervals_for_ten_minutes(self) -> None:
        workflow = load_workflow("ci-windows.yml")
        inputs = workflow["on"]["workflow_call"]["inputs"]
        polling_action_path = (
            ROOT / ".github" / "actions" / "signpath-submit-and-wait" / "action.yml"
        )

        self.assertEqual(
            inputs["signpath_wait_for_completion_timeout_in_seconds"]["default"],
            "600",
        )
        self.assertTrue(polling_action_path.is_file())
        action_text = polling_action_path.read_text(encoding="utf-8")
        self.assertIn("wait-for-completion: false", action_text)
        self.assertIn("$timeoutSeconds = 0", action_text)
        self.assertIn("Start-Sleep -Seconds 5", action_text)
        self.assertIn("AddSeconds($timeoutSeconds)", action_text)


class WindowsWorkflowEfficiencyTest(unittest.TestCase):
    def test_release_build_uses_shallow_cached_dependencies(self) -> None:
        workflow = load_workflow("ci-windows.yml")
        workflow_text = (ROOT / ".github" / "workflows" / "ci-windows.yml").read_text(
            encoding="utf-8"
        )
        build_steps = workflow["jobs"]["build_windows"]["steps"]
        package_steps = workflow["jobs"]["package_windows"]["steps"]

        build_checkout = next(step for step in build_steps if step["name"] == "Checkout")
        package_checkout = next(step for step in package_steps if step["name"] == "Checkout")
        self.assertEqual(build_checkout["with"]["fetch-depth"], "1")
        self.assertEqual(build_checkout["with"]["submodules"], "recursive")
        self.assertEqual(package_checkout["with"]["fetch-depth"], "1")

        self.assertNotIn(
            "Update Windows dependencies",
            {step["name"] for step in build_steps},
        )
        setup = next(
            step for step in build_steps if step["name"] == "Setup Dependencies Windows"
        )
        self.assertEqual(setup["with"]["cache"], "true")
        install = setup["with"]["install"]
        packages = (
            "git",
            "mingw-w64-${{ matrix.toolchain }}-boost",
            "mingw-w64-${{ matrix.toolchain }}-cmake",
            "mingw-w64-${{ matrix.toolchain }}-cppwinrt",
            "mingw-w64-${{ matrix.toolchain }}-curl-winssl",
            "mingw-w64-${{ matrix.toolchain }}-gcc",
            "mingw-w64-${{ matrix.toolchain }}-MinHook",
            "mingw-w64-${{ matrix.toolchain }}-miniupnpc",
            "mingw-w64-${{ matrix.toolchain }}-ninja",
            "mingw-w64-${{ matrix.toolchain }}-nlohmann-json",
            "mingw-w64-${{ matrix.toolchain }}-onevpl",
            "mingw-w64-${{ matrix.toolchain }}-openssl",
            "mingw-w64-${{ matrix.toolchain }}-opus",
            "mingw-w64-${{ matrix.toolchain }}-sqlite3",
            "mingw-w64-${{ matrix.toolchain }}-tools",
        )
        for package in packages:
            self.assertIn(package, install)
        self.assertNotIn("wget", install)
        self.assertNotIn("-toolchain", install)
        self.assertNotIn(
            "-DBUILD_WERROR=ON \\\n            # Release tag builds",
            workflow_text,
        )
        self.assertIn(
            "          # Release tag builds save unsigned artifacts and rely on prior branch/PR testing; other calls retain tests.\n"
            "          cmake \\",
            workflow_text,
        )
        self.assertIn("Record release artifact provenance", workflow_text)
        self.assertIn("Upload release provenance", workflow_text)
        self.assertIn("source_build_run_id", workflow_text)
        self.assertIn(
            "-DBUILD_TESTS=${{ inputs.build_tests && 'ON' || 'OFF' }}",
            workflow_text,
        )
        options_text = (ROOT / "cmake" / "prep" / "options.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn('option(BUILD_TESTS "Build unit tests." ON)', options_text)
        self.assertNotIn("set(BUILD_TESTS", options_text)


class WindowsWorkflowEfficiencyTest(unittest.TestCase):
    def test_web_dependency_install_does_not_invalidate_cmake_globs(self) -> None:
        web_targets = (ROOT / "cmake" / "targets" / "web.cmake").read_text(
            encoding="utf-8"
        )

        self.assertNotRegex(web_targets, r"(?m)^\s*CONFIGURE_DEPENDS\s*$")
        self.assertEqual(web_targets.count("--prefer-offline"), 2)

    def test_explicit_build_version_does_not_require_branch_context(self) -> None:
        version_script = (ROOT / "cmake" / "prep" / "build_version.cmake").read_text(
            encoding="utf-8"
        )

        self.assertIn(
            'if(DEFINED ENV{BUILD_VERSION} AND NOT "$ENV{BUILD_VERSION}" STREQUAL "")',
            version_script,
        )
        self.assertIn(
            'if(DEFINED BUILD_VERSION AND NOT "${BUILD_VERSION}" STREQUAL "")',
            version_script,
        )
        self.assertNotIn(
            "if((DEFINED ENV{BRANCH}) AND (DEFINED ENV{BUILD_VERSION}))",
            version_script,
        )

        with tempfile.TemporaryDirectory() as temporary_dir:
            probe = Path(temporary_dir) / "probe-version.cmake"
            probe.write_text(
                f'include("{(ROOT / "cmake" / "prep" / "build_version.cmake").as_posix()}")\n'
                'if(NOT PROJECT_VERSION_FULL STREQUAL "1.19.0-beta.5")\n'
                '  message(FATAL_ERROR "explicit cache version was not retained: ${PROJECT_VERSION_FULL}")\n'
                'endif()\n'
                'if(NOT PROJECT_VERSION_NUMERIC STREQUAL "1.19.0")\n'
                '  message(FATAL_ERROR "numeric version was not split correctly: ${PROJECT_VERSION_NUMERIC}")\n'
                'endif()\n',
                encoding="utf-8",
            )
            environment = os.environ.copy()
            environment.pop("BUILD_VERSION", None)
            environment.pop("BRANCH", None)
            subprocess.run(
                ["cmake", "-DBUILD_VERSION=1.19.0-beta.5", "-P", str(probe)],
                cwd=ROOT,
                env=environment,
                check=True,
                capture_output=True,
                text=True,
            )

            empty_environment = environment.copy()
            empty_environment["BUILD_VERSION"] = ""
            subprocess.run(
                ["cmake", "-DBUILD_VERSION=1.19.0-beta.5", "-P", str(probe)],
                cwd=ROOT,
                env=empty_environment,
                check=True,
                capture_output=True,
                text=True,
            )

            invalid_version = subprocess.run(
                ["cmake", "-DBUILD_VERSION=1.19", "-P", str(probe)],
                cwd=ROOT,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(invalid_version.returncode, 0)
            self.assertIn("Invalid Vibeshine build version", invalid_version.stderr)

            zero_version = subprocess.run(
                ["cmake", "-DBUILD_VERSION=0.0.0", "-P", str(probe)],
                cwd=ROOT,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(zero_version.returncode, 0)
            self.assertIn("Version resolution produced 0.0.0", zero_version.stderr)

    def test_stable_respins_keep_the_stable_windows_version_ordinal(self) -> None:
        wix_version = (ROOT / "cmake" / "packaging" / "windows_wix.cmake").read_text(
            encoding="utf-8"
        )
        executable_version = (
            ROOT / "cmake" / "prep" / "emit_windows_versioninfo.cmake"
        ).read_text(encoding="utf-8")
        bootstrapper = (
            ROOT / "packaging" / "windows" / "bootstrapper" / "VibeshineInstaller.cs"
        ).read_text(encoding="utf-8")

        self.assertIn(
            'elseif(_pre_tag STREQUAL "stable")\n'
            '    # Stable respins remain in the stable channel. Their sortable ProductCodes\n'
            '    # distinguish stable.N packages that share this MSI ProductVersion.\n'
            '    set(_WIX_PRERELEASE_ORDINAL 99)',
            wix_version,
        )
        self.assertIn(
            'elseif("${_pre_tag}" STREQUAL "stable")\n'
            '            # Keep stable respins in the stable channel. The timed revision\n'
            '            # orders successive stable.N executable builds.\n'
            '            set(_ordinal 99)',
            executable_version,
        )
        self.assertIn(
            'if (string.Equals(tag, "stable", StringComparison.Ordinal)) {\n'
            '        // Stable respins share the stable ordinal; sortable ProductCodes order\n'
            '        // distinct MSI packages within that channel.\n'
            '        return 99;',
            bootstrapper,
        )

    def test_windows_steam_artwork_dependencies(self) -> None:
        workflow = load_workflow("ci-windows.yml")
        steps = workflow["jobs"]["build_windows"]["steps"]
        setup = next(step for step in steps if step["name"] == "Setup Dependencies Windows")
        for package in ("libjpeg-turbo", "libpng", "libwebp"):
            self.assertIn("mingw-w64-${{ matrix.toolchain }}-" + package, setup["with"]["install"])

if __name__ == "__main__":
    unittest.main()
