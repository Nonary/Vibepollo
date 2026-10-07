import type {
  RTSPSession,
  SessionDetail,
  SessionStatus,
  SessionSummary,
  WebRTCSession,
} from '../types/sessions';

export declare function fetchSessionHistory(
  limit?: number,
  offset?: number,
): Promise<SessionSummary[]>;
export declare function fetchSessionDetail(
  uuid: string,
  options?: { full?: boolean },
): Promise<SessionDetail>;
export declare function fetchSessionStatus(): Promise<SessionStatus | null>;
export declare function fetchRtspSessions(): Promise<RTSPSession[] | null>;
export declare function fetchWebRtcSessions(): Promise<WebRTCSession[] | null>;
export declare function deleteSessionHistory(uuid: string): Promise<void>;
