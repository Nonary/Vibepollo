export type EncodingType = 'h264' | 'hevc' | 'av1';

export interface StreamConfig {
  width: number;
  height: number;
  fps: number;
  encoding: EncodingType;
  bitrateKbps?: number;
}

export interface WebRtcSessionInfo {
  sessionId: string;
  iceServers: RTCIceServer[];
}

export interface WebRtcOffer {
  type: RTCSdpType;
  sdp: string;
}

export interface WebRtcAnswer {
  type: RTCSdpType;
  sdp: string;
}

export interface WebRtcStatsSnapshot {
  videoBitrateKbps?: number;
  videoFps?: number;
  audioBitrateKbps?: number;
  packetsLost?: number;
  roundTripTimeMs?: number;
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
