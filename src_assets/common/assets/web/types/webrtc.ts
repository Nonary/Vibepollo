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
  profile?: string;
  appId?: number;
  resume?: boolean;
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
  videoFramesDecoded?: number;
  videoFramesDropped?: number;
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
    };
