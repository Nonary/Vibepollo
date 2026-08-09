import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]


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
        self.assertIn("awaiting-signing", jobs)
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
        self.assertNotIn("def canonical_release_tag", workflow_text)

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
            "600",
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
        self.assertNotIn(
            "if((DEFINED ENV{BRANCH}) AND (DEFINED ENV{BUILD_VERSION}))",
            version_script,
        )

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


if __name__ == "__main__":
    unittest.main()
