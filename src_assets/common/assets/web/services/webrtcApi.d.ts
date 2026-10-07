import type {
  StreamConfig,
  WebRtcAnswer,
  WebRtcOffer,
  WebRtcSessionInfo,
  WebRtcSessionState,
} from '../types/webrtc';

export interface WebRtcApi {
  createSession(config: StreamConfig): Promise<WebRtcSessionInfo>;
  getSessionState(sessionId: string): Promise<WebRtcSessionFetchResult>;
  sendOffer(sessionId: string, offer: WebRtcOffer): Promise<WebRtcAnswer | null>;
  sendIceCandidates(sessionId: string, candidates: RTCIceCandidateInit[]): Promise<void>;
  sendIceCandidate(sessionId: string, candidate: RTCIceCandidateInit): Promise<void>;
  subscribeRemoteCandidates(
    sessionId: string,
    onCandidate: (candidate: RTCIceCandidateInit) => void,
  ): () => void;
  endSession(sessionId: string, options?: WebRtcSessionEndOptions): Promise<void>;
}

export interface WebRtcSessionFetchResult {
  status: number;
  session: WebRtcSessionState | null;
  error?: string;
}

export interface WebRtcSessionEndOptions {
  keepalive?: boolean;
}

export declare class WebRtcHttpApi implements WebRtcApi {
  createSession(config: StreamConfig): Promise<WebRtcSessionInfo>;
  getSessionState(sessionId: string): Promise<WebRtcSessionFetchResult>;
  sendOffer(sessionId: string, offer: WebRtcOffer): Promise<WebRtcAnswer | null>;
  sendIceCandidate(sessionId: string, candidate: RTCIceCandidateInit): Promise<void>;
  sendIceCandidates(sessionId: string, candidates: RTCIceCandidateInit[]): Promise<void>;
  subscribeRemoteCandidates(
    sessionId: string,
    onCandidate: (candidate: RTCIceCandidateInit) => void,
  ): () => void;
  endSession(sessionId: string, options?: WebRtcSessionEndOptions): Promise<void>;
}
