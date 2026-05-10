import { http } from '@/http';
import type {
  SessionDetail,
  SessionHistoryResponse,
  SessionStatus,
  SessionSummary,
  RTSPSession,
  WebRTCSession,
} from '@/types/sessions';

export async function fetchSessionHistory(
  limit?: number,
  offset?: number,
): Promise<SessionSummary[]> {
  const params: Record<string, number> = {};
  if (limit != null) params['limit'] = limit;
  if (offset != null) params['offset'] = offset;
  const r = await http.get<SessionHistoryResponse>('/api/history/sessions', { params });
  return r.data?.sessions ?? [];
}

export async function fetchSessionDetail(
  uuid: string,
  options?: { full?: boolean },
): Promise<SessionDetail> {
  const params: Record<string, string> = {};
  if (options?.full) {
    params['full'] = '1';
  }
  const r = await http.get<SessionDetail>(`/api/history/sessions/${encodeURIComponent(uuid)}`, {
    params,
  });
  return r.data;
}

export async function fetchSessionStatus(): Promise<SessionStatus | null> {
  try {
    const r = await http.get<SessionStatus>('/api/session/status', {
      validateStatus: () => true,
    });
    return r.status === 200 && r.data ? r.data : null;
  } catch {
    return null;
  }
}

export async function fetchRtspSessions(): Promise<RTSPSession[] | null> {
  try {
    const r = await http.get<{ sessions: RTSPSession[] }>('/api/rtsp/sessions', {
      validateStatus: () => true,
    });
    return r.status === 200 && r.data?.sessions ? r.data.sessions : null;
  } catch {
    return null;
  }
}

export async function fetchWebRtcSessions(): Promise<WebRTCSession[] | null> {
  try {
    const r = await http.get<{ sessions: WebRTCSession[] }>('/api/webrtc/sessions', {
      validateStatus: () => true,
    });
    return r.status === 200 && r.data?.sessions ? r.data.sessions : null;
  } catch {
    return null;
  }
}

export async function deleteSessionHistory(uuid: string): Promise<void> {
  await http.delete(`/api/history/sessions/${encodeURIComponent(uuid)}`);
}
