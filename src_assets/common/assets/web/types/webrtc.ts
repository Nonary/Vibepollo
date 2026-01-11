export type EncodingType = 'h264' | 'hevc' | 'av1';

export interface StreamConfig {
  width: number;
  height: number;
  fps: number;
  encoding: EncodingType;
  bitrateKbps?: number;
  hdr?: boolean;
  audioChannels?: number;
  audioCodec?: 'opus' | 'aac';
  muteHostAudio?: boolean;
  profile?: string;
  appId?: number;
  resume?: boolean;
  clientName?: string;
  clientUuid?: string;
}

export interface WebRtcSessionInfo {
  sessionId: string;
  iceServers: RTCIceServer[];
  certFingerprint?: string;
  certPem?: string;
}

export interface WebRtcSessionState {
  id: string;
  audio?: boolean;
  video?: boolean;
  encoded?: boolean;
  audio_packets?: number;
  video_packets?: number;
  audio_dropped?: number;
  video_dropped?: number;
  audio_queue_frames?: number;
  video_queue_frames?: number;
  video_inflight_frames?: number;
  has_remote_offer?: boolean;
  has_local_answer?: boolean;
  ice_candidates?: number;
  width?: number | null;
  height?: number | null;
  fps?: number | null;
  bitrate_kbps?: number | null;
  codec?: string | null;
  hdr?: boolean | null;
  audio_channels?: number | null;
  audio_codec?: string | null;
  profile?: string | null;
  client_name?: string | null;
  client_uuid?: string | null;
  last_audio_bytes?: number;
  last_video_bytes?: number;
  last_video_idr?: boolean;
  last_video_frame_index?: number;
  last_audio_age_ms?: number | null;
  last_video_age_ms?: number | null;
}

export interface WebRtcOffer {
  type: RTCSdpType;
  sdp: string;
}

export interface WebRtcAnswer {
  type: RTCSdpType;
  sdp: string;
}

export interface WebRtcIceCandidate {
  sdpMid: string;
  sdpMLineIndex: number;
  candidate: string;
  index?: number;
}

export interface WebRtcStatsSnapshot {
  videoBitrateKbps?: number;
  videoFps?: number;
  audioBitrateKbps?: number;
  packetsLost?: number;
  roundTripTimeMs?: number;
  videoBytesReceived?: number;
  audioBytesReceived?: number;
  videoPacketsReceived?: number;
  audioPacketsReceived?: number;
  videoFramesReceived?: number;
  videoFramesDecoded?: number;
  videoFramesDropped?: number;
  videoDecodeMs?: number;
  videoJitterMs?: number;
  audioJitterMs?: number;
  videoJitterBufferMs?: number;
  audioJitterBufferMs?: number;
  videoPlayoutDelayMs?: number;
  audioPlayoutDelayMs?: number;
  videoCodec?: string;
  audioCodec?: string;
  candidatePair?: {
    state?: string;
    protocol?: string;
    localAddress?: string;
    localPort?: number;
    localType?: string;
    remoteAddress?: string;
    remotePort?: number;
    remoteType?: string;
  };
}

export interface GamepadFeedbackMessage {
  type: 'gamepad_feedback';
  event: 'rumble' | 'rumble_triggers' | 'motion_event_state';
  id: number;
  lowfreq?: number;
  highfreq?: number;
  left?: number;
  right?: number;
  motionType?: number;
  reportRate?: number;
}

export interface InputModifiers {
  alt: boolean;
  ctrl: boolean;
  shift: boolean;
  meta: boolean;
}

export type InputMessage =
  | {
      type: 'mouse_move';
      x: number;
      y: number;
      buttons: number;
      modifiers: InputModifiers;
      ts: number;
    }
  | {
      type: 'mouse_down' | 'mouse_up';
      button: number;
      x: number;
      y: number;
      modifiers: InputModifiers;
      ts: number;
    }
  | {
      type: 'wheel';
      dx: number;
      dy: number;
      x: number;
      y: number;
      modifiers: InputModifiers;
      ts: number;
    }
  | {
      type: 'key_down' | 'key_up';
      key: string;
      code: string;
      repeat: boolean;
      modifiers: InputModifiers;
      ts: number;
    }
  | {
      type: 'gamepad_connect';
      id: number;
      gamepadType: number;
      capabilities: number;
      supportedButtons: number;
      ts: number;
    }
  | {
      type: 'gamepad_disconnect';
      id: number;
      activeMask: number;
      ts: number;
    }
  | {
      type: 'gamepad_state';
      id: number;
      activeMask: number;
      buttons: number;
      gamepadType?: number;
      capabilities?: number;
      supportedButtons?: number;
      lt: number;
      rt: number;
      lsX: number;
      lsY: number;
      rsX: number;
      rsY: number;
      ts: number;
    }
  | {
      type: 'gamepad_motion';
      id: number;
      motionType: number;
      x: number;
      y: number;
      z: number;
      ts: number;
    };
