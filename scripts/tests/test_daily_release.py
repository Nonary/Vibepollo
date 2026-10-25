import datetime as dt
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import textwrap
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("daily", Path(__file__).resolve().parents[1] / "daily_release.py")
daily = importlib.util.module_from_spec(spec)
spec.loader.exec_module(daily)
SHA = "a" * 40


class DailyTests(unittest.TestCase):
    def setUp(self):
        context = patch.dict(os.environ, GITHUB_EVENT_NAME="workflow_dispatch",
                             GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="master")
        context.start()
        self.addCleanup(context.stop)

    def routing(self, text, job, **overrides):
        values = {'github.event_name':'workflow_dispatch', 'github.repository':'Nonary/Vibepollo',
                  'github.ref_type':'branch', 'github.ref_name':'canary',
                  'github.event.repository.default_branch':'master', 'vars.DAILY_RELEASE_ENABLED':'true',
                  'vars.DAILY_RELEASE_SOURCE_BRANCH':'canary',
                  'vars.DAILY_RELEASE_SOURCE_CONFIRMED':'Nonary/Vibepollo:canary'}
        values.update(overrides)
        expression = re.search(r"\n  " + job + r":\n.*?    if: >-\n(.*?)\n    runs-on:", text, re.S)[1]
        for key in sorted(values, key=len, reverse=True):
            expression = expression.replace(key, repr(values[key]))
        expression = expression.replace('&&', ' and ').replace('||', ' or ')
        return eval(' '.join(expression.split()), {'__builtins__':{}})

    def test_child_and_launcher_branch_event_disabled_routing(self):
        root = Path(__file__).resolve().parents[2]
        child = (root/'.github/workflows/daily-prerelease.yml').read_text()
        launcher = (Path(__file__).parent/'fixtures/daily-prerelease-launcher.yml').read_text()
        self.assertTrue(self.routing(child, 'prepare'))
        self.assertTrue(self.routing(child, 'prepare', **{'vars.DAILY_RELEASE_ENABLED':'false'}))
        for changed in ({'github.ref_name':'feature'}, {'github.ref_name':'master'},
                        {'github.event_name':'schedule'}, {'github.ref_type':'tag'},
                        {'github.repository':'Other/fork'}, {'vars.DAILY_RELEASE_SOURCE_BRANCH':'master'},
                        {'vars.DAILY_RELEASE_SOURCE_CONFIRMED':'Nonary/Vibepollo:master'}):
            with self.subTest(child=changed): self.assertFalse(self.routing(child, 'prepare', **changed))
        base = {'github.event_name':'schedule', 'github.ref_name':'master'}
        self.assertTrue(self.routing(launcher, 'dispatch', **base))
        for changed in ({'vars.DAILY_RELEASE_ENABLED':'false'}, {'github.ref_name':'canary'},
                        {'github.ref_type':'tag'}, {'github.repository':'Other/fork'},
                        {'github.event_name':'push'}, {'vars.DAILY_RELEASE_SOURCE_BRANCH':'master'},
                        {'vars.DAILY_RELEASE_SOURCE_CONFIRMED':'Nonary/Vibepollo:master'}):
            with self.subTest(launcher=changed):
                self.assertFalse(self.routing(launcher, 'dispatch', **(base|changed)))
        self.assertNotIn('  schedule:', child)
        self.assertIn("cron: '32 6 * * *'", launcher)
        self.assertIn('group: daily-windows-launcher', launcher)
        self.assertIn('group: daily-windows-prerelease', child)
        self.assertIn('cancel-in-progress: false', launcher)
        self.assertIn('cancel-in-progress: false', child)
        self.assertIn('permissions: {}', launcher)
        self.assertIn('      actions: write', launcher)
        self.assertNotIn('contents: write', launcher)
        self.assertNotIn('secrets.', launcher)
        self.assertNotIn('uses: ./.github/workflows/ci-', launcher)
        for name in ('ci-windows.yml', 'ci-archlinux.yml'):
            self.assertIn('uses: ./.github/workflows/'+name, child)
            reusable = (root/'.github/workflows'/name).read_text()
            self.assertIn('ref: ${{ inputs.release_commit }}', reusable)
        self.assertIn('ref: ${{ github.sha }}', child)
        self.assertIn('ref: ${{ needs.prepare.outputs.commit }}', child)
        self.assertEqual(child.count('release_commit: ${{ needs.prepare.outputs.commit }}'), 2)
        reservation = re.search(r"if: (steps.identity.outputs.should_build[^\n]+)", child)[1]
        for enabled, publish in ((True,True), (False,True), (True,False), (False,False)):
            expression = reservation.replace("steps.identity.outputs.should_build", repr('true'))
            expression = expression.replace('vars.DAILY_RELEASE_ENABLED', repr(str(enabled).lower()))
            expression = expression.replace('inputs.publish', repr(publish)).replace('true', 'True')
            self.assertEqual(eval(expression.replace('&&',' and '), {'__builtins__':{}}), enabled and publish)

    def test_launcher_dispatch_payload_and_failure(self):
        bash, jq = shutil.which('bash'), shutil.which('jq')
        if not bash or not jq: self.skipTest('Isolated launcher shell proof requires Bash and jq')
        template = (Path(__file__).parent/'fixtures/daily-prerelease-launcher.yml').read_text()
        script = textwrap.dedent(template.split('        run: |\n', 1)[1])
        fake = r'''gh() {
  test "$1" = api && test "$2" = --method && test "$3" = POST || return 90
  test "$4" = repos/Nonary/Vibepollo/actions/workflows/daily-prerelease.yml/dispatches || return 91
  test "$5" = --input || return 92
  cp "$6" "$PAYLOAD_LOG"
  return "$DISPATCH_FAILURE"
}
'''
        for publish, failure in (('true',0), ('false',0), ('true',1)):
            with self.subTest(publish=publish, failure=failure), tempfile.TemporaryDirectory() as temp:
                summary, payload = Path(temp)/'summary', Path(temp)/'payload'
                env = dict(os.environ, GITHUB_REPOSITORY='Nonary/Vibepollo', PUBLISH=publish,
                           GITHUB_STEP_SUMMARY=str(summary), PAYLOAD_LOG=str(payload), DISPATCH_FAILURE=str(failure))
                result = subprocess.run([bash,'-c',fake+script], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, failure, result.stderr)
                self.assertEqual(json.loads(payload.read_text()), dict(ref='canary', inputs=dict(publish=publish=='true')))
                self.assertEqual(summary.exists(), failure==0)

    def test_manual_canary_source_is_exact_dispatch_sha(self):
        for advanced in (False, True):
            with self.subTest(advanced=advanced), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / 'output'
                with patch.dict(os.environ, GITHUB_REPOSITORY='Nonary/Vibepollo', GITHUB_SHA=SHA,
                                GITHUB_REF_NAME='canary', DAILY_PRIMARY_BRANCH='master',
                                DAILY_RELEASE_SOURCE_BRANCH='canary',
                                DAILY_RELEASE_SOURCE_CONFIRMED='Nonary/Vibepollo:canary', GITHUB_OUTPUT=str(output)):
                    commands = ['', 'b'*40] if advanced else ['', SHA, '', '[[]]', '', '2.0.0', '']
                    with patch.object(daily, 'run', side_effect=commands) as calls:
                        if advanced:
                            with self.assertRaisesRegex(ValueError, 'advanced after dispatch'): daily.resolve()
                            self.assertFalse(output.exists())
                        else:
                            daily.resolve()
                            self.assertIn(f'commit={SHA}', output.read_text())
                            self.assertIn('should_build=true', output.read_text())
                        self.assertEqual(calls.call_args_list[0].args[-1], 'refs/heads/canary:refs/remotes/origin/daily-source')

    def test_ref_and_event_restrictions_apply_before_fetch(self):
        for event, ref_type, ref, source in (
                ('schedule', 'branch', 'canary', 'canary'),
                ('workflow_dispatch', 'branch', 'feature', 'canary'),
                ('workflow_dispatch', 'tag', 'canary', 'canary'),
                ('workflow_dispatch', 'branch', 'canary', 'master'),
                ('push', 'branch', 'master', 'canary')):
            with self.subTest(event=event, ref=ref, source=source):
                with patch.dict(os.environ, GITHUB_EVENT_NAME=event, GITHUB_REF_TYPE=ref_type,
                                GITHUB_REF_NAME=ref, GITHUB_SHA=SHA, GITHUB_REPOSITORY='Nonary/Vibepollo',
                                DAILY_PRIMARY_BRANCH='master', DAILY_RELEASE_SOURCE_BRANCH=source,
                                DAILY_RELEASE_SOURCE_CONFIRMED=f'Nonary/Vibepollo:{source}'):
                    with patch.object(daily, 'run') as commands:
                        with self.assertRaises(ValueError): daily.resolve()
                        commands.assert_not_called()

    def test_linux_requires_exact_source_version_run_and_hash(self):
        for scenario in ('valid', 'source', 'version', 'run', 'hash', 'extra'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temp:
                root, linux = Path(temp) / 'assets', Path(temp) / 'linux'
                root.mkdir(); linux.mkdir()
                name = 'vibeshine-2.0.0.daily-1-x86_64.pkg.tar.zst'
                (linux / name).write_bytes(b'package')
                evidence = dict(source_commit=SHA, version='version', workflow_run_id='123', assets={name: hashlib.sha256(b'package').hexdigest()})
                if scenario == 'source': evidence['source_commit'] = 'b' * 40
                if scenario == 'version': evidence['version'] = 'other'
                if scenario == 'run': evidence['workflow_run_id'] = '456'
                if scenario == 'hash': (linux / name).write_bytes(b'tampered')
                if scenario == 'extra': (linux / 'vibeshine-extra.pkg.tar.zst').write_bytes(b'other')
                (linux / 'linux-provenance.json').write_text(json.dumps(evidence))
                (root / 'daily-manifest.json').write_text(json.dumps({'assets': {}}))
                with patch.dict(os.environ, GITHUB_RUN_ID='123'):
                    if scenario == 'valid':
                        daily.include_linux(root, linux, 'Vibeshine', SHA, 'version')
                        self.assertEqual((root / name).read_bytes(), b'package')
                        self.assertEqual(json.loads((root / 'daily-manifest.json').read_text())['linux'], evidence)
                    else:
                        with self.assertRaises(ValueError): daily.include_linux(root, linux, 'Vibeshine', SHA, 'version')

    def test_calendar_identity_fits_msi(self):
        first = daily.identity(SHA, dt.date(2026, 10, 11), "2.0.0")
        self.assertEqual(first["tag"], "2.0.0-nightly.d20261011.r.1.aaaaaaaaaaaa")
        self.assertEqual(first["version"], first["tag"])
        for day in (dt.date(2026, 12, 31), dt.date(2027, 1, 1), dt.date(2028, 2, 29)):
            major, minor, patch_number = daily.identity(SHA, day, "2.0.0")["version"].split("-")[0].split(".")
            self.assertLessEqual(int(major), 255)
            self.assertLessEqual(int(minor), 255)
            self.assertLessEqual(int(patch_number) * 100 + 90, 65534)
        with self.assertRaises(ValueError):
            daily.identity("HEAD", dt.date(2026, 10, 11), "2.0.0")
        for base in ("0.0.0", "256.0.0", "2.256.0", "2.0.655", "2.0", "2.0.0-beta.1"):
            with self.assertRaises(ValueError):
                daily.identity(SHA, dt.date(2026, 10, 11), base)

    def test_revision_reservation_failure_and_retry(self):
        day = dt.date(2026, 10, 25)
        old = "2.0.1-nightly.d20261025." + "b" * 12
        first = daily.identity(SHA, day, "2.0.1")
        for inventory, expected in (("", first), (f"{'b'*40} refs/tags/{old}", first),
                                     (f"{'b'*40} refs/tags/{old}\n{'c'*40} refs/tags/{old.replace('b'*12, 'c'*12)}", first),
                                     (f"{SHA} refs/tags/{first['tag']}", first)):
            with self.subTest(inventory=inventory), patch.object(daily, "run", return_value=inventory):
                self.assertEqual(daily.reserve_identity(SHA, day, "2.0.1", [[]]), expected)
        with patch.object(daily, "run", return_value=f"{SHA} refs/tags/{first['tag']}"):
            with self.assertRaisesRegex(ValueError, "already exists"):
                daily.reserve_identity(SHA, day, "2.0.1", [[dict(tag_name=first['tag'], draft=True)]])
        for revision in (0, -1, 1000, True, "2"):
            with self.assertRaises(ValueError): daily.identity(SHA, day, "2.0.1", revision)
        last = daily.identity("b"*40, day, "2.0.1", 999)
        with patch.object(daily, "run", return_value=f"{'b'*40} refs/tags/{last['tag']}"):
            with self.assertRaisesRegex(ValueError, "revision"):
                daily.reserve_identity(SHA, day, "2.0.1", [[]])
        duplicate = f"{'b'*40} refs/tags/{first['tag']}\n{SHA} refs/tags/{first['tag']}"
        with patch.object(daily, "run", return_value=duplicate):
            with self.assertRaises(ValueError): daily.reserve_identity(SHA, day, "2.0.1", [[]])
        corrupt = daily.identity('b'*40, day, '2.0.1', 2)['tag']
        with patch.object(daily, 'run', return_value=f"{SHA} refs/tags/{corrupt}"):
            with self.assertRaisesRegex(ValueError, 'suffix'):
                daily.reserve_identity(SHA, day, '2.0.1', [[]])

    def test_republished_older_source_and_rewind_are_blocked(self):
        old = "2.0.1-nightly.d20261020." + SHA[:12]
        releases = [[dict(tag_name=old, draft=False, prerelease=True, published_at="2026-10-20T12:00:00Z")]]
        for same_source in (True, False):
            with tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / 'output'
                with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo", GITHUB_SHA=SHA,
                                DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="canary",
                                DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/Vibepollo:canary", GITHUB_OUTPUT=str(output)):
                    values = ["", SHA, "", json.dumps(releases), "", "2.0.0", SHA if same_source else "b"*40]
                    if not same_source:
                        values.append(daily.subprocess.CalledProcessError(1, ['git', 'merge-base']))
                    with patch.object(daily, "run", side_effect=values):
                        with patch.object(daily, "previous_successful_nightly", return_value={'tag':'previous', 'source_commit':'c'*40}):
                            if same_source:
                                daily.resolve()
                                self.assertIn('should_build=false', output.read_text())
                                self.assertIn('Source already published', output.read_text())
                            else:
                                with self.assertRaises(daily.subprocess.CalledProcessError): daily.resolve()
                                self.assertFalse(output.exists())

    def test_publish_rechecks_duplicate_draft_rewind_and_later_release(self):
        day = dt.datetime.now(dt.timezone.utc).date()
        tag = daily.identity(SHA, day, "2.0.1", 2)['tag']
        other = 'b' * 40
        for scenario in ('success', 'draft', 'same-source', 'later-identity', 'rewind', 'wrong-tag'):
            with self.subTest(scenario=scenario):
                releases = [[]]
                values = [SHA if scenario != 'wrong-tag' else other, json.dumps(releases)]
                if scenario == 'draft':
                    values[1] = json.dumps([[dict(tag_name=tag, draft=True)]])
                if scenario in ('same-source', 'later-identity'):
                    existing = daily.identity(other, day, '2.0.1', 3)['tag']
                    values[1] = json.dumps([[dict(tag_name=existing, draft=False, prerelease=True, published_at='today')]])
                    values.append(SHA if scenario == 'same-source' else other)
                if scenario == 'rewind':
                    values.append(daily.subprocess.CalledProcessError(1, ['git','merge-base']))
                with patch.object(daily, 'run', side_effect=values):
                    with patch.object(daily, 'previous_successful_nightly', return_value=(dict(tag='old', source_commit=other) if scenario == 'rewind' else None)):
                        if scenario == 'success':
                            daily.guard_publication('Nonary/Vibepollo', SHA, tag)
                        else:
                            with self.assertRaises((ValueError, daily.subprocess.CalledProcessError)):
                                daily.guard_publication('Nonary/Vibepollo', SHA, tag)

    def test_publish_orders_numeric_base_before_same_day_revision(self):
        day = dt.datetime.now(dt.timezone.utc).date()
        candidate = daily.identity(SHA, day, '2.0.2', 1)['tag']
        for base, blocked in (('2.0.1', False), ('2.0.3', True)):
            tag = daily.identity('b'*40, day, base, 99)['tag']
            releases = [[dict(tag_name=tag, draft=False, prerelease=True, published_at='today')]]
            with patch.object(daily, 'run', return_value='b'*40):
                if blocked:
                    with self.assertRaisesRegex(ValueError, 'newer numeric'):
                        daily.published_source(SHA, day, releases, candidate)
                else:
                    self.assertIsNone(daily.published_source(SHA, day, releases, candidate))

    def test_next_actual_day_resets_revision_and_future_dates_fail(self):
        day = dt.date(2026, 10, 25)
        tomorrow = day + dt.timedelta(days=1)
        future = daily.identity('b'*40, tomorrow, '2.0.1', 1)['tag']
        releases = [[dict(tag_name=future, draft=False, prerelease=True, published_at='future')]]
        with patch.object(daily, 'run', return_value='b'*40):
            with self.assertRaisesRegex(ValueError, 'newer than the current UTC date'):
                daily.published_source(SHA, day, releases)
        with patch.object(daily, 'run', return_value='') as commands:
            self.assertIn('.d20261026.r.1.', daily.reserve_identity(SHA, tomorrow, '2.0.1', [[]])['tag'])
            self.assertEqual(commands.call_args.args[-1], 'refs/tags/2.0.1-nightly.d20261026.*')

    def test_source_requires_exact_upstream_confirmation(self):
        for repository, branch, confirmation in (("Other/fork", "main", "Other/fork:main"), ("Nonary/vibeshine", "", ""), ("Nonary/vibeshine", "master", "true")):
            with self.assertRaises(ValueError):
                daily.validate_source(repository, branch, confirmation)
        daily.validate_source("Nonary/vibeshine", "master", "Nonary/vibeshine:master")
        daily.validate_source("Nonary/vibeshine", "canary", "Nonary/vibeshine:canary")
        daily.validate_source("Nonary/Vibepollo", "canary", "Nonary/Vibepollo:canary")
        for repository, confirmation in (("Nonary/vibeshine", "Nonary/vibeshine:master"),
                                         ("Nonary/Vibepollo", "Nonary/Vibepollo:master")):
            with self.assertRaises(ValueError):
                daily.validate_source(repository, "canary", confirmation)
        for retired in ("vibe", "vibe-test"):
            with self.assertRaises(ValueError):
                daily.validate_source("Nonary/vibeshine", retired, f"Nonary/vibeshine:{retired}")

    def test_reserved_nightly_tag_reuses_only_exact_source(self):
        day = dt.datetime.now(dt.timezone.utc).date()
        tag = daily.identity(SHA, day, "2.0.1")["tag"]
        for target, should_pass in ((SHA, True), ("b" * 40, False)):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / "output"
                with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo", GITHUB_SHA=SHA, DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master", DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/Vibepollo:master", GITHUB_OUTPUT=str(output)):
                    with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]", "", "2.0.0", f"{target} refs/tags/{tag}"]):
                        if should_pass:
                            daily.resolve()
                            self.assertIn(f"tag={tag}", output.read_text())
                        else:
                            with self.assertRaisesRegex(ValueError, "Immutable nightly tag"):
                                daily.resolve()

    def test_published_same_day_nightly_allows_changed_source(self):
        day = dt.datetime.now(dt.timezone.utc).date()
        released = daily.identity("b" * 40, day, "2.0.1")["tag"]
        releases = [[dict(tag_name=released, draft=False, prerelease=True, published_at="2026-10-22T12:00:00Z")]]
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo", GITHUB_SHA=SHA, DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master", DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/Vibepollo:master", GITHUB_OUTPUT=str(output)):
                with patch.object(daily, "run", side_effect=["", SHA, "", json.dumps(releases), "", "2.0.0", "b" * 40, f"{'b' * 40} refs/tags/{released}"]):
                    with patch.object(daily, "previous_successful_nightly", return_value=None):
                        daily.resolve()
            self.assertIn("should_build=true", output.read_text())
            self.assertIn(".r.2.", output.read_text())

    def test_resolve_fetches_confirmed_remote_commit_and_emits_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA=SHA, DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master", DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:master", GITHUB_OUTPUT=str(output)):
                with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]", "", "2.0.0-beta.4\n2.0.0", ""]) as commands:
                    daily.resolve()
            self.assertIn(f"commit={SHA}", output.read_text())
            self.assertIn("version=2.0.1-nightly", output.read_text())
            self.assertIn("should_build=true", output.read_text())
            self.assertEqual(commands.call_args_list[0].args, ("git", "fetch", "--no-tags", "origin", "refs/heads/master:refs/remotes/origin/daily-source"))

    def test_canary_pins_remote_tip_separately_from_default_workflow_commit(self):
        control = "b" * 40
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA=control,
                            DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="canary",
                            DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:canary", GITHUB_OUTPUT=str(output)):
                with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]", "", "2.0.0", ""]) as commands:
                    daily.resolve()
            self.assertIn(f"commit={SHA}", output.read_text())
            self.assertIn("should_build=true", output.read_text())
            self.assertEqual(commands.call_args_list[0].args, (
                "git", "fetch", "--no-tags", "origin", "refs/heads/canary:refs/remotes/origin/daily-source"))
            self.assertIn((("git", "diff", "--exit-code", control, SHA, "--", ".github/workflows"),),
                          commands.call_args_list)

    def test_canary_workflow_drift_blocks_publication(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA="b" * 40,
                            DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="canary",
                            DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:canary", GITHUB_OUTPUT=str(output)):
                mismatch = daily.subprocess.CalledProcessError(1, ["git", "diff"])
                with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]", mismatch]):
                    with self.assertRaises(daily.subprocess.CalledProcessError):
                        daily.resolve()
            self.assertFalse(output.exists())

    def test_vibepollo_canary_accepts_independent_commits_without_migration_pin(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo", GITHUB_SHA="b" * 40,
                            DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="canary",
                            DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/Vibepollo:canary", GITHUB_OUTPUT=str(output),
                            DAILY_RELEASE_MIGRATION_CONFIRMED="stale primary migration pin"):
                with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]", "", "2.0.0", ""]) as commands:
                    daily.resolve()
            self.assertIn(f"commit={SHA}", output.read_text())
            self.assertIn("should_build=true", output.read_text())
            self.assertEqual(commands.call_args_list[0].args, (
                "git", "fetch", "--no-tags", "origin", "refs/heads/canary:refs/remotes/origin/daily-source"))

    def test_unchanged_repo_skips_even_if_upstream_migration_pin_is_stale(self):
        for repository in ("Nonary/vibeshine", "Nonary/Vibepollo"):
            with self.subTest(repository=repository), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / "output"
                with patch.dict(os.environ, GITHUB_REPOSITORY=repository, GITHUB_SHA=SHA,
                                DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master",
                                DAILY_RELEASE_SOURCE_CONFIRMED=f"{repository}:master", GITHUB_OUTPUT=str(output),
                                DAILY_RELEASE_MIGRATION_CONFIRMED="stale upstream pin"):
                    with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]"]) as commands:
                        with patch.object(daily, "previous_successful_nightly", return_value={"tag": "daily-2026-10-10", "source_commit": SHA}) as baseline:
                            daily.resolve()
                baseline.assert_called_once_with(repository, [[]])
                self.assertIn("should_build=false", output.read_text())
                self.assertNotIn("version=", output.read_text())
                self.assertFalse(any("ls-remote" in call.args for call in commands.call_args_list))

    def test_changed_vibepollo_source_builds_without_migration_confirmation(self):
        for branch in ("master", "canary"):
            for event, migration in (("schedule", ""), ("workflow_dispatch", ""), ("workflow_dispatch", "stale upstream pin")):
                with self.subTest(branch=branch, event=event, migration=migration), tempfile.TemporaryDirectory() as temp:
                    output = Path(temp) / "output"
                    with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo", GITHUB_SHA=SHA,
                                    DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH=branch,
                                    DAILY_RELEASE_SOURCE_CONFIRMED=f"Nonary/Vibepollo:{branch}",
                                    GITHUB_OUTPUT=str(output), DAILY_RELEASE_MIGRATION_CONFIRMED=migration, GITHUB_EVENT_NAME=event):
                        with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]", "", "2.0.0", "", ""]) as commands:
                            with patch.object(daily, "previous_successful_nightly", return_value={"tag": "previous", "source_commit": "c" * 40}):
                                daily.resolve()
                    self.assertIn("should_build=true", output.read_text())
                    self.assertIn(f"commit={SHA}", output.read_text())
                    self.assertIn("version=2.0.1-nightly.", output.read_text())
                    self.assertFalse(any("https://github.com/Nonary/vibeshine.git" in call.args for call in commands.call_args_list))
                    self.assertIn((("git", "diff", "--exit-code", SHA, SHA, "--", ".github/workflows"),),
                                  commands.call_args_list)

    def test_vibepollo_invalid_source_still_fails_before_fetch(self):
        for branch, confirmation, primary in (
                ("vibe", "Nonary/Vibepollo:vibe", "master"),
                ("master", "", "master"),
                ("main", "Nonary/Vibepollo:main", "master")):
            with self.subTest(branch=branch, confirmation=confirmation):
                with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo",
                                DAILY_PRIMARY_BRANCH=primary, DAILY_RELEASE_SOURCE_BRANCH=branch,
                                DAILY_RELEASE_SOURCE_CONFIRMED=confirmation):
                    with patch.object(daily, "run") as commands:
                        with self.assertRaises(ValueError):
                            daily.resolve()
                    commands.assert_not_called()

    def test_manual_workflow_preserves_confirmed_source_and_publication_guards(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/daily-prerelease.yml").read_text()
        self.assertIn("  workflow_dispatch:", workflow)
        self.assertIn("github.event_name == 'workflow_dispatch'", workflow)
        self.assertIn("github.ref_name == 'canary'", workflow)
        self.assertIn("DAILY_RELEASE_SOURCE_BRANCH: ${{ vars.DAILY_RELEASE_SOURCE_BRANCH }}", workflow)
        self.assertIn("DAILY_RELEASE_SOURCE_CONFIRMED: ${{ vars.DAILY_RELEASE_SOURCE_CONFIRMED }}", workflow)
        self.assertIn("vars.DAILY_RELEASE_ENABLED == 'true' && inputs.publish == true", workflow)
        self.assertIn("needs.build.result == 'success' && needs.linux.result == 'success' && vars.DAILY_RELEASE_ENABLED == 'true'", workflow)
        self.assertIn("python scripts/daily_release.py publication", workflow)
        self.assertNotIn("DAILY_RELEASE_MIGRATION_CONFIRMED", workflow)
        self.assertNotIn("upstream_tag", workflow)
        self.assertNotIn("github.actor", workflow)

    def test_only_published_validated_nightly_is_a_skip_baseline(self):
        releases = [[
            {"tag_name": "daily-2026-10-11", "draft": True, "prerelease": True, "published_at": "2026-10-11T00:00:00Z"},
            {"tag_name": "daily-2026-10-10", "draft": False, "prerelease": True, "published_at": "2026-10-10T00:00:00Z", "assets": [{"name": "nightly-metadata.json", "id": 1}]},
        ]]
        metadata = {"schema_version": 1, "channel": "nightly", "tag": "daily-2026-10-10", "source_commit": SHA,
                    "version": f"2.0.0-daily.d20261010+{SHA[:12]}", "nightly_sequence": 20261010}
        with patch.object(daily, "run", side_effect=[json.dumps(metadata), SHA]):
            self.assertEqual(daily.previous_successful_nightly("Nonary/vibeshine", releases)["source_commit"], SHA)
        with patch.object(daily, "run", side_effect=[json.dumps(metadata), "b" * 40]):
            with self.assertRaisesRegex(ValueError, "tag"):
                daily.previous_successful_nightly("Nonary/vibeshine", releases)
        with patch.object(daily, "run") as commands:
            self.assertIsNone(daily.previous_successful_nightly("Nonary/vibeshine", [[releases[0][0]]]))
            self.assertIsNone(daily.previous_successful_nightly("Nonary/vibeshine", [[
                {"tag_name": "daily-2026-10-09", "draft": False, "prerelease": True, "published_at": "2026-10-09T00:00:00Z", "assets": []}
            ]]))
            commands.assert_not_called()

    def test_finalized_assets_reject_corruption_wrong_run_and_extra_files(self):
        info = daily.identity(SHA, dt.date(2026, 10, 11), "2.0.0")
        for scenario in ("valid", "corrupt", "wrong-run", "wrong-source", "extra", "duplicate", "missing", "public-msi", "unverified-msi"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "VibepolloSetup.exe").write_bytes(b"setup")
                provenance = {"schema_version": 1, "release_tag": info["tag"], "release_commit": SHA, "source_build_run_id": "123", "assets": {"VibepolloSetup.exe": hashlib.sha256(b"setup").hexdigest()}}
                provenance["embedded_msi"] = {"verified": True, "sha256": hashlib.sha256(b"msi").hexdigest()}
                if scenario == "corrupt":
                    (root / "VibepolloSetup.exe").write_bytes(b"tampered")
                if scenario == "wrong-run":
                    provenance["source_build_run_id"] = "456"
                if scenario == "wrong-source":
                    provenance["release_commit"] = "b" * 40
                if scenario == "extra":
                    (root / "local-driver.cat").write_bytes(b"untrusted")
                if scenario == "duplicate":
                    (root / "VibepolloSetup-copy.exe").write_bytes(b"setup")
                if scenario == "missing":
                    (root / "VibepolloSetup.exe").unlink()
                if scenario == "public-msi":
                    (root / "Vibepollo.msi").write_bytes(b"msi")
                if scenario == "unverified-msi":
                    provenance["embedded_msi"]["verified"] = False
                (root / "release-provenance.json").write_text(json.dumps(provenance))
                with patch.dict(os.environ, GITHUB_RUN_ID="123"):
                    if scenario == "valid":
                        daily.verify_assets(root, "Vibepollo", SHA, info["tag"], info["version"])
                        manifest = json.loads((root / "daily-manifest.json").read_text())
                        self.assertTrue(manifest["prerelease"])
                        self.assertFalse(manifest["signpath"])
                    else:
                        with self.assertRaises(ValueError):
                            daily.verify_assets(root, "Vibepollo", SHA, info["tag"], info["version"])


if __name__ == "__main__":
    unittest.main()
