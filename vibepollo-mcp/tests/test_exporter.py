import vibepollo_exporter as exporter


def test_render_metrics_uses_latest_sample_and_max_latency():
    detail = {
        "uuid": "session-1",
        "codec": "HEVC",
        "protocol": "rtsp",
        "client_name": "client",
        "verdict": "degraded",
        "width": 1920,
        "samples": [
            {"timestamp_unix": 1, "encode_latency_ms": 3.0, "host_net_tx_bps": 10},
            {"timestamp_unix": 2, "encode_latency_ms": 384.2, "host_net_tx_bps": 20},
        ],
    }
    text = exporter.render_metrics([detail], True, 0.1)
    assert 'vibepollo_session_active{session_uuid="session-1"} 1' in text
    assert 'vibepollo_encode_latency_ms{session_uuid="session-1"} 384.2' in text
    assert 'vibepollo_encode_latency_max_ms{session_uuid="session-1"} 384.2' in text
    assert 'vibepollo_host_net_tx_bps{session_uuid="session-1"} 20' in text


def test_active_session_summaries_accepts_empty_and_wrapped_payloads():
    assert exporter.active_session_summaries({"sessions": []}) == []
    assert exporter.active_session_summaries({"session": {"uuid": "x"}}) == [{"uuid": "x"}]
    assert exporter.active_session_summaries([{"uuid": "x"}]) == [{"uuid": "x"}]


def test_render_metrics_includes_host_session_history_and_rtsp_data():
    text = exporter.render_metrics(
        [],
        True,
        0.1,
        host_stats={"gpu_encoder_percent": 42.5, "vram_used_bytes": 123},
        session_status={"activeSessions": 1, "appRunning": True, "paused": False},
        history={
            "history_status": {"available": True, "degraded": False, "dropped_samples": 2},
            "sessions": [{
                "uuid": "old-1",
                "end_time_unix": 10,
                "start_time_unix": 5,
                "duration_seconds": 5,
                "codec": "HEVC",
                "protocol": "rtsp",
                "client_name": "fedora",
                "verdict": "degraded",
            }],
        },
        rtsp_sessions=[{"session_id": "rtsp-1", "frame_id": 7, "encode_ms": 1.5}],
    )
    assert "vibepollo_host_gpu_encoder_percent 42.5" in text
    assert "vibepollo_session_active_sessions 1" in text
    assert "vibepollo_session_app_running 1" in text
    assert 'vibepollo_last_session_info{client_name="fedora",codec="HEVC",protocol="rtsp",session_uuid="old-1",verdict="degraded"} 1' in text
    assert "vibepollo_history_dropped_samples 2" in text
    assert "vibepollo_rtsp_sessions 1" in text
    assert 'vibepollo_rtsp_session_info{session_id="rtsp-1"} 1' in text
    assert 'vibepollo_rtsp_frame_id{session_id="rtsp-1"} 7' in text
    assert 'vibepollo_rtsp_encode_ms{session_id="rtsp-1"} 1.5' in text


def test_poll_once_keeps_successful_endpoint_data_when_one_endpoint_fails():
    class API:
        def get_json(self, path):
            if path == "/api/session/status":
                raise RuntimeError("status unavailable")
            if path == "/api/host/stats":
                return {"gpu_encoder_percent": 12}
            if path == "/api/history/sessions":
                return {"sessions": []}
            if path == "/api/rtsp/sessions":
                return {"sessions": []}
            if path == "/api/history/sessions/active":
                return []
            raise AssertionError(path)

    details, host, status, history, rtsp, success, error = exporter.poll_once(API())
    assert details == []
    assert host["gpu_encoder_percent"] == 12
    assert status == {}
    assert history["sessions"] == []
    assert rtsp == []
    assert not success
    assert "status unavailable" in error
