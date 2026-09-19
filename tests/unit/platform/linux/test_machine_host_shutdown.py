#!/usr/bin/env python3
"""Exercise the real supervisor with private, unprivileged process fixtures."""

import os
import pathlib
import shlex
import signal
import subprocess
import sys
import tempfile
import time


def record(directory: pathlib.Path, event: str) -> None:
    descriptor = os.open(directory / "events", os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.write(descriptor, (event + "\n").encode())
    finally:
        os.close(descriptor)


def wait_for(predicate, label: str, timeout: float = 5) -> None:
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out waiting for {label}")
        time.sleep(0.01)


def fake_broker(directory: pathlib.Path) -> None:
    def interrupted(signum, frame):
        record(directory, "broker-signal")
        raise SystemExit(2)

    signal.signal(signal.SIGTERM, interrupted)
    (directory / "broker-pid").write_text(str(os.getpid()))
    sys.stdin.buffer.read()
    record(directory, "broker-disconnected")


def fake_detached_client(directory: pathlib.Path) -> None:
    def interrupted(signum, frame):
        # Model the fresh kernel membership snapshot after this client exits;
        # the deliberately stale, migrated outsider is no longer listed either.
        members = (directory / "cgroup.procs").read_text().split()
        retained = [member for member in members if member != str(os.getpid()) and
                    (directory / "proc" / member / "cgroup").read_text() ==
                    "0::/system.slice/vibepollo.service\n"]
        (directory / "cgroup.procs").write_text("\n".join(retained) + "\n")
        record(directory, "detached-client-term")
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, interrupted)
    pid = os.getpid()
    process_directory = directory / "proc" / str(pid)
    process_directory.mkdir()
    (process_directory / "cgroup").write_text("0::/system.slice/vibepollo.service\n")
    with (directory / "cgroup.procs").open("a") as members:
        members.write(f"{pid}\n")
    (directory / "detached-client-pid").write_text(str(pid))
    while True:
        signal.pause()


def fake_host(directory: pathlib.Path) -> None:
    stopping = False

    def interrupted(signum, frame):
        nonlocal stopping
        record(directory, "host-term")
        stopping = True

    signal.signal(signal.SIGTERM, interrupted)
    broker = subprocess.Popen(
        [sys.executable, str(pathlib.Path(__file__).resolve()), "--broker", str(directory)],
        stdin=subprocess.PIPE,
    )
    try:
        wait_for(lambda: (directory / "broker-pid").exists(), "broker startup")
        # Detached commands do not close just because their launching host
        # exits. The supervisor must retire this client after the host drain.
        subprocess.Popen(
            [sys.executable, str(pathlib.Path(__file__).resolve()), "--detached-client", str(directory)],
            stdin=subprocess.DEVNULL,
        )
        wait_for(lambda: (directory / "detached-client-pid").exists(), "detached client startup")
        (directory / "host-pid").write_text(f"{os.getpid()} {os.getppid()}")
        log = directory / "logs/vibepollo-20260918-120000-000.log"
        log.write_text("Configuration UI available at localhost\nFound H.264 encoder: fixture\n")
        (directory / "host-started").touch()
        if (directory / "scenario").read_text() == "early-exit":
            broker.stdin.close()
            assert broker.wait(timeout=3) == 0
            record(directory, "host-exited")
            return
        wait_for(lambda: stopping, "host termination request")
        wait_for(lambda: (directory / "finish-drain").exists(), "capture drain")
        assert broker.poll() is None, "broker died before capture drained"
        record(directory, "host-drained")
        broker.stdin.close()
        assert broker.wait(timeout=3) == 0, "broker received a service termination signal"
        record(directory, "host-exited")
    finally:
        if broker.poll() is None:
            broker.kill()
            broker.wait()


def run_case(source: pathlib.Path, scenario: str) -> None:
    with tempfile.TemporaryDirectory(prefix="vibepollo-host-shutdown-") as temporary:
        directory = pathlib.Path(temporary)
        (directory / "scenario").write_text(scenario)
        (directory / "logs").mkdir()
        (directory / "runtime").mkdir()
        (directory / "proc/self").mkdir(parents=True)
        (directory / "proc/self/cgroup").write_text("0::/system.slice/vibepollo.service\n")
        if scenario == "preflight-outside-group":
            (directory / "proc/self/cgroup").write_text("0::/system.slice/another.service\n")
        outsider = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], start_new_session=True)
        outsider_directory = directory / "proc" / str(outsider.pid)
        outsider_directory.mkdir()
        (outsider_directory / "cgroup").write_text("0::/system.slice/another.service\n")
        # A membership snapshot may contain a process that moved away. It
        # must be checked again after pidfd_open, not signalled from the list.
        (directory / "cgroup.procs").write_text(f"{outsider.pid}\n")
        (directory / "vibepollo.conf").symlink_to(pathlib.Path(__file__).resolve())
        text = source.read_text()
        start = text.index("\nrun_host() {\n") + 1
        end = text.index("\n}\n", start) + 3
        definition = text[start:end]
        # Redirect the privileged filesystem boundary, not signal handling,
        # readiness loops, waiting, or the real setpriv/exec launch helper.
        definition = definition.replace("$machine_profile", "$fixture_profile")
        definition = definition.replace("$machine_host_executable", "$fixture_executable")
        definition = definition.replace("/run/vibepollo/host", str(directory / "runtime"))
        launch = "  capability_clean_exec /usr/bin/env -i"
        assert definition.count(launch) == 1
        definition = definition.replace(launch, "  test_before_launch\n" + launch)
        assignment = "  host_pid=$!"
        assert definition.count(assignment) == 1
        # Deterministically schedule the fork/assignment race after the fake
        # host has installed its handler, without changing $! or the PID used
        # by the production supervisor.
        definition = definition.replace(assignment, "  test_after_launch\n" + assignment)
        if scenario == "readiness-timeout":
            definition = definition.replace("readiness_attempt<400", "readiness_attempt<1")
        start = text.index("\ncleanup_host_clients() {\n") + 1
        end = text.index("\n}\n", start) + 3
        cleanup = text[start:end]
        cleanup = cleanup.replace(
            "/sys/fs/cgroup/system.slice/vibepollo.service/cgroup.procs", str(directory / "cgroup.procs")
        )
        cleanup = cleanup.replace('f"/proc/{pid}/cgroup"', f'f"{directory}/proc/{{pid}}/cgroup"')
        supervisor_script = f"""
source {shlex.quote(str(source))}
fixture_profile={shlex.quote(str(directory))}
fixture_executable={shlex.quote(str(pathlib.Path(sys.executable).resolve()))}
scenario={shlex.quote(scenario)}
{cleanup}
{definition}
mkdir "$fixture_profile/proc/$BASHPID"
printf '0::/system.slice/vibepollo.service\\n' >"$fixture_profile/proc/$BASHPID/cgroup"
printf '%s\\n' "$BASHPID" >>"$fixture_profile/cgroup.procs"
load_session() {{ session_role=desktop; }}
capability_free_exec() {{ :; }}
function /usr/bin/id() {{ printf '%s\\n' "$service_user"; }}
function /usr/bin/stat() {{
  if [[ "${{@: -1}}" == "$fixture_executable" ]]; then
    printf 'root:%s:750:regular file\\n' "$service_user"
  else
    printf '%s:%s:700:directory\\n' "$service_user" "$service_user"
  fi
}}
function /usr/bin/getcap() {{ printf '%s cap_sys_admin,cap_sys_nice=p\\n' "$fixture_executable"; }}
function /usr/bin/systemd-notify() {{
  touch "$fixture_profile/notified"
  [[ "$scenario" != notify-failure ]]
}}
find_host_readiness_log() {{
  touch "$fixture_profile/readiness-probed"
  [[ "$scenario" == ready || "$scenario" == notify-failure ]] || return 1
  host_log="$fixture_profile/logs/vibepollo-20260918-120000-000.log"
  [[ -f "$host_log" ]]
}}
test_before_launch() {{
  [[ "$scenario" == before-launch ]] || return 0
  touch "$fixture_profile/before-launch"
  while [[ ! -e "$fixture_profile/allow-launch" ]]; do /usr/bin/sleep 0.01; done
}}
test_after_launch() {{
  while [[ ! -e "$fixture_profile/host-started" ]]; do /usr/bin/sleep 0.01; done
  [[ "$scenario" == before-assignment ]] || return 0
  touch "$fixture_profile/before-assignment"
  while [[ ! -e "$fixture_profile/allow-assignment" ]]; do /usr/bin/sleep 0.01; done
}}
run_host
"""
        if scenario == "supervisor-error":
            # An unrelated failure after the launch must execute the same
            # stop-and-drain EXIT guard, without early client cancellation.
            supervisor_script = supervisor_script.replace(
                "  if ((shutting_down)); then request_host_shutdown; fi",
                "  if ((shutting_down)); then request_host_shutdown; fi\n  (exit 17)",
            )
        supervisor = subprocess.Popen(
            ["/usr/bin/bash", "-c", supervisor_script],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        try:
            if scenario == "preflight-outside-group":
                output, error = supervisor.communicate(timeout=3)
                assert supervisor.returncode == 1, error.decode()
                assert b"outside the host service cgroup" in error, error.decode()
                assert not (directory / "host-started").exists(), "host started despite failed cleanup preflight"
                assert outsider.poll() is None
                print(f"PASS: supervisor ordered shutdown ({scenario})")
                return
            if scenario == "before-launch":
                wait_for(lambda: (directory / "before-launch").exists(), "prelaunch trap")
                supervisor.send_signal(signal.SIGTERM)
                time.sleep(0.05)
                assert supervisor.poll() is None, "supervisor died before the child launched"
                (directory / "allow-launch").touch()
            wait_for(lambda: (directory / "host-started").exists(), "host startup")
            host_pid, parent_pid = map(int, (directory / "host-pid").read_text().split())
            assert parent_pid == supervisor.pid, "host launch left an intermediary shell as host_pid"
            if scenario == "early-exit":
                output, error = supervisor.communicate(timeout=3)
                assert supervisor.returncode == 1, error.decode()
                assert (directory / "events").read_text().splitlines() == [
                    "broker-disconnected", "host-exited", "detached-client-term"
                ]
                assert outsider.poll() is None
                print(f"PASS: supervisor ordered shutdown ({scenario})")
                return
            if scenario == "before-assignment":
                wait_for(lambda: (directory / "before-assignment").exists(), "PID assignment barrier")
                supervisor.send_signal(signal.SIGTERM)
                time.sleep(0.05)
                (directory / "allow-assignment").touch()
            elif scenario not in ("before-launch", "supervisor-error", "readiness-timeout", "notify-failure"):
                marker = "notified" if scenario == "ready" else "readiness-probed"
                wait_for(lambda: (directory / marker).exists(), marker)
                supervisor.send_signal(signal.SIGTERM)

            events = lambda: (directory / "events").read_text().splitlines() if (directory / "events").exists() else []
            wait_for(lambda: "host-term" in events(), "forwarded host TERM")
            # Wait remains interruptible while the host deliberately holds its
            # subordinate broker connection open to finish the GPU drain.
            for _ in range(3):
                supervisor.send_signal(signal.SIGTERM)
                time.sleep(0.05)
            assert supervisor.poll() is None, "supervisor exited before capture drained"
            assert events() == ["host-term"], f"duplicate or premature signal: {events()}"
            os.kill(host_pid, 0)
            os.kill(int((directory / "broker-pid").read_text()), 0)
            os.kill(int((directory / "detached-client-pid").read_text()), 0)
            assert outsider.poll() is None, "an unrelated process was signalled"
            (directory / "finish-drain").touch()
            output, error = supervisor.communicate(timeout=3)
            expected_status = 17 if scenario == "supervisor-error" else (
                1 if scenario in ("readiness-timeout", "notify-failure") else 0
            )
            assert supervisor.returncode == expected_status, error.decode()
            assert events() == [
                "host-term", "host-drained", "broker-disconnected", "host-exited", "detached-client-term"
            ], events()
            assert outsider.poll() is None, "cleanup signalled a process outside its own cgroup"
        finally:
            # Kill only this test's isolated process group if a regression left
            # a child waiting; no installed unit or real host is contacted.
            try:
                os.killpg(supervisor.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            supervisor.communicate(timeout=3)
            outsider.kill()
            outsider.wait(timeout=3)
        print(f"PASS: supervisor ordered shutdown ({scenario})")


if __name__ == "__main__":
    if pathlib.Path(sys.argv[0]).name == "vibepollo.conf":
        fake_host(pathlib.Path(sys.argv[0]).parent)
    elif sys.argv[1] == "--broker":
        fake_broker(pathlib.Path(sys.argv[2]))
    elif sys.argv[1] == "--detached-client":
        fake_detached_client(pathlib.Path(sys.argv[2]))
    else:
        for case in (
            "before-launch", "before-assignment", "readiness", "ready", "supervisor-error",
            "readiness-timeout", "notify-failure", "early-exit", "preflight-outside-group",
        ):
            run_case(pathlib.Path(sys.argv[1]).resolve(), case)
