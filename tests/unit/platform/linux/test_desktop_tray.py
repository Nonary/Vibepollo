#!/usr/bin/env python3
"""Exercise the desktop companion without touching the installed host."""
import contextlib
import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location("tray", ROOT / "packaging/linux/vibepollo-tray.py")
tray = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tray)


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
        body = json.dumps(payload if payload is not None else {
            "status": True, "activeSessions": 1, "appRunning": True,
            "appName": "Test game", "paused": False,
        }).encode()

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


if __name__ == "__main__":
    unittest.main()
