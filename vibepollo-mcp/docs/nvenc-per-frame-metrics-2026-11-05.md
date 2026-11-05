# Per-frame host metrics

Implemented in the Vibepollo source checkout.

## Exposed fields

`GET /api/rtsp/sessions` now includes the latest completed frame metrics for
each active RTSP session:

```text
frame_id
encode_wait_ms
encode_ms
packetize_ms
send_wait_ms
encode_queue_depth
send_queue_depth
pacing_lag_ms
socket_send_blocked_ms
frame_size_bytes
```

The Python exporter publishes the same values as:

```text
vibepollo_rtsp_frame_id
vibepollo_rtsp_encode_wait_ms
vibepollo_rtsp_encode_ms
vibepollo_rtsp_packetize_ms
vibepollo_rtsp_send_wait_ms
vibepollo_rtsp_encode_queue_depth
vibepollo_rtsp_send_queue_depth
vibepollo_rtsp_pacing_lag_ms
vibepollo_rtsp_socket_send_blocked_ms
vibepollo_rtsp_frame_size_bytes
```

All exporter metrics are labelled by `session_id`; `frame_id` is a metric
value, not a label, so frame-by-frame operation does not create unbounded
Prometheus cardinality.

## Semantics

- `encode_ms`: wall-clock time spent inside `NvEncEncodePicture()`.
- `encode_wait_ms`: Windows NVENC completion-event wait. It is zero for the
  synchronous path.
- `packetize_ms`: time from the video packet leaving the host send queue until
  the first send batch is ready; it includes FEC/header/encryption preparation
  and excludes pacing sleep.
- `send_wait_ms`: time spent sleeping for the configured pacing deadline.
- `pacing_lag_ms`: time by which the pacing deadline was already missed.
- `socket_send_blocked_ms`: wall-clock time inside the batch send operation,
  including fallback sends.
- `send_queue_depth`: queue depth observed when the frame is taken by the
  broadcast thread.
- `encode_queue_depth`: zero for the current synchronous single-frame NVENC
  submission path; it is reserved for a future pipelined encoder queue.
- `frame_size_bytes`: encoded frame payload size before transport framing.

These are host-observed timings. `encode_wait_ms` includes NVENC queueing plus
GPU completion; it is not a pure GPU-engine execution timestamp.
