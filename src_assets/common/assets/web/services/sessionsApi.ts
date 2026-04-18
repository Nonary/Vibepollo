import { http } from '@/http';
import type {
  SessionSummary,
  SessionDetail,
  ActiveSession,
  SessionHistoryResponse,
  ActiveSessionsResponse,
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

export async function fetchSessionDetail(uuid: string): Promise<SessionDetail> {
  const r = await http.get<SessionDetail>(`/api/history/sessions/${encodeURIComponent(uuid)}`);
  return r.data;
}

export async function fetchActiveSessions(): Promise<ActiveSession[]> {
  const r = await http.get<ActiveSessionsResponse>('/api/history/sessions/active');
  return r.data?.sessions ?? [];
}
