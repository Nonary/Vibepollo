import type { WebRtcApi } from '../../services/webrtcApi';
import type {
  GamepadFeedbackMessage,
  StreamConfig,
  WebRtcStatsSnapshot,
} from '../../types/webrtc';

export interface WebRtcClientCallbacks {
  onRemoteStream?: (stream: MediaStream) => void;
  onConnectionState?: (state: RTCPeerConnectionState) => void;
  onIceState?: (state: RTCIceConnectionState) => void;
  onInputChannelState?: (state: RTCDataChannelState) => void;
  onStats?: (stats: WebRtcStatsSnapshot) => void;
  onInputMessage?: (message: GamepadFeedbackMessage) => void;
  onNegotiatedEncoding?: (encoding: string) => void;
  onWarning?: (warning: string) => void;
  onError?: (error: Error) => void;
}

export interface WebRtcClientConnectOptions {
  inputPriority?: RTCPriorityType;
}

export interface WebRtcDisconnectOptions {
  keepalive?: boolean;
}

export declare class WebRtcClient {
  constructor(api: WebRtcApi);
  get connectionState(): RTCPeerConnectionState | undefined;
  get inputChannelState(): RTCDataChannelState | undefined;
  get inputChannelBufferedAmount(): number | undefined;
  get peerConnection(): RTCPeerConnection | undefined;
  connect(
    config: StreamConfig,
    callbacks?: WebRtcClientCallbacks,
    options?: WebRtcClientConnectOptions,
  ): Promise<string>;
  disconnect(options?: WebRtcDisconnectOptions): Promise<void>;
  setAudioLatencyTargets(targetMs: number, playoutDelayHintMs?: number): void;
  setVideoLatencyTarget(targetMs?: number): void;
  sendInput(payload: string | ArrayBuffer): boolean;
}
