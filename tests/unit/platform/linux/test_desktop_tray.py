#!/usr/bin/env python3
"""Exercise the desktop companion without touching the installed host.

Adapted from funkyotc's Nonary/Vibepollo PR601 (38525645ef25).
"""
import contextlib
import hashlib
import http.server
import importlib.util
import json
import os
import configparser
import queue
import re
import shutil
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import types
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location("tray", ROOT / "packaging/linux/vibepollo-tray.py")
tray = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tray)


class DesktopStandins:
    """Queue real worker completions into main() without a native Qt runtime.

    Only signal dispatch and display objects are substituted. main's action,
    completion, polling and menu-update callbacks execute unchanged.
    """

    def __init__(self, scenario):
        self.events = queue.Queue()
        self.actions = {}
        self.labels = []
        self.warnings = []
        self.timers = []
        desktop = self

        class Signal:
            def __init__(self, *args):
                self.slots = []

            def connect(self, slot):
                self.slots.append(slot)

            def emit(self, *args):
                for slot in self.slots:
                    desktop.events.put(lambda slot=slot, args=args: slot(*args))

        class Action:
            def __init__(self, text, parent=None):
                self.text, self.enabled = text, True
                self.triggered = Signal()

            def setText(self, text):
                self.text = text
                if text.startswith("Vibepollo: "):
                    desktop.labels.append(text)

            def setEnabled(self, enabled):
                self.enabled = enabled

        class Menu:
            def addAction(self, text, callback=None):
                action = text if isinstance(text, Action) else Action(text)
                desktop.actions[action.text] = action
                if callback:
                    action.triggered.connect(callback)
                return action

            def addSeparator(self):
                pass

        class Timer:
            def __init__(self):
                self.timeout = Signal()
                desktop.timers.append(self)

            def start(self, interval):
                self.interval = interval

            @staticmethod
            def singleShot(interval, callback):
                desktop.events.put(callback)

        widgets = types.ModuleType("PySide6.QtWidgets")
        core = types.ModuleType("PySide6.QtCore")
        gui = types.ModuleType("PySide6.QtGui")
        core.QObject, core.Signal, core.QTimer = object, Signal, Timer
        core.QLockFile, core.QStandardPaths, core.QUrl = MagicMock(), MagicMock(), MagicMock()
        core.QStandardPaths.writableLocation.return_value = "/tmp"
        gui.QAction = Action
        for name in ("QColor", "QDesktopServices", "QIcon", "QPainter", "QPixmap"):
            setattr(gui, name, MagicMock())
        widgets.QMenu = Menu
        widgets.QApplication = MagicMock()
        widgets.QApplication.return_value.exec.side_effect = scenario
        widgets.QSystemTrayIcon = MagicMock()
        for name in ("QDialog", "QDialogButtonBox", "QFormLayout", "QLabel", "QLineEdit"):
            setattr(widgets, name, MagicMock())
        widgets.QMessageBox = MagicMock()
        widgets.QMessageBox.question.return_value = widgets.QMessageBox.StandardButton.Yes
        widgets.QMessageBox.warning.side_effect = lambda *args: desktop.warnings.append(args[-1])
        self.modules = {"PySide6": types.ModuleType("PySide6"),
                        "PySide6.QtCore": core, "PySide6.QtGui": gui,
                        "PySide6.QtWidgets": widgets}

    def dispatch(self):
        try:
            callback = self.events.get(timeout=5)
        except queue.Empty as error:
            raise AssertionError("production callback did not deliver the expected refresh") from error
        callback()

    def trigger(self, name):
        self.actions[name].triggered.emit()
        self.dispatch()


class TrayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        directory = Path(cls.temporary.name)
        cls.cert = directory / "certificate.pem"
        cls.key = directory / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                        "-nodes", "-keyout", str(cls.key), "-out", str(cls.cert),
                        "-days", "1", "-subj", "/CN=localhost"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        cls.pin = hashlib.sha256(ssl.PEM_cert_to_DER_cert(cls.cert.read_text())).hexdigest()

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @contextlib.contextmanager
    def server(self, status=200, payload=None):
        calls = []
        default = {
            "status": True, "activeSessions": 1, "appRunning": True,
            "appName": "Test game", "paused": False,
        }

        class Handler(http.server.BaseHTTPRequestHandler):
            def handle(self):
                # Discovery and rejected pins close immediately after TLS.
                try:
                    super().handle()
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def reply(self):
                calls.append((self.command, self.path,
                              self.headers.get("Authorization"),
                              self.rfile.read(int(self.headers.get("Content-Length", 0)))))
                body = json.dumps(payload() if callable(payload) else payload if payload is not None else default).encode()
                self.send_response(status)
                if status == 302:
                    self.send_header("Location", "https://example.com/stolen")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            do_GET = reply
            do_POST = reply

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.cert, self.key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            yield server.server_port, calls
        finally:
            server.shutdown()
            server.server_close()
            worker.join()

    def test_certificate_discovery_sends_no_credentials_or_http(self):
        with self.server() as (port, calls):
            self.assertEqual(tray.certificate_fingerprint(port), self.pin)
            self.assertEqual(calls, [])

    def test_certificate_change_rejects_before_sending_token(self):
        with self.server() as (port, calls):
            for pin in (None, "0" * 64):
                with self.assertRaises(tray.CertificateChanged) as error:
                    tray.api_request(port, pin, "private-token", "/api/session/status")
                self.assertEqual(error.exception.fingerprint, self.pin)
            self.assertEqual(calls, [])

    def test_pinned_get_and_application_stop_use_scoped_token(self):
        with self.server() as (port, calls):
            session = tray.api_request(port, self.pin, "private-token", "/api/session/status")
            self.assertEqual(session["appName"], "Test game")
            tray.api_request(port, self.pin, "private-token", "/api/apps/close", "POST")
            self.assertEqual(calls, [
                ("GET", "/api/session/status", "Bearer private-token", b""),
                ("POST", "/api/apps/close", "Bearer private-token", b"{}"),
            ])

    def test_server_generator_alphabet_tokens_reach_pinned_tls(self):
        # Read the production generator's declared default rather than copying
        # a base64 assumption into the fixture. Exercise every character in a
        # 32-byte minted-token shape, including an interior '='.
        declaration = (ROOT / "src/crypto.h").read_text()
        alphabet = re.search(r'rand_alphabet\([^;]+std::string_view \{"([^"\n]+)"\}',
                             declaration).group(1)
        tokens = ["A" * 15 + character + "z" * 16 for character in alphabet]
        with self.server() as (port, calls):
            for token in tokens:
                with self.subTest(character=token[15]):
                    self.assertTrue(tray.api_request(port, self.pin, token,
                                                    "/api/session/status")["status"])
            self.assertEqual([call[2] for call in calls], ["Bearer " + token for token in tokens])
        with self.server() as (port, calls):
            token = "!%&()=" * 5 + "=-"
            with self.assertRaises(tray.CertificateChanged):
                tray.api_request(port, "0" * 64, token, "/api/session/status")
            self.assertEqual(calls, [])

    def test_redirect_does_not_forward_credentials(self):
        with self.server(status=302) as (port, calls):
            with self.assertRaisesRegex(tray.ApiError, "HTTP 302"):
                tray.api_request(port, self.pin, "private-token", "/api/session/status")
            self.assertEqual(len(calls), 1)

    def test_denied_token_error_does_not_leak_token(self):
        for status in (401, 403):
            with self.server(status=status) as (port, _):
                with self.assertRaises(tray.ApiError) as error:
                    tray.api_request(port, self.pin, "private-token", "/api/session/status")
                self.assertNotIn("private-token", str(error.exception))

    def test_unconfirmed_response_is_not_reported_as_success(self):
        with self.server(payload={"status": False}) as (port, _):
            with self.assertRaises(tray.ApiError):
                tray.api_request(port, self.pin, "private-token", "/api/apps/close", "POST")

    def test_malformed_session_status_is_not_presented_as_ready(self):
        with self.server(payload={"status": True, "activeSessions": "1"}) as (port, _):
            with self.assertRaisesRegex(tray.ApiError, "invalid session status"):
                tray.api_request(port, self.pin, "private-token", "/api/session/status")

    def test_unknown_routes_and_injected_token_are_rejected(self):
        with self.assertRaises(ValueError):
            tray.api_request(1, self.pin, "token", "/api/quit", "POST")
        with self.assertRaises(tray.ApiError):
            tray.api_request(1, self.pin, "token\r\nInjected: value", "/api/session/status")

    def test_service_control_preserves_controller_cleanup_authority(self):
        for action in ("start", "restart", "stop"):
            self.assertEqual(tray.service_command(action),
                             ["/usr/bin/systemctl", action, tray.CONTROLLER])
        with self.assertRaises(ValueError):
            tray.service_command("enable")
        with patch.object(tray.subprocess, "run", return_value=
                          subprocess.CompletedProcess([], 1, "", "Access denied")):
            with self.assertRaisesRegex(tray.ApiError, "Access denied"):
                tray.run_service_action("stop")

    def test_system_service_state_is_parsed_by_id(self):
        output = (f"ActiveState=inactive\nId={tray.HOST}\nSubState=dead\n\n"
                  f"SubState=running\nId={tray.CONTROLLER}\nActiveState=active\n")
        with patch.object(tray.subprocess, "run", return_value=
                          subprocess.CompletedProcess([], 0, output, "")):
            self.assertEqual(tray.read_services(), {tray.HOST: "inactive", tray.CONTROLLER: "active"})

    def test_latest_release_selection_excludes_drafts_and_separates_channels(self):
        def release(tag, date, **fields):
            return {"tag_name": tag, "published_at": date, **fields}
        stable = release("v2.0.0", "2026-10-10T00:00:00Z")
        nightly = release("v2.1.0-nightly.1", "2026-10-12T00:00:00Z", prerelease=True)
        self.assertEqual(tray.select_releases([
            release("draft", "2026-10-13T00:00:00Z", draft=True),
            nightly, release("old", "2026-10-01T00:00:00Z"), stable, {"invalid": True},
        ]), [stable, nightly])
        self.assertEqual(tray.select_releases([]), [])
        with self.assertRaises(tray.ApiError):
            tray.select_releases({"message": "rate limited"})
        self.assertEqual(tray.release_url("tag/with space"),
                         "https://github.com/Nonary/Vibepollo/releases/tag/tag%2Fwith%20space")

    def test_update_metadata_request_does_not_carry_local_credentials(self):
        response = unittest.mock.MagicMock()
        response.__enter__.return_value.read.return_value = b"[]"
        version = subprocess.CompletedProcess([], 0, "Vibepollo version 2.0.0", "")
        with patch.object(tray.urllib.request, "urlopen", return_value=response) as request:
            with patch.object(tray.subprocess, "run", return_value=version):
                self.assertEqual(tray.check_updates(), ("Vibepollo version 2.0.0", []))
        headers = request.call_args.args[0].headers
        self.assertNotIn("Authorization", headers)
        self.assertEqual(request.call_args.args[0].full_url, tray.RELEASES_API)

    def test_stream_paused_ready_stopped_and_auth_failure_states(self):
        active = {tray.HOST: "active", tray.CONTROLLER: "active"}
        self.assertEqual(tray.presentation(active, {"activeSessions": 1, "appName": "Game"})[:2],
                         ("Streaming: Game", "apollo-playing"))
        self.assertEqual(tray.presentation(active, {"appRunning": True, "appName": "Game"})[:2],
                         ("Paused: Game", "apollo-pausing"))
        self.assertEqual(tray.presentation(active, {})[0], "Ready")
        self.assertEqual(tray.presentation(active, error="Denied")[2], "#f5b041")
        self.assertEqual(tray.presentation({tray.HOST: "inactive", tray.CONTROLLER: "inactive"})[0], "Stopped")
        self.assertEqual(tray.presentation({tray.HOST: "failed", tray.CONTROLLER: "active"})[0], "Failed")
        self.assertEqual(tray.presentation({tray.HOST: "inactive", tray.CONTROLLER: "active"})[0], "Starting / unavailable")

    def test_token_storage_and_forget_are_private_and_atomic(self):
        with tempfile.TemporaryDirectory() as directory:
            store = tray.Settings(Path(directory) / "settings")
            self.assertEqual(store.load(), {})
            store.save({"token": "private-token", "fingerprint": self.pin})
            self.assertEqual(store.load()["token"], "private-token")
            self.assertEqual(store.path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(store.directory.stat().st_mode & 0o777, 0o700)
            store.save({"fingerprint": self.pin})
            self.assertNotIn("token", store.load())
            self.assertEqual([path.name for path in store.directory.iterdir()], ["settings.json"])
            store.path.write_text("not json")
            self.assertEqual(store.load(), {})

    def test_polling_real_tls_stream_transitions_and_forget(self):
        services = {tray.HOST: "active", tray.CONTROLLER: "active"}
        current = {"status": True, "activeSessions": 0, "appRunning": False, "appName": ""}
        with self.server(payload=lambda: current) as (port, calls):
            with patch.object(tray, "read_services", return_value=services):
                settings = {"token": "private-token", "fingerprint": self.pin}
                def label():
                    observed, session, error = tray.poll_status(port, settings)
                    return tray.presentation(observed, session, error)[0]
                self.assertEqual(label(), "Ready")
                current.update(activeSessions=1, appRunning=True, appName="Game")
                self.assertEqual(label(), "Streaming: Game")
                current.update(activeSessions=0)
                self.assertEqual(label(), "Paused: Game")
                current.update(appRunning=False, appName="")
                self.assertEqual(label(), "Ready")
                self.assertEqual(len(calls), 4)
                settings.pop("token")
                self.assertIn("connect a token", label())
                self.assertEqual(len(calls), 4)

    def test_bad_pin_poll_is_unknown_not_ready_and_sends_no_token(self):
        services = {tray.HOST: "active", tray.CONTROLLER: "active"}
        with self.server() as (port, calls):
            with patch.object(tray, "read_services", return_value=services):
                observed, session, error = tray.poll_status(port, {"token": "private-token", "fingerprint": "0" * 64})
                self.assertIsNone(session)
                self.assertTrue(error)
                self.assertEqual(tray.presentation(observed, session, error)[0], "Running — stream status unavailable")
                self.assertEqual(calls, [])

    def test_async_poll_completion_and_failure_are_invalidated_by_credentials(self):
        delivered = queue.Queue()
        requests = tray.BackgroundRequests(lambda *result: delivered.put(result))
        started, finish = threading.Event(), threading.Event()
        def slow_poll():
            started.set()
            self.assertTrue(finish.wait(5))
            return {"appName": "Old credential result"}
        self.assertTrue(requests.submit("poll", slow_poll))
        self.assertTrue(started.wait(5))
        self.assertFalse(requests.submit("poll", lambda: self.fail("duplicate IPC")))
        requests.advance()
        finish.set()
        kind, envelope, error = delivered.get(timeout=5)
        self.assertIsNone(error)
        accepted, result = requests.finish(kind, envelope)
        self.assertFalse(accepted)
        self.assertEqual(result["appName"], "Old credential result")
        self.assertNotIn("poll", requests.busy)
        self.assertTrue(requests.submit("poll", lambda: "current"))
        kind, envelope, error = delivered.get(timeout=5)
        self.assertEqual(requests.finish(kind, envelope), (True, "current"))
        def failed_poll():
            raise OSError("old error")
        self.assertTrue(requests.submit("poll", failed_poll))
        kind, envelope, error = delivered.get(timeout=5)
        requests.advance()
        self.assertFalse(requests.finish(kind, envelope)[0])
        self.assertIsInstance(error, OSError)

    def test_actual_action_completion_retires_older_polls_before_presenting(self):
        # Drive main's queued signal callback, not a duplicate epoch algorithm.
        # All systemctl execution is replaced at the subprocess boundary.
        for action_error in (False, True):
            for poll_error in (False, True):
                with self.subTest(action_error=action_error, poll_error=poll_error):
                    service_started, service_release = threading.Event(), threading.Event()
                    poll_started, poll_release = threading.Event(), threading.Event()
                    hold_poll = [False]
                    active = {tray.HOST: "active", tray.CONTROLLER: "active"}
                    inactive = {tray.HOST: "inactive", tray.CONTROLLER: "inactive"}
                    current_services = [active]

                    def systemctl(command, **kwargs):
                        if command == tray.service_command("stop"):
                            service_started.set()
                            if not service_release.wait(5):
                                raise TimeoutError("fixture did not release service action")
                            current_services[0] = active if action_error else inactive
                            return subprocess.CompletedProcess(command, int(action_error), "",
                                                               "Action denied" if action_error else "")
                        self.assertEqual(command[:2], ["/usr/bin/systemctl", "show"])
                        snapshot = dict(current_services[0])
                        if hold_poll[0]:
                            hold_poll[0] = False
                            poll_started.set()
                            if not poll_release.wait(5):
                                raise TimeoutError("fixture did not release held poll")
                            if poll_error:
                                raise OSError("stale service query error")
                        output = "\n\n".join(f"Id={unit}\nActiveState={state}\nSubState=running"
                                              for unit, state in snapshot.items())
                        return subprocess.CompletedProcess(command, 0, output, "")

                    def scenario():
                        desktop.dispatch()  # Initial poll -> ordinary running state.
                        initial = list(desktop.labels)
                        hold_poll[0] = True
                        desktop.trigger("Refresh status")  # Snapshot precedes the action.
                        self.assertTrue(poll_started.wait(5))
                        desktop.trigger("Quit Vibepollo")
                        self.assertTrue(service_started.wait(5))
                        self.assertFalse(desktop.actions["Start Vibepollo"].enabled)
                        self.assertFalse(desktop.actions["Restart"].enabled)
                        self.assertFalse(desktop.actions["Quit Vibepollo"].enabled)
                        service_release.set()
                        desktop.dispatch()  # Actual async service completion boundary.
                        self.assertEqual(desktop.labels, initial)
                        self.assertEqual(desktop.warnings, ["Action denied"] if action_error else [])
                        poll_release.set()
                        desktop.dispatch()  # Old success/error must never reach show().
                        self.assertEqual(desktop.labels, initial)
                        self.assertFalse(desktop.actions["Quit Vibepollo"].enabled)
                        self.assertNotIn("stale service query error", desktop.warnings)
                        desktop.dispatch()  # Fresh poll's real completion updates the menu.
                        expected = "Running — connect a token for stream status" if action_error else "Stopped"
                        self.assertEqual(desktop.labels[-1], "Vibepollo: " + expected)
                        self.assertEqual(desktop.actions["Start Vibepollo"].enabled, not action_error)
                        self.assertEqual(desktop.actions["Quit Vibepollo"].enabled, action_error)
                        return 0

                    desktop = DesktopStandins(scenario)
                    try:
                        with patch.dict(tray.sys.modules, desktop.modules), \
                                patch.dict(os.environ, {"DBUS_SESSION_BUS_ADDRESS": "fixture",
                                                        "WAYLAND_DISPLAY": "fixture"}), \
                                patch.object(tray.os, "getuid", return_value=1000), \
                                patch.object(tray.os, "geteuid", return_value=1000), \
                                patch.object(tray, "Settings") as settings, \
                                patch.object(tray.subprocess, "run", side_effect=systemctl):
                            settings.return_value.load.return_value = {}
                            self.assertEqual(tray.main(["--no-notifications"]), 0)
                    finally:
                        service_release.set()
                        poll_release.set()

    def test_pending_service_defers_polls_until_action_completion(self):
        # The action REMAINS pending while an older poll completes and repeated
        # timer/manual triggers run through main(). The old zero-delay retry is
        # caught before dispatching it, so the red control cannot launch a loop.
        real_thread = threading.Thread
        for action_error in (False, True):
            for poll_error in (False, True):
                with self.subTest(action_error=action_error, poll_error=poll_error):
                    service_started, service_release = threading.Event(), threading.Event()
                    poll_started, poll_release = threading.Event(), threading.Event()
                    hold_poll = [False]
                    workers, queries = [], []
                    active = {tray.HOST: "active", tray.CONTROLLER: "active"}
                    inactive = {tray.HOST: "inactive", tray.CONTROLLER: "inactive"}
                    current_services = [active]

                    def launch_worker(*args, **kwargs):
                        worker = real_thread(*args, **kwargs)
                        workers.append(worker)
                        return worker

                    def systemctl(command, **kwargs):
                        if command == tray.service_command("stop"):
                            service_started.set()
                            if not service_release.wait(5):
                                raise TimeoutError("fixture did not release service action")
                            current_services[0] = active if action_error else inactive
                            return subprocess.CompletedProcess(command, int(action_error), "",
                                                               "Action denied" if action_error else "")
                        self.assertEqual(command[:2], ["/usr/bin/systemctl", "show"])
                        queries.append(command)
                        snapshot = dict(current_services[0])
                        if hold_poll[0]:
                            hold_poll[0] = False
                            poll_started.set()
                            if not poll_release.wait(5):
                                raise TimeoutError("fixture did not release held poll")
                            if poll_error:
                                raise OSError("stale service query error")
                        output = "\n\n".join(f"Id={unit}\nActiveState={state}\nSubState=running"
                                              for unit, state in snapshot.items())
                        return subprocess.CompletedProcess(command, 0, output, "")

                    def scenario():
                        desktop.dispatch()
                        initial = list(desktop.labels)
                        hold_poll[0] = True
                        desktop.trigger("Refresh status")
                        self.assertTrue(poll_started.wait(5))
                        desktop.trigger("Quit Vibepollo")
                        self.assertTrue(service_started.wait(5))
                        poll_release.set()
                        desktop.dispatch()  # Success/error arrives with action still pending.
                        self.assertFalse(service_release.is_set())
                        self.assertEqual(desktop.labels, initial)
                        self.assertEqual(desktop.warnings, [])
                        self.assertTrue(desktop.events.empty(), "rejected poll queued an immediate retry")
                        self.assertEqual(len(workers), 3, "rejected poll launched another worker")
                        self.assertEqual(desktop.timers[0].interval, 5000)
                        for _ in range(12):
                            desktop.trigger("Refresh status")
                            desktop.timers[0].timeout.emit()
                            desktop.dispatch()
                            self.assertTrue(desktop.events.empty())
                            self.assertEqual(len(workers), 3, "pending action admitted status IPC")
                            self.assertEqual(len(queries), 2)
                            self.assertEqual(desktop.labels, initial)
                            for name in ("Start Vibepollo", "Restart", "Quit Vibepollo",
                                         "No application running"):
                                self.assertFalse(desktop.actions[name].enabled)
                        service_release.set()
                        desktop.dispatch()  # Success or denial requests exactly one refresh.
                        self.assertEqual(len(workers), 4)
                        self.assertEqual(desktop.warnings, ["Action denied"] if action_error else [])
                        desktop.dispatch()
                        expected = "Running — connect a token for stream status" if action_error else "Stopped"
                        self.assertEqual(desktop.labels[-1], "Vibepollo: " + expected)
                        self.assertEqual(desktop.actions["Start Vibepollo"].enabled, not action_error)
                        self.assertEqual(desktop.actions["Quit Vibepollo"].enabled, action_error)
                        self.assertEqual(len(queries), 3)
                        self.assertTrue(desktop.events.empty(), "fresh poll queued an unsolicited retry")
                        # External changes use the ordinary timer, not action epochs.
                        current_services[0] = inactive if action_error else active
                        self.assertTrue(desktop.events.empty())
                        desktop.timers[0].timeout.emit()
                        desktop.dispatch()
                        desktop.dispatch()
                        self.assertEqual(len(queries), 4)
                        self.assertEqual(len(workers), 5)
                        expected = "Stopped" if action_error else "Running — connect a token for stream status"
                        self.assertEqual(desktop.labels[-1], "Vibepollo: " + expected)
                        self.assertTrue(desktop.events.empty())
                        return 0

                    desktop = DesktopStandins(scenario)
                    try:
                        with patch.dict(tray.sys.modules, desktop.modules), \
                                patch.dict(os.environ, {"DBUS_SESSION_BUS_ADDRESS": "fixture",
                                                        "WAYLAND_DISPLAY": "fixture"}), \
                                patch.object(tray.os, "getuid", return_value=1000), \
                                patch.object(tray.os, "geteuid", return_value=1000), \
                                patch.object(tray, "Settings") as settings, \
                                patch.object(tray.threading, "Thread", side_effect=launch_worker), \
                                patch.object(tray.subprocess, "run", side_effect=systemctl):
                            settings.return_value.load.return_value = {}
                            self.assertEqual(tray.main(["--no-notifications"]), 0)
                    finally:
                        service_release.set()
                        poll_release.set()
                        for worker in workers:
                            worker.join(timeout=5)
                            self.assertFalse(worker.is_alive(), "fixture left a worker running")

    def test_older_poll_completions_while_service_is_running_are_rejected(self):
        for failed in (False, True):
            delivered = queue.Queue()
            requests = tray.BackgroundRequests(lambda *result: delivered.put(result))
            service_release, poll_release = threading.Event(), threading.Event()
            def action():
                if not service_release.wait(5):
                    raise TimeoutError("fixture did not release action")
            def poll():
                if not poll_release.wait(5):
                    raise TimeoutError("fixture did not release poll")
                if failed:
                    raise OSError("old error")
                return "old state"
            try:
                self.assertTrue(requests.submit("poll", poll))
                self.assertTrue(requests.submit("service", action))
                poll_release.set()
                kind, envelope, error = delivered.get(timeout=5)
                self.assertEqual(kind, "poll")
                self.assertFalse(requests.finish(kind, envelope)[0])
                self.assertEqual(isinstance(error, OSError), failed)
                self.assertFalse(requests.submit("poll", lambda: self.fail("action-era IPC")))
            finally:
                service_release.set()
                poll_release.set()
            kind, envelope, error = delivered.get(timeout=5)
            self.assertEqual(kind, "service")
            self.assertTrue(requests.finish(kind, envelope)[0])

    def test_service_completion_fences_work_started_during_action(self):
        for action_error in (False, True):
            with self.subTest(action_error=action_error):
                delivered = queue.Queue()
                requests = tray.BackgroundRequests(lambda *result: delivered.put(result))
                service_release, certificate_release = threading.Event(), threading.Event()

                def action():
                    if not service_release.wait(5):
                        raise TimeoutError("fixture did not release service action")
                    if action_error:
                        raise OSError("Action denied")

                def certificate():
                    if not certificate_release.wait(5):
                        raise TimeoutError("fixture did not release certificate discovery")
                    return "old action-era certificate"

                try:
                    self.assertTrue(requests.submit("service", action))
                    self.assertTrue(requests.submit("certificate", certificate))
                    service_release.set()
                    kind, envelope, error = delivered.get(timeout=5)
                    self.assertEqual(kind, "service")
                    self.assertEqual(isinstance(error, OSError), action_error)
                    self.assertTrue(requests.finish(kind, envelope)[0])
                    certificate_release.set()
                    kind, envelope, error = delivered.get(timeout=5)
                    self.assertEqual(kind, "certificate")
                    self.assertFalse(requests.finish(kind, envelope)[0])
                    self.assertNotIn("certificate", requests.busy)
                finally:
                    service_release.set()
                    certificate_release.set()

    def test_unsafe_storage_rejects_links_permissions_and_special_files(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            target = base / "target"
            target.mkdir(mode=0o700)
            linked = base / "linked"
            linked.symlink_to(target, target_is_directory=True)
            store = tray.Settings(linked)
            with self.assertRaises(OSError):
                store.save({"token": "secret"})
            self.assertEqual(list(target.iterdir()), [])
            store = tray.Settings(target)
            secret = base / "elsewhere"
            secret.write_text("unchanged")
            secret.chmod(0o600)
            store.path.symlink_to(secret)
            with self.assertRaises(OSError):
                store.load()
            store.save({"token": "new"})
            self.assertEqual(secret.read_text(), "unchanged")
            self.assertFalse(store.path.is_symlink())
            store.path.chmod(0o644)
            with self.assertRaises(OSError):
                store.load()
            store.path.unlink()
            os.mkfifo(store.path, mode=0o600)
            with self.assertRaises(OSError):
                store.load()
            target.chmod(0o755)
            with self.assertRaises(OSError):
                store.save({"token": "secret"})
            self.assertEqual(target.stat().st_mode & 0o777, 0o755)

    def test_unsafe_header_values_are_rejected_before_connecting(self):
        with patch.object(tray, "PinnedConnection") as connection:
            for token in ("", "has\x00null", "token\x1binjected", "x" * 4097, [],
                          "token\r\nInjected: value", "token\n", "token\r", "token\t",
                          "has space", "\u00e9", "token:extra", "token\\extra", "token\x7f"):
                with self.subTest(token=repr(token)), self.assertRaises(tray.ApiError):
                    tray.api_request(1, self.pin, token, "/api/session/status")
            connection.assert_not_called()

    def test_privileged_and_non_desktop_launches_stop_before_gui_or_ipc(self):
        with patch.object(tray.os, "getuid", return_value=0):
            with patch.object(tray.subprocess, "run") as run:
                self.assertEqual(tray.main([]), 1)
                run.assert_not_called()
        with patch.dict(os.environ, {}, clear=True):
            with patch.object(tray.os, "getuid", return_value=1000):
                with patch.object(tray.os, "geteuid", return_value=1000):
                    with patch.object(tray.subprocess, "run") as run:
                        self.assertEqual(tray.main([]), 1)
                        run.assert_not_called()


class TrayPackagingTests(unittest.TestCase):
    def test_arch_package_requires_the_tray_runtime(self):
        metadata = subprocess.run(["bash", "-c",
                                   '_use_cuda=false; source "$1"; printf "%s\\n" "${depends[@]}"',
                                   "tray-package", str(ROOT / "packaging/linux/Arch/PKGBUILD")],
                                  capture_output=True, text=True, check=True)
        self.assertIn("pyside6", metadata.stdout.splitlines())

    def test_native_cmake_stage_installs_companion_and_login_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            project = base / "project"
            project.mkdir()
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(tray_stage C)\n"
                "include(GNUInstallDirs)\n"
                'set(PROJECT_FQDN "io.github.Nonary.vibepollo")\n'
                f'include("{ROOT}/cmake/packaging/linux_desktop_tray.cmake")\n')
            for options, installed in (([], True), (["-DSUNSHINE_TRAY=0"], False),
                                       (["-DSUNSHINE_BUILD_APPIMAGE=ON"], False),
                                       (["-DSUNSHINE_BUILD_FLATPAK=ON"], False),
                                       (["-DSUNSHINE_BUILD_STEAMOS=ON"], False)):
                index = len(list(base.glob("build*")))
                build, stage = base / f"build{index}", base / f"stage{index}"
                subprocess.run(["cmake", "-S", str(project), "-B", str(build),
                                "-DCMAKE_INSTALL_PREFIX=/usr", "-DCMAKE_INSTALL_SYSCONFDIR=/etc",
                                "-DSUNSHINE_TRAY=1", *options], check=True, capture_output=True)
                subprocess.run(["cmake", "--install", str(build)], check=True,
                               env={**os.environ, "DESTDIR": str(stage)}, capture_output=True)
                if not installed:
                    self.assertFalse(stage.exists())
                    continue
                executable = stage / "usr/bin/vibepollo-tray"
                self.assertEqual(executable.stat().st_mode & 0o777, 0o755)
                subprocess.run([str(executable), "--help"], check=True, capture_output=True)
                login = stage / "etc/xdg/autostart/vibepollo-tray.desktop"
                launcher = stage / "usr/share/applications/vibepollo-tray.desktop"
                self.assertEqual(login.read_bytes(), launcher.read_bytes())
                entry = configparser.ConfigParser()
                entry.read(login)
                values = entry["Desktop Entry"]
                self.assertEqual(values["Exec"], "/usr/bin/vibepollo-tray")
                self.assertEqual(values["TryExec"], values["Exec"])
                self.assertEqual(values["Icon"], "apollo")
                self.assertEqual(values["Type"], "Application")
                self.assertEqual(values["Terminal"], "false")
                if shutil.which("desktop-file-validate"):
                    subprocess.run(["desktop-file-validate", str(login)], check=True)


if __name__ == "__main__":
    unittest.main()
