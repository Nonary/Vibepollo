#!/usr/bin/python3
"""Unprivileged desktop companion for Vibepollo's native machine service.

Adapted from funkyotc's Nonary/Vibepollo PR601, commit
38525645ef25482a66fc010cea26fd67996b8aa8 (GPL-3.0-only).

Requires PySide6. Service actions retain systemd/polkit authorization; stream
actions use a user-supplied, scoped Web API token over certificate-pinned TLS.
"""

import argparse
import hashlib
import http.client
import json
import os
import re
import stat
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import secrets
import threading
import urllib.parse
import urllib.request

CONTROLLER = "vibepollo-session-controller.service"
HOST = "vibepollo.service"
SCOPES = {"/api/session/status": "GET", "/api/apps/close": "POST"}
RELEASES_API = "https://api.github.com/repos/Nonary/Vibepollo/releases"
RELEASES_PAGE = "https://github.com/Nonary/Vibepollo/releases"


class CertificateChanged(Exception):
    def __init__(self, fingerprint):
        super().__init__("The local host certificate needs approval")
        self.fingerprint = fingerprint


class ApiError(Exception):
    pass


def certificate_fingerprint(port):
    # No HTTP data or credentials are sent during certificate discovery.
    context = ssl._create_unverified_context()
    with socket.create_connection(("127.0.0.1", port), timeout=5) as raw:
        with context.wrap_socket(raw, server_hostname="localhost") as connection:
            return hashlib.sha256(connection.getpeercert(binary_form=True)).hexdigest()


class PinnedConnection(http.client.HTTPSConnection):
    def __init__(self, port, fingerprint):
        super().__init__("127.0.0.1", port, timeout=5,
                         context=ssl._create_unverified_context())
        self.fingerprint = fingerprint

    def connect(self):
        super().connect()
        actual = hashlib.sha256(self.sock.getpeercert(binary_form=True)).hexdigest()
        if not self.fingerprint or actual != self.fingerprint:
            self.close()
            raise CertificateChanged(actual)


def api_request(port, fingerprint, token, path, method="GET"):
    if SCOPES.get(path) != method:
        raise ValueError("Unsupported tray API operation")
    # The server's crypto::rand_alphabet includes !%&() and interior '='.
    # Keep tokens bounded and header-safe without assuming base64 encoding.
    if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9._~+!%&()=/-]{1,4096}", token):
        raise ApiError("Connect an API token to use stream controls")
    connection = PinnedConnection(port, fingerprint)
    try:
        connection.request(method, path, body="{}" if method == "POST" else None,
                           headers={"Authorization": "Bearer " + token,
                                    "Content-Type": "application/json"})
        response = connection.getresponse()
        # http.client does not follow redirects, keeping credentials on loopback.
        if response.status in (401, 403):
            raise ApiError("API token expired, revoked, or missing the required scope")
        if response.status != 200:
            raise ApiError(f"Web API returned HTTP {response.status}")
        payload = response.read(1024 * 1024 + 1)
        if len(payload) > 1024 * 1024:
            raise ApiError("Web API response is too large")
        result = json.loads(payload)
        if not isinstance(result, dict) or result.get("status") is not True:
            raise ApiError("Web API did not confirm the operation")
        if path == "/api/session/status" and (
            type(result.get("activeSessions")) is not int
            or result["activeSessions"] < 0
            or type(result.get("appRunning")) is not bool
            or not isinstance(result.get("appName"), str)
        ):
            raise ApiError("Web API returned invalid session status")
        return result
    finally:
        connection.close()


def read_services():
    result = subprocess.run(
        ["/usr/bin/systemctl", "show", HOST, CONTROLLER,
         "--property=Id", "--property=ActiveState", "--property=SubState"],
        capture_output=True, text=True, timeout=5, check=True,
    )
    services = {}
    for block in result.stdout.strip().split("\n\n"):
        fields = dict(line.split("=", 1) for line in block.splitlines() if "=" in line)
        if "Id" in fields:
            services[fields["Id"]] = fields.get("ActiveState", "unknown")
    return services


def service_command(action):
    if action not in ("start", "restart", "stop"):
        raise ValueError("Unsupported controller operation")
    # Do not target the static host: controller cleanup owns the GPU/broker drain.
    # The desktop's normal polkit agent handles any required authorization.
    return ["/usr/bin/systemctl", action, CONTROLLER]


def run_service_action(action):
    result = subprocess.run(service_command(action), capture_output=True,
                            text=True, timeout=90)
    if result.returncode:
        raise ApiError(result.stderr.strip() or "Service operation failed")


def select_releases(data):
    if not isinstance(data, list):
        raise ApiError("Invalid release metadata")
    releases = [release for release in data if isinstance(release, dict)
                and not release.get("draft")
                and isinstance(release.get("tag_name"), str)
                and isinstance(release.get("published_at"), str)]
    selected = []
    for prerelease in (False, True):
        candidates = [release for release in releases
                      if bool(release.get("prerelease")) == prerelease]
        if candidates:
            selected.append(max(candidates, key=lambda release: release["published_at"]))
    return selected


def check_updates():
    request = urllib.request.Request(RELEASES_API, headers={
        "Accept": "application/vnd.github+json", "User-Agent": "Vibepollo-Desktop-Tray",
    })
    # Public metadata uses the system CA store and carries no local API token.
    with urllib.request.urlopen(request, timeout=10) as response:
        payload = response.read(2 * 1024 * 1024 + 1)
    if len(payload) > 2 * 1024 * 1024:
        raise ApiError("Release metadata is too large")
    releases = select_releases(json.loads(payload))
    version = subprocess.run(["/usr/bin/vibepollo", "--version"],
                             capture_output=True, text=True, timeout=5)
    return version.stdout.strip()[:500] or "Installed version unknown", releases


def release_url(tag):
    return RELEASES_PAGE + "/tag/" + urllib.parse.quote(tag, safe="")


def presentation(services, session=None, error=""):
    host = services.get(HOST, "unknown")
    controller = services.get(CONTROLLER, "unknown")
    if "failed" in (host, controller):
        return "Failed", "apollo-locked", "#e74c3c"
    if host == "inactive" and controller == "inactive":
        return "Stopped", "apollo-locked", "#e74c3c"
    if host != "active" or controller != "active":
        return "Starting / unavailable", "apollo-locked", "#f5b041"
    if error:
        return "Running — stream status unavailable", "apollo-tray", "#f5b041"
    if session is not None:
        name = str(session.get("appName") or "Desktop")[:160]
        if session.get("activeSessions", 0) > 0:
            return f"Streaming: {name}", "apollo-playing", "#2ecc71"
        if session.get("appRunning"):
            return f"Paused: {name}", "apollo-pausing", "#f5b041"
        return "Ready", "apollo-tray", "#2ecc71"
    return "Running — connect a token for stream status", "apollo-tray", "#2ecc71"


def poll_status(port, settings):
    services = read_services()
    session, error = None, ""
    if services.get(HOST) == "active" and settings.get("token"):
        try:
            session = api_request(port, settings.get("fingerprint"),
                                  settings["token"], "/api/session/status")
        except Exception as exception:
            error = str(exception)
    return services, session, error


class Settings:
    """Private, descriptor-relative storage; never chmod an existing path."""

    def __init__(self, directory):
        self.directory = Path(directory).absolute()
        self.path = self.directory / "settings.json"

    def _open_directory(self, create=False):
        descriptor = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            for component in self.directory.parts[1:]:
                if component in (".", ".."):
                    raise OSError("Unsafe tray settings path")
                if create:
                    try:
                        os.mkdir(component, mode=0o700, dir_fd=descriptor)
                    except FileExistsError:
                        pass
                child = os.open(component, os.O_RDONLY | os.O_DIRECTORY |
                                os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=descriptor)
                os.close(descriptor)
                descriptor = child
                attributes = os.fstat(descriptor)
                # Shared sticky ancestors (/tmp) are safe to traverse, but the
                # final private directory must belong to this desktop user.
                if attributes.st_mode & 0o022 and not attributes.st_mode & stat.S_ISVTX:
                    raise OSError("Writable ancestor in tray settings path")
            attributes = os.fstat(descriptor)
            if attributes.st_uid != os.getuid() or stat.S_IMODE(attributes.st_mode) != 0o700:
                raise OSError("Tray settings directory must be owned by this user and mode 0700")
            return descriptor
        except BaseException:
            os.close(descriptor)
            raise

    def load(self):
        try:
            directory = self._open_directory()
        except FileNotFoundError:
            return {}
        try:
            try:
                descriptor = os.open("settings.json", os.O_RDONLY | os.O_NOFOLLOW |
                                     os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=directory)
            except FileNotFoundError:
                return {}
            with os.fdopen(descriptor) as stream:
                attributes = os.fstat(stream.fileno())
                if (not stat.S_ISREG(attributes.st_mode) or attributes.st_uid != os.getuid()
                        or stat.S_IMODE(attributes.st_mode) != 0o600 or attributes.st_nlink != 1):
                    raise OSError("Unsafe tray settings file")
                contents = stream.read(16385)
                if len(contents) > 16384:
                    raise OSError("Tray settings file is too large")
                try:
                    data = json.loads(contents)
                    return data if isinstance(data, dict) else {}
                except ValueError:
                    return {}
        finally:
            os.close(directory)

    def save(self, data):
        directory = self._open_directory(create=True)
        temporary = ".settings-" + secrets.token_hex(16)
        try:
            descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                                 os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=directory)
            with os.fdopen(descriptor, "w") as stream:
                json.dump(data, stream)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, "settings.json", src_dir_fd=directory, dst_dir_fd=directory)
            os.fsync(directory)
        finally:
            try:
                os.unlink(temporary, dir_fd=directory)
            except FileNotFoundError:
                pass
            os.close(directory)


class RequestEpoch:
    """Reject worker responses from before a credential or service change."""

    def __init__(self):
        self.value = 0

    def advance(self):
        self.value += 1

    def accepts(self, value):
        return value == self.value



class BackgroundRequests(RequestEpoch):
    """Run blocking IPC off the desktop loop and tag every completion."""

    def __init__(self, deliver):
        super().__init__()
        self.deliver = deliver
        self.busy = set()

    def submit(self, kind, function):
        # Action-era status cannot be presented. Coalesce timer/manual requests
        # until service completion asks for one fresh poll, instead of doing IPC
        # whose completion would immediately be discarded and retried.
        if kind in self.busy or (kind == "poll" and "service" in self.busy):
            return False
        if kind == "service":
            self.advance()
        self.busy.add(kind)
        expected_generation = self.value

        def worker():
            try:
                result, error = function(), None
            except Exception as exception:
                result, error = None, exception
            self.deliver(kind, (expected_generation, result), error)

        threading.Thread(target=worker, daemon=True).start()
        return True

    def finish(self, kind, envelope):
        observed_generation, result = envelope
        accepted = self.accepts(observed_generation)
        self.busy.discard(kind)
        if kind == "service":
            # Fence work admitted during the action (for example certificate
            # discovery) on both success and failure at the GUI's async
            # completion boundary, even if credentials changed in the meantime.
            self.advance()
        elif kind == "poll" and "service" in self.busy:
            accepted = False
        return accepted, result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=47990, help="Local Web UI HTTPS port")
    parser.add_argument("--no-notifications", action="store_true")
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    if os.getuid() == 0 or os.geteuid() != os.getuid():
        print("Run the tray as an ordinary desktop user.", file=sys.stderr)
        return 1
    if not os.environ.get("DBUS_SESSION_BUS_ADDRESS") or not (
            os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY")):
        print("Run the tray inside a graphical desktop session.", file=sys.stderr)
        return 1
    try:
        from PySide6.QtCore import QLockFile, QObject, QStandardPaths, QTimer, QUrl, Signal
        from PySide6.QtGui import QAction, QColor, QDesktopServices, QIcon, QPainter, QPixmap
        from PySide6.QtWidgets import (QApplication, QDialog, QDialogButtonBox,
                                      QFormLayout, QLabel, QLineEdit, QMenu,
                                      QMessageBox, QSystemTrayIcon)
    except ImportError:
        print("Vibepollo tray requires PySide6 (QtWidgets).", file=sys.stderr)
        return 1

    app = QApplication([sys.argv[0]])
    app.setApplicationName("Vibepollo Tray")
    app.setQuitOnLastWindowClosed(False)
    # QLockFile handles stale owners and concurrent desktop/manual launches.
    runtime = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.RuntimeLocation)
    instance = QLockFile(str(Path(runtime) / f"vibepollo-tray-{args.port}.lock"))
    instance.setStaleLockTime(0)
    if not instance.tryLock(0):
        return 0

    config_root = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    store = Settings(config_root / "vibepollo-tray" / str(args.port))
    try:
        settings = store.load()
    except OSError as error:
        QMessageBox.warning(None, "Vibepollo", "Could not read private tray settings: " + str(error))
        settings = {}

    class Results(QObject):
        ready = Signal(str, object, object)

    signals = Results()
    tray = QSystemTrayIcon()
    menu = QMenu()
    status = QAction("Vibepollo: checking…", menu)
    status.setEnabled(False)
    menu.addAction(status)
    base_url = f"https://127.0.0.1:{args.port}"

    def open_ui(path="/"):
        QDesktopServices.openUrl(QUrl(base_url + path))

    menu.addAction("Open Vibepollo", lambda: open_ui())
    menu.addAction("Pair a client", lambda: open_ui("/v2/pair"))
    menu.addSeparator()
    close_action = menu.addAction("No application running")
    updates_action = menu.addAction("Check for Update")
    start_action = menu.addAction("Start Vibepollo")
    restart_action = menu.addAction("Restart")
    stop_action = menu.addAction("Quit Vibepollo")
    menu.addSeparator()
    connect_action = menu.addAction("Connect API token…")
    forget_action = menu.addAction("Forget API token")
    refresh_action = menu.addAction("Refresh status")
    menu.addAction("Exit tray", app.quit)
    tray.setContextMenu(menu)
    requests = BackgroundRequests(signals.ready.emit)
    busy = requests.busy
    epoch = requests
    submit = requests.submit
    last_state = None

    def show(label, icon_name, color, detail=""):
        pixmap = QPixmap(64, 64)
        pixmap.fill(QColor("transparent"))
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        icon = QIcon.fromTheme(icon_name, QIcon.fromTheme("apollo"))
        if icon.isNull():
            painter.setBrush(QColor("#526579"))
            painter.drawEllipse(4, 4, 52, 52)
        else:
            icon.paint(painter, 0, 0, 56, 56)
        painter.setPen(QColor("white"))
        painter.setBrush(QColor(color))
        painter.drawEllipse(39, 39, 23, 23)
        painter.end()
        tray.setIcon(QIcon(pixmap))
        tray.setToolTip(f"Vibepollo: {label}" + ("\n" + detail if detail else ""))
        status.setText(f"Vibepollo: {label}")

    def poll():
        snapshot = dict(settings)
        submit("poll", lambda: poll_status(args.port, snapshot))

    def service_action(action):
        if "service" in busy:
            return
        if action in ("restart", "stop"):
            verb = "Restart" if action == "restart" else "Quit"
            if QMessageBox.question(None, f"{verb} Vibepollo?",
                                    "This ends active streams and the running application.") != QMessageBox.StandardButton.Yes:
                return
        start_action.setEnabled(False)
        restart_action.setEnabled(False)
        stop_action.setEnabled(False)
        close_action.setEnabled(False)
        submit("service", lambda: run_service_action(action))

    def close_application():
        if not settings.get("token"):
            connect_token()
            return
        if QMessageBox.question(None, "Stop current application?",
                                "This ends the running application and its active streams.") != QMessageBox.StandardButton.Yes:
            return
        snapshot = dict(settings)
        epoch.advance()
        close_action.setEnabled(False)
        submit("close", lambda: api_request(args.port, snapshot.get("fingerprint"),
                                            snapshot["token"], "/api/apps/close", "POST"))

    def connect_token():
        submit("certificate", lambda: certificate_fingerprint(args.port))

    def request_update_check():
        updates_action.setEnabled(False)
        submit("updates", check_updates)

    def forget_token():
        epoch.advance()
        settings.pop("token", None)
        try:
            store.save(settings)
        except OSError as error:
            QMessageBox.warning(None, "Vibepollo", "Could not remove the saved token: " + str(error))
        # Polls already in flight belong to the old token generation.
        QTimer.singleShot(0, poll)

    def token_dialog(fingerprint):
        if settings.get("fingerprint") != fingerprint:
            formatted = ":".join(fingerprint[i:i + 2] for i in range(0, len(fingerprint), 2))
            if QMessageBox.question(None, "Trust local Vibepollo certificate?",
                                    "Verify this SHA-256 fingerprint against the certificate in your local Web UI before connecting:\n\n" + formatted) != QMessageBox.StandardButton.Yes:
                return
        dialog = QDialog()
        dialog.setWindowTitle("Connect Vibepollo tray")
        form = QFormLayout(dialog)
        instructions = QLabel("Create a token in Vibepollo → API Tokens with:\n"
                              "GET /api/session/status\nPOST /api/apps/close\n\n"
                              "The token is saved privately for automatic login startup.")
        form.addRow(instructions)
        open_tokens = QDialogButtonBox()
        tokens_button = open_tokens.addButton("Open API Tokens", QDialogButtonBox.ButtonRole.ActionRole)
        tokens_button.clicked.connect(lambda: open_ui("/v2/api-tokens"))
        form.addRow(open_tokens)
        token_edit = QLineEdit()
        token_edit.setEchoMode(QLineEdit.EchoMode.Password)
        form.addRow("API token", token_edit)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        form.addRow(buttons)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        token = token_edit.text()
        if not token or any(character.isspace() for character in token):
            QMessageBox.warning(None, "Invalid token", "Enter a valid API token without whitespace.")
            return
        # Validate before replacing a working token or trusting a new certificate.
        submit("connect", lambda: (fingerprint, token,
                                    api_request(args.port, fingerprint, token, "/api/session/status")))

    def completed(kind, result, error):
        nonlocal last_state
        accepted, result = requests.finish(kind, result)
        if kind == "updates":
            updates_action.setEnabled(True)
        if not accepted:
            # finish() has retired this request. Admission defers the refresh
            # while a service action or an older poll still owns its slot; that
            # owner's completion asks again. No completion-driven timer loop.
            poll()
            return
        if error is not None:
            if kind == "poll":
                show("Status unavailable", "apollo-locked", "#f5b041", "Could not read system service status")
                for action in (start_action, restart_action, stop_action, close_action):
                    action.setEnabled(False)
            else:
                # Tokens never appear in exception messages or desktop notifications.
                QMessageBox.warning(None, "Vibepollo", str(error))
                poll()
            return
        if kind == "certificate":
            token_dialog(result)
        elif kind == "updates":
            version, releases = result
            dialog = QMessageBox()
            dialog.setWindowTitle("Vibepollo updates")
            labels = ["Latest " + ("prerelease" if release.get("prerelease") else "stable")
                      + ": " + release["tag_name"] for release in releases]
            dialog.setText(version + "\n\n" + ("\n".join(labels) or "No published releases found"))
            # Show the actual tags rather than claiming a nightly/stable ordering
            # that may not match the host's installed release channel.
            release_buttons = {}
            for release in releases:
                label = "Open " + ("prerelease" if release.get("prerelease") else "stable release")
                button = dialog.addButton(label, QMessageBox.ButtonRole.ActionRole)
                release_buttons[button] = release_url(release["tag_name"])
            dialog.addButton(QMessageBox.StandardButton.Close)
            dialog.exec()
            if dialog.clickedButton() in release_buttons:
                QDesktopServices.openUrl(QUrl(release_buttons[dialog.clickedButton()]))
        elif kind == "connect":
            fingerprint, token, _ = result
            settings.update(fingerprint=fingerprint, token=token)
            try:
                store.save(settings)
            except OSError as error:
                QMessageBox.warning(None, "Vibepollo", "Connected for this session, but could not save the token: " + str(error))
            epoch.advance()
            poll()
        elif kind == "poll":
            services, session, api_error = result
            label, icon, color = presentation(services, session, api_error)
            detail = f"Host: {services.get(HOST, 'unknown')}\nController: {services.get(CONTROLLER, 'unknown')}"
            if api_error:
                detail += "\n" + api_error
            show(label, icon, color, detail)
            host_active = services.get(HOST) == "active"
            controller_state = services.get(CONTROLLER)
            start_action.setEnabled(controller_state in ("inactive", "failed"))
            restart_action.setEnabled(controller_state in ("active", "failed"))
            stop_action.setEnabled(controller_state == "active")
            can_close = bool(session and session.get("appRunning"))
            close_action.setEnabled(can_close and "close" not in busy)
            close_action.setText("Stop " + str(session.get("appName") or "current application")[:160]
                                 if can_close else "No application running" if session is not None
                                 else "Connect token for application controls" if host_active
                                 else "No application running")
            forget_action.setEnabled(bool(settings.get("token")))
            state = icon, label, services.get(HOST), services.get(CONTROLLER)
            if last_state is not None and state != last_state and not args.no_notifications:
                tray.showMessage("Vibepollo", label)
            last_state = state
        else:
            poll()

    signals.ready.connect(completed)
    connect_action.triggered.connect(connect_token)
    forget_action.triggered.connect(forget_token)
    refresh_action.triggered.connect(poll)
    updates_action.triggered.connect(request_update_check)
    close_action.triggered.connect(close_application)
    start_action.triggered.connect(lambda: service_action("start"))
    restart_action.triggered.connect(lambda: service_action("restart"))
    stop_action.triggered.connect(lambda: service_action("stop"))
    tray.activated.connect(lambda reason: open_ui() if reason == QSystemTrayIcon.ActivationReason.Trigger else None)
    timer = QTimer()
    timer.timeout.connect(poll)
    timer.start(5000)
    show("Checking", "apollo-tray", "#f5b041")
    tray.show()
    poll()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
