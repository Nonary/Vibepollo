"""Small Prometheus exporter for Vibepollo session history.

The exporter intentionally uses only the Python standard library.  It polls
Vibepollo's authenticated read-only history endpoints and exposes the latest
sample of every active stream on a local HTTP endpoint.
"""
from __future__ import annotations

import json
import logging
import os
import ssl
import threading
import time
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


LOG = logging.getLogger("vibepollo_exporter")


def load_env_file() -> None:
    """Load the local service env file without overwriting real environment."""
    path = os.getenv(
        "VIBEPOLLO_EXPORTER_ENV_FILE",
        r"C:\prometheus\vibepollo-exporter.env" if os.name == "nt" else "",
    )
    if not path or not os.path.isfile(path):
        return
    with open(path, encoding="utf-8") as stream:
        for raw_line in stream:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip())


def number(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def label(value: Any) -> str:
    text = str(value if value is not None else "")
    return text.replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')


def metric_name(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in value)


def samples_from(detail: dict[str, Any]) -> list[dict[str, Any]]:
    samples = detail.get("samples", [])
    if not isinstance(samples, list):
        return []
    return [sample for sample in samples if isinstance(sample, dict)]


def latest_sample(detail: dict[str, Any]) -> dict[str, Any]:
    samples = samples_from(detail)
    if not samples:
        return {}
    return max(samples, key=lambda item: number(item.get("timestamp_unix")))


def active_session_summaries(payload: Any) -> list[dict[str, Any]]:
    if isinstance(payload, dict):
        payload = payload.get("sessions", payload.get("session", []))
    if isinstance(payload, dict):
        payload = [payload]
    if not isinstance(payload, list):
        return []
    return [item for item in payload if isinstance(item, dict)]


def payload_sessions(payload: Any) -> list[dict[str, Any]]:
    if isinstance(payload, dict):
        payload = payload.get("sessions", [])
    return active_session_summaries(payload)


@dataclass(frozen=True)
class Config:
    base_url: str
    token: str
    verify_tls: bool
    poll_seconds: float
    timeout_seconds: float
    listen_address: str
    listen_port: int

    @classmethod
    def from_env(cls) -> "Config":
        token = os.getenv("VIBEPOLLO_API_TOKEN", "")
        if not token:
            raise ValueError("VIBEPOLLO_API_TOKEN is required")
        return cls(
            base_url=os.getenv("VIBEPOLLO_URL", "https://127.0.0.1:47990").rstrip("/"),
            token=token,
            verify_tls=os.getenv("VIBEPOLLO_VERIFY_TLS", "false").lower()
            not in {"0", "false", "no", "off"},
            poll_seconds=max(0.2, float(os.getenv("VIBEPOLLO_EXPORTER_POLL_SECONDS", "1"))),
            timeout_seconds=max(0.2, float(os.getenv("VIBEPOLLO_EXPORTER_TIMEOUT", "3"))),
            listen_address=os.getenv("VIBEPOLLO_EXPORTER_LISTEN_ADDRESS", "127.0.0.1"),
            listen_port=int(os.getenv("VIBEPOLLO_EXPORTER_PORT", "9836")),
        )


class VibepolloAPI:
    def __init__(self, config: Config):
        self.config = config
        self.context = (
            ssl.create_default_context()
            if config.verify_tls
            else ssl._create_unverified_context()
        )

    def get_json(self, path: str) -> Any:
        request = Request(
            self.config.base_url + path,
            headers={
                "Accept": "application/json",
                "Authorization": f"Bearer {self.config.token}",
            },
            method="GET",
        )
        try:
            with urlopen(request, timeout=self.config.timeout_seconds, context=self.context) as response:
                return json.loads(response.read().decode("utf-8"))
        except HTTPError as exc:
            raise RuntimeError(f"Vibepollo HTTP {exc.code} on {path}") from exc
        except (URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"Vibepollo request failed on {path}: {exc}") from exc


class Metrics:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.text = ""
        self.last_success = 0.0
        self.last_error = ""

    def set(self, text: str, success: bool, error: str = "") -> None:
        with self.lock:
            self.text = text
            if success:
                self.last_success = time.time()
                self.last_error = ""
            else:
                self.last_error = error

    def get(self) -> str:
        with self.lock:
            return self.text


def render_metrics(
    details: list[dict[str, Any]],
    scrape_success: bool,
    scrape_duration: float,
    error: str = "",
    host_stats: dict[str, Any] | None = None,
    session_status: dict[str, Any] | None = None,
    history: dict[str, Any] | None = None,
    rtsp_sessions: list[dict[str, Any]] | None = None,
) -> str:
    lines: list[str] = []

    def emit(name: str, value: Any, labels: dict[str, Any] | None = None) -> None:
        rendered = metric_name(name)
        if labels:
            rendered_labels = ",".join(
                f'{metric_name(key)}="{label(val)}"' for key, val in sorted(labels.items())
            )
            rendered += "{" + rendered_labels + "}"
        lines.append(f"{rendered} {number(value):.12g}")

    emit("vibepollo_exporter_scrape_success", 1 if scrape_success else 0)
    emit("vibepollo_exporter_scrape_duration_seconds", scrape_duration)
    emit("vibepollo_exporter_last_success_timestamp_seconds", time.time() if scrape_success else 0)
    host_stats = host_stats or {}
    for field in (
        "cpu_percent", "cpu_temp_c", "gpu_encoder_percent", "gpu_percent",
        "gpu_temp_c", "net_rx_bps", "net_tx_bps", "ram_percent",
        "ram_total_bytes", "ram_used_bytes", "vram_percent",
        "vram_total_bytes", "vram_used_bytes",
    ):
        if field in host_stats:
            emit(f"vibepollo_host_{field}", host_stats[field])

    session_status = session_status or {}
    session_metric_names = {
        "activeSessions": "active_sessions",
        "appRunning": "app_running",
        "paused": "paused",
        "status": "status",
    }
    for field in session_metric_names:
        if field in session_status:
            value = session_status[field]
            if isinstance(value, bool):
                value = int(value)
            emit(f"vibepollo_session_{session_metric_names[field]}", value)
    if "appName" in session_status:
        emit("vibepollo_session_app_info", 1, {"app_name": session_status["appName"]})

    history = history or {}
    history_status = history.get("history_status", {})
    if isinstance(history_status, dict):
        for field in (
            "available", "degraded", "dropped_samples", "failed_writes",
            "pending_samples",
        ):
            if field in history_status:
                value = history_status[field]
                if isinstance(value, bool):
                    value = int(value)
                emit(f"vibepollo_history_{field}", value)
    history_items = payload_sessions(history)
    emit("vibepollo_history_sessions_total", len(history_items))
    if history_items:
        last = max(history_items, key=lambda item: number(item.get("end_time_unix")))
        session_uuid = last.get("uuid") or last.get("session_uuid") or "unknown"
        info_labels = {
            "session_uuid": session_uuid,
            "codec": last.get("codec", ""),
            "protocol": last.get("protocol", ""),
            "client_name": last.get("client_name", ""),
            "verdict": last.get("verdict", ""),
        }
        emit("vibepollo_last_session_info", 1, info_labels)
        for field in (
            "start_time_unix", "end_time_unix", "duration_seconds",
            "requested_bitrate_kbps", "encoder_bitrate_kbps",
        ):
            if field in last:
                emit(f"vibepollo_last_session_{field.removesuffix('_unix') if field.endswith('_unix') else field}",
                     last[field])

    rtsp_sessions = rtsp_sessions or []
    emit("vibepollo_rtsp_sessions", len(rtsp_sessions))
    for rtsp in rtsp_sessions:
        session_id = rtsp.get("id") or rtsp.get("session_id") or rtsp.get("uuid")
        if session_id:
            session_labels = {"session_id": session_id}
            emit("vibepollo_rtsp_session_info", 1, session_labels)
            for field in (
                "frame_id", "encode_wait_ms", "encode_ms", "packetize_ms",
                "send_wait_ms", "encode_queue_depth", "send_queue_depth",
                "pacing_lag_ms", "socket_send_blocked_ms", "frame_size_bytes",
            ):
                if field in rtsp:
                    emit(f"vibepollo_rtsp_{field}", rtsp[field], session_labels)

    for detail in details:
        session_id = detail.get("uuid") or detail.get("session_uuid") or "unknown"
        session_labels = {"session_uuid": session_id}
        latest = latest_sample(detail)
        samples = samples_from(detail)
        info_labels = {
            **session_labels,
            "codec": detail.get("codec", ""),
            "protocol": detail.get("protocol", ""),
            "client_name": detail.get("client_name", ""),
            "verdict": detail.get("verdict", ""),
        }
        emit("vibepollo_session_active", 1, session_labels)
        emit("vibepollo_session_info", 1, info_labels)
        for field in ("width", "height", "target_fps", "requested_bitrate_kbps", "duration_seconds"):
            if field in detail:
                emit(f"vibepollo_session_{field}", detail[field], session_labels)
        emit("vibepollo_sample_timestamp_seconds", latest.get("timestamp_unix", 0), session_labels)
        emit("vibepollo_encode_latency_ms", latest.get("encode_latency_ms", 0), session_labels)
        emit("vibepollo_encode_latency_max_ms",
             max((number(sample.get("encode_latency_ms")) for sample in samples), default=0),
             session_labels)
        sample_fields = (
            "actual_bitrate_kbps",
            "actual_fps",
            "host_cpu_percent",
            "host_gpu_percent",
            "host_gpu_encoder_percent",
            "host_gpu_temp_c",
            "host_vram_percent",
            "host_net_rx_bps",
            "host_net_tx_bps",
            "client_reported_losses",
            "video_dropped",
            "audio_dropped",
            "ref_invalidations",
            "idr_requests",
            "frames_sent",
            "packets_sent_video",
            "bytes_sent_total",
            "frame_interval_jitter_ms",
        )
        for field in sample_fields:
            emit(f"vibepollo_{field}", latest.get(field, 0), session_labels)
    if error:
        LOG.debug("last scrape error: %s", error)
    return "\n".join(lines) + "\n"


def poll_once(api: VibepolloAPI) -> tuple[
    list[dict[str, Any]], dict[str, Any], dict[str, Any], dict[str, Any],
    list[dict[str, Any]], bool, str
]:
    errors: list[str] = []

    def get(path: str, default: Any) -> Any:
        try:
            return api.get_json(path)
        except Exception as exc:
            errors.append(str(exc))
            return default

    host_stats = get("/api/host/stats", {})
    session_status = get("/api/session/status", {})
    history = get("/api/history/sessions", {})
    rtsp_payload = get("/api/rtsp/sessions", {})
    active = active_session_summaries(get("/api/history/sessions/active", []))
    details: list[dict[str, Any]] = []
    for summary in active:
        session_id = summary.get("uuid") or summary.get("session_uuid")
        if not session_id:
            continue
        detail = get(f"/api/history/sessions/{session_id}", {})
        if isinstance(detail, dict):
            details.append(detail)
    return (
        details,
        host_stats if isinstance(host_stats, dict) else {},
        session_status if isinstance(session_status, dict) else {},
        history if isinstance(history, dict) else {},
        payload_sessions(rtsp_payload),
        not errors,
        "; ".join(errors),
    )


def run(config: Config) -> None:
    api = VibepolloAPI(config)
    metrics = Metrics()

    def poller() -> None:
        while True:
            started = time.monotonic()
            try:
                details, host_stats, session_status, history, rtsp_sessions, success, error = poll_once(api)
                metrics.set(
                    render_metrics(
                        details, success, time.monotonic() - started, error,
                        host_stats, session_status, history, rtsp_sessions,
                    ),
                    success,
                    error,
                )
            except Exception as exc:  # exporter must remain scrapeable after API errors
                error = str(exc)
                LOG.warning("%s", error)
                metrics.set(
                    render_metrics([], False, time.monotonic() - started, error),
                    False,
                    error,
                )
            time.sleep(config.poll_seconds)

    threading.Thread(target=poller, name="vibepollo-poller", daemon=True).start()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802
            if self.path.split("?", 1)[0] != "/metrics":
                self.send_error(404)
                return
            body = metrics.get().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, _format: str, *_args: Any) -> None:
            return

    server = ThreadingHTTPServer((config.listen_address, config.listen_port), Handler)
    LOG.info("listening on %s:%s", config.listen_address, config.listen_port)
    server.serve_forever()


def main() -> None:
    load_env_file()
    logging.basicConfig(
        level=os.getenv("VIBEPOLLO_EXPORTER_LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    run(Config.from_env())


if __name__ == "__main__":
    main()
