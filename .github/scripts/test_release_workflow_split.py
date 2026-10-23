import io
import os
import re
import xml.etree.ElementTree as ET
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
COMPOSE_RELEASE_NOTES = ROOT / ".github" / "scripts" / "compose_release_notes.py"


def load_workflow(name: str) -> dict:
    with (ROOT / ".github" / "workflows" / name).open(encoding="utf-8") as stream:
        return yaml.load(stream, Loader=yaml.BaseLoader)


class ReleaseWorkflowSplitTest(unittest.TestCase):
    def test_release_tag_uses_hosted_runners_with_explicit_manual_opt_in(self) -> None:
        ci_workflow = load_workflow("ci.yml")
        dispatch_input = ci_workflow["on"]["workflow_dispatch"]["inputs"][
            "use_self_hosted"
        ]
        self.assertEqual(dispatch_input["type"], "boolean")
        self.assertEqual(dispatch_input["default"], "false")

        expected_forwarding = (
            "${{ github.event_name == 'workflow_dispatch' && inputs.use_self_hosted }}"
        )
        self.assertEqual(
            ci_workflow["jobs"]["build-windows"]["with"]["use_self_hosted"],
            expected_forwarding,
        )
        self.assertEqual(
            ci_workflow["jobs"]["build-archlinux"]["with"]["use_self_hosted"],
            expected_forwarding,
        )
        for job in ("build-windows", "build-archlinux"):
            forwarding = ci_workflow["jobs"][job]["with"]["use_self_hosted"]
            for event, opt_in, expected in (
                ("push", False, False),
                ("push", True, False),
                ("workflow_dispatch", False, False),
                ("workflow_dispatch", True, True),
            ):
                with self.subTest(job=job, event=event, opt_in=opt_in):
                    expression = forwarding.removeprefix("${{").removesuffix("}}")
                    expression = expression.replace("github.event_name", repr(event))
                    expression = expression.replace("inputs.use_self_hosted", repr(opt_in))
                    expression = expression.replace("&&", "and").replace("||", "or")
                    self.assertEqual(eval(expression.strip(), {"__builtins__": {}}), expected)

        validation_workflow = load_workflow("validate-windows.yml")
        validation_input = validation_workflow["on"]["workflow_dispatch"]["inputs"][
            "use_self_hosted"
        ]
        self.assertEqual(validation_input["default"], "false")
        self.assertEqual(
            validation_workflow["jobs"]["windows"]["with"]["use_self_hosted"],
            "${{ inputs.use_self_hosted }}",
        )

        runner_cases = (
            ("ci-windows.yml", "build_windows", "windows-2022", "windows-release"),
            ("ci-archlinux.yml", "build_archlinux", "ubuntu-latest", "linux-release"),
        )
        for workflow_name, job_name, hosted_runner, self_hosted_label in runner_cases:
            workflow = load_workflow(workflow_name)
            self_hosted_input = workflow["on"]["workflow_call"]["inputs"][
                "use_self_hosted"
            ]
            self.assertEqual(self_hosted_input["type"], "boolean")
            self.assertEqual(self_hosted_input["default"], "false")
            runner_expression = workflow["jobs"][job_name]["runs-on"]
            self.assertIn("inputs.use_self_hosted", runner_expression)
            self.assertIn(hosted_runner, runner_expression)
            self.assertIn(self_hosted_label, runner_expression)

    def test_package_compilers_have_bounded_parallelism(self) -> None:
        for workflow_name in ('ci-windows.yml', 'ci-archlinux.yml'):
            workflow = load_workflow(workflow_name)
            if workflow_name == 'ci-archlinux.yml':
                build = workflow['jobs']['build_archlinux']
                self.assertNotIn('CMAKE_BUILD_PARALLEL_LEVEL', build.get('env', {}))
                self.assertNotIn('--cpus', build['container'].get('options', ''))
                self.assertNotIn('--memory', build['container'].get('options', ''))
            else:
                build = next(job for job in workflow['jobs'].values()
                             if 'CMAKE_BUILD_PARALLEL_LEVEL' in job.get('env', {}))
                self.assertEqual(build['env']['CMAKE_BUILD_PARALLEL_LEVEL'], '6')
            scripts = '\n'.join(step.get('run', '') for step in build['steps'])
            self.assertNotIn('$(nproc)', scripts)
            if workflow_name == 'ci-archlinux.yml':
                detector = next(step for step in build['steps'] if step.get('name') == 'Detect actual build budget')
                self.assertIn('python3 scripts/linux-build-budget.py', detector['run'])
                self.assertIn('test_linux_build_budget.py', detector['run'])
                self.assertIn('LTOFLAGS="-flto=1"', detector['run'])
                package = next(step for step in build['steps'] if step.get('name') == 'Build PKGBUILD')
                self.assertIn('CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL}"', package['run'])
                linux_script = (ROOT / 'scripts' / 'linux_build.sh').read_text(encoding='utf-8')
                self.assertIn('ninja -C "build" -j"${num_processors}"', linux_script)
            else:
                commands = [line for line in scripts.splitlines() if 'cmake --build ' in line]
                self.assertEqual(len(commands), 3)
                for command in commands:
                    self.assertIn('--parallel "$CMAKE_BUILD_PARALLEL_LEVEL"', command)

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
        self.assertEqual(build_inputs["build_only"], "true")
        for job in ("build-windows", "build-archlinux"):
            self.assertEqual(
                jobs[job]["with"]["build_tests"],
                "${{ needs.release-candidate.outputs.should_release == 'true' }}",
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
        self.assertIn('if tag.startswith("v"):', workflow_text)
        self.assertIn("release source tags must be v-less", workflow_text)
        self.assertIn(
            "valid_candidates.append((key, tag, notes_file, release_commit))",
            workflow_text,
        )
        self.assertNotIn("vibe-test", workflow_text)
        self.assertNotIn("Linux branch candidate:", workflow_text)
        self.assertIn("should_release=\"false\"", workflow_text)
        self.assertNotIn("def canonical_release_tag", workflow_text)

    def test_ci_stops_before_consumer_setup_for_validation_and_tag_builds(self) -> None:
        caller = load_workflow("ci.yml")["jobs"]["build-windows"]["with"]
        windows = load_workflow("ci-windows.yml")["jobs"]
        self.assertEqual(caller["build_only"], "true")

        def evaluate(condition, values):
            expression = condition.removeprefix("${{").removesuffix("}}")
            for key, value in values.items():
                expression = expression.replace(key, repr(value))
            expression = expression.replace("always()", "True").replace("!cancelled()", "True")
            expression = re.sub(r"\btrue\b", "True", expression)
            expression = re.sub(r"\bfalse\b", "False", expression)
            expression = expression.replace("&&", "and").replace("||", "or")
            return eval(" ".join(expression.split()), {"__builtins__": {}})

        # Evaluate the actual reusable job graph. CI (PR or tag) stops after
        # building the MSI; the independent signing/daily callers still finalize.
        for build_only, expected in ((True, False), (False, True)):
            values = {"inputs.build_only": build_only, "needs.release_artifacts.result": "success"}
            for name in ("sign_windows_msi", "package_windows", "sign_windows_installer", "finalize_windows"):
                with self.subTest(build_only=build_only, job=name):
                    selected = evaluate(windows[name]["if"], values)
                    self.assertEqual(selected, expected)
                    values[f"needs.{name}.result"] = "success" if selected else "skipped"

        steps = windows["build_windows"]["steps"]
        names = [step.get("name", "") for step in steps]
        gate = next(step for step in steps if step.get("name") == "Test packaged MSI version and validation safety")
        self.assertNotIn("if", gate)
        self.assertGreater(names.index(gate["name"]), names.index("Package Windows MSI"))
        self.assertLess(names.index(gate["name"]), names.index("Upload unsigned MSI for SignPath"))

    def test_final_windows_retention_honors_validation_without_shortening_releases(self) -> None:
        windows = load_workflow("ci-windows.yml")
        uploads = [step for step in windows["jobs"]["finalize_windows"]["steps"]
                   if step.get("uses", "").startswith("actions/upload-artifact@")]
        self.assertEqual({step["with"]["name"] for step in uploads}, {"release-provenance", "build-Windows"})
        default_days = int(windows["on"]["workflow_call"]["inputs"]["release_artifact_retention_days"]["default"])
        caller = load_workflow("ci.yml")["jobs"]["build-windows"]["with"]["release_artifact_retention_days"]
        for release, tag, expected_days in (("false", "", 1), ("true", "2.0.2", 90)):
            expression = caller.removeprefix("${{").removesuffix("}}")
            expression = expression.replace("needs.release-candidate.outputs.should_release", repr(release))
            days = eval(expression.replace("&&", "and").replace("||", "or").strip(), {"__builtins__": {}})
            for upload in uploads:
                with self.subTest(release=release, artifact=upload["with"]["name"]):
                    expression = upload["with"]["retention-days"].removeprefix("${{").removesuffix("}}")
                    expression = expression.replace("inputs.release_tag", repr(tag))
                    expression = expression.replace("inputs.release_artifact_retention_days", str(days))
                    actual = eval(expression.replace("&&", "and").replace("||", "or").strip(), {"__builtins__": {}})
                    self.assertEqual(actual, expected_days)
        cases = (("", default_days, 1), ("", 14, 14),
                 ("2.0.2-nightly.d20261022.aaaaaaaaaaaa", 7, 90), ("2.0.2", default_days, 90))
        for tag, days, expected in cases:
            for upload in uploads:
                with self.subTest(tag=tag, days=days, artifact=upload["with"]["name"]):
                    expression = upload["with"]["retention-days"].removeprefix("${{").removesuffix("}}")
                    expression = expression.replace("inputs.release_tag", repr(tag))
                    expression = expression.replace("inputs.release_artifact_retention_days", str(days))
                    actual = eval(expression.replace("&&", "and").replace("||", "or").strip(), {"__builtins__": {}})
                    self.assertEqual(actual, expected)

    def test_installer_regressions_run_after_wix_for_all_test_builds(self) -> None:
        steps = load_workflow("ci-windows.yml")["jobs"]["build_windows"]["steps"]
        named_steps = {step["name"]: step for step in steps}
        names = [step["name"] for step in steps]
        for gate in ("Test installer driver safety and rollback", "Test production bootstrap migration"):
            with self.subTest(gate=gate):
                self.assertEqual(named_steps[gate]["if"], "inputs.build_tests")
                self.assertGreater(names.index(gate), names.index("Install WiX Toolset"))
                self.assertLess(names.index(gate), names.index("Build Windows"))
                self.assertIn("$LASTEXITCODE -ne 0", named_steps[gate]["run"])
        self.assertIn("test_driver_install_safety.ps1", named_steps["Test installer driver safety and rollback"]["run"])
        migration = named_steps["Test production bootstrap migration"]["run"]
        self.assertIn("test_bootstrap_migration.ps1 -OutputDirectory $outputDirectory", migration)
        self.assertIn("Join-Path $env:RUNNER_TEMP", migration)
        daily = named_steps["Enforce unsigned daily build policy"]
        self.assertEqual(daily["if"], "inputs.daily_unsigned")
        self.assertIn("$env:SIGNPATH_ENABLED -ne 'false'", daily["run"])
        self.assertIn("$env:BUILD_TESTS -ne 'true'", daily["run"])
        self.assertNotIn("test_driver_install_safety.ps1", daily["run"])
        self.assertIn("test_daily_version_contract.cmake", daily["run"])
        self.assertIn("test_daily_gamepad_signing.ps1", daily["run"])

    def test_nonrelease_selection_skips_without_git_or_release_api_calls(self) -> None:
        workflow = load_workflow("ci.yml")
        steps = workflow["jobs"]["release-candidate"]["steps"]
        step = next(step for step in steps if step.get("id") == "release-candidate")
        script = step["run"].split("python <<'PY'\n", 1)[1].rsplit("\nPY", 1)[0]
        portable_steps = [step for step in load_workflow("ci-windows.yml")["jobs"]["build_windows"]["steps"]
                          if step.get("name", "").endswith("Windows portable ZIP (CI only)")]
        self.assertEqual(len(portable_steps), 2)
        fallback = re.fullmatch(
            r"\$\{\{ needs\.release-candidate\.outputs\.release_version \|\| '([^']+)' \}\}",
            workflow["jobs"]["build-windows"]["with"]["release_version"],
        ).group(1)
        self.assertEqual(fallback, "2.0.0-pr-validation")
        self.assertEqual(
            workflow["jobs"]["build-archlinux"]["with"]["release_version"],
            workflow["jobs"]["build-windows"]["with"]["release_version"],
        )
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / "probe-version.cmake"
            probe.write_text(
                f'include("{(ROOT / "cmake" / "prep" / "build_version.cmake").as_posix()}")\n'
                'if(NOT PROJECT_VERSION_NUMERIC STREQUAL "2.0.0")\n'
                '  message(FATAL_ERROR "PR validation numeric version changed")\n'
                'endif()\n',
                encoding="utf-8",
            )
            subprocess.run(
                ["cmake", f"-DBUILD_VERSION={fallback}", "-P", str(probe)],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            )
        for event in ("push", "pull_request"):
            with self.subTest(event=event), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "outputs"
                environment = {
                    "EVENT_NAME": event,
                    "GITHUB_REF_TYPE": "branch",
                    "GITHUB_REF": "refs/heads/master",
                    "GITHUB_REPOSITORY": "Nonary/test",
                    "GITHUB_OUTPUT": str(output),
                }
                with patch.dict(os.environ, environment, clear=True), patch(
                    "subprocess.run", side_effect=AssertionError("nonrelease selection invoked a command")
                ), patch("sys.stdout", new=io.StringIO()), self.assertRaises(SystemExit) as stopped:
                    exec(compile(script, "ci-release-selection", "exec"), {})
                self.assertEqual(stopped.exception.code, 0)
                outputs = dict(line.split("=", 1) for line in output.read_text().splitlines())
                self.assertEqual(outputs["should_release"], "false")
                self.assertEqual(outputs["tag_name"], "")
                self.assertEqual(outputs["release_version"], "")
                if event == "pull_request":
                    for portable in portable_steps:
                        expression = portable["if"].replace("github.event_name", repr(event))
                        expression = expression.replace("inputs.release_version", repr(outputs["release_version"] or fallback))
                        expression = expression.replace("!startsWith", "not startsWith").replace("&&", "and")
                        self.assertFalse(eval(expression, {"__builtins__": {}, "startsWith": lambda a, b: a.startswith(b)}))
        for portable in portable_steps:
            for event, version, expected in (
                ("pull_request", "2.0.0", False),
                ("pull_request", "2.0.1-beta.1", False),
                ("push", "", False),
                ("push", "0.0.0", False),
                ("push", "v0.0.0-pr.1", False),
                ("push", "2.0.1-beta.1", True),
                ("workflow_dispatch", "v2.0.1-beta.1", True),
                ("schedule", "2.0.0-daily.d20261020", True),
            ):
                with self.subTest(step=portable["name"], event=event, version=version):
                    expression = portable["if"].replace("github.event_name", repr(event))
                    expression = expression.replace("inputs.release_version", repr(version))
                    expression = expression.replace("!startsWith", "not startsWith").replace("&&", "and")
                    self.assertEqual(eval(expression, {"__builtins__": {}, "startsWith": lambda a, b: a.startswith(b)}), expected)

    def test_package_builds_wait_for_tag_push(self) -> None:
        jobs = load_workflow("ci.yml")["jobs"]
        cases = (
            ("push", "branch", "false", False),
            # Even when a branch HEAD already has a release tag, only its tag event builds.
            ("push", "branch", "true", False),
            ("push", "tag", "true", True),
            ("push", "tag", "false", False),
            ("workflow_dispatch", "branch", "false", True),
            ("pull_request", "branch", "false", True),
        )
        for job in ("build-windows", "build-archlinux"):
            for event, ref_type, release, expected in cases:
                with self.subTest(job=job, event=event, ref_type=ref_type, release=release):
                    expression = jobs[job]["if"].strip().removeprefix("${{").removesuffix("}}")
                    for name, value in (
                        ("inputs.validate_hosted_linux", False),
                        ("github.event_name", event),
                        ("github.ref_type", ref_type),
                        ("needs.release-candidate.outputs.should_release", release),
                    ):
                        expression = expression.replace(name, repr(value))
                    expression = expression.replace("!= true", "!= True").replace("||", "or").replace("&&", "and")
                    expression = " ".join(expression.split())
                    self.assertEqual(eval(expression, {"__builtins__": {}}), expected)

    def test_arch_linux_interface_gate_fails_before_full_build(self) -> None:
        recipe = (ROOT / "packaging/linux/Arch/PKGBUILD").read_text(encoding="utf-8")
        registrations = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        component = registrations.split("NAME test_component_linux_routed_link", 1)[1].split("endif()", 1)[0]
        self.assertIn("LINK_LIBRARIES Boost::asio", component)
        self.assertIn("ctest --test-dir . --output-on-failure --no-tests=error", recipe)
        build_function = "build() {" + recipe.split("build() {", 1)[1].split("\ncheck() {", 1)[0]
        bash = r"C:\Program Files\Git\bin\bash.exe" if os.name == "nt" else "bash"
        for tests, fail_gate in (("ON", "false"), ("ON", "true"), ("OFF", "false")):
            with self.subTest(tests=tests, fail_gate=fail_gate), tempfile.TemporaryDirectory() as temporary:
                stage = Path(temporary)
                script = r"""set -e
python3() {
  [[ "$*" == *"scripts/build-linux-component-tests.py build" ]] || return 1
  echo interface >> calls
  [[ "$FAIL_GATE" == false ]]
}
cmake() {
  if [[ "$*" == "--build build" ]]; then
    echo full >> calls
  else
    echo configure >> calls
  fi
}
appstreamcli() { :; }
appstream-util() { :; }
desktop-file-validate() { :; }
_commit=exact-source
_gcc_env_suffix=-14
_use_cuda=true
pkgname=fixture
srcdir="$PWD"
""" + build_function + "\nbuild\n"
                environment = os.environ.copy()
                environment.update(_build_tests=tests, FAIL_GATE=fail_gate, MAKEFLAGS="-j4", CMAKE_BUILD_PARALLEL_LEVEL="4")
                result = subprocess.run([bash, "-c", script], cwd=stage, env=environment, capture_output=True, text=True)
                calls = (stage / "calls").read_text(encoding="utf-8").splitlines()
                expected = ["configure", "interface"] if fail_gate == "true" else (["configure", "interface", "full"] if tests == "ON" else ["configure", "full"])
                self.assertEqual(calls, expected, result.stdout + result.stderr)
                self.assertEqual(result.returncode == 0, fail_gate == "false", result.stdout + result.stderr)

    def test_validation_source_seed_and_partial_caches_remain_scoped(self) -> None:
        workflow = load_workflow("ci-archlinux.yml")
        steps = {step["name"]: step for step in workflow["jobs"]["build_archlinux"]["steps"] if "name" in step}
        build = steps["Build PKGBUILD"]["run"]
        validation = build.split('if [[ "$VALIDATION_ONLY" == "true" ]]; then', 1)[1].split("\nfi", 1)[0]
        self.assertIn("seed-linux-package-source.sh", validation)
        self.assertIn("package_options=(-s --holdver)", validation)
        self.assertIn("package_options=(-si)", build.split("validation_tree=", 1)[0])
        self.assertIn('_validation_expected_tree="$validation_tree"', build)
        guard = steps["Verify failed-validation partial cache eligibility"]["if"]
        for requirement in ("failure()", "inputs.validation_only", "workflow_dispatch", "github.event.repository.full_name == github.repository", "steps.build.outcome == 'failure'", "steps.toolchain.outputs.abi != ''"):
            self.assertIn(requirement, guard)
        for name, output in (("Save checksum-verified CUDA installer", "cuda"), ("Save compiler cache", "compiler")):
            self.assertIn("success()", steps[name]["if"])
            self.assertIn("steps.partial-cache.outputs." + output + " == 'true'", steps[name]["if"])
        cached_paths = "\n".join(steps[name]["with"]["path"] for name in ("Save checksum-verified CUDA installer", "Save compiler cache"))
        self.assertNotIn("src/build", cached_paths)
        self.assertNotIn("pkg.tar", cached_paths)

    def test_arch_dependency_setup_honors_installed_pinned_compiler(self) -> None:
        workflow = load_workflow("ci-archlinux.yml")
        job = workflow["jobs"]["build_archlinux"]
        script = next(step["run"] for step in job["steps"] if step["name"] == "Install exact package build dependencies")
        bash = r"C:\Program Files\Git\bin\bash.exe" if os.name == "nt" else "bash"
        for scenario in ("ok", "wrong_compiler", "install_failure", "unsatisfied"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as directory:
                stage = Path(directory)
                (stage / "pkg").mkdir()
                (stage / "pkg/PKGBUILD").write_text("depends=('installed>=2' 'missing>=1')\nmakedepends=('gcc14')\n")
                (stage / "pacman").write_text('''#!/bin/bash
case "$1" in
  -Q)
    version=$GCC14_ARCHIVE_VERSION
    [[ "$MOCK_SCENARIO" == wrong_compiler ]] && version=wrong
    echo "$2 $version";;
  -T)
    echo "deptest $*" >> operations
    if [[ ! -f synchronized || "$MOCK_SCENARIO" == unsatisfied ]]; then
      echo "missing>=1"
      exit 127
    fi;;
  -S)
    echo "sync $*" >> operations
    [[ "$*" == "-S --needed --noconfirm missing>=1" ]] || exit 99
    [[ "$MOCK_SCENARIO" == install_failure ]] && exit 1
    touch synchronized;;
  *) exit 98;;
esac
''')
                subprocess.run([bash, "-c", "chmod +x pacman"], cwd=stage, check=True)
                environment = dict(os.environ, GCC14_ARCHIVE_VERSION=job["env"]["GCC14_ARCHIVE_VERSION"], MOCK_SCENARIO=scenario)
                result = subprocess.run([bash, "-c", 'export PATH="$PWD:$PATH"; ' + script], cwd=stage, env=environment, capture_output=True, text=True)
                if scenario == "ok":
                    self.assertEqual(result.returncode, 0, result.stderr)
                    operations = (stage / "operations").read_text().splitlines()
                    self.assertEqual(sum(line.startswith("deptest ") for line in operations), 2)
                    self.assertIn("sync -S --needed --noconfirm missing>=1", operations)
                    self.assertFalse(any("gcc14" in line for line in operations if line.startswith("sync ")))
                else:
                    self.assertNotEqual(result.returncode, 0)

    def test_registered_linux_flag_isolates_exact_source_without_release_selection(self) -> None:
        workflow = load_workflow("ci.yml")
        inputs = workflow["on"]["workflow_dispatch"]["inputs"]
        self.assertEqual(inputs["validate_hosted_linux"]["type"], "boolean")
        self.assertEqual(inputs["validate_hosted_linux"]["default"], "false")
        jobs = workflow["jobs"]
        lane = jobs["validate-hosted-linux"]
        self.assertNotIn("needs", lane)
        self.assertNotIn("secrets", lane)
        self.assertEqual(lane["permissions"], {"contents": "read"})
        self.assertEqual(lane["with"]["release_commit"], "${{ github.sha }}")
        self.assertEqual(lane["with"]["release_version"], "1.18.4-linux-validation")
        self.assertNotEqual(lane["with"]["release_version"].split("-", 1)[0], "0.0.0")
        self.assertEqual(lane["with"]["use_self_hosted"], "false")
        self.assertEqual(lane["with"]["validation_only"], "true")
        self.assertEqual(lane["with"]["build_tests"], "true")
        self.assertEqual(lane["with"]["artifact_retention_days"], "1")
        self.assertEqual(lane["with"]["cache_namespace"], "${{ inputs.linux_cache_namespace }}")
        # A reachable unreleased tag must not make any ordinary job reachable
        # when the explicit manual validation flag is true.
        for event in ("workflow_dispatch", "push", "pull_request"):
            for enabled in (False, True):
                active = event == "workflow_dispatch" and enabled
                for name, job in jobs.items():
                    expression = job["if"].strip().removeprefix("${{").removesuffix("}}")
                    for token, value in (
                        ("inputs.validate_hosted_linux", enabled),
                        ("github.event_name", event),
                        ("github.ref_type", "tag"),
                        ("needs.release-candidate.outputs.should_release", "true"),
                    ):
                        expression = expression.replace(token, repr(value))
                    expression = expression.replace("!= true", "!= True").replace("== true", "== True")
                    expression = " ".join(expression.replace("&&", "and").replace("||", "or").split())
                    actual = eval(expression, {"__builtins__": {}})
                    with self.subTest(event=event, enabled=enabled, job=name):
                        self.assertEqual(actual, active if name == "validate-hosted-linux" else not active)

    def test_hosted_linux_validation_cannot_sign_install_or_publish(self) -> None:
        wrapper = load_workflow("validate-linux.yml")
        self.assertEqual(set(wrapper["on"]), {"workflow_dispatch"})
        job = wrapper["jobs"]["arch"]
        self.assertEqual(job["with"]["release_commit"], "${{ github.sha }}")
        self.assertEqual(job["with"]["release_version"], "1.18.4-linux-validation")
        self.assertNotEqual(job["with"]["release_version"].split("-", 1)[0], "0.0.0")
        self.assertEqual(job["with"]["use_self_hosted"], "false")
        self.assertEqual(job["with"]["validation_only"], "true")
        self.assertEqual(job["with"]["build_tests"], "true")
        self.assertNotIn("secrets", job)
        arch = load_workflow("ci-archlinux.yml")
        build = arch["jobs"]["build_archlinux"]
        self.assertIn("!inputs.validation_only", build["runs-on"])
        steps = {step["name"]: step for step in build["steps"]}
        drm = next(step["run"] for step in build["steps"] if step["name"].startswith("Compile "))
        guard = drm.index('if [[ "$VALIDATION_ONLY" == "true" ]]')
        end = drm.index("exit 0", guard)
        for operation in ("openssl req", "dkms add", "dkms build", "dkms install"):
            self.assertGreater(drm.index(operation), end)
        self.assertIn('package_options=(-s --holdver)', steps["Build PKGBUILD"]["run"])
        self.assertIn('if [[ "$VALIDATION_ONLY" == "true" ]]', steps["Build PKGBUILD"]["run"])
        recipe = (ROOT / "packaging/linux/Arch/PKGBUILD").read_text()
        self.assertIn("--no-tests=error", recipe)
        self.assertIn("sm_61", recipe)

    def test_arch_package_is_built_and_carried_into_release(self) -> None:
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
        self.assertEqual(checkout["with"]["submodules"], "false")
        drm_checkout = next(
            step for step in arch_workflow["jobs"]["build_archlinux"]["steps"]
            if step["name"] == "Checkout packaging driver sources"
        )
        self.assertEqual(
            drm_checkout["run"].splitlines()[0],
            'git -c safe.directory="$GITHUB_WORKSPACE" submodule update --init --depth 1 -- third-party/libvirtualdisplay third-party/libvirtualgamepad',
        )
        for required in (
            "third-party/libvirtualdisplay/linux/vibeshine-drm/Makefile",
            "third-party/libvirtualgamepad/linux/vibeshine-ds5/Makefile",
            "third-party/libvirtualgamepad/linux/vibeshine-ds5/dkms.conf.in",
        ):
            self.assertIn("test -s " + required, drm_checkout["run"])
        self.assertNotIn("--recursive", drm_checkout["run"])
        package_configuration = (ROOT / "cmake/prep/special_package_configuration.cmake").read_text()
        self.assertIn("libvirtualgamepad Linux DualSense sources are unavailable", package_configuration)
        self.assertIn("libvirtualdisplay Linux assets are unavailable", package_configuration)
        self.assertNotIn("--global", drm_checkout["run"])
        self.assertNotIn("safe.directory=*", drm_checkout["run"])
        self.assertNotIn('safe.directory="*"', drm_checkout["run"])
        recipe = (ROOT / "packaging/linux/Arch/PKGBUILD").read_text()
        self.assertIn("submodule update --recursive --init --depth 1", recipe)
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
        self.assertIn('if [[ "${BUILD_VERSION}" == *-* ]]', arch_text)
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
        self.assertIn("installs the kernel headers for your running kernel", (ROOT / "docs" / "linux" / "install.md").read_text(encoding="utf-8"))

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
        self.assertIn("arch_package_version=${RELEASE_VERSION//-/}", workflow_text)
        self.assertIn("arch_package_version=${arch_package_version//+/.}", workflow_text)

        release_steps = jobs["release"]["steps"]
        self.assertFalse(
            any(step["name"] == "Close fixed issues for release" for step in release_steps)
        )
        self.assertNotIn("state=closed", workflow_text)
        self.assertIn('--argjson draft "${RELEASE_DRAFT}"', workflow_text)
        self.assertIn('if [[ "${RELEASE_DRAFT}" == "true" ]]', workflow_text)

    def test_daily_fixed_issue_follow_up_is_verified_and_permission_bounded(self) -> None:
        follow_up = load_workflow("fixed-issue-follow-up.yml")
        self.assertIn("issues", follow_up["on"])
        self.assertIn("release", follow_up["on"])
        self.assertIn("repository_dispatch", follow_up["on"])
        self.assertEqual(
            follow_up["on"]["repository_dispatch"]["types"],
            ["daily-release-published"],
        )
        self.assertEqual(
            follow_up["jobs"]["follow-up-verified-daily"]["permissions"],
            {"issues": "write"},
        )
        for workflow_name in ("fixed-issue-follow-up.yml", "sign-release.yml"):
            workflow_text = (ROOT / ".github" / "workflows" / workflow_name).read_text(
                encoding="utf-8"
            )
            self.assertNotIn("state=closed", workflow_text)
            self.assertNotIn("state: 'closed'", workflow_text)
            self.assertNotIn('state: "closed"', workflow_text)
            self.assertNotIn("state_reason=completed", workflow_text)

        daily = load_workflow("daily-prerelease.yml")
        self.assertEqual(daily["jobs"]["build"]["with"]["upload_symbols_artifact"], "true")
        publish = daily["jobs"]["publish"]
        self.assertEqual(publish["permissions"], {"actions": "read", "contents": "write"})
        self.assertNotIn("issues", publish["permissions"])
        publish_steps = {step["name"]: step for step in publish["steps"] if "name" in step}
        symbols = publish_steps["Retain exact-build Windows symbols"]
        self.assertEqual(symbols["with"]["retention-days"], "90")
        self.assertEqual(symbols["with"]["name"], "nightly-windows-symbols")
        self.assertEqual(publish_steps["Download exact-build Windows symbols from this run"]["with"]["name"], "windows-symbols-Windows")
        self.assertIn('test -s "symbol-assets/$symbol_pack"',
                      publish_steps["Verify exact-build Windows symbols"]["run"])
        self.assertIn('VibepolloSymbols-${DAILY_VERSION}-windows-x64.7z',
                      publish_steps["Verify exact-build Windows symbols"]["run"])
        self.assertLess(
            list(publish_steps).index("Verify exact-build Windows symbols"),
            list(publish_steps).index("Verify finalized provenance, identity and complete asset set"),
        )
        windows = load_workflow("ci-windows.yml")
        finalize_steps = {step["name"]: step for step in windows["jobs"]["finalize_windows"]["steps"] if "name" in step}
        self.assertEqual(finalize_steps["Include symbols in build artifact"]["if"],
                         "inputs.upload_symbols_artifact && !inputs.daily_unsigned")
        publish_script = "\n".join(step.get("run", "") for step in publish["steps"])
        self.assertIn('event_type:"daily-release-published"', publish_script)

    def test_fixed_issue_follow_up_decisions_with_mocked_github_calls(self) -> None:
        test_file = ROOT / ".github" / "scripts" / "test_fixed_issue_follow_up.js"
        subprocess.run(
            ["node", str(test_file)],
            cwd=ROOT,
            check=True,
            text=True,
        )

    def test_reusable_windows_workflow_supports_deferred_signing(self) -> None:
        workflow_path = ROOT / ".github" / "workflows" / "ci-windows.yml"
        workflow = load_workflow("ci-windows.yml")
        inputs = workflow["on"]["workflow_call"]["inputs"]
        jobs = workflow["jobs"]
        workflow_text = workflow_path.read_text(encoding="utf-8")

        self.assertIn("artifact_source_run_id", inputs)
        self.assertIn("build_only", inputs)
        self.assertEqual(inputs["build_tests"]["type"], "boolean")
        self.assertEqual(inputs["build_tests"]["default"], "false")
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


class WindowsWorkflowBehaviorTest(unittest.TestCase):
    def test_release_build_uses_shallow_cached_dependencies(self) -> None:
        workflow = load_workflow("ci-windows.yml")
        workflow_text = (ROOT / ".github" / "workflows" / "ci-windows.yml").read_text(
            encoding="utf-8"
        )
        build_steps = workflow["jobs"]["build_windows"]["steps"]
        package_steps = workflow["jobs"]["package_windows"]["steps"]

        build_checkout = next(step for step in build_steps if step["name"] == "Checkout")
        package_checkout = next(step for step in package_steps if step["name"] == "Checkout")
        depth_expression = build_checkout["with"]["fetch-depth"]
        for daily, expected_depth in ((False, "1"), (True, "0")):
            with self.subTest(daily=daily):
                expression = depth_expression.removeprefix("${{").removesuffix("}}")
                expression = expression.replace("inputs.daily_unsigned", repr(daily))
                expression = expression.replace("&&", "and").replace("||", "or")
                actual = eval(expression.strip(), {"__builtins__": {}})
                self.assertEqual(actual, expected_depth)
        self.assertEqual(build_checkout["with"]["fetch-tags"], "${{ inputs.daily_unsigned }}")
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
            "          # Release tag builds omit test targets; ordinary reusable-workflow calls retain them.\n"
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

        # Execute the production numeric transforms without configuring WiX or
        # generating a Windows binary on the audit host.
        wix_numeric = "set(_RAW_VER" + wix_version.split("set(_RAW_VER", 1)[1].split(
            "# Merge our custom actions", 1
        )[0]
        executable_functions = "function(_wv_int_or_zero" + executable_version.split(
            "function(_wv_int_or_zero", 1
        )[1].split("function(_wv_current_time", 1)[0]
        with tempfile.TemporaryDirectory() as temporary:
            probe = Path(temporary) / "version-ordinals.cmake"
            probe.write_text(
                'set(PROJECT_VERSION_NUMERIC "2.0.1")\n' + wix_numeric +
                executable_functions +
                '_wv_encoded_build(1 "${PROJECT_VERSION_FULL}" encoded)\n'
                'if(NOT CPACK_WIX_PRODUCT_VERSION STREQUAL "2.0.${EXPECTED_BUILD}.0" OR\n'
                '   NOT encoded EQUAL EXPECTED_BUILD)\n'
                '  message(FATAL_ERROR "MSI/executable ordinal mismatch")\n'
                'endif()\n', encoding="utf-8"
            )
            for version, encoded in (
                ("2.0.1", 199), ("2.0.1-stable", 199),
                ("2.0.1-stable.1", 199), ("2.0.1-stable.29", 199),
                ("2.0.1-beta.29", 159), ("2.0.1-rc.29", 189),
            ):
                with self.subTest(version=version):
                    result = subprocess.run(
                        ["cmake", f"-DPROJECT_VERSION_FULL={version}",
                         f"-DEXPECTED_BUILD={encoded}", "-P", str(probe)],
                        capture_output=True, text=True, check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        # Equal numeric stable versions must still replace inside the MSI
        # transaction. A GUID comparison alone cannot implement that contract.
        template = (ROOT / "packaging/windows/wix/WIX.template.in").read_text(encoding="utf-8")
        # Parse the authored element before CPack expands its surrounding XML
        # namespace placeholder and WiX preprocesses the conditional sections.
        upgrade_elements = re.findall(r"<MajorUpgrade\b[^>]+/>", template)
        self.assertEqual(len(upgrade_elements), 1)
        major_upgrade = ET.fromstring(upgrade_elements[0])
        self.assertEqual(major_upgrade.attrib["Schedule"], "afterInstallInitialize")
        self.assertEqual(major_upgrade.attrib["AllowDowngrades"], "yes")
        stable_branch = re.search(
            r'if \(string.Equals\(tag, "stable", StringComparison.Ordinal\)\) \{([^}]+)\}',
            bootstrapper,
        )
        self.assertIsNotNone(stable_branch)
        self.assertRegex(stable_branch.group(1), r"return\s+99\s*;")

    def test_release_build_uses_shallow_cached_dependencies(self) -> None:
        workflow = load_workflow("ci-windows.yml")
        workflow_text = (ROOT / ".github" / "workflows" / "ci-windows.yml").read_text(
            encoding="utf-8"
        )
        build_steps = workflow["jobs"]["build_windows"]["steps"]
        package_steps = workflow["jobs"]["package_windows"]["steps"]

        build_checkout = next(step for step in build_steps if step["name"] == "Checkout")
        package_checkout = next(step for step in package_steps if step["name"] == "Checkout")
        depth_expression = build_checkout["with"]["fetch-depth"]
        for daily, expected_depth in ((False, "1"), (True, "0")):
            with self.subTest(daily=daily):
                expression = depth_expression.removeprefix("${{").removesuffix("}}")
                expression = expression.replace("inputs.daily_unsigned", repr(daily))
                expression = expression.replace("&&", "and").replace("||", "or")
                actual = eval(expression.strip(), {"__builtins__": {}})
                self.assertEqual(actual, expected_depth)
        self.assertEqual(build_checkout["with"]["fetch-tags"], "${{ inputs.daily_unsigned }}")
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
            "          # Release tag builds omit test targets; ordinary reusable-workflow calls retain them.\n"
            "          cmake \\",
            workflow_text,
        )
        self.assertIn(
            "-DBUILD_TESTS=${{ inputs.build_tests && 'ON' || 'OFF' }}",
            workflow_text,
        )
    def test_windows_steam_artwork_dependencies(self) -> None:
        workflow = load_workflow("ci-windows.yml")
        steps = workflow["jobs"]["build_windows"]["steps"]
        setup = next(step for step in steps if step["name"] == "Setup Dependencies Windows")
        for package in ("libjpeg-turbo", "libpng", "libwebp"):
            self.assertIn("mingw-w64-${{ matrix.toolchain }}-" + package, setup["with"]["install"])

if __name__ == "__main__":
    unittest.main()
