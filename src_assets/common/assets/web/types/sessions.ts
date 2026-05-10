export interface SessionStatus {
  activeSessions: number;
  appRunning: boolean;
  appName: string;
  paused: boolean;
  status: boolean;
}

export interface RTSPSession {
  uuid: string;
  device_name: string;
  width: number;
  height: number;
  fps: number;
  encoder_bitrate_kbps: number;
  requested_bitrate_kbps?: number;
  video_format: number;
  codec: string;
  hdr: boolean;
  yuv444: boolean;
  audio_channels: number;
  state: string;
  frames_sent: number;
  packets_sent: number;
  bytes_sent: number;
  idr_requests: number;
  invalidate_ref_count: number;
  client_reported_losses: number;
  encode_latency_ms: number;
  last_frame_index: number;
  uptime_seconds: number;
}

export interface WebRTCSession {
  id: string;
  audio: boolean;
  video: boolean;
  encoded: boolean;
  audio_packets: number;
  video_packets: number;
  audio_dropped: number;
  video_dropped: number;
  audio_queue_frames: number;
  video_queue_frames: number;
  video_inflight_frames: number;
  has_remote_offer: boolean;
  has_local_answer: boolean;
  ice_candidates: number;
  width?: number;
  height?: number;
  fps?: number;
  encoder_bitrate_kbps?: number;
  requested_bitrate_kbps?: number;
  codec?: string;
  hdr?: boolean;
  yuv444?: boolean;
  audio_channels?: number;
  audio_codec?: string;
  profile?: string;
  video_pacing_mode?: string;
  video_pacing_slack_ms?: number;
  video_max_frame_age_ms?: number;
  video_bytes_total: number;
  audio_bytes_total: number;
  bytes_sent: number;
  last_audio_bytes: number;
  last_video_bytes: number;
  last_video_idr: boolean;
  last_video_frame_index: number;
  last_audio_age_ms?: number;
  last_video_age_ms?: number;
}

export interface SessionSummary {
  uuid: string;
  protocol: string;
  client_name: string;
  device_name: string;
  app_name: string;
  codec: string;
  verdict?: string;
  width: number;
  height: number;
  target_fps: number;
  encoder_bitrate_kbps: number;
  requested_bitrate_kbps?: number;
  audio_channels: number;
  hdr: boolean;
  yuv444?: boolean;
  server_version?: string;
  start_time_unix: number;
  end_time_unix?: number;
  duration_seconds: number;
  host_cpu_model?: string;
  host_gpu_model?: string;
}

export interface SessionSample {
  session_uuid: string;
  timestamp_unix: number;
  bytes_sent_total: number;
  packets_sent_video: number;
  frames_sent: number;
  last_frame_index: number;
  video_dropped: number;
  audio_dropped: number;
  client_reported_losses: number;
  idr_requests: number;
  ref_invalidations: number;
  encode_latency_ms: number;
  actual_fps: number;
  actual_bitrate_kbps: number;
  frame_interval_jitter_ms: number;
  host_cpu_percent?: number;
  host_gpu_percent?: number;
  host_gpu_encoder_percent?: number;
  host_ram_percent?: number;
  host_vram_percent?: number;
  host_cpu_temp_c?: number;
  host_gpu_temp_c?: number;
  host_net_rx_bps?: number;
  host_net_tx_bps?: number;
}

export interface SessionEvent {
  session_uuid: string;
  timestamp_unix: number;
  event_type: string;
  payload: string;
}

export interface SessionDetail extends SessionSummary {
  total_samples?: number;
  total_events?: number;
  samples_truncated?: boolean;
  events_truncated?: boolean;
  samples: SessionSample[];
  events: SessionEvent[];
}

export interface ActiveSession {
  uuid: string;
  protocol: string;
  client_name: string;
  device_name: string;
  app_name: string;
  codec: string;
  width: number;
  height: number;
  target_fps: number;
  encoder_bitrate_kbps: number;
  requested_bitrate_kbps?: number;
  hdr: boolean;
  yuv444?: boolean;
  uptime_seconds: number;
  actual_fps: number;
  actual_bitrate_kbps: number;
  encode_latency_ms: number;
  frame_interval_jitter_ms: number;
  frames_sent: number;
  bytes_sent: number;
  client_reported_losses: number;
  idr_requests: number;
}

export interface SessionHistoryResponse {
  sessions: SessionSummary[];
}

export interface ActiveSessionsResponse {
  sessions: ActiveSession[];
}
