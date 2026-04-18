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
  target_bitrate_kbps: number;
  client_bitrate_kbps?: number;
  audio_channels: number;
  hdr: boolean;
  start_time_unix: number;
  end_time_unix?: number;
  duration_seconds: number;
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
}

export interface SessionEvent {
  session_uuid: string;
  timestamp_unix: number;
  event_type: string;
  payload: string;
}

export interface SessionDetail extends SessionSummary {
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
  target_bitrate_kbps: number;
  client_bitrate_kbps?: number;
  hdr: boolean;
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
