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
        self.assertEqual(
            build_inputs["build_tests"],
            "${{ needs.release-candidate.outputs.should_release != 'true' }}",
        )
        self.assertNotIn("require_signpath_signing", build_inputs)
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

    def test_reusable_windows_workflow_supports_deferred_signing(self) -> None:
        workflow_path = ROOT / ".github" / "workflows" / "ci-windows.yml"
        workflow = load_workflow("ci-windows.yml")
        inputs = workflow["on"]["workflow_call"]["inputs"]
        jobs = workflow["jobs"]

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


if __name__ == "__main__":
    unittest.main()
