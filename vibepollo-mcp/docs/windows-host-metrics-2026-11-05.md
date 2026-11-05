# Windows host metrics for Vibepollo

Applied on `nsmrnov@192.168.1.158` on 2026-11-05.

## Runtime

- `windows_exporter` 0.32.1 installed as an automatic Windows service.
- Listen address: `:9182` (Prometheus scrapes it locally as `127.0.0.1:9182`).
- Enabled collectors: `cpu,memory,logical_disk,net,os,service,system,process,time,gpu`.
- Process collector is limited to `sunshine` and `prometheus`.
- Prometheus config: `C:\prometheus\prometheus.yml`.
- Prometheus service was restarted after config validation with `promtool check config`.
- Backup created beside the config as `prometheus.yml.bak.<timestamp>`.

## Prometheus labels

```text
job="windows-host"
device="vibepollo-host"
role="moonlight-host"
host_os="windows"
instance="127.0.0.1:9182"
```

## Useful queries

```promql
up{job="windows-host"}

rate(windows_process_cpu_time_seconds_total{
  job="windows-host",process="sunshine"
}[1m])

windows_process_working_set_bytes{
  job="windows-host",process="sunshine"
}

rate(windows_gpu_engine_time_seconds_total{
  job="windows-host",process_id=~".+",engtype="VideoEncode"
}[1m])

rate(windows_gpu_engine_time_seconds_total{
  job="windows-host",process_id="10256",engtype="VideoEncode"
}[1m])

windows_gpu_adapter_memory_committed_bytes{job="windows-host"}

rate(windows_net_bytes_sent_total{job="windows-host"}[1m])
rate(windows_net_bytes_received_total{job="windows-host"}[1m])
```

The Sunshine PID is not stable. Resolve it from the current series instead of hard-coding `10256`:

```promql
windows_process_info{job="windows-host",process="sunshine"}
```

## Verification

At completion:

- `windows_exporter` service: `Running`, `Automatic`.
- `Prometheus` service: `Running`, `Automatic`.
- Prometheus target `windows-host`: `up`, empty `lastError`.
- Prometheus target `openwrt-router`: still `up`, empty `lastError`.
- GPU `VideoEncode` engine series exists for the Sunshine process.

## Boundary

This collects Windows/GPU/process/network telemetry. It does **not** expose Vibepollo's internal per-frame `encode_wait_ms`, `send_wait_ms`, pacing queue depth, or NVENC API call duration. Those require instrumentation in the Vibepollo/Sunshine streaming code or an authenticated application metrics endpoint.

## NVIDIA GPU exporter

- `nvidia_gpu_exporter` 1.17.0 copied to `C:\prometheus\nvidia_gpu_exporter.exe`.
- Windows service: `NvidiaGpuExporter`, automatic, listening on `:9835`.
- Backend: `nvml`; per-process graphics metrics enabled; background collection interval: 1s.
- Prometheus job: `nvidia-gpu`, target `127.0.0.1:9835`, labels `device=vibepollo-gpu`, `role=nvenc`.

Useful GPU queries:

```promql
up{job="nvidia-gpu"}
nvidia_smi_utilization_encoder_ratio{job="nvidia-gpu"}
nvidia_smi_encoder_stats_average_latency{job="nvidia-gpu"}
nvidia_smi_memory_used_bytes{job="nvidia-gpu"}
nvidia_smi_temperature_gpu{job="nvidia-gpu"}
nvidia_smi_power_draw_watts{job="nvidia-gpu"}
```

## Vibepollo Python exporter

- Source: `vibepollo_exporter.py`.
- Installed on Windows as `C:\prometheus\vibepollo_exporter.py`.
- Service: `VibepolloExporter` via NSSM, automatic, listening on `127.0.0.1:9836`.
- Credentials are stored in the ACL-restricted `C:\prometheus\vibepollo-exporter.env`, not in the repository.
- Prometheus job: `vibepollo-exporter`, target `127.0.0.1:9836`.

The exporter polls:

```text
/api/host/stats
/api/session/status
/api/history/sessions
/api/history/sessions/active
/api/history/sessions/<UUID>
/api/rtsp/sessions
```

It exposes host/NVENC utilization, session state, history queue health and
latest completed-session metadata, RTSP session count, and the newest sample
for each active session plus the maximum encode latency observed in the
currently returned detail window.

Useful queries:

```promql
up{job="vibepollo-exporter"}
vibepollo_exporter_scrape_success{job="vibepollo-exporter"}
vibepollo_encode_latency_ms{job="vibepollo-exporter"}
vibepollo_encode_latency_max_ms{job="vibepollo-exporter"}
vibepollo_host_net_tx_bps{job="vibepollo-exporter"}
vibepollo_host_gpu_encoder_percent{job="vibepollo-exporter"}
vibepollo_session_active_sessions{job="vibepollo-exporter"}
vibepollo_history_degraded{job="vibepollo-exporter"}
vibepollo_history_pending_samples{job="vibepollo-exporter"}
vibepollo_history_sessions_total{job="vibepollo-exporter"}
vibepollo_last_session_info{job="vibepollo-exporter"}
vibepollo_rtsp_sessions{job="vibepollo-exporter"}
vibepollo_rtsp_encode_ms{job="vibepollo-exporter"}
vibepollo_rtsp_encode_wait_ms{job="vibepollo-exporter"}
vibepollo_rtsp_packetize_ms{job="vibepollo-exporter"}
vibepollo_rtsp_send_wait_ms{job="vibepollo-exporter"}
vibepollo_rtsp_send_queue_depth{job="vibepollo-exporter"}
vibepollo_rtsp_pacing_lag_ms{job="vibepollo-exporter"}
vibepollo_rtsp_socket_send_blocked_ms{job="vibepollo-exporter"}
vibepollo_rtsp_frame_size_bytes{job="vibepollo-exporter"}
vibepollo_client_reported_losses{job="vibepollo-exporter"}
vibepollo_ref_invalidations{job="vibepollo-exporter"}
```

No stream was started solely for verification; while no session is active the
exporter correctly exposes health metrics but no per-session series.
